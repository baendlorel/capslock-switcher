#define NOMINMAX
#include <Windows.h>
#include <imm.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include "version.h"
#include "resource.h"

// imm32 for the IME state query, gdiplus for decoding the splash PNG. Declared
// here so the project file needs no edit.
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"CapsLockSwitcherClass";
constexpr wchar_t kWindowTitle[] = L"CapsLock Switcher";
constexpr wchar_t kMutexName[] = L"CapsLockSwitcherMutex";
constexpr wchar_t kOverlayClass[] = L"CapsLockSwitcherOverlay";

// Non-English text below is written as \u escapes on purpose: this file is kept pure
// ASCII so the compiler never has to guess a source code page. A BOM-less UTF-8 file
// is decoded as the system code page, and a multi-byte character then swallows the
// closing quote of the literal it sits in. For reference, in order of appearance:
//   \u5DF2\u542F\u7528 = "enabled", \u5DF2\u7981\u7528 = "disabled",
//   \u6620\u5C04 = "mapping", \u542F\u7528 = "enable",
//   \u4E2D = the Chinese glyph the overlay shows.
constexpr wchar_t kTooltipOn[] = L"CapsLock Switcher - \u5DF2\u542F\u7528";
constexpr wchar_t kTooltipOff[] = L"CapsLock Switcher - \u5DF2\u7981\u7528";

// Registered (system-wide) message, so a second launch can talk to the instance
// that already owns the mutex.
constexpr wchar_t kShowYourselfMessage[] = L"CapsLockSwitcher.ShowYourself";

// Application-private messages live above WM_APP. WM_USER is reserved for the
// window class itself, so keep out of that range.
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_SWITCH_IME = WM_APP + 2;

// Tray menu command ids.
constexpr UINT kMenuIdToggleMapping = 1;
constexpr UINT kMenuIdToggleStartup = 2;
constexpr UINT kMenuIdExit = 3;

constexpr UINT_PTR kTimerRetryStartup = 1;
constexpr UINT_PTR kTimerSelfHeal = 2;
constexpr UINT_PTR kTimerShowImeState = 3;
constexpr UINT_PTR kTimerOverlayFade = 4;
constexpr UINT_PTR kTimerSplashFade = 5;
constexpr UINT_PTR kTimerRefreshStartup = 6;

constexpr UINT kRetryStartupMs = 2000;

// Windows removes a low-level hook whose callback takes longer than
// LowLevelHooksTimeout, and on Windows 7 and later it does so silently - there is
// no notification and no API to ask whether our hook is still installed. It also
// gives no notice when a tray icon is dropped. Re-asserting both on a slow timer is
// the only way to keep working through a long session.
constexpr UINT kSelfHealMs = 60000;

// The injected Ctrl+Space needs a moment to be registered by the target window and
// its IME before asking which mode it landed in.
constexpr UINT kImeQueryDelayMs = 50;

// Overlay: shown centred, held, then faded out. Sizes are in DIPs and get scaled.
constexpr int kOverlaySizeDip = 132;
constexpr int kOverlayTextDip = 62;
// Fully opaque so the two state colours show up exactly as specified; the fade-out
// still animates this value down from here.
constexpr int kOverlayAlpha = 255;
constexpr int kOverlayFadeStep = 22;
constexpr UINT kOverlayHoldMs = 500;
constexpr UINT kOverlayFadeStepMs = 16;
constexpr wchar_t kOverlayFontFace[] = L"Microsoft YaHei UI";

// Overlay colours: blue while the IME is English, red while it is Chinese.
constexpr COLORREF kOverlayEnglishColor = RGB(0x00, 0x73, 0xFF);  // #0073FF
constexpr COLORREF kOverlayChineseColor = RGB(0xFF, 0x1F, 0x45);  // #FF1F45
constexpr COLORREF kOverlayFrameColor = RGB(0xFF, 0xFF, 0xFF);

// Startup splash: the keycap image, held for a moment and then faded out. It needs
// its own window because it has per-pixel alpha, which rules out the single
// window-wide alpha the overlay uses.
constexpr wchar_t kSplashClass[] = L"CapsLockSwitcherSplash";
constexpr int kSplashAlphaMax = 255;
constexpr int kSplashFadeStep = 6;  // alpha units per tick
constexpr UINT kSplashHoldMs = 1400;
constexpr UINT kSplashFadeStepMs = 16;
constexpr int kSplashMaxHeightPercent = 30;  // of the work area height; never upscaled

// Autostart: a logon task that the task scheduler runs with highest privileges, so
// logging on does not raise a UAC prompt. Creating it does need administrator
// rights, which is why the toggle re-launches this program elevated.
constexpr wchar_t kStartupTaskName[] = L"CapsLock Switcher";
constexpr wchar_t kCommandStartupEnable[] = L"--startup-enable";
constexpr wchar_t kCommandStartupDisable[] = L"--startup-disable";
constexpr UINT kStartupRefreshDelayMs = 3000;

