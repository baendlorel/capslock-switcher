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
static unsigned posted = 0;
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
static BOOL WINAPI FakePost(HWND, UINT, WPARAM, LPARAM) {
    ++posted;
    return postSucceeds;
}
static LRESULT WINAPI FakeNext(HHOOK, int, WPARAM, LPARAM) { return 0; }
static UINT WINAPI FakeSend(UINT count, LPINPUT inputs, int) {
    sentBatches.emplace_back(inputs, inputs + count);
    return sentBatches.size() == 1 && firstSendCount != UINT_MAX ? firstSendCount : count;
}

// 键盘 API 换成替身，且必须在包含各模块之前定义。
#define GetAsyncKeyState FakeKeyState
#define GetForegroundWindow FakeForeground
#define PostMessageW FakePost
#define CallNextHookEx FakeNext
#define SendInput FakeSend
#define SetWindowsHookExW FakeInstall
#define UnhookWindowsHookEx FakeUnhook

// 各模块的 .cpp 都编进这一个翻译单元，替身才对所有模块都生效。
#include "../capslock-switcher/main.cpp"
#include "../capslock-switcher/banner.cpp"
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
    g_enabled = true;
    posted = 0;
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
    Check(posted == 1, "long press switches exactly once");
    Check(Key(WM_KEYUP) == 1, "mapped CapsLock up swallowed");
    Key(WM_KEYDOWN);
    Key(WM_KEYUP);
    Check(posted == 2, "second press switches again");

    ResetKeyboard();
    Check(Key(WM_SYSKEYDOWN, LLKHF_ALTDOWN) == 0, "Alt CapsLock passed through");
    Check(Key(WM_KEYDOWN) == 0, "Alt released during hold does not start mapping");
    Check(Key(WM_KEYUP) == 0 && posted == 0, "Alt CapsLock up stays paired");

    ResetKeyboard();
    Key(WM_KEYDOWN);
    g_enabled = false;
    Check(Key(WM_SYSKEYUP, LLKHF_ALTDOWN) == 1, "disable/Alt during hold preserves swallowed up");
    Check(Key(WM_KEYDOWN) == 0, "disabled down passes through");
    g_enabled = true;
    Check(Key(WM_KEYUP) == 0, "enable during hold preserves passed-through up");

    ResetKeyboard();
    Check(Key(WM_KEYDOWN, LLKHF_INJECTED) == 0, "injected CapsLock passes through");
    Check(Key(WM_KEYUP, LLKHF_INJECTED) == 0 && posted == 0, "injected keys do not alter physical press");
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
    // 提示横幅：中文红底、英文蓝底，圆角必须真的透明，颜色只预乘一次。
    for (int chinese = 0; chinese <= 1; ++chinese) {
        g_bannerChinese = chinese != 0;
        g_bannerText = chinese != 0 ? L"中文" : L"English";
        const int width = 200;
        const int height = 96;
        Check(RenderBanner(width, height, 96), "banner renders");
        const auto* pixels = static_cast<const DWORD*>(g_bannerBits);
        Check(pixels[0] == 0, "banner corner is transparent");
        Check((pixels[10 * width + width / 2] >> 24) == 255, "banner interior opaque");
        const COLORREF color = chinese != 0 ? kChineseColor : kEnglishColor;
        bool hasEdge = false;
        for (int i = 0; i < width * height; ++i) {
            const DWORD alpha = pixels[i] >> 24;
            if (alpha > 0 && alpha < 255) {
                hasEdge = true;
                Check(((pixels[i] >> 16) & 255) == GetRValue(color) * alpha / 255 &&
                      ((pixels[i] >> 8) & 255) == GetGValue(color) * alpha / 255 &&
                      (pixels[i] & 255) == GetBValue(color) * alpha / 255,
                      "banner edge colour premultiplied exactly once");
            }
        }
        Check(hasEdge, "banner has an anti-aliased edge");
        ReleaseBannerSurface();
    }
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

int main() {
    TestKeyboard();
    TestHookThread();
    TestRendering();
    std::puts("All regression checks passed.");
    return 0;
}
