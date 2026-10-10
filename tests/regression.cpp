// 独立的回归检查：键盘 API 全部用替身，所以运行这个可执行文件期间
// 不会注入任何按键，也不会安装全局键盘钩子。
#define NOMINMAX
#include <Windows.h>
#include <rtcapi.h>  // /RTC1 的报错回调：别弹对话框，直接报出来退出
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <climits>
#include <string>

// /RTC1 发现问题（比如栈被写坏）时默认弹一个模态对话框，脚本跑起来会一直卡在那儿。
// 换成打到 stderr 然后退出，回归检查就能干净地失败。
static int __cdecl OnRuntimeCheckFailure(int, const char* fileName, int line, const char* module,
                                         const char* format, ...) {
    std::fprintf(stderr, "RUNTIME CHECK FAILED: ");
    if (format != nullptr) { std::fputs(format, stderr); }
    std::fprintf(stderr, " (%s:%d, %s)\n", fileName != nullptr ? fileName : "?", line,
                 module != nullptr ? module : "?");
    std::fflush(stderr);
    std::exit(1);
}

static bool ctrlHeld = false;
static bool spaceHeld = false;
static bool postSucceeds = true;
struct PostedMessage {
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
};
static std::vector<PostedMessage> posted;
static UINT firstSendCount = UINT_MAX;
static std::vector<std::vector<INPUT>> sentBatches;
static std::atomic<DWORD> hookOwner{ 0 };
static std::atomic<unsigned> hooksRemoved{ 0 };
static HANDLE hookInstalledEvent = nullptr;
static HHOOK WINAPI FakeInstall(int, HOOKPROC, HINSTANCE, DWORD) {
    hookOwner = GetCurrentThreadId();
    SetEvent(hookInstalledEvent);
    return reinterpret_cast<HHOOK>(1);
}
static BOOL WINAPI FakeUnhook(HHOOK) { ++hooksRemoved; return TRUE; }
static SHORT WINAPI FakeKeyState(int key) {
    return ((key == VK_CONTROL && ctrlHeld) || (key == VK_SPACE && spaceHeld))
        ? static_cast<SHORT>(0x8000) : 0;
}
static HWND WINAPI FakeForeground() { return reinterpret_cast<HWND>(1); }
static BOOL WINAPI FakePost(HWND, UINT message, WPARAM wParam, LPARAM lParam) {
    posted.push_back({ message, wParam, lParam });
    return postSucceeds;
}
static LRESULT WINAPI FakeNext(HHOOK, int, WPARAM, LPARAM) { return 0; }
static UINT WINAPI FakeSend(UINT count, LPINPUT inputs, int) {
    sentBatches.emplace_back(inputs, inputs + count);
    return sentBatches.size() == 1 && firstSendCount != UINT_MAX ? firstSendCount : count;
}

// 鼠标指针那几个会真动系统状态的 API 也换成替身。声明放在这里、定义放在
// 各模块之后：替身要用 cursor.cpp 里的辅助函数去读回光标像素。
static BOOL WINAPI FakeSetSystemCursor(HCURSOR cursor, DWORD id);
static BOOL WINAPI FakeSystemParametersInfo(UINT action, UINT param, PVOID data, UINT winIni);
static HWINEVENTHOOK WINAPI FakeSetWinEventHook(DWORD, DWORD, HMODULE, WINEVENTPROC, DWORD, DWORD,
                                                DWORD);
static BOOL WINAPI FakeUnhookWinEvent(HWINEVENTHOOK hook);

// 键盘 API 换成替身，且必须在包含各模块之前定义。
#define GetAsyncKeyState FakeKeyState
#define GetForegroundWindow FakeForeground
#define PostMessageW FakePost
#define CallNextHookEx FakeNext
#define SendInput FakeSend
#define SetWindowsHookExW FakeInstall
#define UnhookWindowsHookEx FakeUnhook
#define SetSystemCursor FakeSetSystemCursor
#define SystemParametersInfoW FakeSystemParametersInfo
#define SetWinEventHook FakeSetWinEventHook
#define UnhookWinEvent FakeUnhookWinEvent

static std::wstring cursorFixturePath;
static DWORD cursorBaseSize = 48;
static int cursorDpi = 144;
static bool highContrast = false;
static LSTATUS WINAPI FakeCursorRegGetValue(HKEY, LPCWSTR, LPCWSTR name, DWORD, LPDWORD,
                                             PVOID data, LPDWORD bytes) {
    if (wcscmp(name, L"CursorBaseSize") == 0) {
        if (*bytes < sizeof(cursorBaseSize)) { return ERROR_MORE_DATA; }
        CopyMemory(data, &cursorBaseSize, sizeof(cursorBaseSize));
        *bytes = sizeof(cursorBaseSize);
    } else {
        const DWORD needed = static_cast<DWORD>((cursorFixturePath.size() + 1) * sizeof(wchar_t));
        if (*bytes < needed) { return ERROR_MORE_DATA; }
        CopyMemory(data, cursorFixturePath.c_str(), needed);
        *bytes = needed;
    }
    return ERROR_SUCCESS;
}
static int FakeCursorDpi(HWND, HWND) { return cursorDpi; }

