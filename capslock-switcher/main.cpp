#include <Windows.h>
#include <cstdio>
#include <cwchar>
#include "version.h"
#include "resource.h"

namespace {

constexpr wchar_t kWindowClass[] = L"CapsLockSwitcherClass";
constexpr wchar_t kWindowTitle[] = L"CapsLock Switcher";
constexpr wchar_t kMutexName[] = L"CapsLockSwitcherMutex";

// Registered (system-wide) message, so a second launch can talk to the instance
// that already owns the mutex.
constexpr wchar_t kShowYourselfMessage[] = L"CapsLockSwitcher.ShowYourself";

// Application-private messages live above WM_APP. WM_USER is reserved for the
// window class itself, so keep out of that range.
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_SWITCH_IME = WM_APP + 2;

constexpr UINT_PTR kTimerRetryStartup = 1;
constexpr UINT_PTR kTimerRearmHook = 2;

constexpr UINT kRetryStartupMs = 2000;

// Windows removes a low-level hook whose callback takes longer than
// LowLevelHooksTimeout, and on Windows 7 and later it does so silently - there is
// no notification and no API to ask whether our hook is still installed. Re-arming
// on a slow timer is the only way to keep CapsLock working through a long session.
constexpr UINT kRearmHookMs = 60000;

HINSTANCE g_hInst = nullptr;
HWND g_hWnd = nullptr;
HHOOK g_hKeyboardHook = nullptr;
NOTIFYICONDATA g_nid = {};
bool g_trayIconAdded = false;
UINT g_taskbarCreatedMessage = 0;
UINT g_showYourselfMessage = 0;

void SendCtrlSpace() {
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

	SendInput(4, inputs, sizeof(INPUT));
}

LRESULT CALLBACK LowLevelKeyboardProc(const int nCode, const WPARAM wParam, const LPARAM lParam) {
	if (nCode == HC_ACTION) {
		const auto* pKeyboard = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

		if (pKeyboard->vkCode == VK_CAPITAL) {
			if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
				// This callback must return as fast as possible. Injecting the
				// combination right here used to be the reason CapsLock eventually
				// stopped working: SendInput can block, the callback then blows the
				// hook timeout, and Windows throws the hook away without telling us.
				// Hand the work to the message loop instead.
				PostMessageW(g_hWnd, WM_SWITCH_IME, 0, 0);
				return 1;
			}
			if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
				return 1;
			}
		}
	}

	return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

bool InstallHook() {
	if (g_hKeyboardHook != nullptr) {
		return true;
	}
	g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_hInst, 0);
	return g_hKeyboardHook != nullptr;
}

void UninstallHook() {
	if (g_hKeyboardHook != nullptr) {
		UnhookWindowsHookEx(g_hKeyboardHook);
		g_hKeyboardHook = nullptr;
	}
}

void FillTrayData(const HWND hwnd) {
	g_nid.cbSize = sizeof(NOTIFYICONDATA);
	g_nid.hWnd = hwnd;
	g_nid.uID = 1;
	g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	g_nid.uCallbackMessage = WM_TRAYICON;
	if (g_nid.hIcon == nullptr) {
		// Fall back to a stock icon rather than handing the shell a NULL HICON.
		g_nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_MAINICON));
		if (g_nid.hIcon == nullptr) {
			g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
		}
	}
	wcscpy_s(g_nid.szTip, kWindowTitle);
}

bool AddTrayIcon(const HWND hwnd) {
	if (g_trayIconAdded) {
		return true;
	}
	FillTrayData(hwnd);
	g_trayIconAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
	return g_trayIconAdded;
}

// Refreshes the icon in place, and re-registers it when the shell no longer has
// it. Going through NIM_MODIFY first is what keeps g_trayIconAdded honest: an
// unconditional NIM_ADD would fail for an icon that is already there and leave us
// believing it is missing, so the icon would never be removed on exit.
void RefreshTrayIcon(const HWND hwnd) {
	FillTrayData(hwnd);
	if (!g_trayIconAdded || !Shell_NotifyIconW(NIM_MODIFY, &g_nid)) {
		g_trayIconAdded = false;
		AddTrayIcon(hwnd);
	}
}

void RemoveTrayIcon() {
	if (g_trayIconAdded) {
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		g_trayIconAdded = false;
	}
}

