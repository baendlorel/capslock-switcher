// CapsLock Switcher：把 CapsLock 映射成 Ctrl+Space，方便切换中英文输入法。
//
// 这个文件只负责"进程外壳"：单实例、隐藏的主窗口，以及它的消息循环。
// 具体功能各自成模块：
//   keyboard.cpp  键盘钩子与按键注入
//   banner.cpp    切换提示横幅（屏幕中央闪一下中/英文）
//   tray.cpp      托盘图标与右键菜单
//   startup.cpp   开机启动（计划任务 + 提权副本）
//   splash.cpp    启动画面
//   settings.cpp  设置页
//   logging.cpp   诊断日志
//   surface.cpp   分层窗口与 DPI 辅助

#include "app.h"

#include "banner.h"
#include "keyboard.h"
#include "logging.h"
#include "settings.h"
#include "splash.h"
#include "surface.h"
#include "startup.h"
#include "tray.h"

#include <shellapi.h>  // CommandLineToArgvW

#include <atomic>

HINSTANCE g_hInst = nullptr;
HWND g_mainWnd = nullptr;

// 托盘菜单和设置页共同控制的"启用映射"开关。
std::atomic_bool g_enabled{ true };

// 缓存"登录任务装没装"的答案，启动时和每次改动之后刷新。
bool g_startupTaskInstalled = false;

// Alt+CapsLock 放行给原来的大写锁定（默认开）。
std::atomic_bool g_altPassThrough{ true };

