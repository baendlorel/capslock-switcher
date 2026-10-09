#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <atomic>
#include <memory>
#include "version.h"
#include "resource.h"

// gdiplus 用来解码启动画面的 PNG，键帽的逐像素透明就靠它。链接在这里声明，
// 项目文件都不用改。
#pragma comment(lib, "gdiplus.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"CapsLockSwitcherClass";
constexpr wchar_t kWindowTitle[] = L"CapsLock Switcher";
constexpr wchar_t kMutexName[] = L"CapsLockSwitcherMutex";

// 字符串里的中文一律写成 \u 转义，例如 \u542F\u7528 就是"启用"：这样即使文件
// 被存成不带 BOM，也不会在按系统代码页解释（中文系统是 936）时，让多字节字符
// 吞掉它所在字符串的结尾引号。注释改用中文，所以本文件保存为 UTF-8 带 BOM，
// 见 README。按出现顺序对照：
//   \u5DF2\u542F\u7528 = "已启用"，\u5DF2\u7981\u7528 = "已禁用"，
//   \u6620\u5C04 = "映射"，\u542F\u7528 = "启用"，
constexpr wchar_t kTooltipOn[] = L"CapsLock Switcher - \u5DF2\u542F\u7528";
constexpr wchar_t kTooltipOff[] = L"CapsLock Switcher - \u5DF2\u7981\u7528";

// 注册成系统级消息，好让第二次启动能和已经持有互斥体的实例说上话。
// 名字见下面的 kShowYourselfMessage。
constexpr wchar_t kShowYourselfMessage[] = L"CapsLockSwitcher.ShowYourself";

// 应用私有消息都排在 WM_APP 之上。WM_USER 那一段留给窗口类自己，
// 不要占用。
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_SWITCH_IME = WM_APP + 2;
constexpr UINT WM_REINSTALL_HOOK = WM_APP + 3;

// 托盘菜单命令 ID。
constexpr UINT kMenuIdToggleMapping = 1;
constexpr UINT kMenuIdToggleStartup = 2;
constexpr UINT kMenuIdExit = 3;

constexpr UINT_PTR kTimerRetryStartup = 1;
constexpr UINT_PTR kTimerSelfHeal = 2;
constexpr UINT_PTR kTimerSplashFade = 3;
constexpr UINT_PTR kTimerRefreshStartup = 4;

constexpr UINT kRetryStartupMs = 2000;

// 回调耗时超过 LowLevelHooksTimeout 的低级钩子会被 Windows 摘掉，而且从
// Windows 7 起是静默摘掉的：既没有通知，也没有接口能查询钩子是否还在。
// 托盘图标被丢掉时同样不给任何提示。所以只能用一个慢速定时器把两者重新
// 挂上，这是长时间运行后还能继续工作的唯一办法。
constexpr UINT kSelfHealMs = 60000;

// 启动画面：键帽图先停留片刻再淡出，需要单独一个窗口——它带逐像素 Alpha，
// 整窗统一透明度表达不出这种效果。
constexpr wchar_t kSplashClass[] = L"CapsLockSwitcherSplash";
constexpr int kSplashAlphaMax = 255;
constexpr UINT kSplashHoldMs = 500;  // 这段时间保持完全不透明
constexpr UINT kSplashFadeMs = 300;  // 之后用这么长时间淡出
constexpr UINT kSplashTickMs = 15;   // 淡出定时器的粒度
constexpr int kSplashMaxHeightPercent = 30;  // 占工作区高度的比例；从不放大

// 开机启动：一条"登录时"计划任务，由任务计划程序以最高权限拉起，
// 所以登录不会弹 UAC。创建这条任务本身需要管理员权限，
// 这正是那个勾选项要提权重新启动本程序的原因。
constexpr wchar_t kStartupTaskName[] = L"CapsLock Switcher";
constexpr wchar_t kCommandStartupEnable[] = L"--startup-enable";
constexpr wchar_t kCommandStartupDisable[] = L"--startup-disable";
constexpr UINT kStartupRefreshDelayMs = 3000;

