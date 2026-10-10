#include "cursor.h"

#include "app.h"
#include "banner.h"
#include "logging.h"
#include "tray.h"

#include <vector>

namespace {

// 染哪几个光标槽。等待/后台等待是 .ani 动画光标，换成静止的一帧看着像卡死，不碰。
// winuser.h 里 OCR_* 被 OEMRESOURCE 包着、IDC_* 又是 MAKEINTRESOURCE 指针，
// 所以直接写数字（和 banner.cpp 里手写 IMC_GETCONVERSIONMODE 是同一个做法）。
constexpr DWORD kCursorIds[] = {
	32512,  // OCR_NORMAL：箭头
	32513,  // OCR_IBEAM：文本 I 型
	32649,  // OCR_HAND：链接手型
};

constexpr UINT kSettleMs = 50;         // 和横幅一样：等目标窗口先吃下这次切换
constexpr UINT kPollMs = 500;          // 同窗口内自己换输入法时的兜底
constexpr UINT kReapplyTicks = 20;     // 每 20 个轮询周期（10 秒）无条件重刷一次
constexpr DWORD kProbeTimeoutMs = 50;  // 轮询路径的探测超时，别把主线程钉住

struct CursorSource {
	int width = 0;
	int height = 0;
	DWORD xHotspot = 0;
	DWORD yHotspot = 0;
	bool darkInk = false;  // 本体是黑的还是白的：决定染色朝哪一边靠
	bool ready = false;
	std::vector<DWORD> pixels;  // straight alpha，自顶向下，0xAARRGGBB
};

enum class Tint {
	None,
	Chinese,
	English,
};

CursorSource g_sources[std::size(kCursorIds)];
Tint g_applied = Tint::None;  // 系统光标槽现在被染成什么样
Tint g_target = Tint::None;   // 想要的颜色，重刷时直接用，不用再探测
HWINEVENTHOOK g_foregroundHook = nullptr;
ULONGLONG g_lastProbeTick = 0;
unsigned g_pollTicks = 0;
bool g_resetting = false;  // 我们自己发起的重载：别当成"用户改了方案"再处理一遍

// 0% 就是"不变"：这时和以前关掉那个开关是一回事，系统光标保持用户自己的样子。
bool TintOn() {
	return g_cursorTintPercent.load() > 0;
}

int Luma(const DWORD px) {
	return static_cast<int>(((px >> 16 & 0xFF) * 299 + (px >> 8 & 0xFF) * 587 +
	                         (px & 0xFF) * 114) /
	                        1000);
}

// 本体是亮还是暗。Win10/11 默认箭头是"白体黑边"，也有方案是"黑体白边"，
// 上色的方向正好相反，所以先看一眼再决定。
bool DetectDarkInk(const std::vector<DWORD>& pixels) {
	long long sum = 0;
	int count = 0;
	for (const DWORD px : pixels) {
		if ((px >> 24) < 200) {
			continue;
		}
		sum += Luma(px);
		++count;
	}
	return count > 0 && sum / count < 128;
}

// 保形上色：本体朝目标色靠拢，另一头的描边保持原样。
//   亮体：out = lerp(黑, 目标色, L/255)      —— 白体变目标色，黑边还是黑
//   暗体：out = lerp(白, 目标色, (255-L)/255) —— 黑体变目标色，白边还是白
// percent 是上色程度：100 就是上面那个满色，0 原样不动，中间按比例在"原来的像素"
// 和满色之间插值——所以滑块越小，红/蓝越淡。
// 只动 RGB 不动 A，保持 straight alpha。
void TintPixels(DWORD* pixels, const size_t count, const COLORREF color, const bool darkInk,
                const int percent) {
	const int tint[3] = { GetRValue(color), GetGValue(color), GetBValue(color) };
	const int keep = darkInk ? 255 : 0;
	for (size_t i = 0; i < count; ++i) {
		const DWORD px = pixels[i];
		if ((px >> 24) == 0) {
			continue;
		}
		const int weight = darkInk ? 255 - Luma(px) : Luma(px);
		const int channel[3] = { static_cast<int>((px >> 16) & 0xFF),
			                     static_cast<int>((px >> 8) & 0xFF),
			                     static_cast<int>(px & 0xFF) };
		DWORD out = px & 0xFF000000;
		for (int c = 0; c < 3; ++c) {
			const int full = keep + (tint[c] - keep) * weight / 255;
			out |= static_cast<DWORD>(channel[c] + (full - channel[c]) * percent / 100)
			       << (16 - c * 8);
		}
		pixels[i] = out;
	}
}

// 32bpp 自顶向下 DIB。注意不要多建一个内存 DC 把位图选进去：
// CreateIconIndirect 要求这两份位图没有被任何 DC 选着。
HBITMAP MakeColorDib(const std::vector<DWORD>& pixels, const int width, const int height) {
	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	const HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (bitmap != nullptr && bits != nullptr) {
		CopyMemory(bits, pixels.data(), pixels.size() * sizeof(DWORD));
	}
	return bitmap;
}

// 掩码按 alpha 反推：全透明的像素是 1。全 0 的掩码在只看 alpha 的路径上没问题，
// 但在忽略 alpha 的路径（远程桌面之类）会画出一整块色块。
HBITMAP MakeMaskDib(const std::vector<DWORD>& pixels, const int width, const int height) {
	const int stride = (width + 31) / 32 * 4;
	std::vector<BYTE> bits(static_cast<size_t>(stride) * height, 0);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if ((pixels[static_cast<size_t>(y) * width + x] >> 24) == 0) {
				bits[y * stride + x / 8] |= static_cast<BYTE>(0x80 >> (x % 8));
			}
		}
	}
	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 1;
	info.bmiHeader.biCompression = BI_RGB;
	info.bmiColors[1].rgbRed = info.bmiColors[1].rgbGreen = info.bmiColors[1].rgbBlue = 255;
	void* target = nullptr;
	const HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &target, nullptr, 0);
	if (bitmap != nullptr && target != nullptr) {
		CopyMemory(target, bits.data(), bits.size());
	}
	return bitmap;
}