// IMC_GETCONVERSIONMODE, sent to the IME window through WM_IME_CONTROL. It is the
// GET counterpart of IMC_SETCONVERSIONMODE (0x0002), which immdev.h documents;
// imm.h does not declare the GET form.
constexpr WPARAM kImeGetConversionMode = 0x0001;

HINSTANCE g_hInst = nullptr;
HWND g_hWnd = nullptr;
HHOOK g_hKeyboardHook = nullptr;
NOTIFYICONDATA g_nid = {};
bool g_trayIconAdded = false;
UINT g_taskbarCreatedMessage = 0;
UINT g_showYourselfMessage = 0;

// The switch the tray menu drives. While it is off the hook lets CapsLock through
// untouched, so the key behaves exactly like a normal CapsLock again.
bool g_enabled = true;

HWND g_overlayWnd = nullptr;
wchar_t g_overlayText[8] = {};
// Which of the two states the overlay is currently showing, so WM_PAINT can pick
// the matching background colour.
bool g_overlayChinese = false;
int g_overlayAlpha = 0;
int g_overlayTextHeight = kOverlayTextDip;
UINT g_overlayHoldTicks = 0;

// Last state we actually managed to read, so a failed query reuses it instead of
// flashing a wrong answer.
bool g_lastImeWasChinese = false;

// Startup splash window and the premultiplied ARGB surface it draws.
HWND g_splashWnd = nullptr;
HBITMAP g_splashBitmap = nullptr;
HDC g_splashMemDc = nullptr;
int g_splashWidth = 0;
int g_splashHeight = 0;
int g_splashAlpha = 0;
UINT g_splashHoldTicks = 0;

// Cached answer to "is the logon task installed?", refreshed at startup, after a
// change, and on the slow self-heal tick.
bool g_startupTaskInstalled = false;

ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusReady = false;

wchar_t g_exePath[1024] = {};
wchar_t g_logPath[1024] = {};
ULONGLONG g_startTick = 0;
bool g_installTroubleLogged = false;
ULONGLONG g_installTroubleTick = 0;
unsigned g_installAttempts = 0;

// ---------------------------------------------------------------------------
// Diagnostics
//
// A log that cannot be written must not stop the program from starting or from
// shutting down cleanly, so every failure in here is swallowed deliberately.
// The file sits next to the executable and gets a UTF-8 byte order mark the
// first time it is created.
// ---------------------------------------------------------------------------

void AppendLogLine(const wchar_t* line) {
	if (g_logPath[0] == L'\0' || line == nullptr) {
		return;
	}

	// GENERIC_WRITE also carries the attribute access GetFileSizeEx needs.
	const HANDLE file = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
	                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}

	LARGE_INTEGER size = {};
	if (GetFileSizeEx(file, &size) && size.QuadPart == 0) {
		static const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
		DWORD written = 0;
		WriteFile(file, bom, sizeof(bom), &written, nullptr);
	} else {
		SetFilePointer(file, 0, nullptr, FILE_END);
	}

	char utf8[4096] = {};
	const int chars = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8,
	                                      static_cast<int>(sizeof(utf8)), nullptr, nullptr);
	if (chars > 1) {
		DWORD written = 0;
		// chars includes the terminating NUL, which does not belong in the file.
		WriteFile(file, utf8, static_cast<DWORD>(chars - 1), &written, nullptr);
	}

	CloseHandle(file);
}

// Records the executable directory and derives the log path from it.
void BuildPaths() {
	const DWORD length = GetModuleFileNameW(nullptr, g_exePath, _countof(g_exePath));
	if (length == 0 || length >= _countof(g_exePath)) {
		g_exePath[0] = L'\0';
		return;
	}

	wcscpy_s(g_logPath, g_exePath);
	wchar_t* slash = wcsrchr(g_logPath, L'\\');
	if (slash == nullptr) {
		g_logPath[0] = L'\0';
		return;
	}
	*(slash + 1) = L'\0';
	wcscat_s(g_logPath, L"capslock-switcher.log");
}

void Log(const wchar_t* format, ...) {
	wchar_t body[1024] = {};
	va_list args;
	va_start(args, format);
	_vsnwprintf_s(body, _countof(body), _TRUNCATE, format, args);
	va_end(args);

	SYSTEMTIME now = {};
	GetLocalTime(&now);

	wchar_t line[1200] = {};
	swprintf_s(line, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
	           now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
	           now.wSecond, now.wMilliseconds, body);

	AppendLogLine(line);
}

const wchar_t* ErrorText(const DWORD code, wchar_t (&buffer)[512]) {
	if (code == ERROR_SUCCESS) {
		wcscpy_s(buffer, L"(GetLastError() had no detail to add)");
		return buffer;
	}
	if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
	                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer, 512, nullptr) == 0) {
		wcscpy_s(buffer, L"(no system text for this code)");
		return buffer;
	}
	// FormatMessage leaves a trailing CR/LF (and sometimes a full stop plus space).
	for (size_t len = wcslen(buffer); len > 0; --len) {
		const wchar_t c = buffer[len - 1];
		if (c == L'\r' || c == L'\n' || c == L' ' || c == L'\t' || c == L'.') {
			buffer[len - 1] = L'\0';
		} else {
			break;
		}
	}
	return buffer;
}