// 各模块的 .cpp 都编进这一个翻译单元，替身才对所有模块都生效。
#include "../capslock-switcher/main.cpp"
#include "../capslock-switcher/banner.cpp"
#define RegGetValueW FakeCursorRegGetValue
#define ScreenDpi FakeCursorDpi
#include "../capslock-switcher/cursor.cpp"
#undef RegGetValueW
#undef ScreenDpi
#include "../capslock-switcher/keyboard.cpp"
#include "../capslock-switcher/logging.cpp"
#include "../capslock-switcher/config.cpp"
#include "../capslock-switcher/settings.cpp"
#include "../capslock-switcher/splash.cpp"
#include "../capslock-switcher/startup.cpp"
#include "../capslock-switcher/surface.cpp"
#include "../capslock-switcher/tray.cpp"

#undef GetAsyncKeyState
#undef GetForegroundWindow
#undef PostMessageW
#undef CallNextHookEx
#undef SendInput
#undef SetWindowsHookExW
#undef UnhookWindowsHookEx
#undef SetSystemCursor
#undef SystemParametersInfoW
#undef SetWinEventHook
#undef UnhookWinEvent

// —— 鼠标指针的替身 ——
// 真 SetSystemCursor 会把传进来的句柄收走（自己销毁），替身也照做，
// 顺带把光标像素读下来，好在测试里断言"喂进去的确实是目标颜色"。
static unsigned setCursorCalls = 0;
static DWORD lastCursorId = 0;
static std::vector<DWORD> lastCursorPixels;
static int lastCursorWidth = 0;
static int lastCursorHeight = 0;
static DWORD lastCursorX = 0;
static DWORD lastCursorY = 0;
static unsigned winEventHooks = 0;
static unsigned winEventUnhooks = 0;

static BOOL WINAPI FakeSetSystemCursor(HCURSOR cursor, DWORD id) {
    ++setCursorCalls;
    lastCursorId = id;
    lastCursorPixels.clear();
    ICONINFO info = {};
    if (cursor != nullptr && GetIconInfo(cursor, &info)) {
        CursorSource source;
        if (ReadColorPixels(info.hbmColor, source)) {
            lastCursorPixels = source.pixels;
            lastCursorWidth = source.width;
            lastCursorHeight = source.height;
            lastCursorX = info.xHotspot;
            lastCursorY = info.yHotspot;
        }
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
    }
    if (cursor != nullptr) {
        DestroyCursor(cursor);
    }
    return TRUE;
}

// 别的 SystemParametersInfo 调用（surface.cpp 要用的 SPI_GETWORKAREA、cursor.cpp 要用的
// SPI_GETHIGHCONTRAST）转给真函数。
static BOOL WINAPI FakeSystemParametersInfo(UINT action, UINT param, PVOID data, UINT winIni) {
    if (action == SPI_SETCURSORS) {
        std::fputs("FAIL: tests must not reload real system cursors\n", stderr);
        std::exit(1);
    }
    if (action == SPI_GETHIGHCONTRAST) {
        static_cast<HIGHCONTRASTW*>(data)->dwFlags = highContrast ? HCF_HIGHCONTRASTON : 0;
        return TRUE;
    }
    using RealFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT);
    static const auto real = reinterpret_cast<RealFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoW"));
    return real != nullptr ? real(action, param, data, winIni) : FALSE;
}

static HWINEVENTHOOK WINAPI FakeSetWinEventHook(DWORD, DWORD, HMODULE, WINEVENTPROC, DWORD, DWORD,
                                                DWORD) {
    ++winEventHooks;
    return reinterpret_cast<HWINEVENTHOOK>(1);
}

static BOOL WINAPI FakeUnhookWinEvent(HWINEVENTHOOK) {
    ++winEventUnhooks;
    return TRUE;
}

static void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

static LRESULT Key(WPARAM message, DWORD flags = 0) {
    KBDLLHOOKSTRUCT key = {};
    key.vkCode = VK_CAPITAL;
    key.flags = flags;
    return LowLevelKeyboardProc(HC_ACTION, message, reinterpret_cast<LPARAM>(&key));
}

static void ResetKeyboard() {
    g_capsDown = false;
    g_capsSwallowed = false;
    g_capsLockOn = false;
    g_capsPassedAsAlt = false;
    g_enabled = true;
    g_altPassThrough = true;
    posted.clear();
    postSucceeds = true;
    ctrlHeld = spaceHeld = false;
    firstSendCount = UINT_MAX;
    sentBatches.clear();
}