void ShowBalloon(const wchar_t* text) {
	if (!g_trayIconAdded) {
		return;
	}
	g_nid.uFlags = NIF_INFO;
	g_nid.dwInfoFlags = NIIF_INFO;
	wcscpy_s(g_nid.szInfoTitle, kWindowTitle);
	wcscpy_s(g_nid.szInfo, text);
	Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// Brings up the hook and the tray icon, retrying on a timer while either is
// missing. Neither failure is fatal any more: the shell is regularly not ready
// for a tray icon yet when a startup entry runs at logon, and quitting at that
// point is what made the program appear to start and vanish.
void EnsureInstalled(const HWND hwnd) {
	const bool hookReady = InstallHook();
	const bool trayReady = AddTrayIcon(hwnd);

	if (hookReady && trayReady) {
		KillTimer(hwnd, kTimerRetryStartup);
	}
}

void ReinstallHook(const HWND hwnd) {
	// Install the replacement before dropping the old hook. Either order of the
	// two hooks swallows CapsLock and returns 1, so exactly one Ctrl+Space is sent
	// and there is never a moment with no hook at all - a unhook-then-rehook gap
	// would let the odd CapsLock press through and toggle the real lock.
	const HHOOK previous = g_hKeyboardHook;
	g_hKeyboardHook = nullptr;

	if (InstallHook()) {
		if (previous != nullptr) {
			UnhookWindowsHookEx(previous);
		}
		return;
	}

	// Re-installing failed, so keep the hook we already had.
	g_hKeyboardHook = previous;
	SetTimer(hwnd, kTimerRetryStartup, kRetryStartupMs, nullptr);
}

void ShowContextMenu(const HWND hwnd) {
	POINT pt;
	GetCursorPos(&pt);

	HMENU hMenu = CreatePopupMenu();
	if (hMenu == nullptr) {
		return;
	}

	wchar_t versionText[64] = {};
	swprintf_s(versionText, L"CapsLock Switcher v%hs", APP_VERSION);
	AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, versionText);
	AppendMenuW(hMenu, MF_STRING, 1, L"Exit");

	SetForegroundWindow(hwnd);

	const UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);

	// Documented requirement after TrackPopupMenu, otherwise the menu can stay
	// stuck on screen until the user clicks somewhere else.
	PostMessageW(hwnd, WM_NULL, 0, 0);
	DestroyMenu(hMenu);

	if (cmd == 1) {
		PostMessageW(hwnd, WM_CLOSE, 0, 0);
	}
}

LRESULT CALLBACK WndProc(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam) {
	switch (message) {
	case WM_CREATE:
		g_hWnd = hwnd;  // the hook callback posts to this window
		EnsureInstalled(hwnd);
		SetTimer(hwnd, kTimerRearmHook, kRearmHookMs, nullptr);
		break;

	case WM_TIMER:
		if (wParam == kTimerRetryStartup) {
			EnsureInstalled(hwnd);
		} else if (wParam == kTimerRearmHook) {
			ReinstallHook(hwnd);
		}
		break;

	case WM_SWITCH_IME:
		SendCtrlSpace();
		break;

	case WM_TRAYICON:
		if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
			ShowContextMenu(hwnd);
		}
		break;

	case WM_DESTROY:
		KillTimer(hwnd, kTimerRetryStartup);
		KillTimer(hwnd, kTimerRearmHook);
		UninstallHook();
		RemoveTrayIcon();
		g_hWnd = nullptr;
		PostQuitMessage(0);
		break;

	default:
		if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage) {
			// Explorer restarted, and every tray icon went with it. Without this
			// the icon simply never came back and the running program became
			// invisible - no icon, no way to exit.
			g_trayIconAdded = false;
			AddTrayIcon(hwnd);
			break;
		}
		if (g_showYourselfMessage != 0 && message == g_showYourselfMessage) {
			// A second launch found us and asked us to make ourselves visible.
			RefreshTrayIcon(hwnd);
			ShowBalloon(L"Already running - the tray icon has been restored.");
			break;
		}
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}
	return 0;
}

}  // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
	g_showYourselfMessage = RegisterWindowMessageW(kShowYourselfMessage);

	HANDLE hMutex = CreateMutex(nullptr, TRUE, kMutexName);
	if (hMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
		// Ask the instance that is already running to put its tray icon back: it
		// may well be invisible after an Explorer restart, which is exactly when
		// users believe the program died and reach for the shortcut again.
		const HWND existing = FindWindowW(kWindowClass, kWindowTitle);
		if (existing != nullptr && g_showYourselfMessage != 0) {
			PostMessageW(existing, g_showYourselfMessage, 0, 0);
		}
		MessageBoxW(nullptr, L"CapsLock Switcher is already running.", kWindowTitle, MB_OK | MB_ICONINFORMATION);
		CloseHandle(hMutex);
		return 0;
	}

	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.lpfnWndProc = WndProc;
	wcex.hInstance = hInstance;
	wcex.lpszClassName = kWindowClass;

	if (!RegisterClassEx(&wcex)) {
		MessageBoxW(nullptr, L"Failed to register class", L"Error", MB_OK | MB_ICONERROR);
		if (hMutex != nullptr) {
			CloseHandle(hMutex);
		}
		return 1;
	}

	g_hWnd = CreateWindowEx(
		0,
		kWindowClass,
		kWindowTitle,
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
		nullptr,
		nullptr,
		hInstance,
		nullptr
	);

	if (g_hWnd == nullptr) {
		MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
		if (hMutex != nullptr) {
			CloseHandle(hMutex);
		}
		return 1;
	}

	MSG msg = {};
	BOOL result = 0;
	// GetMessage returns -1 on error, which is non-zero and would therefore look
	// like success; the MSG would stay uninitialised and dispatching it is what
	// can take the process down. Check for it explicitly.
	while ((result = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
		if (result == -1) {
			break;
		}
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	if (hMutex != nullptr) {
		ReleaseMutex(hMutex);
		CloseHandle(hMutex);
	}

	return result == -1 ? 1 : static_cast<int>(msg.wParam);
}
