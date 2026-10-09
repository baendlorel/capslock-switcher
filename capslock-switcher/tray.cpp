#include "tray.h"

#include "app.h"
#include "logging.h"
#include "resource.h"
#include "settings.h"
#include "version.h"

#include <cstdio>
#include <string>
#include <winnls.h>

bool g_trayIconAdded = false;

namespace {

// 悬停提示：不打开菜单也能看出映射开着还是关着。
constexpr wchar_t kTooltipOn[] = L"CapsLock Switcher - \u5DF2\u542F\u7528";
constexpr wchar_t kTooltipOff[] = L"CapsLock Switcher - \u5DF2\u7981\u7528";

// 托盘菜单命令 ID。
constexpr UINT kMenuIdSettings = 1;
constexpr UINT kMenuIdExit = 2;

NOTIFYICONDATA g_nid = {};

std::wstring GetCurrentInputMethodName() {
	HKL hkl = GetKeyboardLayout(0);
	if (hkl == nullptr) {
		return L"Unknown";
	}

	LANGID langId = LOWORD(hkl);
	wchar_t buffer[256] = {};

	if (GetLocaleInfoW(langId, LOCALE_SNATIVELANGNAME, buffer, std::size(buffer)) > 0) {
		return buffer;
	}

	if (GetLocaleInfoW(langId, LOCALE_SLANGUAGE, buffer, std::size(buffer)) > 0) {
		return buffer;
	}

	return L"Unknown";
}

void FillTrayData(const HWND hwnd) {
	g_nid.cbSize = sizeof(NOTIFYICONDATA);
	g_nid.hWnd = hwnd;
	g_nid.uID = 1;
	g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	g_nid.uCallbackMessage = WM_TRAYICON;
	if (g_nid.hIcon == nullptr) {
		// 宁可退回到系统自带图标，也不给外壳传一个 NULL HICON。
		g_nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_MAINICON));
		if (g_nid.hIcon == nullptr) {
			g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
		}
	}
	wcscpy_s(g_nid.szTip, g_enabled ? kTooltipOn : kTooltipOff);
}

// 让悬停提示跟着开关走，不打开菜单也能看出状态。
// 图标每次（重新）挂上时也会用到。
void UpdateTrayTooltip() {
	if (!g_trayIconAdded) {
		return;
	}
	g_nid.uFlags = NIF_TIP;
	wcscpy_s(g_nid.szTip, g_enabled ? kTooltipOn : kTooltipOff);
	Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

}  // 匿名命名空间

bool AddTrayIcon(const HWND hwnd) {
	if (g_trayIconAdded) {
		return true;
	}
	FillTrayData(hwnd);
	g_trayIconAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
	return g_trayIconAdded;
}

// 就地刷新图标，外壳那边已经没有了就重新注册。先走 NIM_MODIFY 才能让
// g_trayIconAdded 保持可信：对一个本来就在的图标，无条件 NIM_ADD 会失败，
// 于是我们会误以为图标不在，退出时就永远不会去删它。
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
	wcscpy_s(g_nid.szInfoTitle, kAppTitle);
	wcscpy_s(g_nid.szInfo, text);
	Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void SetMappingEnabled(const bool enabled) {
	g_enabled = enabled;
	UpdateTrayTooltip();
	ShowBalloon(enabled ? L"CapsLock \u6620\u5C04\u5DF2\u542F\u7528"
	                    : L"CapsLock \u6620\u5C04\u5DF2\u7981\u7528");
}

void SetAltCapsLockEnabled(const bool enabled) {
	g_altPassThrough = enabled;
	ShowBalloon(enabled ? L"Alt+CapsLock\uFF1A\u653E\u884C\u5927\u5199\u9501\u5B9A" : L"Alt+CapsLock\uFF1A\u5DF2\u5173\u95ED");
}

// 右键菜单：开关都在设置页里，这里只留版本号、打开设置和退出。
void ShowTrayMenu(const HWND hwnd) {
	POINT pt;
	GetCursorPos(&pt);

	HMENU hMenu = CreatePopupMenu();
	if (hMenu == nullptr) {
		return;
	}

	wchar_t versionText[64] = {};
	swprintf_s(versionText, L"CapsLock Switcher v%hs", APP_VERSION);
	AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, versionText);
	AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

	std::wstring imeText = L"\u5F53\u524D\u8F93\u5165\u6CD5\uFF1A" + GetCurrentInputMethodName();
	AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, imeText.c_str());
	AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

	AppendMenuW(hMenu, MF_STRING, kMenuIdSettings, L"\u8BBE\u7F6E...");
	AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(hMenu, MF_STRING, kMenuIdExit, L"Exit");

	SetForegroundWindow(hwnd);

	const UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);

	// TrackPopupMenu 之后文档要求这么做，否则菜单可能一直卡在屏幕上，
	// 直到用户点到别处才消失。
	PostMessageW(hwnd, WM_NULL, 0, 0);
	DestroyMenu(hMenu);

	if (cmd == kMenuIdSettings) {
		OpenSettingsWindow();
	} else if (cmd == kMenuIdExit) {
		PostMessageW(hwnd, WM_CLOSE, 0, 0);
	}
}