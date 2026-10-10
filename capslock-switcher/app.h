#pragma once

#include <Windows.h>
#include <atomic>

// 应用级的共享状态与入口。各模块直接调用这里声明的东西，
// 中间不再加回调、适配器之类的一层。

constexpr wchar_t kAppTitle[] = L"CapsLock Switcher";
// 主窗口的类名：设置界面（另一个进程）靠它找到主程序，把"设置变了"的通知投过去。
constexpr wchar_t kMainWindowClass[] = L"CapsLockSwitcherClass";

// 中英文状态色：中央横幅和鼠标指针共用同一份，两边的颜色必须一致。
constexpr COLORREF kChineseColor = RGB(0xFF, 0x1F, 0x45);  // #FF1F45
constexpr COLORREF kEnglishColor = RGB(0x00, 0x73, 0xFF);  // #0073FF

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
constexpr UINT WM_SWITCH_IME = WM_APP + 2;
constexpr UINT WM_REINSTALL_HOOK = WM_APP + 3;
// CapsLock 这次没被映射掉（Alt+CapsLock，或者映射被关掉了）：只把这件事记进日志。
// wParam 非 0 表示是 Alt 按着。
constexpr UINT WM_CAPS_LOCK_PASSED = WM_APP + 4;
// 设置界面（另一个进程）改完 ini 之后发过来的：重新读一遍并立刻生效。
constexpr UINT WM_RELOAD_SETTINGS = WM_APP + 5;

// 主窗口上的定时器。
constexpr UINT_PTR kTimerRetryStartup = 1;
constexpr UINT_PTR kTimerSelfHeal = 2;
constexpr UINT_PTR kTimerRefreshStartup = 3;
constexpr UINT_PTR kTimerCursorSettle = 4;  // 前台窗口变化后的去抖（见 cursor.cpp）
constexpr UINT_PTR kTimerCursorPoll = 5;    // 兜底轮询同窗口内切换输入法的情况

constexpr UINT kRetryStartupMs = 2000;
constexpr UINT kStartupRefreshDelayMs = 3000;

void SetStartupEnabled(bool enable);  // startup.cpp
// cursor.cpp：把 ini 里的开关和浓度应用下去。开关状态本身由设置界面维护，这里只负责生效。
void ApplyCursorTintSettings(bool enabled, int percent);
