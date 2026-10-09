#include "banner.h"

#include "app.h"
#include "logging.h"
#include "surface.h"

#include <gdiplus.h>

namespace {

// 提示横幅的尺寸、颜色和淡出节奏。尺寸都是 DIP，按屏幕 DPI 缩放。
constexpr wchar_t kBannerClass[] = L"CapsLockSwitcherBanner";
constexpr wchar_t kBannerFontFace[] = L"Microsoft YaHei UI";
constexpr int kBannerWidthDip = 200;
constexpr int kBannerHeightDip = 96;
constexpr int kBannerRadiusDip = 26;
constexpr int kBannerTextDip = 40;
constexpr int kBannerAlpha = 255;
constexpr int kBannerFadeStep = 22;
constexpr UINT kBannerHoldMs = 500;
constexpr UINT kBannerFadeStepMs = 16;
constexpr UINT_PTR kTimerBannerFade = 1;
constexpr UINT_PTR kTimerSwitchSettle = 2;

// 注入的 Ctrl+Space 要先被目标窗口处理掉，不然读到的还是切换前的旧状态。
constexpr UINT kSwitchSettleMs = 50;

// 中文红底、英文蓝底（这两个跟鼠标指针共用，在 app.h 里）、大写锁定紫底，文字都是白的。
constexpr COLORREF kCapsLockColor = RGB(0x93, 0x33, 0xEA);       // #9333EA
// 小写用淡一半的紫（就是上面那个紫往白里兑 50%），一眼能和大写区分开。
constexpr COLORREF kCapsLockLowerColor = RGB(0xC9, 0x99, 0xF4);  // #C999F4

// 横幅要显示哪一种：输入法的中/英，还是大写锁定的大写/小写。
enum class BannerKind {
	InputMethod,
	CapsLock,
};

// WM_IME_CONTROL 的消息本身在 winuser.h 里；这两个常量定义在 immdev.h 里，
// 直接写出来就不用链接 imm32 了。
constexpr WPARAM kImeGetConversionMode = 0x0001;  // IMC_GETCONVERSIONMODE
constexpr DWORD_PTR kImeCmodeNative = 0x0001;     // IME_CMODE_NATIVE

HWND g_bannerWnd = nullptr;
HBITMAP g_bannerBitmap = nullptr;
HDC g_bannerMemDc = nullptr;
void* g_bannerBits = nullptr;
int g_bannerWidth = 0;
int g_bannerHeight = 0;
int g_bannerAlpha = 0;
UINT g_bannerHoldTicks = 0;
BannerKind g_bannerKind = BannerKind::InputMethod;
// 输入法横幅：是不是中文；大写锁定横幅：是不是大写。
bool g_bannerState = false;
const wchar_t* g_bannerText = L"";
// 上一次成功读到的状态：读不到时沿用，免得闪出一个错的中英文。
bool g_lastKnownChinese = false;

BOOL CALLBACK FindImeWindow(HWND wnd, LPARAM param) {
	wchar_t cls[64] = {};
	if (GetClassNameW(wnd, cls, _countof(cls)) != 0 && wcscmp(cls, L"IME") == 0) {
		*reinterpret_cast<HWND*>(param) = wnd;
		return FALSE;
	}
	return TRUE;
}

// 前台窗口所在线程的 IME 窗口。ImmGetDefaultIMEWnd 做的就是这件事，但那要 imm32。
HWND DefaultImeWindow(const HWND foreground) {
	const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
	if (thread == 0) {
		return nullptr;
	}
	HWND found = nullptr;
	EnumThreadWindows(thread, FindImeWindow, reinterpret_cast<LPARAM>(&found));
	return found;
}

// 这次横幅该用什么底色。
COLORREF BannerFill() {
	if (g_bannerKind == BannerKind::CapsLock) {
		return g_bannerState ? kCapsLockColor : kCapsLockLowerColor;
	}
	return g_bannerState ? kChineseColor : kEnglishColor;
}

// 一个像素被圆角矩形盖住的比例，按 4x4 子采样算。
unsigned char RoundedRectCoverage(const int px, const int py, const Gdiplus::REAL radius) {
	const Gdiplus::REAL right = static_cast<Gdiplus::REAL>(g_bannerWidth) - radius;
	const Gdiplus::REAL bottom = static_cast<Gdiplus::REAL>(g_bannerHeight) - radius;
	int hits = 0;
	for (int sy = 0; sy < 4; ++sy) {
		for (int sx = 0; sx < 4; ++sx) {
			const Gdiplus::REAL x = static_cast<Gdiplus::REAL>(px) + (sx + 0.5f) * 0.25f;
			const Gdiplus::REAL y = static_cast<Gdiplus::REAL>(py) + (sy + 0.5f) * 0.25f;
			// 把点夹进中间那个矩形，四个圆角就变成到最近圆心距离的比较。
			const Gdiplus::REAL nx = x < radius ? radius : (x > right ? right : x);
			const Gdiplus::REAL ny = y < radius ? radius : (y > bottom ? bottom : y);
			const Gdiplus::REAL dx = x - nx;
			const Gdiplus::REAL dy = y - ny;
			if ((dx * dx + dy * dy) <= (radius * radius)) {
				++hits;
			}
		}
	}
	return static_cast<unsigned char>(hits * 255 / 16);
}

// GDI 不维护 Alpha 通道，所以颜色画完之后，按同一套圆角几何把 Alpha 蒙版盖上去。
void StampRoundedAlpha(const Gdiplus::REAL radius) {
	auto* pixels = static_cast<DWORD*>(g_bannerBits);
	if (pixels == nullptr) {
		return;
	}
	const COLORREF fill = BannerFill();
	for (int y = 0; y < g_bannerHeight; ++y) {
		for (int x = 0; x < g_bannerWidth; ++x) {
			const unsigned char coverage = RoundedRectCoverage(x, y, radius);
			DWORD& pixel = pixels[y * g_bannerWidth + x];
			if (coverage == 255) {
				pixel |= 0xFF000000u;  // 内部：不透明，颜色不动
			} else if (coverage == 0) {
				pixel = 0;
			} else {
				// GDI+ 画的边已经是预乘的，这里用我们自己的覆盖度重建，
				// 免得 Alpha 被乘两遍。
				const DWORD red = (GetRValue(fill) * coverage) / 255;
				const DWORD green = (GetGValue(fill) * coverage) / 255;
				const DWORD blue = (GetBValue(fill) * coverage) / 255;
				pixel = (static_cast<DWORD>(coverage) << 24) | (red << 16) | (green << 8) | blue;
			}
		}
	}
}

void ReleaseBannerSurface() {
	DestroyArgbSurface(g_bannerBitmap, g_bannerMemDc, g_bannerBits);
	g_bannerWidth = 0;
	g_bannerHeight = 0;
}

bool RenderBanner(const int width, const int height, const int dpi) {
	if (g_bannerBitmap == nullptr || g_bannerWidth != width || g_bannerHeight != height) {
		ReleaseBannerSurface();
		if (!CreateArgbSurface(width, height, g_bannerBitmap, g_bannerMemDc, g_bannerBits)) {
			Log(L"banner: could not create the %dx%d surface (%lu)", width, height, GetLastError());
			return false;
		}
		g_bannerWidth = width;
		g_bannerHeight = height;
	}

	Gdiplus::REAL radius = static_cast<Gdiplus::REAL>(MulDiv(kBannerRadiusDip, dpi, 96));
	const int textHeight = MulDiv(kBannerTextDip, dpi, 96);
	const Gdiplus::REAL surfaceW = static_cast<Gdiplus::REAL>(g_bannerWidth);
	const Gdiplus::REAL surfaceH = static_cast<Gdiplus::REAL>(g_bannerHeight);
	if (radius > surfaceH * 0.5f) {
		radius = surfaceH * 0.5f;
	}
	const COLORREF fill = BannerFill();

	// 形状走 GDI+，圆角才有抗锯齿；它的析构会把绘制刷完再让 GDI 画字。
	{
		Gdiplus::Bitmap target(g_bannerWidth, g_bannerHeight, g_bannerWidth * 4,
		                       PixelFormat32bppPARGB, static_cast<BYTE*>(g_bannerBits));
		Gdiplus::Graphics canvas(&target);
		canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
		canvas.Clear(Gdiplus::Color(0, 0, 0, 0));
		canvas.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
		canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		canvas.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

		const Gdiplus::REAL diameter = radius * 2.0f;
		const Gdiplus::RectF r(0.0f, 0.0f, surfaceW, surfaceH);
		Gdiplus::GraphicsPath fillPath;
		fillPath.AddArc(r.X, r.Y, diameter, diameter, 180.0f, 90.0f);
		fillPath.AddArc(r.GetRight() - diameter, r.Y, diameter, diameter, 270.0f, 90.0f);
		fillPath.AddArc(r.GetRight() - diameter, r.GetBottom() - diameter, diameter, diameter, 0.0f, 90.0f);
		fillPath.AddArc(r.X, r.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
		fillPath.CloseFigure();
		Gdiplus::SolidBrush fillBrush(
		    Gdiplus::Color(255, GetRValue(fill), GetGValue(fill), GetBValue(fill)));
		canvas.FillPath(&fillBrush, &fillPath);
	}

	// 文字走 GDI：构造一次 Gdiplus::FontFamily 会让 GDI+ 枚举全部已安装字体，
	// 本机实测好几秒，提示会迟迟不出现。
	{
		const HFONT font = CreateFontW(-textHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
		                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
		                               ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
		                               kBannerFontFace);
		const HGDIOBJ previous = SelectObject(
		    g_bannerMemDc, font != nullptr ? font : GetStockObject(DEFAULT_GUI_FONT));
		SetBkMode(g_bannerMemDc, TRANSPARENT);
		SetTextColor(g_bannerMemDc, RGB(245, 245, 245));
		RECT textArea = { 0, 0, g_bannerWidth, g_bannerHeight };
		DrawTextW(g_bannerMemDc, g_bannerText, -1, &textArea,
		          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(g_bannerMemDc, previous);
		if (font != nullptr) {
			DeleteObject(font);
		}
	}

	GdiFlush();  // 把排队的 GDI 文字写完，再动 DIB 的内存
	StampRoundedAlpha(radius);
	return true;
}

void ShowBannerNow() {
	if (g_bannerWnd == nullptr) {
		return;
	}
	if (g_bannerKind == BannerKind::InputMethod) {
		g_bannerState = CurrentInputIsChinese();
		g_bannerText = g_bannerState ? L"中文" : L"English";
	} else {
		g_bannerText = g_bannerState ? L"大写" : L"小写";
	}

	const HWND foreground = GetForegroundWindow();
	const int dpi = ScreenDpi(foreground, g_bannerWnd);
	if (!RenderBanner(MulDiv(kBannerWidthDip, dpi, 96), MulDiv(kBannerHeightDip, dpi, 96), dpi)) {
		return;
	}

	g_bannerAlpha = kBannerAlpha;
	if (!PushLayeredSurface(g_bannerWnd, g_bannerMemDc, g_bannerWidth, g_bannerHeight, g_bannerAlpha,
	                        foreground)) {
		Log(L"banner: UpdateLayeredWindow failed (%lu)", GetLastError());
		return;
	}
	ShowWindow(g_bannerWnd, SW_SHOWNA);  // 可见，但绝不抢焦点
	g_bannerHoldTicks = kBannerHoldMs / kBannerFadeStepMs;
	SetTimer(g_bannerWnd, kTimerBannerFade, kBannerFadeStepMs, nullptr);
}

// 提示只是被"推"上去、并不绘制；停留和淡出由它自己的定时器驱动。
LRESULT CALLBACK BannerProc(const HWND hwnd, const UINT message, const WPARAM wParam,
                            const LPARAM lParam) {
	if (message == WM_TIMER) {
		if (wParam == kTimerSwitchSettle) {
			KillTimer(hwnd, kTimerSwitchSettle);
			ShowBannerNow();
			return 0;
		}
		if (wParam == kTimerBannerFade) {
			if (g_bannerHoldTicks > 0) {
				--g_bannerHoldTicks;
			} else {
				g_bannerAlpha -= kBannerFadeStep;
				if (g_bannerAlpha <= 0) {
					KillTimer(hwnd, kTimerBannerFade);
					ShowWindow(hwnd, SW_HIDE);
				} else {
					// 拿同一张位图、更小的整体透明度再推一次，位图本身不变。
					PushLayeredSurface(hwnd, g_bannerMemDc, g_bannerWidth, g_bannerHeight,
					                   g_bannerAlpha, GetForegroundWindow());
				}
			}
			return 0;
		}
	}
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // 匿名命名空间

// 前台线程的输入法是不是停在中文（native）模式。放在匿名命名空间外面：
// 鼠标指针那边（cursor.cpp）要用同一个判断，不能各读各的。
bool CurrentInputIsChinese(const DWORD timeoutMs) {
	const HWND foreground = GetForegroundWindow();
	if (foreground == nullptr) {
		return g_lastKnownChinese;
	}
	const HWND imeWnd = DefaultImeWindow(foreground);
	if (imeWnd == nullptr) {
		return g_lastKnownChinese;
	}
	DWORD_PTR mode = 0;
	if (SendMessageTimeoutW(imeWnd, WM_IME_CONTROL, kImeGetConversionMode, 0,
	                        SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, &mode) == 0) {
		return g_lastKnownChinese;
	}
	g_lastKnownChinese = (mode & kImeCmodeNative) != 0;
	return g_lastKnownChinese;
}

void CreateBannerWindow(const HINSTANCE instance) {
	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.lpfnWndProc = BannerProc;
	wcex.hInstance = instance;
	wcex.lpszClassName = kBannerClass;
	RegisterClassExW(&wcex);  // 注册失败就是没有提示，不影响别的功能

	g_bannerWnd = CreateWindowExW(
	    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
	    kBannerClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
}

void ShowSwitchBanner() {
	if (g_bannerWnd == nullptr) {
		return;
	}
	// 这次要显示的是输入法，必须先把类型改回来：不改的话会沿用上一次
	// （比如 Alt+CapsLock）的类型，切换中英文就只会显示"大写/小写"。
	g_bannerKind = BannerKind::InputMethod;
	// 等注入的快捷键先生效，再读状态。
	SetTimer(g_bannerWnd, kTimerSwitchSettle, kSwitchSettleMs, nullptr);
}

void ShowCapsLockBanner(const bool upper) {
	if (g_bannerWnd == nullptr) {
		return;
	}
	KillTimer(g_bannerWnd, kTimerSwitchSettle);  // 排队中的输入法横幅作废：这次要显示的是大写锁定
	g_bannerKind = BannerKind::CapsLock;
	g_bannerState = upper;
	ShowBannerNow();
}

void DestroyBannerWindow() {
	if (g_bannerWnd != nullptr) {
		DestroyWindow(g_bannerWnd);
		g_bannerWnd = nullptr;
	}
	ReleaseBannerSurface();
}