// 拿一份像素造光标。CreateIconIndirect 会自己拷走两份位图，所以这里用完就地删。
// 返回的句柄归我们所有：SetSystemCursor 收下之后才由系统负责销毁。
HCURSOR PackCursor(const CursorSource& source, const std::vector<DWORD>& pixels) {
	const HBITMAP color = MakeColorDib(pixels, source.width, source.height);
	const HBITMAP mask = MakeMaskDib(pixels, source.width, source.height);
	if (color == nullptr || mask == nullptr) {
		DeleteObject(color);
		DeleteObject(mask);
		return nullptr;
	}
	ICONINFO icon = {};
	icon.fIcon = FALSE;
	icon.xHotspot = source.xHotspot;  // 热点必须原样带上：I 型歪了点击落点就不准
	icon.yHotspot = source.yHotspot;
	icon.hbmMask = mask;
	icon.hbmColor = color;
	const HCURSOR cursor = static_cast<HCURSOR>(CreateIconIndirect(&icon));
	DeleteObject(color);
	DeleteObject(mask);
	return cursor;
}

// hbmColor 里的 32bpp 像素（带 alpha 的光标走这条）。
bool ReadColorPixels(const HBITMAP bitmap, CursorSource& out) {
	BITMAP bm = {};
	if (bitmap == nullptr || GetObjectW(bitmap, sizeof(bm), &bm) == 0 || bm.bmBitsPixel < 24) {
		return false;
	}
	out.width = bm.bmWidth;
	out.height = bm.bmHeight;
	out.pixels.assign(static_cast<size_t>(out.width) * out.height, 0);

	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = out.width;
	info.bmiHeader.biHeight = -out.height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	const HDC screen = GetDC(nullptr);
	const int lines =
	    GetDIBits(screen, bitmap, 0, out.height, out.pixels.data(), &info, DIB_RGB_COLORS);
	ReleaseDC(nullptr, screen);
	if (lines == 0) {
		return false;
	}
	for (const DWORD px : out.pixels) {
		if ((px >> 24) != 0) {
			return true;  // 有 alpha 就用 alpha
		}
	}
	// alpha 一个都没填的光标：实测 200% DPI 下的经典 I 型就是这样——形状画在 RGB 里、
	// alpha 全是 0，系统渲染时根本不看 alpha。这里把"RGB 非 0"当成不透明。
	int promoted = 0;
	for (DWORD& px : out.pixels) {
		if ((px & 0xFFFFFF) != 0) {
			px |= 0xFF000000;
			++promoted;
		}
	}
	return promoted > 0;
}

