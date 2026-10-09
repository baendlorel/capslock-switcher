#include "startup.h"

#include "app.h"
#include "logging.h"
#include "tray.h"

#include <cstdio>

namespace {

constexpr wchar_t kStartupTaskName[] = L"CapsLock Switcher";

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

bool InstallStartupTask() {
	wchar_t command[2048] = {};
	// 靠 /RL HIGHEST 让任务计划程序以提权方式启动它，登录时才不会弹窗。
	swprintf_s(command,
	           L"\"%s\" /Create /TN \"%s\" /TR \"\\\"%s\\\"\" /SC ONLOGON /RL HIGHEST /F",
	           SchtasksPath(), kStartupTaskName, ExePath());
	const DWORD code = RunAndWait(command);
	Log(L"startup task create: schtasks exit code %lu for \"%s\"", code, ExePath());
	return code == 0;
}

bool RemoveStartupTask() {
	wchar_t command[1024] = {};
	swprintf_s(command, L"\"%s\" /Delete /TN \"%s\" /F", SchtasksPath(), kStartupTaskName);
	const DWORD code = RunAndWait(command);
	Log(L"startup task delete: schtasks exit code %lu", code);
	return code == 0;
}

}  // 匿名命名空间

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

// 不管系统是什么显示语言，schtasks 都是"退出码 0 就说明任务存在"，
// 而且查询不需要提权。
bool QueryStartupTask() {
	wchar_t command[1024] = {};
	swprintf_s(command, L"\"%s\" /Query /TN \"%s\"", SchtasksPath(), kStartupTaskName);
	return RunAndWait(command) == 0;
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
	const HINSTANCE launched = ShellExecuteW(nullptr, L"runas", ExePath(),
	                                        enable ? kCommandStartupEnable : kCommandStartupDisable,
	                                        nullptr, SW_HIDE);
	if (reinterpret_cast<INT_PTR>(launched) <= 32) {
		Log(L"elevation refused or failed (ShellExecuteW returned %lld)",
		    static_cast<long long>(reinterpret_cast<INT_PTR>(launched)));
		ShowBalloon(L"\u9700\u8981\u7BA1\u7406\u5458\u6743\u9650");
		return;
	}

	// 提权副本是异步跑的，所以过一会儿再重新看一眼状态。
	SetTimer(g_mainWnd, kTimerRefreshStartup, kStartupRefreshDelayMs, nullptr);
}

// 提权副本：干完活就走，自始至终不创建窗口。
int RunStartupHelper(const bool enable) {
	BuildPaths();

	if (!IsProcessElevated()) {
		Log(L"startup helper started without administrator rights; refusing");
		return 1;
	}
	return (enable ? InstallStartupTask() : RemoveStartupTask()) ? 0 : 1;
}
