#include "cursor.h"

#include "app.h"
#include "banner.h"
#include "logging.h"
#include "settings.h"
#include "surface.h"

#include <algorithm>
#include <climits>
#include <fstream>
#include <vector>

namespace {

// 染哪些光标槽：槽号 + 注册表 Control Panel\Cursors 下的值名。等待/后台等待是 .ani
// 动画光标，换成静止的一帧看着像卡死，不碰。winuser.h 里 OCR_* 被 OEMRESOURCE 包着、
// IDC_* 又是 MAKEINTRESOURCE 指针，所以槽号直接写数字（和 banner.cpp 里手写
// IMC_GETCONVERSIONMODE 是同一个做法）。
struct CursorSlot {
	DWORD id;
	const wchar_t* name;
};

constexpr CursorSlot kSlots[] = {
	{ 32512, L"Arrow" },      // OCR_NORMAL：箭头
	{ 32513, L"IBeam" },      // OCR_IBEAM：文本 I 型
	{ 32649, L"Hand" },       // OCR_HAND：链接手型
	{ 32515, L"Crosshair" },  // OCR_CROSS：十字
	{ 32644, L"SizeWE" },     // OCR_SIZEWE：横向拉伸
	{ 32645, L"SizeNS" },     // OCR_SIZENS：纵向拉伸
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
	std::vector<BYTE> original;  // 单帧 .cur 原始字节：还原保留调色板、掩码和热点
	std::vector<DWORD> pixels;  // premultiplied alpha，自顶向下，0xAARRGGBB
};

enum class Tint {
	None,
	Chinese,
	English,
};

CursorSource g_sources[std::size(kSlots)];
Tint g_applied = Tint::None;  // 系统光标槽现在被染成什么样
Tint g_target = Tint::None;   // 想要的颜色，重刷时直接用，不用再探测
HWINEVENTHOOK g_foregroundHook = nullptr;
ULONGLONG g_lastProbeTick = 0;
unsigned g_pollTicks = 0;
int g_sourceDpi = 0;

// 总开关关掉、或者浓度滑到 0，都等于"不变"：系统光标保持用户自己的样子。
bool TintOn() {
	return g_cursorTintEnabled.load() && g_cursorTintPercent.load() > 0;
}

int Luma(const DWORD px) {
	return static_cast<int>(((px >> 16 & 0xFF) * 299 + (px >> 8 & 0xFF) * 587 +
	                         (px & 0xFF) * 114) /
	                        1000);
}

// 本体是亮还是暗。光标的画法是"填充 + 描边"，描边在最外圈，所以贴着透明的那一圈
// 是什么色，本体就是反过来的那个。不能看整张图的平均亮度：I 型、十字这类细长形状的
// 白色填充面积小，平均下来会被黑描边压过去，方向就反了（结果只有描边在变色）。
bool DetectDarkInk(const std::vector<DWORD>& pixels, const int width, const int height) {
	const auto opaque = [&](const int x, const int y) {
		return x >= 0 && y >= 0 && x < width && y < height &&
		       (pixels[static_cast<size_t>(y) * width + x] >> 24) >= 200;
	};
	long long edgeLuma = 0;
	int edgeCount = 0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const DWORD px = pixels[static_cast<size_t>(y) * width + x];
			if ((px >> 24) < 200) {
				continue;
			}
			if (opaque(x - 1, y) && opaque(x + 1, y) && opaque(x, y - 1) && opaque(x, y + 1)) {
				continue;  // 四面被包住的像素属于填充，不是描边
			}
			edgeLuma += Luma(px);
			++edgeCount;
		}
	}
	// 描边亮 → 本体暗。没有描边可见时按亮体处理，最坏也只是不染，不会染错方向。
	return edgeCount > 0 && edgeLuma / edgeCount >= 128;
}

