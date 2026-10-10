// 显式 opt-in 的桌面集成检查：真实调用 SetSystemCursor，不启动主程序/键盘钩子，
// 不改注册表；结束时按当前方案原图还原三个槽。普通回归不能覆盖系统内部的 DPI 重采样。
#define NOMINMAX
#include <Windows.h>
#include <atomic>
#include <cstdio>

#include "../capslock-switcher/cursor.cpp"
#include "../capslock-switcher/surface.cpp"

HINSTANCE g_hInst = nullptr;
HWND g_mainWnd = nullptr;
std::atomic_bool g_cursorTintEnabled{ true };
std::atomic_int g_cursorTintPercent{ 100 };
bool CurrentInputIsChinese(DWORD) { return false; }
void ShowBalloon(const wchar_t*) {}
void Log(const wchar_t*, ...) {}
void SaveSettings() {}  // 这个探针不改设置文件

int main() {
    EnableDpiAwareness();
    if (HighContrastOn()) {
        std::fputs("Live cursor checks require high contrast to be off.\n", stderr);
        return 1;
    }
    int result = 0;
    for (const int percent : { 100, 60, 5 }) {
        g_cursorTintPercent = percent;
        for (const bool chinese : { false, true }) {
            g_applied = Tint::None;
            ApplyTint(chinese);
            for (size_t i = 0; i < std::size(kSlots); ++i) {
                const auto& source = g_sources[i];
                auto expected = source.pixels;
                TintPixels(expected.data(), expected.size(), chinese ? kChineseColor : kEnglishColor,
                           source.darkInk, percent);
                ICONINFO info = {};
                CursorSource actual;
                const bool read = GetIconInfo(LoadCursorW(nullptr, MAKEINTRESOURCEW(kSlots[i].id)), &info) &&
                                  ReadColorPixels(info.hbmColor, actual);
                DeleteObject(info.hbmColor);
                DeleteObject(info.hbmMask);
                const bool exact = source.ready && read && actual.pixels == expected &&
                    actual.width == source.width && actual.height == source.height &&
                    info.xHotspot == source.xHotspot && info.yHotspot == source.yHotspot;
                std::printf("%s: live slot=%lu size=%dx%d Chinese=%d intensity=%d%% pixels/alpha/hotspot\n",
                            exact ? "PASS" : "FAIL", kSlots[i].id, actual.width, actual.height, chinese, percent);
                if (!exact) { result = 1; }
            }
        }
    }
    // 即使比较失败，也继续还原，不通过 exit/assert 提前离开染色状态。
    RestoreSourceCursors();
    for (size_t i = 0; i < std::size(kSlots); ++i) {
        ICONINFO info = {};
        CursorSource actual;
        const bool read = GetIconInfo(LoadCursorW(nullptr, MAKEINTRESOURCEW(kSlots[i].id)), &info) &&
                          ReadColorPixels(info.hbmColor, actual);
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
        const auto& source = g_sources[i];
        const bool exact = source.ready && read && actual.pixels == source.pixels &&
            actual.width == source.width && actual.height == source.height &&
            info.xHotspot == source.xHotspot && info.yHotspot == source.yHotspot;
        std::printf("%s: restore slot=%lu size=%dx%d original pixels/alpha/hotspot\n",
                    exact ? "PASS" : "FAIL", kSlots[i].id, actual.width, actual.height);
        if (!exact) {
            std::printf("detail: ready=%d read=%d pixels=%d expected=%dx%d hotspot=%lu,%lu actual=%lu,%lu\n",
                        source.ready, read, actual.pixels == source.pixels, source.width, source.height,
                        source.xHotspot, source.yHotspot, info.xHotspot, info.yHotspot);
            result = 1;
        }
    }
    DestroyCursorTint();
    return result;
}