static void TestKeyboard() {
    ResetKeyboard();
    Check(Key(WM_KEYDOWN) == 1, "mapped CapsLock down swallowed");
    for (int i = 0; i < 40; ++i) {
        Check(Key(WM_KEYDOWN) == 1, "auto-repeat swallowed");
    }
    Check(posted.size() == 1 && posted[0].message == WM_SWITCH_IME,
          "long press switches exactly once");
    Check(Key(WM_KEYUP) == 1, "mapped CapsLock up swallowed");
    Key(WM_KEYDOWN);
    Key(WM_KEYUP);
    Check(posted.size() == 2 && posted[1].message == WM_SWITCH_IME, "second press switches again");

    ResetKeyboard();
    Check(Key(WM_SYSKEYDOWN, LLKHF_ALTDOWN) == 0 && posted.empty(),
          "Alt CapsLock passed through");
    Check(Key(WM_KEYDOWN) == 0 && posted.empty(), "Alt released during hold does not start mapping");
    Check(Key(WM_KEYUP) == 0 && posted.size() == 1 &&
          posted[0].message == WM_CAPS_LOCK_PASSED && posted[0].wParam == 1 && posted[0].lParam == 1,
          "Alt CapsLock reports the flipped caps state once, on release");

    // Alt+CapsLock 的放行可以关掉：关掉之后 Alt 不再特殊，和 CapsLock 一样切换输入法。
    ResetKeyboard();
    g_altPassThrough = false;
    Check(Key(WM_SYSKEYDOWN, LLKHF_ALTDOWN) == 1, "pass-through off: Alt CapsLock is swallowed");
    Check(posted.size() == 1 && posted[0].message == WM_SWITCH_IME,
          "pass-through off: Alt CapsLock switches like plain CapsLock");
    Check(Key(WM_SYSKEYUP, LLKHF_ALTDOWN) == 1 && posted.size() == 1,
          "pass-through off: the swallowed pair produces no caps banner");
    g_altPassThrough = true;

    ResetKeyboard();
    Key(WM_KEYDOWN);
    g_enabled = false;
    Check(Key(WM_SYSKEYUP, LLKHF_ALTDOWN) == 1, "disable/Alt during hold preserves swallowed up");
    Check(Key(WM_KEYDOWN) == 0 && posted.size() == 1,
          "disabled down passes through without a banner of its own");
    g_enabled = true;
    Check(Key(WM_KEYUP) == 0 && posted.size() == 2 &&
          posted[1].message == WM_CAPS_LOCK_PASSED && posted[1].wParam == 0 && posted[1].lParam == 1,
          "enable during hold preserves passed-through up and reports the caps state");

    ResetKeyboard();
    Check(Key(WM_KEYDOWN, LLKHF_INJECTED) == 0, "injected CapsLock passes through");
    Check(Key(WM_KEYUP, LLKHF_INJECTED) == 0 && posted.empty() && g_capsLockOn,
          "injected CapsLock flips the tracked caps state but posts nothing");
    postSucceeds = false;
    Check(Key(WM_KEYDOWN) == 0 && Key(WM_KEYUP) == 0, "queue failure preserves original key pair");

    ResetKeyboard();
    Check(SendCtrlSpace(), "normal injection succeeds");
    Check(sentBatches[0].size() == 4, "normal chord has four events");
    ResetKeyboard();
    ctrlHeld = true;
    Check(SendCtrlSpace(), "held Ctrl injection succeeds");
    Check(sentBatches[0].size() == 2 && sentBatches[0][0].ki.wVk == VK_SPACE &&
          sentBatches[0][1].ki.wVk == VK_SPACE, "held Ctrl is never released");
    ResetKeyboard();
    spaceHeld = true;
    Check(!SendCtrlSpace() && sentBatches.empty(), "held Space is left untouched");
    for (UINT inserted = 0; inserted < 4; ++inserted) {
        ResetKeyboard();
        firstSendCount = inserted;
        Check(!SendCtrlSpace(), "partial injection reports failure");
        Check(sentBatches.size() == (inserted == 0 ? 1u : 2u), "only inserted keys need cleanup");
        if (inserted > 0) {
            for (const INPUT& release : sentBatches[1]) {
                Check(release.ki.dwFlags == KEYEVENTF_KEYUP, "cleanup consists of key-up events");
            }
            Check(sentBatches[1].back().ki.wVk == VK_CONTROL, "partial chord releases synthetic Ctrl");
        }
    }
    std::puts("PASS: key pairing, repeat, modifiers, queue failure, partial injection");
}