// 保形上色：本体朝目标色靠拢，另一头的描边保持原样。
//   亮体：out = lerp(黑, 目标色, L/255)      —— 白体变目标色，黑边还是黑
//   暗体：out = lerp(白, 目标色, (255-L)/255) —— 黑体变目标色，白边还是白
// percent 是上色程度：100 就是上面那个满色，0 原样不动，中间按比例在"原来的像素"
// 和满色之间插值——所以滑块越小，红/蓝越淡。
// 只动 RGB 不动 A。GDI 光标是预乘 alpha：暗体的白描边也必须以 A 为上限。
void TintPixels(DWORD* pixels, const size_t count, const COLORREF color, const bool darkInk,
                const int percent) {
	const int tint[3] = { GetRValue(color), GetGValue(color), GetBValue(color) };
	for (size_t i = 0; i < count; ++i) {
		const DWORD px = pixels[i];
		if ((px >> 24) == 0) {
			continue;
		}
		const int alpha = static_cast<int>(px >> 24);
		const int weight = darkInk ? alpha - Luma(px) : Luma(px);
		const int channel[3] = { static_cast<int>((px >> 16) & 0xFF),
			                     static_cast<int>((px >> 8) & 0xFF),
			                     static_cast<int>(px & 0xFF) };
		DWORD out = px & 0xFF000000;
		for (int c = 0; c < 3; ++c) {
			const int full = darkInk ? alpha - (255 - tint[c]) * weight / 255
			                         : tint[c] * weight / 255;
			out |= static_cast<DWORD>(channel[c] + (full - channel[c]) * percent / 100)
			       << (16 - c * 8);
		}
		pixels[i] = out;
	}
}

// 1bpp 位图的调色板要两项（0 = 黑，1 = 白），而 BITMAPINFO 只声明了 bmiColors[1] 一项，
// 直接写 bmiColors[1] 就写到栈上去了——Debug 的 /RTC 会当场报"变量 info 周围的栈被破坏"。
// 自己排一个够大的结构，再当 BITMAPINFO 用。
struct MonoBitmapInfo {
	BITMAPINFOHEADER header;
	RGBQUAD colors[2];
};

// .cur 的目录项是 16 字节；热点位于图像外，传给资源解码器时要补在图像前面。
struct CursorFileEntry {
	BYTE width, height, colors, reserved;
	WORD xHotspot, yHotspot;
	DWORD bytes, offset;
};
static_assert(sizeof(CursorFileEntry) == 16);

// 静态 HCURSOR 经 SetSystemCursor 会先归一化到 96 DPI，再放大回当前 DPI；即使输入
// 已是原生 72px，150% 下也会走 72 -> 48 -> 72。两帧完全相同的 ANI 走保留像素的路径，
// 看起来仍是静止光标；不能缩成一帧，否则 Windows 会再次当作静态光标重采样。
// pixels == nullptr 时包入原 .cur 字节，还原不会丢失单色光标的 AND/XOR 语义。
HCURSOR PackCursor(const CursorSource& source, const std::vector<DWORD>* pixels) {
	std::vector<BYTE> image;
	if (pixels == nullptr) {
		image = source.original;
	} else {
		if (source.width <= 0 || source.height <= 0 ||
		    pixels->size() != static_cast<size_t>(source.width) * source.height) {
			return nullptr;
		}
		const int stride = (source.width + 31) / 32 * 4;
		BITMAPINFOHEADER dib = {};
		dib.biSize = sizeof(dib);
		dib.biWidth = source.width;
		dib.biHeight = source.height * 2;
		dib.biPlanes = 1;
		dib.biBitCount = 32;
		const WORD directory[] = { 0, 2, 1 };
		CursorFileEntry entry = {};
		entry.width = static_cast<BYTE>(source.width >= 256 ? 0 : source.width);
		entry.height = static_cast<BYTE>(source.height >= 256 ? 0 : source.height);
		entry.xHotspot = static_cast<WORD>(source.xHotspot);
		entry.yHotspot = static_cast<WORD>(source.yHotspot);
		entry.offset = sizeof(directory) + sizeof(entry);
		entry.bytes = static_cast<DWORD>(sizeof(dib) + pixels->size() * sizeof(DWORD) +
		                                  static_cast<size_t>(stride) * source.height);
		image.resize(entry.offset + entry.bytes, 0);
		CopyMemory(image.data(), directory, sizeof(directory));
		CopyMemory(image.data() + sizeof(directory), &entry, sizeof(entry));
		CopyMemory(image.data() + entry.offset, &dib, sizeof(dib));
		BYTE* color = image.data() + entry.offset + sizeof(dib);
		BYTE* mask = color + pixels->size() * sizeof(DWORD);
		for (int y = 0; y < source.height; ++y) {
			const int row = source.height - 1 - y;  // .cur DIB 自底向上
			CopyMemory(color + static_cast<size_t>(row) * source.width * sizeof(DWORD),
			           pixels->data() + static_cast<size_t>(y) * source.width,
			           source.width * sizeof(DWORD));
			for (int x = 0; x < source.width; ++x) {
				if (((*pixels)[static_cast<size_t>(y) * source.width + x] >> 24) == 0) {
					mask[row * stride + x / 8] |= static_cast<BYTE>(0x80 >> (x % 8));
				}
			}
		}
	}
	if (image.empty()) {
		return nullptr;
	}
	const DWORD frameBytes = (static_cast<DWORD>(image.size()) + 1) & ~1u;  // RIFF 两字节对齐
	const DWORD header[] = {
		0x46464952, 0, 0x4E4F4341,  // RIFF / 总长度稍后填 / ACON
		0x68696E61, 36,             // anih
		36, 2, 2, 0, 0, 32, 1, 600, 1,  // 两个相同帧，每帧 10 秒，AF_ICON
		0x5453494C, 4 + 2 * (8 + frameBytes), 0x6D617266,  // LIST / fram
	};
	std::vector<BYTE> animation(sizeof(header) + 2 * (8 + frameBytes), 0);
	CopyMemory(animation.data(), header, sizeof(header));
	const DWORD riffBytes = static_cast<DWORD>(animation.size() - 8);
	CopyMemory(animation.data() + 4, &riffBytes, sizeof(riffBytes));
	for (int frame = 0; frame < 2; ++frame) {
		BYTE* chunk = animation.data() + sizeof(header) + frame * (8 + frameBytes);
		const DWORD icon[] = { 0x6E6F6369, static_cast<DWORD>(image.size()) };  // icon
		CopyMemory(chunk, icon, sizeof(icon));
		CopyMemory(chunk + sizeof(icon), image.data(), image.size());
	}
	return static_cast<HCURSOR>(CreateIconFromResourceEx(
	    animation.data(), static_cast<DWORD>(animation.size()), FALSE, 0x00030000,
	    source.width, source.height, 0));
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
	return lines == out.height;
}

