#include "splash.h"

#include "app.h"
#include "logging.h"
#include "resource.h"
#include "surface.h"

#include <cstring>
#include <gdiplus.h>
#include <memory>

// 在这里声明链接，项目文件就不用改。
#pragma comment(lib, "gdiplus.lib")

namespace {

// 启动画面：键帽图先停留片刻再淡出，需要单独一个窗口——它带逐像素 Alpha，
// 整窗统一透明度表达不出这种效果。
constexpr wchar_t kSplashClass[] = L"CapsLockSwitcherSplash";
constexpr int kSplashAlphaMax = 255;
constexpr UINT kSplashHoldMs = 500;  // 这段时间保持完全不透明
constexpr UINT kSplashFadeMs = 300;  // 之后用这么长时间淡出
constexpr UINT kSplashTickMs = 15;   // 淡出定时器的粒度
constexpr int kSplashMaxHeightPercent = 30;  // 占工作区高度的比例；从不放大
constexpr UINT_PTR kTimerSplashFade = 1;     // 启动画面自己的定时器

// 启动画面窗口，以及它绘制用的预乘 Alpha（ARGB）位图。
HWND g_splashWnd = nullptr;
HBITMAP g_splashBitmap = nullptr;
HDC g_splashMemDc = nullptr;
void* g_splashBits = nullptr;
int g_splashWidth = 0;
int g_splashHeight = 0;
int g_splashAlpha = 0;
ULONGLONG g_splashFadeStart = 0;  // 该开始淡出的 tick

ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusReady = false;

bool ApplySplashAlpha() {
	if (g_splashWnd == nullptr || g_splashMemDc == nullptr) {
		return false;
	}
	if (!PushLayeredSurface(g_splashWnd, g_splashMemDc, g_splashWidth, g_splashHeight,
	                        g_splashAlpha, nullptr)) {
		Log(L"splash: UpdateLayeredWindow failed (%lu)", GetLastError());
		return false;
	}
	return true;
}

// 启动画面只是被"推"上去、并不绘制；淡出的每一步由它自己的定时器驱动。
LRESULT CALLBACK SplashProc(const HWND hwnd, const UINT message, const WPARAM wParam,
                            const LPARAM lParam) {
	if (message == WM_TIMER && wParam == kTimerSplashFade) {
		// 用实际流逝的时间驱动淡出，定时器再怎么抖动，停留和淡出
		// 也还是要求的那么长。
		const ULONGLONG now = GetTickCount64();
		if (now >= g_splashFadeStart + kSplashFadeMs) {
			KillTimer(hwnd, kTimerSplashFade);
			g_splashAlpha = 0;
			ShowWindow(hwnd, SW_HIDE);
		} else if (now > g_splashFadeStart) {
			const ULONGLONG elapsed = now - g_splashFadeStart;
			g_splashAlpha = kSplashAlphaMax -
			                static_cast<int>(kSplashAlphaMax * elapsed / kSplashFadeMs);
			ApplySplashAlpha();
		}
		// 否则就是还在停留阶段，什么都不用做
		return 0;
	}
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // 匿名命名空间

void ReleaseSplash() {
	DestroyArgbSurface(g_splashBitmap, g_splashMemDc, g_splashBits);
}

bool PrepareSplash() {
	if (!g_gdiplusReady) {
		return false;
	}
	const HRSRC resource = FindResourceW(g_hInst, MAKEINTRESOURCEW(IDR_SPLASH_IMAGE), RT_RCDATA);
	if (resource == nullptr) {
		Log(L"splash: embedded PNG resource not found");
		return false;
	}
	const DWORD size = SizeofResource(g_hInst, resource);
	const HGLOBAL loaded = LoadResource(g_hInst, resource);
	const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
	if (bytes == nullptr || size == 0) {
		Log(L"splash: could not read the embedded PNG");
		return false;
	}

	HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
	if (copy == nullptr) {
		return false;
	}
	void* destination = GlobalLock(copy);
	if (destination == nullptr) {
		GlobalFree(copy);
		return false;
	}
	memcpy(destination, bytes, size);
	GlobalUnlock(copy);

	IStream* stream = nullptr;
	if (CreateStreamOnHGlobal(copy, TRUE, &stream) != S_OK) {
		GlobalFree(copy);
		return false;
	}

	// GDI+ 可能是惰性读取的，所以要在释放源数据流之前销毁图像。
	const auto releaseStream = [](IStream* value) { value->Release(); };
	const std::unique_ptr<IStream, decltype(releaseStream)> streamOwner(stream, releaseStream);
	const std::unique_ptr<Gdiplus::Image> image(Gdiplus::Image::FromStream(stream));
	if (image == nullptr) {
		return false;
	}
	if (image->GetLastStatus() != Gdiplus::Ok) {
		Log(L"splash: GDI+ could not decode the PNG");
		return false;
	}

	// 图片的原始尺寸按 DIP 算，所以用屏幕 DPI 缩放：启动画面看上去还是原来那么大，
	// 但按真实像素分辨率渲染，而不是被拉伸上去。同时还要塞得进工作区，
	// 而且放大倍数不超过 DPI 系数。
	const int dpi = ScreenDpi(nullptr, g_splashWnd);
	const RECT area = WorkAreaFor(nullptr);
	const int maxHeight = MulDiv(area.bottom - area.top, kSplashMaxHeightPercent, 100);

	int width = MulDiv(static_cast<int>(image->GetWidth()), dpi, 96);
	int height = MulDiv(static_cast<int>(image->GetHeight()), dpi, 96);
	if (maxHeight > 0 && height > maxHeight) {
		width = MulDiv(width, maxHeight, height);
		height = maxHeight;
	}

	if (!CreateArgbSurface(width, height, g_splashBitmap, g_splashMemDc, g_splashBits)) {
		return false;
	}

	{
		// 往 PARGB 位图上绘制时，GDI+ 会边写边做预乘——
		// 这正是 UpdateLayeredWindow 要的格式。
		Gdiplus::Bitmap target(width, height, width * 4, PixelFormat32bppPARGB,
		                       static_cast<BYTE*>(g_splashBits));
		Gdiplus::Graphics canvas(&target);
		canvas.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
		const Gdiplus::Status drawn = canvas.DrawImage(image.get(), Gdiplus::Rect(0, 0, width, height), 0, 0,
		                 static_cast<INT>(image->GetWidth()),
		                 static_cast<INT>(image->GetHeight()),
		                 Gdiplus::UnitPixel);
		if (drawn != Gdiplus::Ok) {
			Log(L"splash: GDI+ rendering failed (%d)", static_cast<int>(drawn));
			return false;
		}
	}

	g_splashWidth = width;
	g_splashHeight = height;
	return true;
}

void ShowSplash() {
	if (g_splashWnd == nullptr || g_splashWidth == 0) {
		return;
	}
	g_splashAlpha = kSplashAlphaMax;
	if (!ApplySplashAlpha()) {
		return;
	}
	ShowWindow(g_splashWnd, SW_SHOWNA);  // 可见，但绝不抢焦点
	g_splashFadeStart = GetTickCount64() + kSplashHoldMs;
	SetTimer(g_splashWnd, kTimerSplashFade, kSplashTickMs, nullptr);
}

void DestroySplashWindow() {
	if (g_splashWnd != nullptr) {
		DestroyWindow(g_splashWnd);
		g_splashWnd = nullptr;
	}
	ReleaseSplash();
}

void CreateSplashWindow(const HINSTANCE instance) {
	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.lpfnWndProc = SplashProc;
	wcex.hInstance = instance;
	wcex.lpszClassName = kSplashClass;
	RegisterClassExW(&wcex);

	g_splashWnd = CreateWindowExW(
	    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
	    kSplashClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
}

bool StartGdiplus() {
	Gdiplus::GdiplusStartupInput input;
	if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) != Gdiplus::Ok) {
		return false;
	}
	g_gdiplusReady = true;
	return true;
}

void StopGdiplus() {
	if (g_gdiplusReady) {
		Gdiplus::GdiplusShutdown(g_gdiplusToken);
		g_gdiplusReady = false;
	}
}