void LogEnvironment() {
	wchar_t product[128] = L"(unknown)";
	wchar_t build[64] = L"(unknown)";
	wchar_t release[64] = L"";
	DWORD bytes = sizeof(product);
	RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
	             L"ProductName", RRF_RT_REG_SZ, nullptr, product, &bytes);
	bytes = sizeof(build);
	RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
	             L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, build, &bytes);
	bytes = sizeof(release);
	RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
	             L"DisplayVersion", RRF_RT_REG_SZ, nullptr, release, &bytes);

	Log(L"version         : %hs (%s)", APP_VERSION, sizeof(void*) == 8 ? L"x64" : L"x86");
	Log(L"executable      : %s", g_exePath);
	Log(L"log file        : %s", g_logPath);
	// ProductName in the registry still reads "Windows 10" on Windows 11, so take
	// the family from the build number and keep the registry strings as detail.
	const unsigned long buildNumber = wcstoul(build, nullptr, 10);
	const wchar_t* family = buildNumber >= 22000 ? L"Windows 11"
	                      : buildNumber >= 10240 ? L"Windows 10"
	                                             : L"Windows";
	Log(L"os              : %s (build %s), registry says \"%s %s\"", family, build, product, release);
	Log(L"process/thread  : pid=%lu tid=%lu", GetCurrentProcessId(), GetCurrentThreadId());
	Log(L"uptime          : %llu ms since start", GetTickCount64() - g_startTick);
}

// The message loop returned -1. Note that GetMessageW leaves MSG untouched in that
// case, so there is nothing to dispatch - what follows is everything the process
// can still observe about why it happened.
void LogMessageLoopFailure(const DWORD lastError) {
	wchar_t text[512] = {};

	Log(L"===== GetMessageW returned -1, shutting down =====");
	LogEnvironment();
	Log(L"GetMessageW     : -1 (documented causes: invalid hWnd or invalid lpMsg)");
	Log(L"GetLastError    : %lu (0x%08lX) %s", lastError, lastError, ErrorText(lastError, text));
	Log(L"queue status    : 0x%08lX", GetQueueStatus(QS_ALLINPUT));
	Log(L"hidden window   : hwnd=0x%p IsWindow=%d visible=%d",
	    static_cast<void*>(g_hWnd), IsWindow(g_hWnd) != FALSE, IsWindowVisible(g_hWnd) != FALSE);
	Log(L"keyboard hook   : installed=%d handle=0x%p",
	    g_hKeyboardHook != nullptr, static_cast<void*>(g_hKeyboardHook));
	Log(L"tray icon       : registered=%d", g_trayIconAdded != FALSE);
	Log(L"message         : MSG was left untouched, so nothing was dispatched");
}

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

		// With the switch off, CapsLock falls through to the next hook and behaves
		// like an ordinary CapsLock - no need to uninstall anything.
		if (g_enabled && pKeyboard->vkCode == VK_CAPITAL) {
			// Alt+CapsLock is the way back to the original key: let the event travel
			// on untouched so the system performs the real lock toggle itself. No
			// overlay here either, because nothing was switched.
			if ((GetAsyncKeyState(VK_LMENU) & 0x8000) != 0 ||
			    (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0) {
				return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
			}

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
	wcscpy_s(g_nid.szTip, g_enabled ? kTooltipOn : kTooltipOff);
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

// Keeps the hover text in step with the switch, so the state is visible without
// opening the menu. Also gets used whenever the icon is (re)added.
void UpdateTrayTooltip() {
	if (!g_trayIconAdded) {
		return;
	}
	g_nid.uFlags = NIF_TIP;
	wcscpy_s(g_nid.szTip, g_enabled ? kTooltipOn : kTooltipOff);
	Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void SetMappingEnabled(const bool enabled) {
	g_enabled = enabled;
	UpdateTrayTooltip();
	ShowBalloon(enabled ? L"CapsLock \u6620\u5C04\u5DF2\u542F\u7528" : L"CapsLock \u6620\u5C04\u5DF2\u7981\u7528");
}

// ---------------------------------------------------------------------------
// Autostart
//
// "Start at logon, with administrator rights, without a UAC prompt" is exactly a
// scheduled task with the highest run level - a shortcut in the Startup folder
// could not do the elevated part. Creating that task needs administrator rights
// itself, so the menu item re-launches this program through the UAC prompt with a
// switch that tells it to do just this job and exit.
// ---------------------------------------------------------------------------

bool IsProcessElevated() {
	HANDLE token = nullptr;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
		return false;
	}
	TOKEN_ELEVATION elevation = {};
	DWORD returned = 0;
	const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
	                                    sizeof(elevation), &returned) != FALSE;
	CloseHandle(token);
	return ok && elevation.TokenIsElevated != 0;
}

// Runs a command line and returns its exit code (-1 when it could not be started).
// The output is discarded on purpose: only the exit status matters, which keeps
// this independent of the display language.
DWORD RunAndWait(const wchar_t* commandLine) {
	wchar_t mutableLine[2048] = {};
	wcscpy_s(mutableLine, commandLine);

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};

	if (CreateProcessW(nullptr, mutableLine, nullptr, nullptr, FALSE,
	                   CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) == FALSE) {
		return static_cast<DWORD>(-1);
	}

	DWORD exitCode = static_cast<DWORD>(-1);
	WaitForSingleObject(process.hProcess, 20000);
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hProcess);
	CloseHandle(process.hThread);
	return exitCode;
}