// 单色光标的掩码始终是上下两半（AND/XOR），与系统默认光标尺寸无关。
// 彩色但无 alpha 的光标只有 AND 掩码；不能用 RGB 非零判断透明度，否则黑边会丢失。
bool ReadMonoPixels(const HBITMAP maskBitmap, CursorSource& out, const bool monochrome) {
	BITMAP bm = {};
	if (maskBitmap == nullptr || GetObjectW(maskBitmap, sizeof(bm), &bm) == 0 ||
	    bm.bmBitsPixel != 1 || bm.bmHeight < 2) {
		return false;
	}
	const bool doubled = monochrome;
	out.width = bm.bmWidth;
	out.height = doubled ? bm.bmHeight / 2 : bm.bmHeight;
	const int stride = (out.width + 31) / 32 * 4;
	std::vector<BYTE> bits(static_cast<size_t>(stride) * bm.bmHeight, 0);

	MonoBitmapInfo info = {};
	info.header.biSize = sizeof(BITMAPINFOHEADER);
	info.header.biWidth = out.width;
	info.header.biHeight = -bm.bmHeight;
	info.header.biPlanes = 1;
	info.header.biBitCount = 1;
	info.header.biCompression = BI_RGB;
	info.colors[1].rgbRed = info.colors[1].rgbGreen = info.colors[1].rgbBlue = 255;
	const HDC screen = GetDC(nullptr);
	const int lines = GetDIBits(screen, maskBitmap, 0, bm.bmHeight, bits.data(),
	                            reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS);
	ReleaseDC(nullptr, screen);
	if (lines == 0) {
		return false;
	}

	if (monochrome) {
		out.pixels.assign(static_cast<size_t>(out.width) * out.height, 0);
	} else if (out.pixels.size() != static_cast<size_t>(out.width) * out.height) {
		return false;
	}
	int opaque = 0;
	for (int y = 0; y < out.height; ++y) {
		for (int x = 0; x < out.width; ++x) {
			const int shift = 7 - x % 8;
			const bool andBit = (bits[y * stride + x / 8] >> shift & 1) != 0;
			const bool xorBit = doubled
			                        ? (bits[(y + out.height) * stride + x / 8] >> shift & 1) != 0
			                        : false;
			DWORD& px = out.pixels[static_cast<size_t>(y) * out.width + x];
			if (andBit && !xorBit) {
				px = 0;
				continue;  // 屏幕照旧 = 透明
			}
			px = monochrome ? (xorBit ? 0xFFFFFFFF : 0xFF000000) : px | 0xFF000000;
			++opaque;
		}
	}
	return opaque > 0;
}