static void TestRendering() {
    Check(!PrepareSplash(), "splash refuses to render before GDI+ is up");
    Check(StartGdiplus(), "GDI+ startup");
    g_hInst = GetModuleHandleW(nullptr);
    for (int pass = 0; pass < 20; ++pass) {
        Check(PrepareSplash(), "embedded PNG decodes and draws with source stream alive");
        Check(g_splashWidth > 0 && g_splashHeight > 0, "splash dimensions valid");
        ReleaseSplash();
    }
    // 提示横幅：输入法红/蓝，大写锁定紫；圆角必须真的透明，颜色只预乘一次。
    struct BannerCase {
        BannerKind kind;
        bool state;
        const wchar_t* text;
        COLORREF fill;
    };
    const BannerCase cases[] = {
        { BannerKind::InputMethod, true, L"中文", kChineseColor },
        { BannerKind::InputMethod, false, L"English", kEnglishColor },
        { BannerKind::CapsLock, true, L"大写", kCapsLockColor },
        { BannerKind::CapsLock, false, L"小写", kCapsLockLowerColor },
    };
    const int bannerWidth = 200;
    const int bannerHeight = 96;
    for (const BannerCase& item : cases) {
        g_bannerKind = item.kind;
        g_bannerState = item.state;
        g_bannerText = item.text;
        Check(RenderBanner(bannerWidth, bannerHeight, 96), "banner renders");
        const auto* pixels = static_cast<const DWORD*>(g_bannerBits);
        Check(pixels[0] == 0, "banner corner is transparent");
        Check((pixels[10 * bannerWidth + bannerWidth / 2] >> 24) == 255, "banner interior opaque");
        bool hasEdge = false;
        for (int i = 0; i < bannerWidth * bannerHeight; ++i) {
            const DWORD alpha = pixels[i] >> 24;
            if (alpha > 0 && alpha < 255) {
                hasEdge = true;
                Check(((pixels[i] >> 16) & 255) == GetRValue(item.fill) * alpha / 255 &&
                      ((pixels[i] >> 8) & 255) == GetGValue(item.fill) * alpha / 255 &&
                      (pixels[i] & 255) == GetBValue(item.fill) * alpha / 255,
                      "banner edge colour premultiplied exactly once");
            }
        }
        Check(hasEdge, "banner has an anti-aliased edge");
        ReleaseBannerSurface();
    }

    // 回归：切换输入法的横幅不能被上一次的大写锁定横幅带偏。曾经因为 ShowSwitchBanner
    // 没把类型改回来，切中英文一直显示"大写/小写"。这里给个假窗口句柄，只为让入口函数
    // 不提前返回；SetTimer 和 UpdateLayeredWindow 都会失败，但都不影响要检查的状态。
    g_bannerWnd = reinterpret_cast<HWND>(1);
    g_bannerKind = BannerKind::CapsLock;
    g_bannerState = true;
    g_bannerText = L"大写";
    ShowSwitchBanner();
    Check(g_bannerKind == BannerKind::InputMethod, "switch banner resets the banner kind");
    ShowCapsLockBanner(true);
    Check(g_bannerKind == BannerKind::CapsLock && wcscmp(g_bannerText, L"大写") == 0 &&
          g_bannerState, "caps banner shows 大写 when the caps lock is on");
    ShowCapsLockBanner(false);
    Check(wcscmp(g_bannerText, L"小写") == 0 && !g_bannerState,
          "caps banner shows 小写 when the caps lock is off");
    ShowSwitchBanner();
    Check(g_bannerKind == BannerKind::InputMethod, "switch banner wins again after a caps banner");
    g_bannerWnd = nullptr;
    StopGdiplus();
    std::puts("PASS: 20 splash decode and release cycles, banner colours and alpha");
}

static void TestHookThread() {
    ResetKeyboard();
    hookInstalledEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(hookInstalledEvent != nullptr, "hook test event created");
    for (int pass = 0; pass < 3; ++pass) {
        Check(InstallHook(), "hook thread starts");
        Check(hookOwner.load() != GetCurrentThreadId(), "hook belongs to a dedicated thread");
        Check(WaitForSingleObject(hookInstalledEvent, 1000) == WAIT_OBJECT_0, "initial install observed");
        Check(PostThreadMessageW(g_hookThreadId, WM_REINSTALL_HOOK, 0, 0) != FALSE, "renewal queued");
        // 这里不跑 UI 消息循环：重挂必须能独立进行。
        Check(WaitForSingleObject(hookInstalledEvent, 1000) == WAIT_OBJECT_0, "renewal works with UI thread blocked");
        UninstallHook();
        Check(g_hKeyboardHook.load() == nullptr && g_hookThread == nullptr &&
              g_hookStopEvent == nullptr && g_hookReadyEvent == nullptr, "thread shutdown releases resources");
    }
    Check(hooksRemoved == 6, "replacement and shutdown each release their hook");
    CloseHandle(hookInstalledEvent);
    std::puts("PASS: hook thread isolation, renewal and repeated shutdown (mock hooks)");
}

// COLORREF 是 0x00BBGGRR，光标像素是 0xAARRGGBB，别弄混。
static DWORD Argb(const COLORREF color) {
    return 0xFF000000 | (static_cast<DWORD>(GetRValue(color)) << 16) |
           (static_cast<DWORD>(GetGValue(color)) << 8) | GetBValue(color);
}

// 这批像素看起来是不是染成了目标色：不透明像素里过半正好是那个色。
static bool LooksTinted(const std::vector<DWORD>& pixels, const COLORREF color) {
    if (pixels.empty()) {
        return false;
    }
    const DWORD wanted = Argb(color);
    int opaque = 0;
    int hits = 0;
    for (const DWORD px : pixels) {
        if ((px >> 24) < 200) {
            continue;
        }
        ++opaque;
        if (px == wanted) {
            ++hits;
        }
    }
    return opaque > 0 && hits * 2 > opaque;
}