const wchar_t* SchtasksPath() {
	static wchar_t path[MAX_PATH] = {};
	if (path[0] == L'\0') {
		wchar_t windows[MAX_PATH] = {};
		if (GetWindowsDirectoryW(windows, MAX_PATH) != 0) {
			swprintf_s(path, L"%s\\System32\\schtasks.exe", windows);
			if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
				wcscpy_s(path, L"schtasks.exe");  // fall back to the PATH
			}
		} else {
			wcscpy_s(path, L"schtasks.exe");
		}
	}
	return path;
}

// schtasks reports "exit code 0 == the task exists" whatever the display language,
// and querying needs no elevation.
bool QueryStartupTask() {
	wchar_t command[1024] = {};
	swprintf_s(command, L"\"%s\" /Query /TN \"%s\"", SchtasksPath(), kStartupTaskName);
	return RunAndWait(command) == 0;
}

bool InstallStartupTask() {
	wchar_t command[2048] = {};
	// /RL HIGHEST is what makes the scheduler start it elevated, so logging on stays
	// prompt-free.
	swprintf_s(command,
	           L"\"%s\" /Create /TN \"%s\" /TR \"\\\"%s\\\"\" /SC ONLOGON /RL HIGHEST /F",
	           SchtasksPath(), kStartupTaskName, g_exePath);
	const DWORD code = RunAndWait(command);
	Log(L"startup task create: schtasks exit code %lu for \"%s\"", code, g_exePath);
	return code == 0;
}

bool RemoveStartupTask() {
	wchar_t command[1024] = {};
	swprintf_s(command, L"\"%s\" /Delete /TN \"%s\" /F", SchtasksPath(), kStartupTaskName);
	const DWORD code = RunAndWait(command);
	Log(L"startup task delete: schtasks exit code %lu", code);
	return code == 0;
}

void SetStartupEnabled(const bool enable) {
	if (IsProcessElevated()) {
		const bool ok = enable ? InstallStartupTask() : RemoveStartupTask();
		g_startupTaskInstalled = ok && enable;
		if (ok) {
			ShowBalloon(enable ? L"\u5DF2\u5F00\u542F\u5F00\u673A\u542F\u52A8"
			                   : L"\u5DF2\u5173\u95ED\u5F00\u673A\u542F\u52A8");
		} else {
			ShowBalloon(L"\u5F00\u673A\u542F\u52A8\u8BBE\u7F6E\u5931\u8D25\uFF0C\u8BE6\u89C1\u65E5\u5FD7");
		}
		return;
	}

	// Not elevated, so hand the job to an elevated copy of ourselves. That is the
	// UAC prompt the administrator requirement implies.
	const HINSTANCE launched = ShellExecuteW(nullptr, L"runas", g_exePath,
	                                        enable ? kCommandStartupEnable : kCommandStartupDisable,
	                                        nullptr, SW_HIDE);
	if (reinterpret_cast<INT_PTR>(launched) <= 32) {
		Log(L"elevation refused or failed (ShellExecuteW returned %lld)",
		    static_cast<long long>(reinterpret_cast<INT_PTR>(launched)));
		ShowBalloon(L"\u9700\u8981\u7BA1\u7406\u5458\u6743\u9650");
		return;
	}

	// The helper runs asynchronously, so look at the state again shortly.
	SetTimer(g_hWnd, kTimerRefreshStartup, kStartupRefreshDelayMs, nullptr);
}