HINSTANCE g_hInst = nullptr;
HWND g_hWnd = nullptr;
std::atomic<HHOOK> g_hKeyboardHook{ nullptr };
HANDLE g_hookThread = nullptr;
HANDLE g_hookStopEvent = nullptr;
HANDLE g_hookReadyEvent = nullptr;
DWORD g_hookThreadId = 0;
// 当前"物理按下 CapsLock"的状态只有钩子线程会碰。
bool g_capsDown = false;
bool g_capsSwallowed = false;
NOTIFYICONDATA g_nid = {};
bool g_trayIconAdded = false;
UINT g_taskbarCreatedMessage = 0;
UINT g_showYourselfMessage = 0;

// 托盘菜单控制的那个开关。关掉时钩子原样放行 CapsLock，
// 这个键就恢复成普通的 CapsLock。
std::atomic_bool g_enabled{ true };

// 启动画面窗口，以及它绘制用的预乘 Alpha（ARGB）位图。
HWND g_splashWnd = nullptr;
HBITMAP g_splashBitmap = nullptr;
HDC g_splashMemDc = nullptr;
void* g_splashBits = nullptr;
int g_splashWidth = 0;
int g_splashHeight = 0;
int g_splashAlpha = 0;
ULONGLONG g_splashFadeStart = 0;  // 该开始淡出的 tick

// 缓存"登录任务装没装"的答案，启动时和每次改动之后刷新。
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
// 诊断日志
//
// 写不进日志绝不能妨碍程序启动或干净退出，所以这里所有失败都故意吞掉。
// 日志文件放在 exe 旁边，第一次创建时会写入 UTF-8 的字节序标记（BOM）。
// ---------------------------------------------------------------------------

void AppendLogLine(const wchar_t* line) {
	if (g_logPath[0] == L'\0' || line == nullptr) {
		return;
	}

	// GENERIC_WRITE 同时也带来 GetFileSizeEx 需要的属性访问权限。
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
		// chars 把结尾的 NUL 也算在内，而 NUL 不属于文件内容。
		WriteFile(file, utf8, static_cast<DWORD>(chars - 1), &written, nullptr);
	}

	CloseHandle(file);
}

// 记下可执行文件所在目录，并据此推出日志路径。
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
	if (wcslen(g_logPath) + wcslen(L"capslock-switcher.log") >= _countof(g_logPath)) {
		g_logPath[0] = L'\0';
		return;
	}
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
	// FormatMessage 会在结尾留下 CR/LF（有时还多一个句点和空格）。
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
	// 注册表里的 ProductName 在 Windows 11 上仍然写着 "Windows 10"，
	// 所以系统大版本按 build 号判断，注册表字符串只当细节保留。
	const unsigned long buildNumber = wcstoul(build, nullptr, 10);
	const wchar_t* family = buildNumber >= 22000 ? L"Windows 11"
	                      : buildNumber >= 10240 ? L"Windows 10"
	                                             : L"Windows";
	Log(L"os              : %s (build %s), registry says \"%s %s\"", family, build, product, release);
	Log(L"process/thread  : pid=%lu tid=%lu", GetCurrentProcessId(), GetCurrentThreadId());
	Log(L"uptime          : %llu ms since start", GetTickCount64() - g_startTick);
}

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
	    static_cast<void*>(g_hWnd), IsWindow(g_hWnd) != FALSE, IsWindowVisible(g_hWnd) != FALSE);
	Log(L"keyboard hook   : installed=%d handle=0x%p",
	    g_hKeyboardHook.load() != nullptr, static_cast<void*>(g_hKeyboardHook.load()));
	Log(L"tray icon       : registered=%d", g_trayIconAdded != FALSE);
	Log(L"message         : MSG was left untouched, so nothing was dispatched");
}

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

		if (pKeyboard->vkCode == VK_CAPITAL && (pKeyboard->flags & LLKHF_INJECTED) == 0) {
			if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
				if (!g_capsDown) {
					g_capsDown = true;
					g_capsSwallowed = g_enabled && (pKeyboard->flags & LLKHF_ALTDOWN) == 0;
					if (g_capsSwallowed) {
						// 现在就把目标窗口记下来：排队中的请求不许切换到一个新应用。
						g_capsSwallowed = PostMessageW(g_hWnd, WM_SWITCH_IME,
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
	wcscpy_s(g_nid.szInfoTitle, kWindowTitle);
	wcscpy_s(g_nid.szInfo, text);
	Shell_NotifyIconW(NIM_MODIFY, &g_nid);
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

void SetMappingEnabled(const bool enabled) {
	g_enabled = enabled;
	UpdateTrayTooltip();
	ShowBalloon(enabled ? L"CapsLock \u6620\u5C04\u5DF2\u542F\u7528" : L"CapsLock \u6620\u5C04\u5DF2\u7981\u7528");
}

// ---------------------------------------------------------------------------
// 开机启动
//
// "登录时启动、带管理员权限、还不弹 UAC"，这正好就是一条"最高权限"的
// 计划任务——启动文件夹里的快捷方式做不到提权那一半。创建这条任务本身需要
// 管理员权限，所以菜单项会带一个参数、通过 UAC 提示重新拉起本程序，
// 让那个副本只干这件事然后退出。
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

// 跑一条命令行并返回它的退出码（起不来时返回 -1）。输出是故意丢掉的：
// 只有退出码有意义，这样就不受系统显示语言影响。
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
				wcscpy_s(path, L"schtasks.exe");  // 退回 PATH 里找
			}
		} else {
			wcscpy_s(path, L"schtasks.exe");
		}
	}
	return path;
}