// 只有掩码的光标（hbmColor 是空的）。两种摆法都要认：
//   高度 = 光标高度的两倍（上半 AND 掩码、下半 XOR 掩码，经典的单色反色光标）
//   高度 = 光标高度（只有一张 AND 掩码：0 = 画成黑色，1 = 屏幕照旧）
bool ReadMonoPixels(const HBITMAP maskBitmap, CursorSource& out) {
	BITMAP bm = {};
	if (maskBitmap == nullptr || GetObjectW(maskBitmap, sizeof(bm), &bm) == 0 ||
	    bm.bmBitsPixel != 1 || bm.bmHeight < 2) {
		return false;
	}
	const bool doubled = bm.bmHeight == GetSystemMetrics(SM_CYCURSOR) * 2;
	out.width = bm.bmWidth;
	out.height = doubled ? bm.bmHeight / 2 : bm.bmHeight;
	const int stride = (out.width + 31) / 32 * 4;
	std::vector<BYTE> bits(static_cast<size_t>(stride) * bm.bmHeight, 0);

	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = out.width;
	info.bmiHeader.biHeight = -bm.bmHeight;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 1;
	info.bmiHeader.biCompression = BI_RGB;
	info.bmiColors[1].rgbRed = info.bmiColors[1].rgbGreen = info.bmiColors[1].rgbBlue = 255;
	const HDC screen = GetDC(nullptr);
	const int lines =
	    GetDIBits(screen, maskBitmap, 0, bm.bmHeight, bits.data(), &info, DIB_RGB_COLORS);
	ReleaseDC(nullptr, screen);
	if (lines == 0) {
		return false;
	}

	out.pixels.assign(static_cast<size_t>(out.width) * out.height, 0);
	int opaque = 0;
	for (int y = 0; y < out.height; ++y) {
		for (int x = 0; x < out.width; ++x) {
			const int shift = 7 - x % 8;
			const bool andBit = (bits[y * stride + x / 8] >> shift & 1) != 0;
			const bool xorBit = doubled
			                        ? (bits[(y + out.height) * stride + x / 8] >> shift & 1) != 0
			                        : false;
			if (andBit && !xorBit) {
				continue;  // 屏幕照旧 = 透明
			}
			// 单张掩码的摆法没有颜色信息，掩码为 0 的地方就是黑色。
			out.pixels[static_cast<size_t>(y) * out.width + x] =
			    !doubled ? 0xFF000000 : (xorBit ? 0xFFFFFFFF : 0xFF000000);
			++opaque;
		}
	}
	return opaque > 0;
}

