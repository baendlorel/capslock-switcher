#include "keyboard.h"

#include "app.h"
#include "logging.h"

#include <imm.h>

#pragma comment(lib, "imm32.lib")

namespace {

// WM_IME_CONTROL 的这两个查询码未在当前 SDK 的 imm.h 中公开。
constexpr WPARAM kImeGetConversionMode = 0x0001;
constexpr WPARAM kImeGetOpenStatus = 0x0005;

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
// 没被映射掉时，抬起那一刻上报给主线程的原因（见 app.h 的 kCapsPassed*）。
WPARAM g_capsPassReason = kCapsPassedMappingOff;

}  // 匿名命名空间

InputMode GetInputMode(const HWND target, const DWORD timeoutMs) {
	const DWORD thread = GetWindowThreadProcessId(target, nullptr);
	if (thread == 0) {
		return InputMode::Unknown;  // 不用 GetKeyboardLayout(0) 误读自身线程
	}
	const HKL layout = GetKeyboardLayout(thread);
	if (layout == nullptr) {
		return InputMode::Unknown;
	}
	const WORD language = PRIMARYLANGID(LOWORD(layout));
	if (language != LANG_CHINESE && language != LANG_JAPANESE) {
		return InputMode::English;
	}
	GUITHREADINFO info = { sizeof(info) };
	if (!GetGUIThreadInfo(thread, &info) || info.hwndFocus == nullptr) {
		return InputMode::Unknown;
	}
	// 用真正编辑控件的默认 IME 窗口，不枚举线程里碰巧遇到的第一个 IME。
	const HWND ime = ImmGetDefaultIMEWnd(info.hwndFocus);
	if (ime == nullptr) {
		return InputMode::Unknown;
	}
	DWORD_PTR value = 0;
	if (language == LANG_JAPANESE) {
		if (!SendMessageTimeoutW(ime, WM_IME_CONTROL, kImeGetOpenStatus, 0,
		                         SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, &value) ||
		    value == static_cast<DWORD_PTR>(-1)) {
			return InputMode::Unknown;
		}
		// 日语关闭 IME 后 conversion mode 仍可能保留旧的假名位，必须先看开关。
		if (value == 0) {
			return InputMode::English;
		}
	}
	if (!SendMessageTimeoutW(ime, WM_IME_CONTROL, kImeGetConversionMode, 0,
	                         SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, &value) ||
	    value == static_cast<DWORD_PTR>(-1)) {
		return InputMode::Unknown;
	}
	if ((value & IME_CMODE_NATIVE) == 0) {
		return InputMode::English;
	}
	if (language == LANG_CHINESE) {
		return InputMode::Chinese;
	}
	return (value & IME_CMODE_KATAKANA) != 0 ? InputMode::Katakana : InputMode::Hiragana;
}

