#include "surface.h"

RECT WorkAreaFor(const HWND reference) {
	RECT area = {};
	MONITORINFO info = {};
	info.cbSize = sizeof(MONITORINFO);
	const HMONITOR monitor = MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST);
	if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
		return info.rcWork;
	}
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
	return area;
}

// 程序里所有尺寸都用 DIP 表示，在这里统一缩放。进程声明了 DPI 感知之后，
// 这里拿到的是屏幕的真实 DPI，所以启动画面按屏幕像素分辨率绘制，
// 而不是被拉伸上去——拉伸正是它以前发虚的原因。
int ScreenDpi(const HWND reference, const HWND surfaceWindow) {
	// 查自己那个 DPI 感知的窗口，因为前台应用可能根本不感知 DPI。
	const auto getWindowDpi = reinterpret_cast<UINT(WINAPI*)(HWND)>(
	    GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
	if (getWindowDpi != nullptr && surfaceWindow != nullptr) {
		if (MonitorFromWindow(surfaceWindow, MONITOR_DEFAULTTONEAREST) !=
		    MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST)) {
			const RECT area = WorkAreaFor(reference);
			SetWindowPos(surfaceWindow, nullptr, area.left, area.top, 0, 0,
			             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
		const UINT dpi = getWindowDpi(surfaceWindow);
		if (dpi != 0) {
			return static_cast<int>(dpi);
		}
	}
	// Windows 8.1 没有 GetDpiForWindow。
	const HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (shcore != nullptr) {
		using GetMonitorDpiFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
		const auto getDpi = reinterpret_cast<GetMonitorDpiFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
		UINT x = 0, y = 0;
		const bool ok = getDpi != nullptr &&
		    SUCCEEDED(getDpi(MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST), 0, &x, &y));
		FreeLibrary(shcore);
		if (ok && y > 0) {
			return static_cast<int>(y);
		}
	}
	const HDC screen = GetDC(nullptr);
	const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
	if (screen != nullptr) {
		ReleaseDC(nullptr, screen);
	}
	return dpi > 0 ? dpi : 96;
}

// 让进程声明"每显示器 DPI 感知"。不声明的话，Windows 会先把我们的窗口
// 渲染到一张更小的虚拟画布上，再拉伸到屏幕，启动画面就会像被放大过一样发虚。
void EnableDpiAwareness() {
	// Windows 10 1703 及以后。用动态解析，好让程序在导出符号更老的
	// user32 上也能启动。
	const HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (user32 != nullptr) {
		using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
		const auto setContext = reinterpret_cast<SetContextFn>(
		    GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
		if (setContext != nullptr &&
		    setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
			return;
		}
	}

	// Windows 8.1 的回退方案。
	const HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (shcore != nullptr) {
		using SetAwarenessFn = HRESULT(WINAPI*)(int);
		const auto setAwareness = reinterpret_cast<SetAwarenessFn>(
		    GetProcAddress(shcore, "SetProcessDpiAwareness"));
		const bool ok = setAwareness != nullptr &&
		                setAwareness(2 /* 每显示器 DPI 感知 */) == S_OK;
		FreeLibrary(shcore);
		if (ok) {
			return;
		}
	}

	SetProcessDPIAware();  // Vista 及以后
}

bool CreateArgbSurface(const int width, const int height, HBITMAP& bitmap, HDC& memoryDc, void*& bits) {
	if (width <= 0 || height <= 0) {
		return false;
	}
	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;  // 自顶向下，和 GDI+ 写入的布局一致
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;

	bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (bitmap == nullptr || bits == nullptr) {
		return false;
	}
	memoryDc = CreateCompatibleDC(nullptr);
	if (memoryDc == nullptr) {
		DeleteObject(bitmap);
		bitmap = nullptr;
		bits = nullptr;
		return false;
	}
	if (SelectObject(memoryDc, bitmap) == nullptr) {
		DeleteDC(memoryDc);
		DeleteObject(bitmap);
		memoryDc = nullptr;
		bitmap = nullptr;
		bits = nullptr;
		return false;
	}
	return true;
}

void DestroyArgbSurface(HBITMAP& bitmap, HDC& memoryDc, void*& bits) {
	if (memoryDc != nullptr) {
		DeleteDC(memoryDc);
		memoryDc = nullptr;
	}
	if (bitmap != nullptr) {
		DeleteObject(bitmap);
		bitmap = nullptr;
	}
	bits = nullptr;
}

// 把位图居中放到参考窗口所在的那台显示器上，并按给定的整体透明度推给窗口；
// 启动画面就是靠它淡出的。
bool PushLayeredSurface(const HWND hwnd, const HDC sourceDc, const int width, const int height,
                        const int alpha, const HWND monitorReference) {
	if (hwnd == nullptr || sourceDc == nullptr || width <= 0 || height <= 0) {
		return false;
	}

	const RECT area = WorkAreaFor(monitorReference);
	POINT destination = {};
	destination.x = area.left + ((area.right - area.left) - width) / 2;
	destination.y = area.top + ((area.bottom - area.top) - height) / 2;
	SIZE size = { width, height };
	POINT source = { 0, 0 };

	BLENDFUNCTION blend = {};
	blend.BlendOp = AC_SRC_OVER;
	blend.SourceConstantAlpha = static_cast<BYTE>(alpha);
	blend.AlphaFormat = AC_SRC_ALPHA;

	return UpdateLayeredWindow(hwnd, nullptr, &destination, &size, sourceDc, &source, 0,
	                           &blend, ULW_ALPHA) != FALSE;
}
