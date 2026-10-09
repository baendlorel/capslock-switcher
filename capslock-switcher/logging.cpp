#include "logging.h"

#include "version.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace {

wchar_t g_exePath[1024] = {};
wchar_t g_logPath[1024] = {};

// 追加一行，必要时先写下 UTF-8 的字节序标记（BOM）。
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

}  // 匿名命名空间

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
	// 进程启动至今的时长：第一次用到时记下起点就够了。
	static const ULONGLONG start = GetTickCount64();
	Log(L"uptime          : %llu ms since start", GetTickCount64() - start);
}

const wchar_t* ExePath() {
	return g_exePath;
}

const wchar_t* LogPath() {
	return g_logPath;
}
