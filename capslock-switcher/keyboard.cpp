#include "keyboard.h"

#include "app.h"
#include "logging.h"

namespace {

// 钩子线程的全部状态：钩子句柄、停止/就绪事件，以及键盘当前状态的记录。
std::atomic<HHOOK> g_hKeyboardHook{ nullptr };
HANDLE g_hookThread = nullptr;
HANDLE g_hookStopEvent = nullptr;
HANDLE g_hookReadyEvent = nullptr;
DWORD g_hookThreadId = 0;
// 当前"物理按下 CapsLock"的状态只有钩子线程会碰。
bool g_capsDown = false;
bool g_capsSwallowed = false;

// 大写锁定的开关状态。钩子看得见每一次 CapsLock 的抬起，所以"放行一次就翻一次"，
// 比去问系统的开关位可靠：那个位只在有焦点的线程输入队列里更新，本程序没有焦点。
bool g_capsLockOn = false;
// 本次物理按下走的是哪条路：true = 按着 Alt 放行（日志和提示文字要用）。
bool g_capsPassedAsAlt = false;

}  // 匿名命名空间

bool SendCtrlSpace() {
	// 用户还按着的修饰键（以及 Space）绝不替他抬起。
	if ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0) {
		return false;
	}
	const bool ownCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0;
	INPUT inputs[4] = {};

	inputs[0].type = INPUT_KEYBOARD;
	inputs[0].ki.wVk = VK_CONTROL;

	inputs[1].type = INPUT_KEYBOARD;
	inputs[1].ki.wVk = VK_SPACE;

	inputs[2].type = INPUT_KEYBOARD;
	inputs[2].ki.wVk = VK_SPACE;
	inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

	inputs[3].type = INPUT_KEYBOARD;
	inputs[3].ki.wVk = VK_CONTROL;
	inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

	const UINT count = ownCtrl ? 4 : 2;
	const UINT sent = SendInput(count, inputs + (ownCtrl ? 0 : 1), sizeof(INPUT));
	if (sent == count) {
		return true;
	}
	const DWORD error = GetLastError();
	// 部分插入失败时，不能把自己合成的按键留在按下状态。
	INPUT releases[2] = {};
	UINT releaseCount = 0;
	if (sent > (ownCtrl ? 1u : 0u)) {
		releases[releaseCount++] = inputs[2];
	}
	if (ownCtrl && sent > 0) {
		releases[releaseCount++] = inputs[3];
	}
	if (releaseCount != 0) {
		SendInput(releaseCount, releases, sizeof(INPUT));
	}
	Log(L"SendInput inserted %u/%u events (error %lu)", sent, count, error);
	return false;
}

LRESULT CALLBACK LowLevelKeyboardProc(const int nCode, const WPARAM wParam, const LPARAM lParam) {
	if (nCode == HC_ACTION) {
		const auto* pKeyboard = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

		// 别的程序注入的 CapsLock 也会真的翻转系统的大写锁定，跟着数一下，
		// 免得自己记的"大写锁定开着没有"慢慢跑偏。物理按键走下面的分支。
		if (pKeyboard->vkCode == VK_CAPITAL && (pKeyboard->flags & LLKHF_INJECTED) != 0) {
			if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
				g_capsLockOn = !g_capsLockOn;
			}
		}

		if (pKeyboard->vkCode == VK_CAPITAL && (pKeyboard->flags & LLKHF_INJECTED) == 0) {
			if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
				if (!g_capsDown) {
					g_capsDown = true;
					const bool alt = (pKeyboard->flags & LLKHF_ALTDOWN) != 0;
					// Alt+CapsLock 放行给原来的大写锁定。这一条可以在托盘菜单和设置页里
					// 关掉；关掉之后 Alt 不再特殊，Alt+CapsLock 就和 CapsLock 一样切换输入法。
					g_capsPassedAsAlt = alt && g_altPassThrough;
					g_capsSwallowed = g_enabled && !g_capsPassedAsAlt;
					if (g_capsSwallowed) {
						// 现在就把目标窗口记下来：排队中的请求不许切换到一个新应用。
						g_capsSwallowed = PostMessageW(g_mainWnd, WM_SWITCH_IME,
						    reinterpret_cast<WPARAM>(GetForegroundWindow()), 0) != FALSE;
					}
				}
				if (g_capsSwallowed) {
					return 1;
				}
			}
			if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
				const bool swallowed = g_capsSwallowed;
				g_capsDown = false;
				g_capsSwallowed = false;
				if (swallowed) {
					return 1;
				}
				// 这一次按键原样放行了（按着 Alt，或者映射被关掉），系统会把它那边的
				// 大写锁定翻个个儿。回调里不能写日志（文件 I/O 会拖长钩子回调），而且此刻
				// 按键还没被系统处理，所以等到抬起再通知主线程：既避开回调里的 I/O，
				// 又保证系统那边已经翻完了。
				g_capsLockOn = !g_capsLockOn;
				PostMessageW(g_mainWnd, WM_CAPS_LOCK_PASSED, g_capsPassedAsAlt ? 1 : 0,
				             g_capsLockOn ? 1 : 0);
			}
		}
	}

	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// 钩子的安装和回调都属于这个专用的消息线程。
