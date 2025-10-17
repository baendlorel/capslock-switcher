#include <Windows.h>
#include <string>
#include "resource.h"

HINSTANCE g_hInst = nullptr;
HWND g_hWnd = nullptr;
HHOOK g_hKeyboardHook = nullptr;
NOTIFYICONDATA g_nid = {};
const UINT WM_TRAYICON = WM_USER + 1;

LRESULT CALLBACK LowLevelKeyboardProc(const int nCode, const WPARAM wParam, const LPARAM lParam) {
	if (nCode == HC_ACTION) {
		KBDLLHOOKSTRUCT* pKeyboard = (KBDLLHOOKSTRUCT*)lParam;

		if (pKeyboard->vkCode == VK_CAPITAL) {
			if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
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
				return 1;
			}
			else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
				return 1;
			}
		}
	}

	return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

bool InstallHook() {
	g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_hInst, 0);
	return g_hKeyboardHook != nullptr;
}

void UninstallHook() {
	if (g_hKeyboardHook) {
		UnhookWindowsHookEx(g_hKeyboardHook);
		g_hKeyboardHook = nullptr;
	}
}

bool AddTrayIcon(const HWND hwnd) {
	g_nid.cbSize = sizeof(NOTIFYICONDATA);
	g_nid.hWnd = hwnd;
	g_nid.uID = 1;
	g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	g_nid.uCallbackMessage = WM_TRAYICON;
	g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	wcscpy_s(g_nid.szTip, L"CapsLock Switcher");

	return Shell_NotifyIcon(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
	Shell_NotifyIcon(NIM_DELETE, &g_nid);
}

void ShowContextMenu(const HWND hwnd) {
	POINT pt;
	GetCursorPos(&pt);

	HMENU hMenu = CreatePopupMenu();
	AppendMenu(hMenu, MF_STRING, 1, L"Exit");

	SetForegroundWindow(hwnd);

	UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);

	if (cmd == 1) {
		PostMessage(hwnd, WM_CLOSE, 0, 0);
	}

	DestroyMenu(hMenu);
}

LRESULT CALLBACK WndProc(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam) {
	switch (message) {
	case WM_CREATE:
		if (!InstallHook()) {
			MessageBox(hwnd, L"Failed to install hook", L"Error", MB_OK | MB_ICONERROR);
			return -1;
		}
		if (!AddTrayIcon(hwnd)) {
			MessageBox(hwnd, L"Failed to add tray icon", L"Error", MB_OK | MB_ICONERROR);
			return -1;
		}
		break;

	case WM_TRAYICON:
		if (lParam == WM_RBUTTONUP) {
			ShowContextMenu(hwnd);
		}
		break;

	case WM_DESTROY:
		UninstallHook();
		RemoveTrayIcon();
		PostQuitMessage(0);
		break;

	default:
		return DefWindowProc(hwnd, message, wParam, lParam);
	}
	return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	g_hInst = hInstance;

	HANDLE hMutex = CreateMutex(nullptr, TRUE, L"CapsLockSwitcherMutex");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		MessageBox(nullptr, L"Already running", L"Info", MB_OK | MB_ICONINFORMATION);
		return 0;
	}

	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.lpfnWndProc = WndProc;
	wcex.hInstance = hInstance;
	wcex.lpszClassName = L"CapsLockSwitcherClass";

	if (!RegisterClassEx(&wcex)) {
		MessageBox(nullptr, L"Failed to register class", L"Error", MB_OK | MB_ICONERROR);
		return 1;
	}

	g_hWnd = CreateWindowEx(
		0,
		L"CapsLockSwitcherClass",
		L"CapsLock Switcher",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
		nullptr,
		nullptr,
		hInstance,
		nullptr
	);

	if (!g_hWnd) {
		MessageBox(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
		return 1;
	}

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	if (hMutex) {
		ReleaseMutex(hMutex);
		CloseHandle(hMutex);
	}

	return static_cast<int>(msg.wParam);
}
