#pragma once

#include <Windows.h>
#include <atomic>

// 应用级的共享状态与入口。各模块直接调用这里声明的东西，
// 中间不再加回调、适配器之类的一层。

constexpr wchar_t kAppTitle[] = L"CapsLock Switcher";
// 主窗口的类名：第二次启动时靠它找到已经在跑的那个实例，把托盘图标重新挂回去。
constexpr wchar_t kMainWindowClass[] = L"CapsLockSwitcherClass";

// 中英文状态色：中央横幅和鼠标指针共用同一份，两边的颜色必须一致。
constexpr COLORREF kChineseColor = RGB(0xFF, 0x1F, 0x45);  // #FF1F45
constexpr COLORREF kEnglishColor = RGB(0x00, 0x73, 0xFF);  // #0073FF
// 日语假名两档的暖底色（横幅和鼠标指针共用，上面写黑字）：平假名黄、片假名橙。
constexpr COLORREF kJapaneseColor = RGB(0xFF, 0xC9, 0x00);  // #FFC900
constexpr COLORREF kKatakanaColor = RGB(0xFF, 0x8B, 0x31);  // #FF8B31
// 与 InputMode 顺序一致：中文红、英文蓝（日语下的英文档也走这一格）、平假名黄、片假名橙。
constexpr COLORREF kInputModeColors[] = {
	kEnglishColor, kEnglishColor, kChineseColor, kJapaneseColor, kKatakanaColor
};

extern HINSTANCE g_hInst;
extern HWND g_mainWnd;  // 隐藏的主窗口：钩子消息和定时器都投给它

// 托盘菜单和设置页共同控制的"启用映射"开关。关掉时钩子原样放行 CapsLock，
// 这个键就恢复成普通的 CapsLock。
extern std::atomic_bool g_enabled;

// 缓存"登录任务装没装"的答案，启动时和每次改动之后刷新。
extern bool g_startupTaskInstalled;

// Alt+CapsLock 是否放行给原来的大写锁定。关掉之后 Alt 不再特殊：
// Alt+CapsLock 和 CapsLock 一样，照样切换输入法。
extern std::atomic_bool g_altPassThrough;

// 鼠标指针跟着中英文状态变色的总开关；关掉时系统光标立刻还原，滑块也不再起作用。
extern std::atomic_bool g_cursorTintEnabled;

// 变色程度：100 是完整的中文红/英文蓝，0 和总开关关闭等价，中间的值按比例把红/蓝减淡。
extern std::atomic_int g_cursorTintPercent;

// 应用私有消息都排在 WM_APP 之上。WM_USER 那一段留给窗口类自己，不要占用。
constexpr UINT WM_TRAYICON = WM_APP + 1;
// wParam = 目标 HWND，lParam = 按下时的 HKL。
constexpr UINT WM_SWITCH_IME = WM_APP + 2;
constexpr UINT WM_REINSTALL_HOOK = WM_APP + 3;
// CapsLock 这次没被映射掉：只把这件事记进日志。wParam 是下面哪种情况，
// lParam 非 0 表示现在是大写。
constexpr UINT WM_CAPS_LOCK_PASSED = WM_APP + 4;
constexpr WPARAM kCapsPassedMappingOff = 0;     // 映射总开关关着
constexpr WPARAM kCapsPassedAlt = 1;            // 按着 Alt，放行给原来的大写锁定
constexpr WPARAM kCapsPassedOtherLanguage = 2;  // 前台不是中/日输入法，留给原键位

// 主窗口上的定时器。
constexpr UINT_PTR kTimerRetryStartup = 1;
constexpr UINT_PTR kTimerSelfHeal = 2;
constexpr UINT_PTR kTimerRefreshStartup = 3;
constexpr UINT_PTR kTimerCursorSettle = 4;  // 前台窗口变化后的去抖（见 cursor.cpp）
constexpr UINT_PTR kTimerCursorPoll = 5;    // 兜底轮询同窗口内切换输入法的情况

constexpr UINT kRetryStartupMs = 2000;
constexpr UINT kStartupRefreshDelayMs = 3000;

void SetStartupEnabled(bool enable);  // startup.cpp
// cursor.cpp：把开关和浓度应用下去。开关状态本身由设置页维护，这里只负责生效。
void ApplyCursorTintSettings(bool enabled, int percent);
// main.cpp：读 exe 旁边的 ini 并应用。启动时一次，设置页改完再一次。
void ApplySettingsFromIni();