// 不读 LoadCursor 的共享槽，也不让 LoadImage 挑帧：实测 EOA 多尺寸 .cur 请求 72px 时
// 后者仍会重采样别的帧。明确选择原生尺寸，交给资源解码器；没有匹配帧才缩小较大的帧。
// .ani 和没有可读文件的槽保持原样，不把动画拍扁，也不猜测/反染系统槽中的像素。
bool DecodeCursor(const wchar_t* path, const int size, CursorSource& out) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	const auto length = file.tellg();
	if (!file || length < 6 || length > 16 * 1024 * 1024 || size <= 0) {
		return false;
	}
	std::vector<BYTE> bytes(static_cast<size_t>(length));
	file.seekg(0);
	if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) {
		return false;
	}
	WORD header[3] = {};
	CopyMemory(header, bytes.data(), sizeof(header));
	const size_t directoryEnd = 6 + static_cast<size_t>(header[2]) * sizeof(CursorFileEntry);
	if (header[0] != 0 || header[1] != 2 || header[2] == 0 || directoryEnd > bytes.size()) {
		return false;
	}
	CursorFileEntry best = {};
	int bestScore = INT_MAX;
	for (size_t i = 0; i < header[2]; ++i) {
		CursorFileEntry entry = {};
		CopyMemory(&entry, bytes.data() + 6 + i * sizeof(entry), sizeof(entry));
		const int width = entry.width == 0 ? 256 : entry.width;
		const int height = entry.height == 0 ? 256 : entry.height;
		if (entry.offset < directoryEnd || entry.offset > bytes.size() || entry.bytes == 0 ||
		    entry.bytes > bytes.size() - entry.offset ||
		    entry.xHotspot >= width || entry.yHotspot >= height) {
			return false;
		}
		const int score = width >= size && height >= size ? (std::max)(width, height) - size
			                                               : 65536 - (std::min)(width, height);
		if (score < bestScore) {
			best = entry;
			bestScore = score;
		}
	}
	out.width = out.height = size;
	const WORD directory[] = { 0, 2, 1 };
	out.original.resize(sizeof(directory) + sizeof(best) + best.bytes);
	CopyMemory(out.original.data(), directory, sizeof(directory));
	CopyMemory(out.original.data() + sizeof(directory) + sizeof(best),
	           bytes.data() + best.offset, best.bytes);
	best.offset = sizeof(directory) + sizeof(best);
	CopyMemory(out.original.data() + sizeof(directory), &best, sizeof(best));
	const HCURSOR cursor = PackCursor(out, nullptr);
	ICONINFO info = {};
	if (cursor == nullptr || !GetIconInfo(cursor, &info)) {
		DestroyCursor(cursor);
		return false;
	}
	bool ok = info.hbmColor == nullptr ? ReadMonoPixels(info.hbmMask, out, true)
	                                  : ReadColorPixels(info.hbmColor, out);
	if (ok && info.hbmColor != nullptr &&
	    std::none_of(out.pixels.begin(), out.pixels.end(), [](DWORD px) { return (px >> 24) != 0; })) {
		ok = ReadMonoPixels(info.hbmMask, out, false);
	}
	DeleteObject(info.hbmColor);
	DeleteObject(info.hbmMask);
	DestroyCursor(cursor);
	if (!ok) {
		return false;
	}
	Log(L"cursor: 原生帧 %ux%u -> %dx%d，热点 (%lu,%lu)，%s",
	    best.width == 0 ? 256 : best.width, best.height == 0 ? 256 : best.height,
	    out.width, out.height, info.xHotspot, info.yHotspot, path);
	out.xHotspot = info.xHotspot;
	out.yHotspot = info.yHotspot;
	out.darkInk = DetectDarkInk(out.pixels, out.width, out.height);
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

// 原始 .cur 字节也走双帧提交，避免还原时再次被 SetSystemCursor 缩小再放大。
void RestoreSourceCursors() {
	for (size_t i = 0; i < std::size(kSlots); ++i) {
		if (!g_sources[i].ready) {
			continue;
		}
		const HCURSOR cursor = PackCursor(g_sources[i], nullptr);
		if (cursor != nullptr && SetSystemCursor(cursor, kSlots[i].id) == FALSE) {
			DestroyCursor(cursor);
		}
	}
	g_applied = Tint::None;
}

