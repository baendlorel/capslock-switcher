#include "startup.h"

#include "app.h"
#include "logging.h"
#include "tray.h"

#include <cstdio>
#include <string>
#include <utility>

namespace {

constexpr wchar_t kStartupTaskName[] = L"CapsLock Switcher";
constexpr DWORD kCommandTimeoutMs = 20000;
constexpr DWORD kCommandExitWaitMs = 1000;

// 跑一条命令行并返回它的退出码（起不来时返回 -1）。output 为空时输出一概丢掉：
// 多数调用只看退出码，这样就不受系统显示语言影响。
DWORD RunAndWait(const wchar_t* commandLine, std::string* output = nullptr) {
	wchar_t mutableLine[2048] = {};
	wcscpy_s(mutableLine, commandLine);

	SECURITY_ATTRIBUTES security = {};
	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;
	HANDLE readEnd = nullptr;
	HANDLE writeEnd = nullptr;
	if (CreatePipe(&readEnd, &writeEnd, &security, 0) == FALSE) {
		return static_cast<DWORD>(-1);
	}

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	startup.hStdOutput = writeEnd;
	startup.hStdError = writeEnd;
	PROCESS_INFORMATION process = {};

	DWORD exitCode = static_cast<DWORD>(-1);
	if (CreateProcessW(nullptr, mutableLine, nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
	                   nullptr, nullptr, &startup, &process) != FALSE) {
		CloseHandle(writeEnd);  // 写端只留给子进程：它一关，我们这边就读到结尾
		writeEnd = nullptr;

		// 输出必须收干净：管道一满子进程就会卡住，而它退出后缓冲里可能还有数据。
		// 收到写端全关（PeekNamedPipe 报错）为止；但也不能无限等，所以同时看死限。
		std::string captured;
		const ULONGLONG deadline = GetTickCount64() + kCommandTimeoutMs;
		for (;;) {
			DWORD available = 0;
			if (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
				break;  // 写端已经关掉，能收的都收完了
			}
			if (available == 0) {
				if (GetTickCount64() >= deadline) {
					break;  // 卡死的 schtasks 不该拖住启动
				}
				Sleep(10);
				continue;
			}
			char chunk[1024] = {};
			const DWORD want = available < sizeof(chunk) ? available
			                                             : static_cast<DWORD>(sizeof(chunk));
			DWORD read = 0;
			if (ReadFile(readEnd, chunk, want, &read, nullptr) == FALSE || read == 0) {
				break;
			}
			captured.append(chunk, read);
		}

		// 进程彻底收尾之后退出码才是最终值，不然可能读到 STILL_ACTIVE。
		WaitForSingleObject(process.hProcess, kCommandExitWaitMs);
		GetExitCodeProcess(process.hProcess, &exitCode);
		CloseHandle(process.hProcess);
		CloseHandle(process.hThread);
		if (output != nullptr) {
			*output = std::move(captured);
		}
	}

	CloseHandle(readEnd);
	if (writeEnd != nullptr) {
		CloseHandle(writeEnd);
	}
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

// schtasks /XML 倒在管道里的是单字节文本（它自己声明的 UTF-16 是假的），编码跟着控制台
// 代码页走：UTF-8 解得动就按 UTF-8 解，解不动就当 ANSI。
bool DecodeConsoleText(const std::string& bytes, std::wstring& out) {
	out.resize(bytes.size() + 1);
	int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
	                                static_cast<int>(bytes.size()), out.data(),
	                                static_cast<int>(out.size()));
	if (chars <= 0) {
		chars = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()),
		                            out.data(), static_cast<int>(out.size()));
	}
	if (chars <= 0) {
		out.clear();
		return false;
	}
	out.resize(static_cast<size_t>(chars));
	return true;
}

// 从任务 XML 里取出动作路径：<Command> 的内容，去掉 XML 转义和 /TR 存下来的外围引号。
bool ExtractCommandPath(const wchar_t* xml, std::wstring& out) {
	const wchar_t* const openTag = L"<Command>";
	const wchar_t* begin = wcsstr(xml, openTag);
	const wchar_t* end =
	    begin != nullptr ? wcsstr(begin + wcslen(openTag), L"</Command>") : nullptr;
	if (end == nullptr) {
		return false;
	}

	std::wstring text(begin + wcslen(openTag), end);
	const std::pair<const wchar_t*, const wchar_t*> escapes[] = {
		{ L"&lt;", L"<" }, { L"&gt;", L">" }, { L"&quot;", L"\"" }, { L"&apos;", L"'" },
		{ L"&amp;", L"&" },
	};
	for (const auto& [escaped, plain] : escapes) {
		for (size_t at = text.find(escaped); at != std::wstring::npos;
		     at = text.find(escaped, at + 1)) {
			text.replace(at, wcslen(escaped), plain);
		}
	}

	if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') {
		text = text.substr(1, text.size() - 2);
	}
	out = std::move(text);
	return !out.empty();
}

// 任务动作里记的 exe 路径。任务不存在、或者 XML 读不出来时报告 false。
bool ReadTaskCommand(std::wstring& out) {
	wchar_t command[1024] = {};
	swprintf_s(command, L"\"%s\" /Query /TN \"%s\" /XML", SchtasksPath(), kStartupTaskName);

	std::string output;
	if (RunAndWait(command, &output) != 0) {
		return false;
	}
	std::wstring xml;
	return DecodeConsoleText(output, xml) && ExtractCommandPath(xml.c_str(), out);
}

// 动计划任务要管理员权限：自己就是管理员就直接干，否则交给提权副本——也就是那次
// 必然出现的 UAC。返回 false 表示提权被拒（或者副本根本没起来），即什么都没发生。
bool RunElevated(const bool enable) {
	if (IsProcessElevated()) {
		return enable ? InstallStartupTask() : RemoveStartupTask();
	}
	const HINSTANCE launched = ShellExecuteW(nullptr, L"runas", ExePath(),
	                                        enable ? kCommandStartupEnable : kCommandStartupDisable,
	                                        nullptr, SW_HIDE);
	if (reinterpret_cast<INT_PTR>(launched) <= 32) {
		Log(L"elevation refused or failed (ShellExecuteW returned %lld)",
		    static_cast<long long>(reinterpret_cast<INT_PTR>(launched)));
		return false;
	}
	// 提权副本是异步跑的，所以过一会儿再重新看一眼状态。
	SetTimer(g_mainWnd, kTimerRefreshStartup, kStartupRefreshDelayMs, nullptr);
	return true;
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
		ShowBalloon(ok ? (enable ? L"已开启开机启动" : L"已关闭开机启动")
		               : L"开机启动设置失败，详见日志");
		return;
	}

	// 自己没提权，就把这件事交给一个提权后的自己去做——这也就是
	// "需要管理员权限"必然带来的一次 UAC 提示。
	if (!RunElevated(enable)) {
		ShowBalloon(L"需要管理员权限");
	}
}

// 计划任务里记的还是老路径（换了目录、或者换了 exe 名字）时，把它改成指当前这份 exe，
// 免得登录时拉起的还是老路径那份。任务不存在、路径已经一致、或者 XML 读不出来时什么都不做。
void SyncStartupTaskPath() {
	std::wstring old;
	if (!ReadTaskCommand(old) || _wcsicmp(old.c_str(), ExePath()) == 0) {
		return;
	}
	Log(L"计划任务的exe路径从 %s 改为 %s", old.c_str(), ExePath());
	if (!RunElevated(true)) {
		Log(L"计划任务的exe路径没能改成 %s", ExePath());
	}
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