// The elevated helper: do the job, then leave without ever creating a window.
int RunStartupHelper(const bool enable) {
	g_startTick = GetTickCount64();
	BuildPaths();

	if (!IsProcessElevated()) {
		Log(L"startup helper started without administrator rights; refusing");
		return 1;
	}
	return (enable ? InstallStartupTask() : RemoveStartupTask()) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Input-method state overlay
// ---------------------------------------------------------------------------

// True when the foreground window's IME sits in a native (Chinese) input mode.
bool ForegroundImeIsChinese() {
	const HWND foreground = GetForegroundWindow();
	if (foreground == nullptr) {
		return g_lastImeWasChinese;
	}

	// Do NOT use ImmGetContext here. An IME context belongs to a thread's input
	// queue, so for a window owned by another thread - which is every window this
	// program ever inspects - it simply returns NULL, and every answer would come
	// out "English". The IME window answers across process boundaries instead.
	const HWND imeWnd = ImmGetDefaultIMEWnd(foreground);
	if (imeWnd == nullptr) {
		return g_lastImeWasChinese;
	}

	// kImeGetConversionMode is the GET counterpart of the IMC_SETCONVERSIONMODE
	// (0x0002) that immdev.h documents. It carries no pointer, so sending it into
	// another process is safe, and the timeout keeps a wedged target from stalling
	// the message loop.
	DWORD_PTR conversion = 0;
	if (SendMessageTimeoutW(imeWnd, WM_IME_CONTROL, kImeGetConversionMode, 0,
	                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &conversion) == 0) {
		return g_lastImeWasChinese;
	}

	// IME_CMODE_NATIVE is the bit Chinese, Japanese and Korean IMEs set while they
	// are in their native (non-alphanumeric) input mode; switching the IME to
	// English clears it (the mode becomes plain 0x0, or 0x401 while Chinese).
	g_lastImeWasChinese = (conversion & IME_CMODE_NATIVE) != 0;
	return g_lastImeWasChinese;
}

LRESULT CALLBACK OverlayProc(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam) {
	switch (message) {
	case WM_ERASEBKGND:
		return 1;  // WM_PAINT covers the entire client area

	case WM_PAINT: {
		PAINTSTRUCT ps = {};
		const HDC dc = BeginPaint(hwnd, &ps);
		if (dc != nullptr) {
			RECT rc = {};
			GetClientRect(hwnd, &rc);

			const HBRUSH background = CreateSolidBrush(
			    g_overlayChinese ? kOverlayChineseColor : kOverlayEnglishColor);
			FillRect(dc, &rc, background);
			DeleteObject(background);

			// A thin white frame keeps the block readable over any wallpaper.
			const HBRUSH frame = CreateSolidBrush(kOverlayFrameColor);
			FrameRect(dc, &rc, frame);
			DeleteObject(frame);

			const HFONT font = CreateFontW(-g_overlayTextHeight, 0, 0, 0, FW_SEMIBOLD,
			                               FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
			                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
			                               kOverlayFontFace);
			const HGDIOBJ previous = SelectObject(
			    dc, font != nullptr ? font : GetStockObject(DEFAULT_GUI_FONT));

			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, RGB(245, 245, 245));
			DrawTextW(dc, g_overlayText, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

			SelectObject(dc, previous);
			if (font != nullptr) {
				DeleteObject(font);
			}
		}
		EndPaint(hwnd, &ps);
		break;
	}

	default:
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}
	return 0;
}

void CreateOverlayWindow(const HINSTANCE instance) {
	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.lpfnWndProc = OverlayProc;
	wcex.hInstance = instance;
	wcex.lpszClassName = kOverlayClass;
	RegisterClassExW(&wcex);  // on failure there is simply no overlay

	// Layered for the fade; transparent and non-activating so it never takes a
	// click or the focus away from whatever the user is typing into.
	g_overlayWnd = CreateWindowExW(
	    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
	    kOverlayClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);

	if (g_overlayWnd != nullptr) {
		g_overlayAlpha = kOverlayAlpha;
		SetLayeredWindowAttributes(g_overlayWnd, 0, static_cast<BYTE>(g_overlayAlpha), LWA_ALPHA);
	}
}

void ShowImeStateOverlay() {
	if (g_overlayWnd == nullptr) {
		return;
	}

	g_overlayChinese = ForegroundImeIsChinese();
	wcscpy_s(g_overlayText, g_overlayChinese ? L"\u4E2D" : L"En");

	const HDC screen = GetDC(nullptr);
	const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
	if (screen != nullptr) {
		ReleaseDC(nullptr, screen);
	}
	const int size = MulDiv(kOverlaySizeDip, dpi, 96);
	g_overlayTextHeight = MulDiv(kOverlayTextDip, dpi, 96);

	// Centre on the monitor the user is actually working on.
	RECT area = {};
	MONITORINFO info = {};
	info.cbSize = sizeof(MONITORINFO);
	const HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
	if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
		area = info.rcWork;
	} else {
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
	}

	const int x = area.left + (((area.right - area.left) - size) / 2);
	const int y = area.top + (((area.bottom - area.top) - size) / 2);

	g_overlayAlpha = kOverlayAlpha;
	SetLayeredWindowAttributes(g_overlayWnd, 0, static_cast<BYTE>(g_overlayAlpha), LWA_ALPHA);
	SetWindowPos(g_overlayWnd, HWND_TOPMOST, x, y, size, size, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	InvalidateRect(g_overlayWnd, nullptr, TRUE);

	g_overlayHoldTicks = kOverlayHoldMs / kOverlayFadeStepMs;
	SetTimer(g_hWnd, kTimerOverlayFade, kOverlayFadeStepMs, nullptr);
}

// Brings up the hook and the tray icon, and keeps retrying on a short timer while
// either one is still missing. Neither failure is fatal any more: a startup entry
// regularly runs before the shell is ready, so SetWindowsHookEx can succeed while
// Shell_NotifyIcon fails, and the icon then has to be added a couple of seconds
// later. Quitting at that point is what used to make the program appear to start
// and vanish.
void EnsureInstalled(const HWND hwnd) {
	const bool hookReady = InstallHook();
	const bool trayReady = AddTrayIcon(hwnd);
	// Read it here, before anything else gets a chance to overwrite it.
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
		// This is the shape a startup entry sees at logon: the hook goes in fine
		// while the shell is not ready to accept a tray icon yet.
		Log(L"startup install incomplete: hook=%d tray=%d, retrying every %u ms",
		    hookReady ? 1 : 0, trayReady ? 1 : 0, kRetryStartupMs);
		Log(L"failure detail  : last error %lu (0x%08lX) %s",
		    failureCode, failureCode, ErrorText(failureCode, text));
	}
	SetTimer(hwnd, kTimerRetryStartup, kRetryStartupMs, nullptr);
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

// Runs once a minute. Windows drops a low-level hook that times out without saying
// so, and the shell drops tray icons whenever Explorer restarts, so both are
// re-asserted here. Together with the fast retry loop and the TaskbarCreated
// handler, this covers either start order between this program and the shell.
void SelfHeal(const HWND hwnd) {
	const bool trayWasRegistered = g_trayIconAdded;

	ReinstallHook(hwnd);
	RefreshTrayIcon(hwnd);

	if (!trayWasRegistered && g_trayIconAdded) {
		// The shell had dropped our icon - typically an Explorer restart whose
		// TaskbarCreated broadcast we missed. Worth a line, because from the user's
		// side this is exactly "the tray icon disappeared".
		Log(L"tray icon was missing and has been re-registered on the self-heal tick");
	}

	if (g_hKeyboardHook == nullptr || !g_trayIconAdded) {
		SetTimer(hwnd, kTimerRetryStartup, kRetryStartupMs, nullptr);
	}
}

// ---------------------------------------------------------------------------
// Startup splash
//
// The keycap image has real transparency, so it cannot use the overlay window:
// that one applies a single alpha to the whole window. UpdateLayeredWindow takes a
// premultiplied ARGB surface instead, which is why this has its own window and its
// own DIB. Fading then means re-uploading the same surface with a smaller constant
// alpha, which costs no re-decoding.
// ---------------------------------------------------------------------------

void ReleaseSplash() {
	if (g_splashMemDc != nullptr) {
		DeleteDC(g_splashMemDc);
		g_splashMemDc = nullptr;
	}
	if (g_splashBitmap != nullptr) {
		DeleteObject(g_splashBitmap);
		g_splashBitmap = nullptr;
	}
}

bool PrepareSplash() {
	const HRSRC resource = FindResourceW(g_hInst, MAKEINTRESOURCEW(IDR_SPLASH_IMAGE), RT_RCDATA);
	if (resource == nullptr) {
		Log(L"splash: embedded PNG resource not found");
		return false;
	}
	const DWORD size = SizeofResource(g_hInst, resource);
	const HGLOBAL loaded = LoadResource(g_hInst, resource);
	const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
	if (bytes == nullptr || size == 0) {
		Log(L"splash: could not read the embedded PNG");
		return false;
	}

	HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
	if (copy == nullptr) {
		return false;
	}
	void* destination = GlobalLock(copy);
	if (destination == nullptr) {
		GlobalFree(copy);
		return false;
	}
	memcpy(destination, bytes, size);
	GlobalUnlock(copy);

	IStream* stream = nullptr;
	if (CreateStreamOnHGlobal(copy, TRUE, &stream) != S_OK) {
		GlobalFree(copy);
		return false;
	}

	Gdiplus::Image* image = Gdiplus::Image::FromStream(stream);
	stream->Release();
	if (image == nullptr) {
		return false;
	}
	if (image->GetLastStatus() != Gdiplus::Ok) {
		Log(L"splash: GDI+ could not decode the PNG");
		delete image;
		return false;
	}

	// Fit the work area, but never enlarge: the source is already big enough and
	// scaling it up would only blur it.
	RECT area = {};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
	const int maxHeight = MulDiv(area.bottom - area.top, kSplashMaxHeightPercent, 100);

	int width = static_cast<int>(image->GetWidth());
	int height = static_cast<int>(image->GetHeight());
	if (maxHeight > 0 && height > maxHeight) {
		width = MulDiv(width, maxHeight, height);
		height = maxHeight;
	}

	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;  // top-down, matching the DIB layout GDI+ writes
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;

	void* bits = nullptr;
	g_splashBitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (g_splashBitmap == nullptr || bits == nullptr) {
		delete image;
		return false;
	}

	{
		// Drawing into a PARGB bitmap makes GDI+ premultiply as it writes, which is
		// exactly the format UpdateLayeredWindow wants.
		Gdiplus::Bitmap target(width, height, width * 4, PixelFormat32bppPARGB,
		                       static_cast<BYTE*>(bits));
		Gdiplus::Graphics canvas(&target);
		canvas.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
		canvas.DrawImage(image, Gdiplus::Rect(0, 0, width, height), 0, 0,
		                 static_cast<INT>(image->GetWidth()),
		                 static_cast<INT>(image->GetHeight()),
		                 Gdiplus::UnitPixel);
	}
	delete image;

	g_splashMemDc = CreateCompatibleDC(nullptr);
	if (g_splashMemDc == nullptr) {
		ReleaseSplash();
		return false;
	}
	SelectObject(g_splashMemDc, g_splashBitmap);

	g_splashWidth = width;
	g_splashHeight = height;
	return true;
}

bool ApplySplashAlpha() {
	if (g_splashWnd == nullptr || g_splashMemDc == nullptr) {
		return false;
	}

	RECT area = {};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
	POINT destination = {};
	destination.x = area.left + ((area.right - area.left) - g_splashWidth) / 2;
	destination.y = area.top + ((area.bottom - area.top) - g_splashHeight) / 2;
	SIZE size = { g_splashWidth, g_splashHeight };
	POINT source = { 0, 0 };

	BLENDFUNCTION blend = {};
	blend.BlendOp = AC_SRC_OVER;
	blend.SourceConstantAlpha = static_cast<BYTE>(g_splashAlpha);
	blend.AlphaFormat = AC_SRC_ALPHA;

	if (UpdateLayeredWindow(g_splashWnd, nullptr, &destination, &size, g_splashMemDc,
	                        &source, 0, &blend, ULW_ALPHA) == FALSE) {
		Log(L"splash: UpdateLayeredWindow failed (%lu)", GetLastError());
		return false;
	}
	return true;
}

void ShowSplash() {
	if (g_splashWnd == nullptr || g_splashWidth == 0) {
		return;
	}
	g_splashAlpha = kSplashAlphaMax;
	if (!ApplySplashAlpha()) {
		return;
	}
	ShowWindow(g_splashWnd, SW_SHOWNA);  // visible, but never taking the focus
	g_splashHoldTicks = kSplashHoldMs / kSplashFadeStepMs;
	SetTimer(g_hWnd, kTimerSplashFade, kSplashFadeStepMs, nullptr);
}

LRESULT CALLBACK SplashProc(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam) {
	// The surface is pushed with UpdateLayeredWindow, so there is nothing to paint.
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

void CreateSplashWindow(const HINSTANCE instance) {
	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.lpfnWndProc = SplashProc;
	wcex.hInstance = instance;
	wcex.lpszClassName = kSplashClass;
	RegisterClassExW(&wcex);

	g_splashWnd = CreateWindowExW(
	    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
	    kSplashClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
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
	AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
	            L"Alt+CapsLock = \u539F\u6765\u7684 CapsLock");
	AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
	// Both ticks are rebuilt every time the menu opens, so the menu can never
	// disagree with the actual state.
	AppendMenuW(hMenu, MF_STRING | (g_enabled ? MF_CHECKED : MF_UNCHECKED), kMenuIdToggleMapping,
	            L"\u542F\u7528\u6620\u5C04 (CapsLock -> Ctrl+Space)");
	AppendMenuW(hMenu, MF_STRING | (g_startupTaskInstalled ? MF_CHECKED : MF_UNCHECKED),
	            kMenuIdToggleStartup, L"\u5F00\u673A\u542F\u52A8 (\u7BA1\u7406\u5458)");
	AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(hMenu, MF_STRING, kMenuIdExit, L"Exit");

	SetForegroundWindow(hwnd);

	const UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);

	// Documented requirement after TrackPopupMenu, otherwise the menu can stay
	// stuck on screen until the user clicks somewhere else.
	PostMessageW(hwnd, WM_NULL, 0, 0);
	DestroyMenu(hMenu);

	if (cmd == kMenuIdToggleMapping) {
		SetMappingEnabled(!g_enabled);
	} else if (cmd == kMenuIdToggleStartup) {
		SetStartupEnabled(!g_startupTaskInstalled);
	} else if (cmd == kMenuIdExit) {
		PostMessageW(hwnd, WM_CLOSE, 0, 0);
	}
}

LRESULT CALLBACK WndProc(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam) {
	switch (message) {
	case WM_CREATE:
		g_hWnd = hwnd;  // the hook callback posts to this window
		CreateOverlayWindow(g_hInst);
		CreateSplashWindow(g_hInst);
		EnsureInstalled(hwnd);
		SetTimer(hwnd, kTimerSelfHeal, kSelfHealMs, nullptr);

		// Ask once whether the logon task is there; the answer is what the menu
		// shows as the tick next to "start at logon".
		g_startupTaskInstalled = QueryStartupTask();

		if (g_gdiplusReady && PrepareSplash()) {
			ShowSplash();
		}
		break;

	case WM_TIMER:
		if (wParam == kTimerRetryStartup) {
			EnsureInstalled(hwnd);
		} else if (wParam == kTimerSelfHeal) {
			SelfHeal(hwnd);
		} else if (wParam == kTimerShowImeState) {
			// One-shot: the interval only existed to let the injected hotkey land.
			KillTimer(hwnd, kTimerShowImeState);
			ShowImeStateOverlay();
		} else if (wParam == kTimerOverlayFade) {
			if (g_overlayHoldTicks > 0) {
				--g_overlayHoldTicks;
			} else {
				g_overlayAlpha -= kOverlayFadeStep;
				if (g_overlayAlpha <= 0) {
					KillTimer(hwnd, kTimerOverlayFade);
					if (g_overlayWnd != nullptr) {
						ShowWindow(g_overlayWnd, SW_HIDE);
					}
				} else if (g_overlayWnd != nullptr) {
					SetLayeredWindowAttributes(g_overlayWnd, 0,
					                           static_cast<BYTE>(g_overlayAlpha), LWA_ALPHA);
				}
			}
		} else if (wParam == kTimerSplashFade) {
			if (g_splashHoldTicks > 0) {
				--g_splashHoldTicks;
			} else {
				g_splashAlpha -= kSplashFadeStep;
				if (g_splashAlpha <= 0) {
					KillTimer(hwnd, kTimerSplashFade);
					if (g_splashWnd != nullptr) {
						ShowWindow(g_splashWnd, SW_HIDE);
					}
				} else {
					ApplySplashAlpha();
				}
			}
		} else if (wParam == kTimerRefreshStartup) {
			// A moment has passed since asking for the change, so pick up what the
			// elevated helper did.
			KillTimer(hwnd, kTimerRefreshStartup);
			g_startupTaskInstalled = QueryStartupTask();
		}
		break;

	case WM_SWITCH_IME:
		SendCtrlSpace();
		// Ask which mode the foreground IME ended up in a moment later, once the
		// injected hotkey has actually been handled.
		SetTimer(hwnd, kTimerShowImeState, kImeQueryDelayMs, nullptr);
		break;

	case WM_TRAYICON:
		if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
			ShowContextMenu(hwnd);
		}
		break;

	case WM_DESTROY:
		KillTimer(hwnd, kTimerRetryStartup);
		KillTimer(hwnd, kTimerSelfHeal);
		KillTimer(hwnd, kTimerShowImeState);
		KillTimer(hwnd, kTimerOverlayFade);
		KillTimer(hwnd, kTimerSplashFade);
		KillTimer(hwnd, kTimerRefreshStartup);
		if (g_overlayWnd != nullptr) {
			DestroyWindow(g_overlayWnd);
			g_overlayWnd = nullptr;
		}
		if (g_splashWnd != nullptr) {
			DestroyWindow(g_splashWnd);
			g_splashWnd = nullptr;
		}
		ReleaseSplash();
		UninstallHook();
		RemoveTrayIcon();
		g_hWnd = nullptr;
		PostQuitMessage(0);
		break;

	default:
		if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage) {
			// Explorer restarted and took the tray icon with it. RefreshTrayIcon
			// works out whether it really is gone: a broadcast we did not need must
			// not make us forget an icon we still own, or it would never be removed
			// on exit.
			Log(L"Explorer restart seen (TaskbarCreated): re-registering the tray icon");
			RefreshTrayIcon(hwnd);
			if (!g_trayIconAdded) {
				SetTimer(hwnd, kTimerRetryStartup, kRetryStartupMs, nullptr);
			}
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
	UNREFERENCED_PARAMETER(nCmdShow);

	// The elevated helper must act before anything else: it has to stay clear of the
	// running instance's single-instance mutex, and it never shows a window. These
	// spellings must match kCommandStartupEnable / kCommandStartupDisable.
	if (lpCmdLine != nullptr) {
		if (strstr(lpCmdLine, "--startup-enable") != nullptr) {
			return RunStartupHelper(true);
		}
		if (strstr(lpCmdLine, "--startup-disable") != nullptr) {
			return RunStartupHelper(false);
		}
	}

	g_startTick = GetTickCount64();
	BuildPaths();

	Gdiplus::GdiplusStartupInput gdiplusInput;
	if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) == Gdiplus::Ok) {
		g_gdiplusReady = true;
	}

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
	// GetMessage returns -1 on error, which is non-zero and would therefore look
	// like success; the MSG would stay untouched and dispatching it would mean
	// dispatching stale stack data. Check for it explicitly.
	while ((result = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
		if (result == -1) {
			// Read the error before anything else can overwrite it, then write down
			// everything the process can still observe about the failure. The log is
			// written while the hook and tray icon are still in place, so the record
			// shows the state the program was actually in when it gave up.
			const DWORD lastError = GetLastError();
			LogMessageLoopFailure(lastError);
			break;
		}
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	if (result == -1 && g_hWnd != nullptr) {
		// Bailing out of the loop this way skips WM_DESTROY, and that is where the
		// hook is unhooked. Leaving the hook installed would be worse than useless:
		// it still swallows CapsLock, but the injection happens on the message
		// loop, so CapsLock would go dead without sending anything. Tear the window
		// down so WM_DESTROY runs and the hook and tray icon are released.
		DestroyWindow(g_hWnd);
	}

	if (result == -1) {
		Log(L"action          : window destroyed, hook and tray icon released");
		Log(L"exit code       : 1");
	}

	if (hMutex != nullptr) {
		ReleaseMutex(hMutex);
		CloseHandle(hMutex);
	}

	// After every GDI+ object is gone.
	if (g_gdiplusReady) {
		Gdiplus::GdiplusShutdown(g_gdiplusToken);
		g_gdiplusReady = false;
	}

	return result == -1 ? 1 : static_cast<int>(msg.wParam);
}