// 物理尺寸 = 用户的基础指针大小 × 鼠标所在显示器的 DPI，绝不是 SM_CXCURSOR。
// 每次从磁盘读原图，强杀残留、部分着色和跨屏重建都不会在旧颜色上继续加工。
void RebuildSources(const int dpi) {
	DWORD baseSize = 32;
	DWORD bytes = sizeof(baseSize);
	RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize",
	             RRF_RT_REG_DWORD, nullptr, &baseSize, &bytes);
	if (baseSize < 32 || baseSize > 256) {
		baseSize = 32;
	}
	const int size = MulDiv(static_cast<int>(baseSize), dpi, 96);
	Log(L"cursor: 基础大小 %lu，DPI %d，目标 %dpx", baseSize, dpi, size);
	for (size_t i = 0; i < std::size(kSlots); ++i) {
		g_sources[i] = CursorSource{};
		wchar_t path[32768] = {};
		bytes = sizeof(path);
		if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", kSlots[i].name,
		                 RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
		                 nullptr, path, &bytes) != ERROR_SUCCESS ||
		    !DecodeCursor(path, size, g_sources[i])) {
			Log(L"cursor: 槽 %lu 无可用静态方案文件，保持原样", kSlots[i].id);
		}
	}
	g_sourceDpi = dpi;
	g_applied = Tint::None;
}

void ApplyTint(const bool chinese) {
	g_target = chinese ? Tint::Chinese : Tint::English;
	if (!TintOn()) {
		return;
	}
	if (HighContrastOn()) {
		if (g_applied != Tint::None) {
			RestoreSourceCursors();
		}
		return;
	}
	POINT point = {};
	GetCursorPos(&point);
	const int dpi = ScreenDpi(WindowFromPoint(point), nullptr);
	if (g_sourceDpi != dpi) {
		RebuildSources(dpi);
	}
	if (g_applied == g_target) {
		return;
	}
	const COLORREF color = chinese ? kChineseColor : kEnglishColor;
	bool applied = false;
	for (size_t i = 0; i < std::size(kSlots); ++i) {
		if (!g_sources[i].ready) {
			continue;
		}
		std::vector<DWORD> pixels = g_sources[i].pixels;
		TintPixels(pixels.data(), pixels.size(), color, g_sources[i].darkInk,
		           g_cursorTintPercent.load());
		const HCURSOR cursor = PackCursor(g_sources[i], &pixels);
		if (cursor == nullptr) {
			continue;
		}
		if (SetSystemCursor(cursor, kSlots[i].id) == FALSE) {
			DestroyCursor(cursor);  // 没送出去就还是我们的，得自己毁掉
			Log(L"cursor: 替换系统光标 %lu 失败（%lu）", kSlots[i].id, GetLastError());
			continue;
		}
		applied = true;
	}
	if (applied) {
		g_applied = g_target;
	}
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
		g_applied = Tint::None;  // Windows 可能自行重载方案，定期补上颜色
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
	g_sourceDpi = 0;  // 用户换方案/大小后直接读新文件，不拿旧像素覆盖新方案
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
	// 只有真染过才去动系统光标：没染过就别白写一遍。
	if (g_applied != Tint::None) {
		RestoreSourceCursors();
	}
	for (CursorSource& source : g_sources) {
		source = CursorSource{};
	}
	g_target = Tint::None;
	g_pollTicks = 0;
	g_sourceDpi = 0;
}

// 总开关或浓度改动之后，让系统光标跟上新状态。wasOn 是改动前的状态：
// 从关到开要重新读方案文件并起轮询，从开到关要把原图装回去。
void SyncCursorTint(const bool wasOn) {
	if (!TintOn()) {
		if (g_mainWnd != nullptr) {
			KillTimer(g_mainWnd, kTimerCursorSettle);
			KillTimer(g_mainWnd, kTimerCursorPoll);
		}
		if (wasOn) {
			RestoreSourceCursors();
		}
		return;
	}
	if (!wasOn) {
		g_sourceDpi = 0;  // 关掉的这段时间可能换过方案，重新打开时重新读文件
		if (g_mainWnd != nullptr) {
			SetTimer(g_mainWnd, kTimerCursorPoll, kPollMs, nullptr);
		}
		g_lastProbeTick = GetTickCount64();
	}
	// 开关或浓度变了，槽里贴着的旧颜色就不作数了，重刷一遍。
	g_applied = Tint::None;
	ApplyTint(CurrentInputIsChinese());
}

void SetCursorTintPercent(const int percent) {
	const int wanted = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	if (wanted == g_cursorTintPercent.load()) {
		return;  // 拖动时同一档会被反复上报：值没变就别重刷、也别再写一次 ini
	}
	const bool wasOn = TintOn();
	g_cursorTintPercent = wanted;
	SyncCursorTint(wasOn);
	SaveSettings();
}

void SetCursorTintEnabled(const bool enabled) {
	const bool wasOn = TintOn();
	g_cursorTintEnabled = enabled;
	SyncCursorTint(wasOn);
	SaveSettings();
}