// 鼠标指针：先是颜色公式这个纯函数，再把整条路（解码系统光标 → 上色 → 还原）
// 用替身走一遍，确认喂给 SetSystemCursor 的确实是目标颜色。
static void TestCursor() {
    DWORD light[4] = { 0xFFFFFFFF, 0xFF000000, 0x00FFFFFF, 0xFF808080 };
    TintPixels(light, 4, kChineseColor, false, 100);
    Check(light[0] == Argb(kChineseColor), "a light body takes the tint colour");
    Check(light[1] == 0xFF000000, "the black outline of a light body stays black");
    Check((light[2] >> 24) == 0, "transparent pixels stay transparent");
    Check((light[3] >> 24) == 0xFF && ((light[3] >> 16) & 0xFF) > (light[3] & 0xFF),
          "half-lit pixels blend towards the tint");

    DWORD dark[2] = { 0xFF000000, 0xFFFFFFFF };
    TintPixels(dark, 2, kChineseColor, true, 100);
    Check(dark[0] == Argb(kChineseColor), "a dark body takes the tint colour");
    Check(dark[1] == 0xFFFFFFFF, "the white outline of a dark body stays white");

    // 滑块：0% 原样不动，100% 是上面那个满色，中间按比例在两个结果之间插值。
    DWORD untouched[3] = { 0xFFFFFFFF, 0xFF000000, 0x00FFFFFF };
    TintPixels(untouched, 3, kChineseColor, false, 0);
    Check(untouched[0] == 0xFFFFFFFF && untouched[1] == 0xFF000000 &&
              (untouched[2] >> 24) == 0,
          "at 0% the pixels come out exactly as they went in");

    DWORD half[1] = { 0xFFFFFFFF };  // 白体：满色时整块变成目标色
    TintPixels(half, 1, kChineseColor, false, 50);
    const DWORD full = Argb(kChineseColor);
    const int halfR = static_cast<int>((half[0] >> 16) & 0xFF);
    const int halfG = static_cast<int>((half[0] >> 8) & 0xFF);
    const int halfB = static_cast<int>(half[0] & 0xFF);
    Check(halfR == 0xFF && halfG > static_cast<int>((full >> 8) & 0xFF) && halfG < 0xFF &&
              halfB > static_cast<int>(full & 0xFF) && halfB < 0xFF,
          "at 50% the pixels land halfway between the original and the full tint");

    // 热点必须原样带过去：I 型的热点不在角上，丢了点击落点就不准。
    CursorSource sample;
    sample.width = 4;
    sample.height = 4;
    sample.xHotspot = 2;
    sample.yHotspot = 3;
    sample.pixels.assign(16, 0xFF000000);
    const HCURSOR packed = PackCursor(sample, &sample.pixels);
    ICONINFO packedInfo = {};
    Check(packed != nullptr && GetIconInfo(packed, &packedInfo) != FALSE,
          "a packed cursor can be read back");
    Check(packedInfo.xHotspot == 2 && packedInfo.yHotspot == 3, "the hotspot survives packing");
    DeleteObject(packedInfo.hbmColor);
    DeleteObject(packedInfo.hbmMask);
    if (packed != nullptr) {
        DestroyCursor(packed);
    }

    // 预乘 alpha 的半透明黑体/白边：着色不能使 RGB 超过 alpha 或移动边缘。
    DWORD edges[3] = { 0x80000000, 0x80808080, 0x40102030 };
    TintPixels(edges, 3, kChineseColor, true, 100);
    Check(edges[0] == 0x80801023 && edges[1] == 0x80808080,
          "dark tint preserves premultiplied antialiased edges");
    for (const DWORD px : edges) {
        const DWORD alpha = px >> 24;
        Check((px & 255) <= alpha && ((px >> 8) & 255) <= alpha && ((px >> 16) & 255) <= alpha,
              "tint channels never exceed alpha");
    }

    // 非系统默认大小的单色光标：高度只由 AND/XOR 两半决定；黑色不能被当成透明。
    MonoBitmapInfo monoInfo = {};
    monoInfo.header.biSize = sizeof(BITMAPINFOHEADER);
    monoInfo.header.biWidth = 9;
    monoInfo.header.biHeight = -14;
    monoInfo.header.biPlanes = 1;
    monoInfo.header.biBitCount = 1;
    monoInfo.colors[1].rgbRed = monoInfo.colors[1].rgbGreen = monoInfo.colors[1].rgbBlue = 255;
    void* monoBits = nullptr;
    HBITMAP monoMask = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&monoInfo),
                                        DIB_RGB_COLORS, &monoBits, nullptr, 0);
    Check(monoMask != nullptr && monoBits != nullptr, "create monochrome mask fixture");
    ZeroMemory(monoBits, 4 * 14);
    FillMemory(monoBits, 4 * 7, 255);
    static_cast<BYTE*>(monoBits)[0] = 0x7F;
    static_cast<BYTE*>(monoBits)[4 * 7] = 0x40;
    CursorSource mono;
    Check(ReadMonoPixels(monoMask, mono, true) && mono.width == 9 && mono.height == 7 &&
              mono.pixels[0] == 0xFF000000 && mono.pixels[1] == 0xFFFFFFFF && mono.pixels[2] == 0,
          "monochrome mask uses its own dimensions and preserves black/inverted/transparent pixels");
    DeleteObject(monoMask);
    monoInfo.header.biHeight = -7;
    monoMask = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&monoInfo),
                               DIB_RGB_COLORS, &monoBits, nullptr, 0);
    Check(monoMask != nullptr && monoBits != nullptr, "create colour AND mask fixture");
    FillMemory(monoBits, 4 * 7, 255);
    static_cast<BYTE*>(monoBits)[0] = 0x5F;
    mono.pixels.assign(9 * 7, 0);
    mono.pixels[2] = 0x007F2020;
    Check(ReadMonoPixels(monoMask, mono, false) && mono.pixels[0] == 0xFF000000 &&
              mono.pixels[1] == 0 && mono.pixels[2] == 0xFF7F2020,
          "a colour cursor without alpha keeps opaque black according to its AND mask");
    DeleteObject(monoMask);

    // 本体方向的判定：贴透明的那一圈是描边。细长形状（I 型、十字）的白芯面积小，
    // 用整图平均亮度会被黑描边带偏，按边界判才稳。
    const auto makeRing = [](const DWORD outline, const DWORD fill) {
        std::vector<DWORD> pixels(81, 0);
        for (int y = 1; y < 8; ++y) {
            for (int x = 1; x < 8; ++x) {
                pixels[y * 9 + x] = (x >= 3 && x <= 5 && y >= 3 && y <= 5) ? fill : outline;
            }
        }
        return pixels;
    };
    std::vector<DWORD> lightBody = makeRing(0xFF000000, 0xFFFFFFFF);
    Check(!DetectDarkInk(lightBody, 9, 9), "a white fill inside a black outline is a light body");
    TintPixels(lightBody.data(), lightBody.size(), kChineseColor, false, 100);
    Check(lightBody[4 * 9 + 4] == Argb(kChineseColor) && lightBody[2 * 9 + 2] == 0xFF000000,
          "tinting a light body colours the fill and keeps the outline");
    Check(DetectDarkInk(makeRing(0xFFFFFFFF, 0xFF000000), 9, 9),
          "a black fill inside a white outline is a dark body");
    Check(!DetectDarkInk(std::vector<DWORD>(16, 0xFF000000), 4, 4),
          "a shape with no visible outline is treated as a light body, so tinting cannot invert it");

    // 自造多尺寸 .cur：每一帧都有不同的像素/热点，不能靠把小帧放大蒙混过关。
    wchar_t tempDirectory[MAX_PATH] = {}, tempFile[MAX_PATH] = {};
    Check(GetTempPathW(MAX_PATH, tempDirectory) != 0 &&
              GetTempFileNameW(tempDirectory, L"cur", 0, tempFile) != 0, "create cursor fixture");
    cursorFixturePath = tempFile;
    const int sizes[] = { 48, 72, 96, 144, 256 };
    const WORD header[] = { 0, 2, static_cast<WORD>(std::size(sizes)) };
    std::vector<BYTE> fixture(sizeof(header) + std::size(sizes) * sizeof(CursorFileEntry));
    CopyMemory(fixture.data(), header, sizeof(header));
    std::vector<std::vector<DWORD>> nativePixels;
    for (size_t f = 0; f < std::size(sizes); ++f) {
        const int side = sizes[f];
        const int maskStride = (side + 31) / 32 * 4;
        BITMAPINFOHEADER dib = {};
        dib.biSize = sizeof(dib);
        dib.biWidth = side;
        dib.biHeight = side * 2;
        dib.biPlanes = 1;
        dib.biBitCount = 32;
        CursorFileEntry entry = {};
        entry.width = entry.height = static_cast<BYTE>(side == 256 ? 0 : side);
        entry.xHotspot = static_cast<WORD>(7 + f * 3);
        entry.yHotspot = static_cast<WORD>(9 + f * 5);
        entry.offset = static_cast<DWORD>(fixture.size());
        entry.bytes = sizeof(dib) + side * side * 4 + maskStride * side;
        CopyMemory(fixture.data() + sizeof(header) + f * sizeof(entry), &entry, sizeof(entry));
        fixture.resize(fixture.size() + entry.bytes, 0);
        CopyMemory(fixture.data() + entry.offset, &dib, sizeof(dib));
        nativePixels.emplace_back(side * side);
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const DWORD alpha = (x * 37 + y * 29 + 255) % 256;
                const DWORD px = (alpha << 24) | ((alpha * static_cast<DWORD>(f + 1) / 5) << 16) |
                                 ((alpha * x / side) << 8) | alpha * y / side;
                nativePixels.back()[y * side + x] = px;
                CopyMemory(fixture.data() + entry.offset + sizeof(dib) +
                               ((side - 1 - y) * side + x) * 4, &px, sizeof(px));
            }
        }
    }
    {
        std::ofstream file(tempFile, std::ios::binary | std::ios::trunc);
        Check(static_cast<bool>(file.write(reinterpret_cast<const char*>(fixture.data()),
                                          fixture.size())), "write cursor fixture");
    }
    for (size_t f = 0; f < std::size(sizes); ++f) {
        CursorSource decoded;
        Check(DecodeCursor(tempFile, sizes[f], decoded), "native cursor frame decodes");
        Check(decoded.width == sizes[f] && decoded.height == sizes[f] &&
                  decoded.xHotspot == 7 + f * 3 && decoded.yHotspot == 9 + f * 5,
              "exact-size frame and its own hotspot are selected");
        Check(decoded.pixels == nativePixels[f], "native frame is pixel-exact, not a resized bitmap");
        for (const int percent : { 0, 25, 60, 100 }) {
            auto pixels = decoded.pixels;
            TintPixels(pixels.data(), pixels.size(), kChineseColor, decoded.darkInk, percent);
            const HCURSOR tinted = PackCursor(decoded, &pixels);
            ICONINFO info = {};
            CursorSource roundTrip;
            Check(tinted != nullptr && GetIconInfo(tinted, &info) &&
                      ReadColorPixels(info.hbmColor, roundTrip), "read back tinted native frame");
            Check(roundTrip.pixels == pixels && roundTrip.width == sizes[f] &&
                      roundTrip.height == sizes[f] && info.xHotspot == decoded.xHotspot &&
                      info.yHotspot == decoded.yHotspot, "packing preserves all pixels, size and hotspot");
            for (size_t i = 0; i < pixels.size(); ++i) {
                Check((pixels[i] >> 24) == (nativePixels[f][i] >> 24),
                      "every slider position preserves every alpha pixel");
            }
            DeleteObject(info.hbmColor);
            DeleteObject(info.hbmMask);
            DestroyCursor(tinted);
        }
    }
    // 截断目录/越界偏移/ANI/无文件：安全跳过，不能带着旧缓存继续染。
    for (const int kind : { 0, 1, 2, 3 }) {
        auto bad = fixture;
        if (kind == 0) { bad.resize(5); }
        if (kind == 1) { bad.resize(7); }
        if (kind == 2) { const DWORD offset = MAXDWORD; CopyMemory(bad.data() + 18, &offset, 4); }
        if (kind == 3) { CopyMemory(bad.data(), "RIFF", 4); }
        {
            std::ofstream file(tempFile, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(bad.data()), bad.size());
        }
        CursorSource badSource;
        Check(!DecodeCursor(tempFile, 72, badSource) && !badSource.ready,
              "invalid cursor files are rejected without a stale source");
    }
    {
        std::ofstream file(tempFile, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(fixture.data()), fixture.size());
    }
    CursorSource missing;
    Check(!DecodeCursor(L"", 72, missing), "an empty scheme path is not read from the tinted slot");

    setCursorCalls = 0;
    InitializeCursorTint();
    Check(winEventHooks == 1, "a foreground hook is registered at startup");
    Check(setCursorCalls == std::size(kSlots), "every cursor slot is tinted at startup");

    Check(lastCursorWidth == 72 && lastCursorHeight == 72,
          "48px accessibility size at 150% DPI installs a native 72px cursor");

    for (size_t i = 0; i < std::size(kSlots); ++i) {
        g_sources[i].darkInk = false;
        g_sources[i].pixels.assign(static_cast<size_t>(g_sources[i].width) * g_sources[i].height,
                                    0xFFFFFFFF);  // 白体，上色后应该正好是目标色
    }

    setCursorCalls = 0;
    ApplyTint(true);
    Check(setCursorCalls == std::size(kSlots), "switching to Chinese repaints every slot");
    Check(LooksTinted(lastCursorPixels, kChineseColor), "the slots now hold the Chinese colour");

    setCursorCalls = 0;
    ApplyTint(true);
    Check(setCursorCalls == 0, "the same state does not repaint");

    setCursorCalls = 0;
    ApplyTint(false);
    Check(setCursorCalls == std::size(kSlots) && LooksTinted(lastCursorPixels, kEnglishColor),
          "switching back repaints in the English colour");

    setCursorCalls = 0;
    ApplyCursorTintSettings(true, 0);
    Check(setCursorCalls == std::size(kSlots), "0% puts the untouched cursors back");
    Check(g_applied == Tint::None, "nothing stays applied at 0%");
    ApplyCursorTintSettings(true, 0);
    Check(setCursorCalls == std::size(kSlots), "the same 0% again does not touch the cursors");
    setCursorCalls = 0;
    ApplyTint(true);
    Check(setCursorCalls == 0, "at 0% the system cursors are never touched");

    ApplyCursorTintSettings(true, 60);
    Check(setCursorCalls == std::size(kSlots), "leaving 0% tints right away");

    // 总开关：关掉立刻还原；关着时浓度再变也不碰系统光标；打开马上重新上色。
    setCursorCalls = 0;
    ApplyCursorTintSettings(false, 60);
    Check(setCursorCalls == std::size(kSlots) && g_applied == Tint::None,
          "turning the master switch off puts the originals back");
    setCursorCalls = 0;
    ApplyCursorTintSettings(false, 80);
    Check(setCursorCalls == 0, "the intensity is ignored while the master switch is off");
    ApplyCursorTintSettings(false, 80);
    Check(setCursorCalls == 0, "an unchanged off state does not touch the cursors again");
    setCursorCalls = 0;
    ApplyCursorTintSettings(true, 80);
    Check(setCursorCalls == std::size(kSlots), "turning the master switch back on tints right away");

    Check(lastCursorWidth == 72, "re-enabling also uses the native physical size");
    const auto originalPixels = g_sources[0].pixels;
    for (const int dpi : { 96, 192, 288, 144 }) {
        cursorDpi = dpi;
        setCursorCalls = 0;
        ApplyTint(false);
        Check(setCursorCalls == std::size(kSlots) && lastCursorWidth == MulDiv(48, dpi, 96),
              "moving between monitor DPIs rebuilds the cursor even without an IME change");
    }
    Check(g_sources[0].pixels == originalPixels, "DPI rebuilds never tint an already tinted source");
    OnSystemCursorsChanged();
    Check(g_sources[0].pixels == originalPixels, "scheme notification reloads untouched file pixels");

    highContrast = true;
    setCursorCalls = 0;
    ApplyTint(true);
    Check(setCursorCalls == std::size(kSlots) && g_applied == Tint::None &&
              lastCursorPixels == nativePixels[1] && lastCursorWidth == 72 &&
              lastCursorX == 10 && lastCursorY == 14, "high contrast restores exact original cursor");
    ApplyTint(false);
    Check(setCursorCalls == std::size(kSlots), "high contrast does not keep replacing cursors");
    highContrast = false;
    ApplyTint(true);

    DestroyCursorTint();
    setCursorCalls = 0;
    DestroyCursorTint();
    Check(setCursorCalls == 0, "repeated shutdown does not touch system cursors");
    Check(DeleteFileW(tempFile) != FALSE, "remove cursor fixture");
    Check(winEventUnhooks == 1, "shutting down twice releases the hook exactly once");
    Check(g_applied == Tint::None, "shutdown does not leave a tint behind");
    std::puts("PASS: native cursor frames, pixel-exact alpha/packing, DPI, malformed files, tint lifecycle");
}