namespace {

constexpr wchar_t kWindowClass[] = L"CapsLockSwitcherClass";
constexpr wchar_t kMutexName[] = L"CapsLockSwitcherMutex";

// 注册成系统级消息，好让第二次启动能和已经持有互斥体的实例说上话。
// 名字见下面的 kShowYourselfMessage。
constexpr wchar_t kShowYourselfMessage[] = L"CapsLockSwitcher.ShowYourself";

// 回调耗时超过 LowLevelHooksTimeout 的低级钩子会被 Windows 摘掉，而且从
// Windows 7 起是静默摘掉的：既没有通知，也没有接口能查询钩子是否还在。
// 托盘图标被丢掉时同样不给任何提示。所以只能用一个慢速定时器把两者重新
// 挂上，这是长时间运行后还能继续工作的唯一办法。
constexpr UINT kSelfHealMs = 60000;

UINT g_taskbarCreatedMessage = 0;
UINT g_showYourselfMessage = 0;

// 启动时挂钩子/托盘图标可能失败，重试期间的记账。
bool g_installTroubleLogged = false;
ULONGLONG g_installTroubleTick = 0;
unsigned g_installAttempts = 0;

// 消息循环返回了 -1。注意这种情况下 GetMessageW 不会碰 MSG，所以没有任何东西
// 可以派发；下面记录的是进程还能观察到的、关于"为什么失败"的一切。
void LogMessageLoopFailure(const DWORD lastError) {
	wchar_t text[512] = {};

	Log(L"===== GetMessageW returned -1, shutting down =====");
	LogEnvironment();
	Log(L"GetMessageW     : -1 (documented causes: invalid hWnd or invalid lpMsg)");
	Log(L"GetLastError    : %lu (0x%08lX) %s", lastError, lastError, ErrorText(lastError, text));
	Log(L"queue status    : 0x%08lX", GetQueueStatus(QS_ALLINPUT));
	Log(L"hidden window   : hwnd=0x%p IsWindow=%d visible=%d",
	    static_cast<void*>(g_mainWnd), IsWindow(g_mainWnd) != FALSE,
	    IsWindowVisible(g_mainWnd) != FALSE);
	Log(L"keyboard hook   : installed=%d", IsHookInstalled() ? 1 : 0);
	Log(L"tray icon       : registered=%d", g_trayIconAdded != FALSE);
	Log(L"message         : MSG was left untouched, so nothing was dispatched");
}

// 挂上钩子和托盘图标，只要有一样还没成功，就用一个短定时器不断重试。
// 这两件事失败都不再致命：开机自启时外壳常常还没准备好，SetWindowsHookEx
// 能成功而 Shell_NotifyIcon 失败，图标得过几秒才补得上。
// 以前正是在这里退出，才让人觉得程序一闪就没了。
void EnsureInstalled(const HWND hwnd) {
	const bool hookReady = InstallHook();
	const bool trayReady = AddTrayIcon(hwnd);
	// 先在这里读出来，免得被后面任何调用覆盖掉。
	const DWORD failureCode = GetLastError();

	if (hookReady && trayReady) {
		KillTimer(hwnd, kTimerRetryStartup);
		if (g_installTroubleLogged) {
			Log(L"startup install recovered: %u attempt(s) over %llu ms",
			    g_installAttempts, GetTickCount64() - g_installTroubleTick);
			g_installTroubleLogged = false;
		}
		return;
	}

	++g_installAttempts;
	if (!g_installTroubleLogged) {
		g_installTroubleLogged = true;
		g_installTroubleTick = GetTickCount64();
		wchar_t text[512] = {};
		// 登录时的自启项看到的就是这个样子：钩子挂得上，
		// 但外壳还没准备好接受托盘图标。
		Log(L"startup install incomplete: hook=%d tray=%d, retrying every %u ms",
		    hookReady ? 1 : 0, trayReady ? 1 : 0, kRetryStartupMs);
		Log(L"failure detail  : last error %lu (0x%08lX) %s",
		    failureCode, failureCode, ErrorText(failureCode, text));
	}
	SetTimer(hwnd, kTimerRetryStartup, kRetryStartupMs, nullptr);
}

// 每分钟跑一次。Windows 会不声不响地摘掉超时的低级钩子，外壳会在 Explorer
// 重启时丢掉托盘图标，所以这里把两者都重新挂一遍。配合快速重试和
// TaskbarCreated 处理，本程序和外壳不管谁先起来都能兜住。
void SelfHeal(const HWND hwnd) {
	const bool trayWasRegistered = g_trayIconAdded;

	RenewHook();
	RefreshTrayIcon(hwnd);

	if (!trayWasRegistered && g_trayIconAdded) {
		// 外壳把我们的图标丢掉了——通常是 Explorer 重启、而它的 TaskbarCreated
		// 广播我们没收到。这值得记一行日志，因为在用户看来这就是
		// "托盘图标不见了"。
		Log(L"tray icon was missing and has been re-registered on the self-heal tick");
	}

	if (!IsHookInstalled() || !g_trayIconAdded) {
		SetTimer(hwnd, kTimerRetryStartup, kRetryStartupMs, nullptr);
	}
}

LRESULT CALLBACK WndProc(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam) {
	switch (message) {
	case WM_CREATE:
		g_mainWnd = hwnd;  // 钩子回调往这个窗口投消息
		CreateSplashWindow(g_hInst);
		CreateBannerWindow(g_hInst);
		EnsureInstalled(hwnd);
		SetTimer(hwnd, kTimerSelfHeal, kSelfHealMs, nullptr);

		// 启动时问一次登录任务在不在，这个答案就是菜单和设置页里
		// "开机启动"旁边那个勾。
		g_startupTaskInstalled = QueryStartupTask();

		// 启动时记一行：设置页的日志窗口一打开就能看到钩子和托盘图标有没有挂上。
		Log(L"启动：键盘钩子 %s，托盘图标 %s",
		    IsHookInstalled() ? L"已挂上" : L"没挂上",
		    g_trayIconAdded ? L"已挂上" : L"没挂上");

		if (StartGdiplus() && PrepareSplash()) {
			ShowSplash();
		}
		break;

	case WM_TIMER:
		if (wParam == kTimerRetryStartup) {
			EnsureInstalled(hwnd);
		} else if (wParam == kTimerSelfHeal) {
			SelfHeal(hwnd);
		} else if (wParam == kTimerRefreshStartup) {
			// 距离上次请求改动已经过了一小会儿，
			// 把提权副本干出来的结果取回来。
			KillTimer(hwnd, kTimerRefreshStartup);
			g_startupTaskInstalled = QueryStartupTask();
		}
		break;

	case WM_SWITCH_IME:
		// 钩子在 CapsLock 按下时就记下了前台窗口：如果用户之后换了窗口，
		// 这条请求就已经过期了。
		if (g_enabled && reinterpret_cast<HWND>(wParam) == GetForegroundWindow()) {
			// 每按一下都记一行，设置页的日志窗口会跟着显示出来。
			Log(SendCtrlSpace() ? L"CapsLock -> Ctrl+Space：切换输入法"
			                    : L"CapsLock -> Ctrl+Space：注入失败，这次没切");
			ShowSwitchBanner();
		}
		break;

	case WM_CAPS_LOCK_PASSED: {
		// 没被映射掉的那一次按键：要么按着 Alt，要么映射被关掉了。
		// lParam 是钩子自己数出来的大写锁定状态（抬起时已经翻过）。
		const bool upper = lParam != 0;
		if (wParam != 0) {
			Log(L"Alt+CapsLock：放行给原来的大写锁定（现在%s）", upper ? L"大写" : L"小写");
			ShowCapsLockBanner(upper);
		} else {
			Log(L"映射已关闭：CapsLock 原样放行（现在%s）", upper ? L"大写" : L"小写");
		}
		break;
	}

	case WM_TRAYICON:
		// 双击直接开设置页，右键出菜单（菜单里也有"设置..."）。
		if (lParam == WM_LBUTTONDBLCLK) {
			OpenSettingsWindow();
		} else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
			ShowTrayMenu(hwnd);
		}
		break;

	case WM_DESTROY:
		KillTimer(hwnd, kTimerRetryStartup);
		KillTimer(hwnd, kTimerSelfHeal);
		KillTimer(hwnd, kTimerRefreshStartup);
		DestroySettingsWindow();
		DestroySplashWindow();
		DestroyBannerWindow();
		UninstallHook();
		RemoveTrayIcon();
		g_mainWnd = nullptr;
		PostQuitMessage(0);
		break;

	default:
		if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage) {
			// Explorer 重启，托盘图标也跟着没了。RefreshTrayIcon 会自己判断
			// 图标是不是真的不见了：一次用不上的广播不能让我们忘记自己
			// 还持有的图标，否则退出时就再也不会去删它。
			Log(L"Explorer restart seen (TaskbarCreated): re-registering the tray icon");
			RefreshTrayIcon(hwnd);
			if (!g_trayIconAdded) {
				SetTimer(hwnd, kTimerRetryStartup, kRetryStartupMs, nullptr);
			}
			break;
		}
		if (g_showYourselfMessage != 0 && message == g_showYourselfMessage) {
			// 第二次启动找到了我们，让我们把自己的图标重新露出来。
			RefreshTrayIcon(hwnd);
			ShowBalloon(L"Already running - the tray icon has been restored.");
			break;
		}
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}
	return 0;
}

}  // 匿名命名空间

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(nCmdShow);
	UNREFERENCED_PARAMETER(lpCmdLine);

	// 提权副本必须最先处理：它得避开正在运行的实例的互斥体，
	// 而且绝不创建窗口。这两个参数由 startup.cpp 提供，
	// 字面量必须和那里一致。
	int argumentCount = 0;
	wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
	const bool enableStartup = arguments != nullptr && argumentCount == 2 &&
	    wcscmp(arguments[1], kCommandStartupEnable) == 0;
	const bool disableStartup = arguments != nullptr && argumentCount == 2 &&
	    wcscmp(arguments[1], kCommandStartupDisable) == 0;
	LocalFree(arguments);
	if (enableStartup || disableStartup) {
		return RunStartupHelper(enableStartup);
	}

	BuildPaths();

	// 要在任何窗口或屏幕 DC 出现之前调用，这样启动画面和由 DPI 算出的
	// 尺寸都工作在真实的屏幕像素上。
	EnableDpiAwareness();

	g_hInst = hInstance;

	g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
	g_showYourselfMessage = RegisterWindowMessageW(kShowYourselfMessage);

	HANDLE hMutex = CreateMutex(nullptr, TRUE, kMutexName);
	if (hMutex == nullptr) {
		Log(L"could not acquire the single-instance mutex (%lu)", GetLastError());
		MessageBoxW(nullptr, L"Cannot acquire the application mutex. Another instance may be running.",
		            kAppTitle, MB_OK | MB_ICONERROR);
		return 1;
	}
	if (hMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
		// 请求已经在运行的那个实例把托盘图标挂回去：Explorer 重启之后
		// 它很可能就看不见了，而这正是用户以为程序死了、
		// 又去点快捷方式的时候。
		const HWND existing = FindWindowW(kWindowClass, kAppTitle);
		if (existing != nullptr && g_showYourselfMessage != 0) {
			PostMessageW(existing, g_showYourselfMessage, 0, 0);
		}
		MessageBoxW(nullptr, L"CapsLock Switcher is already running.", kAppTitle, MB_OK | MB_ICONINFORMATION);
		CloseHandle(hMutex);
		return 0;
	}

	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.lpfnWndProc = WndProc;
	wcex.hInstance = hInstance;
	wcex.lpszClassName = kWindowClass;

	if (!RegisterClassEx(&wcex)) {
		const DWORD lastError = GetLastError();
		wchar_t text[512] = {};
		Log(L"===== RegisterClassEx failed =====");
		LogEnvironment();
		Log(L"GetLastError    : %lu (0x%08lX) %s", lastError, lastError, ErrorText(lastError, text));
		Log(L"exit code       : 1");
		MessageBoxW(nullptr, L"Failed to register class", L"Error", MB_OK | MB_ICONERROR);
		if (hMutex != nullptr) {
			CloseHandle(hMutex);
		}
		return 1;
	}

	// WM_CREATE 会在启动钩子线程之前就写好 g_mainWnd。这里不要再写一遍，
	// 因为那个线程可能已经在读它了。
	const HWND mainWindow = CreateWindowEx(
		0,
		kWindowClass,
		kAppTitle,
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
		nullptr,
		nullptr,
		hInstance,
		nullptr
	);

	if (mainWindow == nullptr) {
		const DWORD lastError = GetLastError();
		wchar_t text[512] = {};
		Log(L"===== CreateWindowEx failed =====");
		LogEnvironment();
		Log(L"GetLastError    : %lu (0x%08lX) %s", lastError, lastError, ErrorText(lastError, text));
		Log(L"exit code       : 1");
		MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
		if (hMutex != nullptr) {
			CloseHandle(hMutex);
		}
		return 1;
	}

	MSG msg = {};
	BOOL result = 0;
	// 出错时 GetMessage 返回 -1，它非零、看起来和成功一样；而且 MSG 不会被写入，
	// 直接派发就等于派发栈上的垃圾数据。所以这里显式检查 -1。
	while ((result = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
		if (result == -1) {
			// 先读错误码，免得被别的调用覆盖，然后把进程还能观察到的失败细节
			// 全部记下来。写日志时钩子和托盘图标都还在，
			// 所以记录反映的是程序放弃时真实所处的状态。
			const DWORD lastError = GetLastError();
			LogMessageLoopFailure(lastError);
			break;
		}
		if (HandleSettingsMessage(&msg)) {
			continue;  // 设置页的 Tab/Esc 由 IsDialogMessage 处理
		}
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	if (result == -1 && g_mainWnd != nullptr) {
		// 这样跳出循环会跳过 WM_DESTROY，而卸钩子正是在那里做的。留着钩子
		// 比没有更糟：它仍然吞掉 CapsLock，可注入动作是在消息循环里做的，
		// 于是 CapsLock 会彻底失效、什么都不发。所以要把窗口拆掉，
		// 让 WM_DESTROY 跑一遍，释放钩子和托盘图标。
		DestroyWindow(g_mainWnd);
	}

	if (result == -1) {
		Log(L"action          : window destroyed, hook and tray icon released");
		Log(L"exit code       : 1");
	}

	StopGdiplus();

	if (hMutex != nullptr) {
		ReleaseMutex(hMutex);
		CloseHandle(hMutex);
	}

	return result == -1 ? 1 : static_cast<int>(msg.wParam);
}