// 不管系统是什么显示语言，schtasks 都是"退出码 0 就说明任务存在"，
// 而且查询不需要提权。
bool QueryStartupTask() {
	wchar_t command[1024] = {};
	swprintf_s(command, L"\"%s\" /Query /TN \"%s\"", SchtasksPath(), kStartupTaskName);
	return RunAndWait(command) == 0;
}

bool InstallStartupTask() {
	wchar_t command[2048] = {};
	// 靠 /RL HIGHEST 让任务计划程序以提权方式启动它，登录时才不会弹窗。
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
		g_startupTaskInstalled = QueryStartupTask();
		if (ok) {
			ShowBalloon(enable ? L"\u5DF2\u5F00\u542F\u5F00\u673A\u542F\u52A8"
			                   : L"\u5DF2\u5173\u95ED\u5F00\u673A\u542F\u52A8");
		} else {
			ShowBalloon(L"\u5F00\u673A\u542F\u52A8\u8BBE\u7F6E\u5931\u8D25\uFF0C\u8BE6\u89C1\u65E5\u5FD7");
		}
		return;
	}

	// 自己没提权，就把这件事交给一个提权后的自己去做——这也就是
	// "需要管理员权限"必然带来的一次 UAC 提示。
	const HINSTANCE launched = ShellExecuteW(nullptr, L"runas", g_exePath,
	                                        enable ? kCommandStartupEnable : kCommandStartupDisable,
	                                        nullptr, SW_HIDE);
	if (reinterpret_cast<INT_PTR>(launched) <= 32) {
		Log(L"elevation refused or failed (ShellExecuteW returned %lld)",
		    static_cast<long long>(reinterpret_cast<INT_PTR>(launched)));
		ShowBalloon(L"\u9700\u8981\u7BA1\u7406\u5458\u6743\u9650");
		return;
	}

	// 提权副本是异步跑的，所以过一会儿再重新看一眼状态。
	SetTimer(g_hWnd, kTimerRefreshStartup, kStartupRefreshDelayMs, nullptr);
}

// 提权副本：干完活就走，自始至终不创建窗口。
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
// 分层窗口
//
// 启动画面是一个分层窗口，内容直接来自一张预乘 Alpha 的 ARGB 位图：
// 这样才有真正的逐像素透明，键帽边缘能干净地合成，
// 而不是留下色键（colour key）那种黑框。
// ---------------------------------------------------------------------------