// 读一个系统光标槽现在的样子。注意 LoadCursor 拿到的就是槽里那个共享句柄，
// 只能读、不能毁，也不能直接喂给 SetSystemCursor。
bool DecodeCursor(const DWORD id, CursorSource& out) {
	const HCURSOR cursor = static_cast<HCURSOR>(LoadCursorW(nullptr, MAKEINTRESOURCEW(id)));
	ICONINFO info = {};
	if (cursor == nullptr || !GetIconInfo(cursor, &info)) {
		return false;
	}
	const bool fromColor = ReadColorPixels(info.hbmColor, out);
	const bool ok = fromColor || ReadMonoPixels(info.hbmMask, out);
	DeleteObject(info.hbmColor);
	DeleteObject(info.hbmMask);
	if (!ok) {
		return false;
	}
	Log(L"cursor: 光标槽 %lu 读到 %dx%d（%s），热点 (%lu,%lu)", id, out.width, out.height,
	    fromColor ? L"32bpp" : L"单色", info.xHotspot, info.yHotspot);
	out.xHotspot = info.xHotspot;
	out.yHotspot = info.yHotspot;
	out.darkInk = DetectDarkInk(out.pixels);
	out.ready = true;
	return true;
}

bool HighContrastOn() {
	HIGHCONTRASTW contrast = {};
	contrast.cbSize = sizeof(contrast);
	if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) == FALSE) {
		return false;
	}
	return (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

// 把系统光标还原成用户自己的方案。这里**绝不能**带 SPIF_UPDATEINIFILE：
// 那会把我们染过色的光标写进用户配置，注销之后还留着。
void ResetSystemCursors() {
	if (g_resetting) {
		return;
	}
	g_resetting = true;
	SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
	g_resetting = false;
	g_applied = Tint::None;
}

// 重新解码前必须先还原：LoadCursor 读到的就是槽里的东西，带着我们的颜色解码
// 会越染越深。
void RebuildSources() {
	// 顺带把上一次强杀留下的颜色冲掉：解码必须在没染色的状态下做，
	// 否则读回来的是我们自己染过的光标，会越染越深。
	Log(L"cursor: 重载用户的光标方案，再重新读一遍三个槽");
	ResetSystemCursors();
	for (size_t i = 0; i < std::size(kCursorIds); ++i) {
		if (!DecodeCursor(kCursorIds[i], g_sources[i])) {
			// 某个槽读不出来（比如用户配了 .ani 动画光标）就跳过它，别的照染。
			Log(L"cursor: 系统光标 %lu 读不出来，这个槽保持原样", kCursorIds[i]);
		}
	}
}

void ApplyTint(const bool chinese) {
	g_target = chinese ? Tint::Chinese : Tint::English;
	if (!TintOn() || g_applied == g_target || HighContrastOn()) {
		return;
	}
	const COLORREF color = chinese ? kChineseColor : kEnglishColor;
	bool applied = false;
	for (size_t i = 0; i < std::size(kCursorIds); ++i) {
		if (!g_sources[i].ready) {
			continue;
		}
		std::vector<DWORD> pixels = g_sources[i].pixels;
		TintPixels(pixels.data(), pixels.size(), color, g_sources[i].darkInk,
		           g_cursorTintPercent.load());
		const HCURSOR cursor = PackCursor(g_sources[i], pixels);
		if (cursor == nullptr) {
			continue;
		}
		if (SetSystemCursor(cursor, kCursorIds[i]) == FALSE) {
			DestroyCursor(cursor);  // 没送出去就还是我们的，得自己毁掉
			Log(L"cursor: 替换系统光标 %lu 失败（%lu）", kCursorIds[i], GetLastError());
			continue;
		}
		applied = true;
	}
	if (applied) {
		g_applied = g_target;
	}
}

// Windows 偶尔会自己把光标方案重载一遍（登录时、会话中途也见过），颜色会被冲掉。
// 直接无条件重刷，不去检测——重新造三个光标比检测便宜。
void ReapplyTint() {
	if (!TintOn() || g_target == Tint::None) {
		return;
	}
	g_applied = Tint::None;
	ApplyTint(g_target == Tint::Chinese);
}

// 前台窗口换了。这里只起个去抖定时器：刚切过去的窗口可能还没挂上 IME 上下文，
// 立刻去读会读到旧状态（和横幅里 kSwitchSettleMs 是同一个理由）。
void CALLBACK ForegroundEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
	if (g_mainWnd != nullptr) {
		SetTimer(g_mainWnd, kTimerCursorSettle, kSettleMs, nullptr);
	}
}

}  // 匿名命名空间

