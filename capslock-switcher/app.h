#pragma once

#include <Windows.h>
#include <atomic>

// 应用级的共享状态与入口。各模块直接调用这里声明的东西，
// 中间不再加回调、适配器之类的一层。

constexpr wchar_t kAppTitle[] = L"CapsLock Switcher";

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

// 应用私有消息都排在 WM_APP 之上。WM_USER 那一段留给窗口类自己，不要占用。
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_SWITCH_IME = WM_APP + 2;
constexpr UINT WM_REINSTALL_HOOK = WM_APP + 3;
// CapsLock 这次没被映射掉（Alt+CapsLock，或者映射被关掉了）：只把这件事记进日志。
// wParam 非 0 表示是 Alt 按着。
constexpr UINT WM_CAPS_LOCK_PASSED = WM_APP + 4;

// 主窗口上的定时器。
constexpr UINT_PTR kTimerRetryStartup = 1;
constexpr UINT_PTR kTimerSelfHeal = 2;
constexpr UINT_PTR kTimerRefreshStartup = 3;

constexpr UINT kRetryStartupMs = 2000;
constexpr UINT kStartupRefreshDelayMs = 3000;

void SetMappingEnabled(bool enabled);  // tray.cpp
void SetStartupEnabled(bool enable);   // startup.cpp
void SetAltCapsLockEnabled(bool enabled);  // tray.cpp