RECT WorkAreaFor(const HWND reference) {
	RECT area = {};
	MONITORINFO info = {};
	info.cbSize = sizeof(MONITORINFO);
	const HMONITOR monitor = MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST);
	if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
		return info.rcWork;
	}
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
	return area;
}

// 程序里所有尺寸都用 DIP 表示，在这里统一缩放。进程声明了 DPI 感知之后，
// 这里拿到的是屏幕的真实 DPI，所以启动画面按屏幕像素分辨率绘制，
// 而不是被拉伸上去——拉伸正是它以前发虚的原因。
int ScreenDpi(const HWND reference = nullptr, const HWND surfaceWindow = nullptr) {
	// 查自己那个 DPI 感知的窗口，因为前台应用可能根本不感知 DPI。
	const auto getWindowDpi = reinterpret_cast<UINT(WINAPI*)(HWND)>(
	    GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
	if (getWindowDpi != nullptr && surfaceWindow != nullptr) {
		if (MonitorFromWindow(surfaceWindow, MONITOR_DEFAULTTONEAREST) !=
		    MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST)) {
			const RECT area = WorkAreaFor(reference);
			SetWindowPos(surfaceWindow, nullptr, area.left, area.top, 0, 0,
			             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
		const UINT dpi = getWindowDpi(surfaceWindow);
		if (dpi != 0) {
			return static_cast<int>(dpi);
		}
	}
	// Windows 8.1 没有 GetDpiForWindow。
	const HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (shcore != nullptr) {
		using GetMonitorDpiFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
		const auto getDpi = reinterpret_cast<GetMonitorDpiFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
		UINT x = 0, y = 0;
		const bool ok = getDpi != nullptr &&
		    SUCCEEDED(getDpi(MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST), 0, &x, &y));
		FreeLibrary(shcore);
		if (ok && y > 0) {
			return static_cast<int>(y);
		}
	}
	const HDC screen = GetDC(nullptr);
	const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
	if (screen != nullptr) {
		ReleaseDC(nullptr, screen);
	}
	return dpi > 0 ? dpi : 96;
}

// 让进程声明"每显示器 DPI 感知"。不声明的话，Windows 会先把我们的窗口
// 渲染到一张更小的虚拟画布上，再拉伸到屏幕，启动画面就会像被放大过一样发虚。
void EnableDpiAwareness() {
	// Windows 10 1703 及以后。用动态解析，好让程序在导出符号更老的
	// user32 上也能启动。
	const HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (user32 != nullptr) {
		using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
		const auto setContext = reinterpret_cast<SetContextFn>(
		    GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
		if (setContext != nullptr &&
		    setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
			return;
		}
	}

	// Windows 8.1 的回退方案。
	const HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (shcore != nullptr) {
		using SetAwarenessFn = HRESULT(WINAPI*)(int);
		const auto setAwareness = reinterpret_cast<SetAwarenessFn>(
		    GetProcAddress(shcore, "SetProcessDpiAwareness"));
		const bool ok = setAwareness != nullptr &&
		                setAwareness(2 /* 每显示器 DPI 感知 */) == S_OK;
		FreeLibrary(shcore);
		if (ok) {
			return;
		}
	}

	SetProcessDPIAware();  // Vista 及以后
}

bool CreateArgbSurface(const int width, const int height, HBITMAP& bitmap, HDC& memoryDc, void*& bits) {
	if (width <= 0 || height <= 0) {
		return false;
	}
	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;  // 自顶向下，和 GDI+ 写入的布局一致
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;

	bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (bitmap == nullptr || bits == nullptr) {
		return false;
	}
	memoryDc = CreateCompatibleDC(nullptr);
	if (memoryDc == nullptr) {
		DeleteObject(bitmap);
		bitmap = nullptr;
		bits = nullptr;
		return false;
	}
	if (SelectObject(memoryDc, bitmap) == nullptr) {
		DeleteDC(memoryDc);
		DeleteObject(bitmap);
		memoryDc = nullptr;
		bitmap = nullptr;
		bits = nullptr;
		return false;
	}
	return true;
}

void DestroyArgbSurface(HBITMAP& bitmap, HDC& memoryDc, void*& bits) {
	if (memoryDc != nullptr) {
		DeleteDC(memoryDc);
		memoryDc = nullptr;
	}
	if (bitmap != nullptr) {
		DeleteObject(bitmap);
		bitmap = nullptr;
	}
	bits = nullptr;
}

// 把位图居中放到参考窗口所在的那台显示器上，并按给定的整体透明度推给窗口；
// 启动画面就是靠它淡出的。
bool PushLayeredSurface(const HWND hwnd, const HDC sourceDc, const int width, const int height,
                        const int alpha, const HWND monitorReference) {
	if (hwnd == nullptr || sourceDc == nullptr || width <= 0 || height <= 0) {
		return false;
	}

	const RECT area = WorkAreaFor(monitorReference);
	POINT destination = {};
	destination.x = area.left + ((area.right - area.left) - width) / 2;
	destination.y = area.top + ((area.bottom - area.top) - height) / 2;
	SIZE size = { width, height };
	POINT source = { 0, 0 };

	BLENDFUNCTION blend = {};
	blend.BlendOp = AC_SRC_OVER;
	blend.SourceConstantAlpha = static_cast<BYTE>(alpha);
	blend.AlphaFormat = AC_SRC_ALPHA;

	return UpdateLayeredWindow(hwnd, nullptr, &destination, &size, sourceDc, &source, 0,
	                           &blend, ULW_ALPHA) != FALSE;
}

// 启动画面只是被"推"上去、并不绘制，所以不需要自定义处理。
LRESULT CALLBACK SurfaceProc(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam) {
	return DefWindowProcW(hwnd, message, wParam, lParam);
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

void ReinstallHook(const HWND hwnd) {
	if (!InstallHook() || !PostThreadMessageW(g_hookThreadId, WM_REINSTALL_HOOK, 0, 0)) {
		SetTimer(hwnd, kTimerRetryStartup, kRetryStartupMs, nullptr);
	}
}

// 每分钟跑一次。Windows 会不声不响地摘掉超时的低级钩子，外壳会在 Explorer
// 重启时丢掉托盘图标，所以这里把两者都重新挂一遍。配合快速重试和
// TaskbarCreated 处理，本程序和外壳不管谁先起来都能兜住。
void SelfHeal(const HWND hwnd) {
	const bool trayWasRegistered = g_trayIconAdded;

	ReinstallHook(hwnd);
	RefreshTrayIcon(hwnd);

	if (!trayWasRegistered && g_trayIconAdded) {
		// 外壳把我们的图标丢掉了——通常是 Explorer 重启、而它的 TaskbarCreated
		// 广播我们没收到。这值得记一行日志，因为在用户看来这就是
		// "托盘图标不见了"。
		Log(L"tray icon was missing and has been re-registered on the self-heal tick");
	}

	if (g_hKeyboardHook == nullptr || !g_trayIconAdded) {
		SetTimer(hwnd, kTimerRetryStartup, kRetryStartupMs, nullptr);
	}
}

// ---------------------------------------------------------------------------
// 启动画面
//
// 键帽是一张带真实透明度的 PNG，所以先画到它自己的预乘 Alpha 位图上；
// 之后停留和淡出都是把同一张位图按更小的整体透明度重新推一次——
// 不重新解码，也不重新绘制。
// ---------------------------------------------------------------------------

void ReleaseSplash() {
	DestroyArgbSurface(g_splashBitmap, g_splashMemDc, g_splashBits);
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

	// GDI+ 可能是惰性读取的，所以要在释放源数据流之前销毁图像。
	const auto releaseStream = [](IStream* value) { value->Release(); };
	const std::unique_ptr<IStream, decltype(releaseStream)> streamOwner(stream, releaseStream);
	const std::unique_ptr<Gdiplus::Image> image(Gdiplus::Image::FromStream(stream));
	if (image == nullptr) {
		return false;
	}
	if (image->GetLastStatus() != Gdiplus::Ok) {
		Log(L"splash: GDI+ could not decode the PNG");
		return false;
	}

	// 图片的原始尺寸按 DIP 算，所以用屏幕 DPI 缩放：启动画面看上去还是原来那么大，
	// 但按真实像素分辨率渲染，而不是被拉伸上去。同时还要塞得进工作区，
	// 而且放大倍数不超过 DPI 系数。
	const int dpi = ScreenDpi(nullptr, g_splashWnd);
	const RECT area = WorkAreaFor(nullptr);
	const int maxHeight = MulDiv(area.bottom - area.top, kSplashMaxHeightPercent, 100);

	int width = MulDiv(static_cast<int>(image->GetWidth()), dpi, 96);
	int height = MulDiv(static_cast<int>(image->GetHeight()), dpi, 96);
	if (maxHeight > 0 && height > maxHeight) {
		width = MulDiv(width, maxHeight, height);
		height = maxHeight;
	}

	if (!CreateArgbSurface(width, height, g_splashBitmap, g_splashMemDc, g_splashBits)) {
		return false;
	}

	{
		// 往 PARGB 位图上绘制时，GDI+ 会边写边做预乘——
		// 这正是 UpdateLayeredWindow 要的格式。
		Gdiplus::Bitmap target(width, height, width * 4, PixelFormat32bppPARGB,
		                       static_cast<BYTE*>(g_splashBits));
		Gdiplus::Graphics canvas(&target);
		canvas.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
		const Gdiplus::Status drawn = canvas.DrawImage(image.get(), Gdiplus::Rect(0, 0, width, height), 0, 0,
		                 static_cast<INT>(image->GetWidth()),
		                 static_cast<INT>(image->GetHeight()),
		                 Gdiplus::UnitPixel);
		if (drawn != Gdiplus::Ok) {
			Log(L"splash: GDI+ rendering failed (%d)", static_cast<int>(drawn));
			return false;
		}
	}

	g_splashWidth = width;
	g_splashHeight = height;
	return true;
}

bool ApplySplashAlpha() {
	if (g_splashWnd == nullptr || g_splashMemDc == nullptr) {
		return false;
	}
	if (!PushLayeredSurface(g_splashWnd, g_splashMemDc, g_splashWidth, g_splashHeight,
	                        g_splashAlpha, nullptr)) {
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
	ShowWindow(g_splashWnd, SW_SHOWNA);  // 可见，但绝不抢焦点
	// 用实际流逝的时间驱动淡出，定时器再怎么抖动，停留和淡出
	// 也还是要求的那么长。
	g_splashFadeStart = GetTickCount64() + kSplashHoldMs;
	SetTimer(g_hWnd, kTimerSplashFade, kSplashTickMs, nullptr);
}

void CreateSplashWindow(const HINSTANCE instance) {
	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.lpfnWndProc = SurfaceProc;
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
	// 每次打开菜单都会重新生成这两个勾选状态，
	// 所以菜单永远不会和真实状态不一致。
	AppendMenuW(hMenu, MF_STRING | (g_enabled ? MF_CHECKED : MF_UNCHECKED), kMenuIdToggleMapping,
	            L"\u542F\u7528\u6620\u5C04 (CapsLock -> Ctrl+Space)");
	AppendMenuW(hMenu, MF_STRING | (g_startupTaskInstalled ? MF_CHECKED : MF_UNCHECKED),
	            kMenuIdToggleStartup, L"\u5F00\u673A\u542F\u52A8 (\u7BA1\u7406\u5458)");
	AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(hMenu, MF_STRING, kMenuIdExit, L"Exit");

	SetForegroundWindow(hwnd);

	const UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);

	// TrackPopupMenu 之后文档要求这么做，否则菜单可能一直卡在屏幕上，
	// 直到用户点到别处才消失。
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
		g_hWnd = hwnd;  // 钩子回调往这个窗口投消息
		CreateSplashWindow(g_hInst);
		EnsureInstalled(hwnd);
		SetTimer(hwnd, kTimerSelfHeal, kSelfHealMs, nullptr);

		// 启动时问一次登录任务在不在，这个答案就是菜单里
		// "开机启动"旁边那个勾。
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
		} else if (wParam == kTimerSplashFade) {
			const ULONGLONG now = GetTickCount64();
			if (now >= g_splashFadeStart + kSplashFadeMs) {
				KillTimer(hwnd, kTimerSplashFade);
				g_splashAlpha = 0;
				if (g_splashWnd != nullptr) {
					ShowWindow(g_splashWnd, SW_HIDE);
				}
			} else if (now > g_splashFadeStart) {
				const ULONGLONG elapsed = now - g_splashFadeStart;
				g_splashAlpha = kSplashAlphaMax -
				                static_cast<int>(kSplashAlphaMax * elapsed / kSplashFadeMs);
				ApplySplashAlpha();
			}
			// 否则就是还在停留阶段，什么都不用做
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
			SendCtrlSpace();
		}
		break;

	case WM_TRAYICON:
		if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
			ShowContextMenu(hwnd);
		}
		break;

	case WM_DESTROY:
		KillTimer(hwnd, kTimerRetryStartup);
		KillTimer(hwnd, kTimerSelfHeal);
		KillTimer(hwnd, kTimerSplashFade);
		KillTimer(hwnd, kTimerRefreshStartup);
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
	// 而且绝不创建窗口。这里的字面量必须和 kCommandStartupEnable /
	// kCommandStartupDisable 一致。
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

	g_startTick = GetTickCount64();
	BuildPaths();

	// 要在任何窗口或屏幕 DC 出现之前调用，这样启动画面和由 DPI 算出的
	// 尺寸都工作在真实的屏幕像素上。
	EnableDpiAwareness();

	Gdiplus::GdiplusStartupInput gdiplusInput;
	if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) == Gdiplus::Ok) {
		g_gdiplusReady = true;
	}
	struct GdiplusCleanup {
		~GdiplusCleanup() {
			if (g_gdiplusReady) {
				Gdiplus::GdiplusShutdown(g_gdiplusToken);
				g_gdiplusReady = false;
			}
		}
	} gdiplusCleanup;

	g_hInst = hInstance;

	g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
	g_showYourselfMessage = RegisterWindowMessageW(kShowYourselfMessage);

	HANDLE hMutex = CreateMutex(nullptr, TRUE, kMutexName);
	if (hMutex == nullptr) {
		Log(L"could not acquire the single-instance mutex (%lu)", GetLastError());
		MessageBoxW(nullptr, L"Cannot acquire the application mutex. Another instance may be running.",
		            kWindowTitle, MB_OK | MB_ICONERROR);
		return 1;
	}
	if (hMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
		// 请求已经在运行的那个实例把托盘图标挂回去：Explorer 重启之后
		// 它很可能就看不见了，而这正是用户以为程序死了、
		// 又去点快捷方式的时候。
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

	// WM_CREATE 会在启动钩子线程之前就写好 g_hWnd。这里不要再写一遍，
	// 因为那个线程可能已经在读它了。
	const HWND mainWindow = CreateWindowEx(
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
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	if (result == -1 && g_hWnd != nullptr) {
		// 这样跳出循环会跳过 WM_DESTROY，而卸钩子正是在那里做的。留着钩子
		// 比没有更糟：它仍然吞掉 CapsLock，可注入动作是在消息循环里做的，
		// 于是 CapsLock 会彻底失效、什么都不发。所以要把窗口拆掉，
		// 让 WM_DESTROY 跑一遍，释放钩子和托盘图标。
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

	return result == -1 ? 1 : static_cast<int>(msg.wParam);
}