bool SwitchInputMode(const HWND target, const HKL layout) {
	const DWORD thread = GetWindowThreadProcessId(target, nullptr);
	if (thread == 0 || target != GetForegroundWindow() || layout == nullptr ||
	    layout != GetKeyboardLayout(thread)) {
		return false;
	}
	const WORD language = PRIMARYLANGID(LOWORD(layout));
	WORD key = VK_SPACE;
	WORD modifier = VK_CONTROL;
	InputMode next = InputMode::Unknown;
	if (language == LANG_JAPANESE) {
		const InputMode current = GetInputMode(target);
		if (current == InputMode::Unknown) {
			Log(L"日语：无法读取输入模式，本次不切换");
			return false;
		}
		next = current == InputMode::Hiragana ? InputMode::Katakana
		     : current == InputMode::Katakana ? InputMode::English : InputMode::Hiragana;
		// Microsoft IME：ImeOn = 平假名，Shift+ImeOn = 全角片假名，ImeOff = 半角英文。
		// 不发 CapsLock，不用 F6/F7/F10 转换正在组合的文字。
		key = next == InputMode::English ? VK_IME_OFF : VK_IME_ON;
		modifier = next == InputMode::Katakana ? VK_SHIFT : 0;
		// Ctrl/Alt/Shift 会改变这些键的语义。绝不替用户松开修饰键，也不触发重转换。
		if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) ||
		    (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000) ||
		    (modifier == 0 && (GetAsyncKeyState(VK_SHIFT) & 0x8000))) {
			Log(L"日语：请松开修饰键后再按 CapsLock");
			return false;
		}
	} else if (language != LANG_CHINESE) {
		return false;
	}
	// IME 查询期间也可能切走窗口/布局；发送前再核对一次，不切到其它应用。
	if (target != GetForegroundWindow() || layout != GetKeyboardLayout(thread) ||
	    (GetAsyncKeyState(key) & 0x8000)) {
		return false;
	}
	const bool ownModifier = modifier != 0 && (GetAsyncKeyState(modifier) & 0x8000) == 0;
	INPUT inputs[4] = {};
	const WORD keys[] = { modifier, key, key, modifier };
	for (UINT i = 0; i < 4; ++i) {
		inputs[i].type = INPUT_KEYBOARD;
		inputs[i].ki.wVk = keys[i];
		inputs[i].ki.dwFlags = i >= 2 ? KEYEVENTF_KEYUP : 0;
	}
	const UINT count = ownModifier ? 4 : 2;
	const UINT sent = SendInput(count, inputs + (ownModifier ? 0 : 1), sizeof(INPUT));
	if (sent == count) {
		if (language == LANG_JAPANESE) {
			Log(L"CapsLock：请求切换日语 → %s", kInputModeNames[static_cast<int>(next)]);
		} else {
			Log(L"CapsLock -> Ctrl+Space：切换输入法");
		}
		return true;
	}
	const DWORD error = GetLastError();
	// 部分插入失败只补抬自己按下的键，两种语言共用这段清理。
	INPUT releases[2] = {};
	UINT releaseCount = 0;
	if (sent > (ownModifier ? 1u : 0u)) {
		releases[releaseCount++] = inputs[2];
	}
	if (ownModifier && sent > 0) {
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
					const HWND target = GetForegroundWindow();
					const DWORD thread = GetWindowThreadProcessId(target, nullptr);
					const HKL layout = thread != 0 ? GetKeyboardLayout(thread) : nullptr;
					const WORD language = PRIMARYLANGID(LOWORD(layout));
					const bool alt = (pKeyboard->flags & LLKHF_ALTDOWN) != 0;
					// Alt+CapsLock 放行给原来的大写锁定。这一条可以在托盘菜单和设置页里
					// 关掉；关掉之后 Alt 不再特殊，Alt+CapsLock 就和 CapsLock 一样切换输入法。
					const bool altPass = alt && g_altPassThrough;
					// 只在这里按语言分流；其它语言保持原键位，直到整次物理按键结束。
					g_capsSwallowed = g_enabled && !altPass &&
					    (language == LANG_CHINESE || language == LANG_JAPANESE);
					g_capsPassReason = !g_enabled ? kCapsPassedMappingOff
					                  : altPass ? kCapsPassedAlt
					                  : kCapsPassedOtherLanguage;
					if (g_capsSwallowed) {
						// 目标窗口按下的那一刻就定下来了：排队中的请求不许切换到一个新应用。
						g_capsSwallowed = PostMessageW(g_mainWnd, WM_SWITCH_IME,
						    reinterpret_cast<WPARAM>(target), reinterpret_cast<LPARAM>(layout)) != FALSE;
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
				// 这一次按键原样放行了（按着 Alt、映射被关掉，或者前台不是中/日输入法），
				// 系统会把它那边的大写锁定翻个个儿。回调里不能写日志（文件 I/O 会拖长
				// 钩子回调），而且此刻按键还没被系统处理，所以等到抬起再通知主线程：
				// 既避开回调里的 I/O，又保证系统那边已经翻完了。
				g_capsLockOn = !g_capsLockOn;
				PostMessageW(g_mainWnd, WM_CAPS_LOCK_PASSED, g_capsPassReason,
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