static void TestStartupTaskXml() {
    // 计划任务的动作路径是从 schtasks /Query /XML 的 <Command> 里抠出来的：
    // 外围引号和 XML 转义都得还原，读不出来时要如实报告"没有"。
    std::wstring path;
    Check(ExtractCommandPath(L"<Exec><Command>\"C:\\A B\\app.exe\"</Command></Exec>", path) &&
              path == L"C:\\A B\\app.exe",
          "the quoted action path unwraps to the real exe path");
    Check(ExtractCommandPath(L"<Command>C:\\a&amp;b\\x.exe</Command>", path) &&
              path == L"C:\\a&b\\x.exe",
          "XML escapes decode back to the real path");
    Check(!ExtractCommandPath(L"<Task><Settings/></Task>", path),
          "a task without an action reports no path");
    // schtasks 倒出来的 XML 是单字节的，UTF-8 解得动就按 UTF-8 解。
    std::wstring decoded;
    Check(DecodeConsoleText("x\346\265\213\350\257\225y", decoded) && decoded == L"x\u6D4B\u8BD5y",
          "UTF-8 console output decodes to the expected text");
    std::puts("PASS: startup task XML");
}

static void TestSettingsFile() {
    // 设置存在 exe 旁边的 ini 里：缺失或写坏的项退回默认值，并把文件重写成合法内容。
    const wchar_t* path = SettingsPath();
    Check(path != nullptr && wcslen(path) > 0, "the ini path comes from the exe location");
    DeleteFileW(path);

    bool repaired = false;
    AppSettings settings = ReadSettings(&repaired);
    Check(repaired && settings.mappingEnabled == kDefaultMappingEnabled &&
              settings.altCapsLockPassThrough == kDefaultAltCapsLockPassThrough &&
              settings.cursorTintEnabled == kDefaultCursorTintEnabled &&
              settings.cursorTintPercent == kDefaultCursorTintPercent,
          "a missing ini falls back to the defaults");
    Check(GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES,
          "a missing ini is written out with the defaults");

    // 合法值原样读回，而且不需要修复。
    WritePrivateProfileStringW(L"General", L"MappingEnabled", L"0", path);
    WritePrivateProfileStringW(L"General", L"AltCapsLockPassThrough", L"0", path);
    WritePrivateProfileStringW(L"General", L"CursorTintEnabled", L"0", path);
    WritePrivateProfileStringW(L"General", L"CursorTintPercent", L"35", path);
    repaired = true;
    settings = ReadSettings(&repaired);
    Check(!repaired && !settings.mappingEnabled && !settings.altCapsLockPassThrough &&
              !settings.cursorTintEnabled && settings.cursorTintPercent == 35,
          "valid ini values load back untouched");

    // 只坏一项时，坏项回默认，别的好值留着。
    WritePrivateProfileStringW(L"General", L"CursorTintPercent", L"abc", path);
    settings = ReadSettings();
    Check(!settings.mappingEnabled && settings.cursorTintPercent == kDefaultCursorTintPercent,
          "only the broken item falls back to its default");
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"General", L"CursorTintPercent", L"", text, 32, path);
    Check(wcscmp(text, L"100") == 0, "the broken ini is rewritten with valid content");

    // 越界、空值、半截数字，都退回默认。
    for (const wchar_t* bad : { L"101", L"-1", L"", L"50x", L"99999999999999999999" }) {
        WritePrivateProfileStringW(L"General", L"CursorTintPercent", bad, path);
        Check(ReadSettings().cursorTintPercent == kDefaultCursorTintPercent,
              "an out-of-range or unparsable intensity falls back to its default");
    }
    for (const wchar_t* bad : { L"2", L"true", L"" }) {
        WritePrivateProfileStringW(L"General", L"CursorTintEnabled", bad, path);
        Check(ReadSettings().cursorTintEnabled == kDefaultCursorTintEnabled,
              "a flag only accepts 0 or 1");
    }

    // 主程序和设置界面共用同一份读写：一边写成什么样，另一边就该读回什么样。
    AppSettings round = {};
    round.mappingEnabled = false;
    round.altCapsLockPassThrough = true;
    round.cursorTintEnabled = true;
    round.cursorTintPercent = 42;
    WriteSettings(round);
    repaired = true;
    const AppSettings loaded = ReadSettings(&repaired);
    Check(!repaired && loaded.mappingEnabled == round.mappingEnabled &&
              loaded.altCapsLockPassThrough == round.altCapsLockPassThrough &&
              loaded.cursorTintEnabled == round.cursorTintEnabled &&
              loaded.cursorTintPercent == round.cursorTintPercent,
          "what one process writes the other reads back");

    DeleteFileW(path);
    Check(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES, "no ini is left behind");
    std::puts("PASS: settings ini, broken values fall back and are rewritten");
}

int main() {
    _RTC_SetErrorFunc(OnRuntimeCheckFailure);
    BuildPaths();  // 设置文件路径跟着 exe 走
    TestKeyboard();
    TestHookThread();
    TestRendering();
    TestCursor();
    TestStartupTaskXml();
    TestSettingsFile();
    std::puts("All regression checks passed.");
    return 0;
}
