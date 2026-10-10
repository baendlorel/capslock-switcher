// 独立的回归检查：键盘 API 全部用替身，所以运行这个可执行文件期间
// 不会注入任何按键，也不会安装全局键盘钩子。
#define NOMINMAX
#include <Windows.h>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <climits>

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

// 各模块的 .cpp 都编进这一个翻译单元，替身才对所有模块都生效。
#include "../capslock-switcher/main.cpp"
#include "../capslock-switcher/banner.cpp"
#include "../capslock-switcher/cursor.cpp"
#include "../capslock-switcher/keyboard.cpp"
#include "../capslock-switcher/logging.cpp"
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
static unsigned cursorReloads = 0;  // SPI_SETCURSORS 被调了几次
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
        }
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
    }
    if (cursor != nullptr) {
        DestroyCursor(cursor);
    }
    return TRUE;
}

// SPI_SETCURSORS 拦下来计数，别的（比如 surface.cpp 要用的 SPI_GETWORKAREA）转给真函数。
static BOOL WINAPI FakeSystemParametersInfo(UINT action, UINT param, PVOID data, UINT winIni) {
    if (action == SPI_SETCURSORS) {
        ++cursorReloads;
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
    const HCURSOR packed = PackCursor(sample, sample.pixels);
    ICONINFO packedInfo = {};
    Check(packed != nullptr && GetIconInfo(packed, &packedInfo) != FALSE,
          "a packed cursor can be read back");
    Check(packedInfo.xHotspot == 2 && packedInfo.yHotspot == 3, "the hotspot survives packing");
    DeleteObject(packedInfo.hbmColor);
    DeleteObject(packedInfo.hbmMask);
    if (packed != nullptr) {
        DestroyCursor(packed);
    }

    // 真光标长什么样取决于系统（DPI 缩放、用户方案、甚至上一次运行留下的颜色），
    // 所以解码那条路只查"读得出来"，颜色检查换成一组合成光源来做。
    CursorSource decoded;
    Check(DecodeCursor(32512, decoded) && decoded.width > 0 && decoded.height > 0,
          "the system arrow cursor decodes to something");

    InitializeCursorTint();
    Check(winEventHooks == 1, "a foreground hook is registered at startup");
    Check(setCursorCalls == std::size(kCursorIds), "every cursor slot is tinted at startup");

    for (size_t i = 0; i < std::size(kCursorIds); ++i) {
        g_sources[i] = CursorSource{};
        g_sources[i].width = 4;
        g_sources[i].height = 4;
        g_sources[i].xHotspot = 1;
        g_sources[i].yHotspot = 1;
        g_sources[i].darkInk = false;
        g_sources[i].ready = true;
        g_sources[i].pixels.assign(16, 0xFFFFFFFF);  // 白体，上色后应该正好是目标色
    }

    setCursorCalls = 0;
    ApplyTint(true);
    Check(setCursorCalls == std::size(kCursorIds), "switching to Chinese repaints every slot");
    Check(LooksTinted(lastCursorPixels, kChineseColor), "the slots now hold the Chinese colour");

    setCursorCalls = 0;
    ApplyTint(true);
    Check(setCursorCalls == 0, "the same state does not repaint");

    setCursorCalls = 0;
    ApplyTint(false);
    Check(setCursorCalls == std::size(kCursorIds) && LooksTinted(lastCursorPixels, kEnglishColor),
          "switching back repaints in the English colour");

    const unsigned reloadsBefore = cursorReloads;
    SetCursorTintPercent(0);
    Check(cursorReloads == reloadsBefore + 1, "sliding to 0% reloads the user's cursors");
    Check(g_applied == Tint::None, "nothing stays applied at 0%");
    setCursorCalls = 0;
    ApplyTint(true);
    Check(setCursorCalls == 0, "at 0% the system cursors are never touched");

    SetCursorTintPercent(60);
    Check(setCursorCalls == std::size(kCursorIds), "leaving 0% tints right away");

    DestroyCursorTint();
    DestroyCursorTint();
    Check(winEventUnhooks == 1, "shutting down twice releases the hook exactly once");
    Check(g_applied == Tint::None, "shutdown does not leave a tint behind");
    std::puts("PASS: cursor tint colour rule, hotspot round trip, state machine, shutdown");
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

int main() {
    TestKeyboard();
    TestHookThread();
    TestRendering();
    TestCursor();
    TestStartupTaskXml();
    std::puts("All regression checks passed.");
    return 0;
}