void InitializeCursorTint() {
	// 挂前台事件：切窗口要立刻跟着变，这是三条触发路径里最及时的一条。
	g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
	                                   ForegroundEventProc, 0, 0,
	                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
	if (!TintOn()) {
		return;
	}
	// 先自愈再解码：上一次要是被强杀，槽里留着的就是我们的颜色。
	RebuildSources();
	if (g_mainWnd != nullptr) {
		SetTimer(g_mainWnd, kTimerCursorPoll, kPollMs, nullptr);
	}
	ApplyTint(CurrentInputIsChinese());
}

void CursorSwitchSettle() {
	if (g_mainWnd == nullptr) {
		return;
	}
	SetTimer(g_mainWnd, kTimerCursorSettle, kSettleMs, nullptr);
}

void CursorSettleTick() {
	if (g_mainWnd != nullptr) {
		KillTimer(g_mainWnd, kTimerCursorSettle);
	}
	if (!TintOn()) {
		return;
	}
	g_lastProbeTick = GetTickCount64();
	ApplyTint(CurrentInputIsChinese());
}

void CursorPollTick() {
	if (!TintOn() || g_mainWnd == nullptr) {
		return;
	}
	if (++g_pollTicks % kReapplyTicks == 0) {
		ReapplyTint();
	}
	if (GetTickCount64() - g_lastProbeTick < kPollMs) {
		return;  // 刚刚探测过（CapsLock 或切窗口那条路），不重复问
	}
	g_lastProbeTick = GetTickCount64();
	ApplyTint(CurrentInputIsChinese(kProbeTimeoutMs));
}

void OnSystemCursorsChanged() {
	if (!TintOn()) {
		return;
	}
	RebuildSources();  // 用户换了方案/大小，旧像素作废；顺带把我们的颜色冲掉
	g_applied = Tint::None;
	ApplyTint(g_target == Tint::Chinese);
}

void DestroyCursorTint() {
	if (g_foregroundHook != nullptr) {
		UnhookWinEvent(g_foregroundHook);
		g_foregroundHook = nullptr;
	}
	if (g_mainWnd != nullptr) {
		KillTimer(g_mainWnd, kTimerCursorSettle);
		KillTimer(g_mainWnd, kTimerCursorPoll);
	}
	// 只有真染过才去动系统光标：没染过还去 SPI_SETCURSORS 会平白把用户的方案重载一遍。
	if (g_applied != Tint::None) {
		ResetSystemCursors();
	}
	for (CursorSource& source : g_sources) {
		source.ready = false;
		source.pixels.clear();
	}
	g_target = Tint::None;
	g_pollTicks = 0;
}

void SetCursorTintPercent(const int percent) {
	const int wanted = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	const bool wasOn = TintOn();
	g_cursorTintPercent = wanted;

	if (wanted == 0) {
		// 0% 就是不变：把系统光标还回去，轮询也停掉，和以前关掉那个开关一样。
		if (g_mainWnd != nullptr) {
			KillTimer(g_mainWnd, kTimerCursorSettle);
			KillTimer(g_mainWnd, kTimerCursorPoll);
		}
		ResetSystemCursors();
		ShowBalloon(L"鼠标指针不再跟着中英文变色");
		return;
	}

	if (!wasOn) {
		// 刚从"不变"里走出来：先自愈再解码。上一次要是被强杀，槽里留着的就是我们染过的
		// 光标，直接拿它当原图会越染越深。
		RebuildSources();
		if (g_mainWnd != nullptr) {
			SetTimer(g_mainWnd, kTimerCursorPoll, kPollMs, nullptr);
		}
		g_lastProbeTick = GetTickCount64();
		ShowBalloon(L"鼠标指针跟着中英文变色");
	}

	// 百分比变了，槽里贴着的旧颜色就不作数了，重刷一遍。
	g_applied = Tint::None;
	ApplyTint(CurrentInputIsChinese());
}