// 解码 PNG、弹菜单、跑 schtasks 都饿不死它的消息泵。
void ReplaceHookOnCurrentThread() {
	const HHOOK replacement = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_hInst, 0);
	if (replacement != nullptr) {
		const HHOOK previous = g_hKeyboardHook.exchange(replacement);
		if (previous != nullptr) {
			UnhookWindowsHookEx(previous);
		}
	}
}

DWORD WINAPI HookThreadProc(void*) {
	MSG msg = {};
	PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
	g_capsDown = (GetAsyncKeyState(VK_CAPITAL) & 0x8000) != 0;
	// 线程的输入队列刚建好，这时读到的开关位就是系统当前的状态；之后靠回调里翻。
	g_capsLockOn = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
	g_capsSwallowed = false;
	ReplaceHookOnCurrentThread();
	SetEvent(g_hookReadyEvent);
	bool running = true;
	while (running) {
		const DWORD wait = MsgWaitForMultipleObjects(1, &g_hookStopEvent, FALSE, INFINITE, QS_ALLINPUT);
		if (wait != WAIT_OBJECT_0 + 1) {
			break;
		}
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				running = false;
				break;
			}
			if (msg.message == WM_REINSTALL_HOOK) {
				ReplaceHookOnCurrentThread();
			} else {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
		}
	}
	const HHOOK hook = g_hKeyboardHook.exchange(nullptr);
	if (hook != nullptr) {
		UnhookWindowsHookEx(hook);
	}
	return 0;
}

void UninstallHook() {
	if (g_hookThread != nullptr) {
		SetEvent(g_hookStopEvent);
		WaitForSingleObject(g_hookThread, INFINITE);
		CloseHandle(g_hookThread);
		g_hookThread = nullptr;
	}
	if (g_hookStopEvent != nullptr) {
		CloseHandle(g_hookStopEvent);
		g_hookStopEvent = nullptr;
	}
	if (g_hookReadyEvent != nullptr) {
		CloseHandle(g_hookReadyEvent);
		g_hookReadyEvent = nullptr;
	}
	g_hookThreadId = 0;
}

bool InstallHook() {
	if (g_hookThread != nullptr && WaitForSingleObject(g_hookThread, 0) == WAIT_TIMEOUT) {
		if (g_hKeyboardHook.load() == nullptr) {
			PostThreadMessageW(g_hookThreadId, WM_REINSTALL_HOOK, 0, 0);
		}
		return g_hKeyboardHook.load() != nullptr;
	}
	UninstallHook();
	g_hookStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_hookReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (g_hookStopEvent != nullptr && g_hookReadyEvent != nullptr) {
		g_hookThread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, &g_hookThreadId);
	}
	if (g_hookThread == nullptr) {
		UninstallHook();
		return false;
	}
	const HANDLE readyOrExited[] = { g_hookReadyEvent, g_hookThread };
	return WaitForMultipleObjects(2, readyOrExited, FALSE, INFINITE) == WAIT_OBJECT_0 &&
	       g_hKeyboardHook.load() != nullptr;
}

bool IsHookInstalled() {
	return g_hKeyboardHook.load() != nullptr;
}

// 每次 CapsLock 都靠它兜底：换一个新的钩子，换不上就排一个重试定时器。
// 先挂新的再摘旧的（在钩子线程里做），所以不存在"没有钩子"的空档。
bool RenewHook() {
	if (InstallHook() && PostThreadMessageW(g_hookThreadId, WM_REINSTALL_HOOK, 0, 0)) {
		return true;
	}
	SetTimer(g_mainWnd, kTimerRetryStartup, kRetryStartupMs, nullptr);
	return false;
}
