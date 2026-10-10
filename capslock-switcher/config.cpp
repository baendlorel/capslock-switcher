#include "config.h"

#include <cstdio>
#include <cstdlib>

namespace {

constexpr wchar_t kIniSection[] = L"General";
constexpr wchar_t kSettingsFileName[] = L"capslock-switcher.ini";

// 只接受整数，而且必须整数完整占满整个值：多一个字符就算写坏了。
bool ReadIniInt(const wchar_t* key, const int low, const int high, int& out) {
	wchar_t text[32] = {};
	GetPrivateProfileStringW(kIniSection, key, L"", text, _countof(text), SettingsPath());
	wchar_t* end = nullptr;
	const long value = wcstol(text, &end, 10);
	if (end == text || *end != L'\0' || value < low || value > high) {
		return false;
	}
	out = static_cast<int>(value);
	return true;
}

}  // 匿名命名空间

const wchar_t* SettingsPath() {
	static wchar_t path[MAX_PATH] = {};
	if (path[0] != L'\0') {
		return path;
	}
	// 文件名写死：主程序和设置界面是两个 exe，跟着 exe 名走的话两边会各写一份 ini。
	wchar_t exe[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameW(nullptr, exe, _countof(exe));
	wchar_t* slash = length != 0 ? wcsrchr(exe, L'\\') : nullptr;
	if (slash == nullptr ||
	    static_cast<size_t>(slash - exe) + wcslen(kSettingsFileName) + 2 > _countof(path)) {
		return path;  // 空串：读写都会失败，设置只是不落盘
	}
	*(slash + 1) = L'\0';
	wcscpy_s(path, exe);
	wcscat_s(path, kSettingsFileName);
	return path;
}

AppSettings ReadSettings(bool* repaired) {
	int mapping = kDefaultMappingEnabled ? 1 : 0;
	int alt = kDefaultAltCapsLockPassThrough ? 1 : 0;
	int tint = kDefaultCursorTintEnabled ? 1 : 0;
	int percent = kDefaultCursorTintPercent;
	const bool valid = ReadIniInt(L"MappingEnabled", 0, 1, mapping) &
	                   ReadIniInt(L"AltCapsLockPassThrough", 0, 1, alt) &
	                   ReadIniInt(L"CursorTintEnabled", 0, 1, tint) &
	                   ReadIniInt(L"CursorTintPercent", 0, 100, percent);

	AppSettings settings = {};
	settings.mappingEnabled = mapping != 0;
	settings.altCapsLockPassThrough = alt != 0;
	settings.cursorTintEnabled = tint != 0;
	settings.cursorTintPercent = percent;
	if (!valid) {
		// 从缺项/乱填里恢复过来的那份也要落盘，免得每次启动都走一遍修复。
		DeleteFileW(SettingsPath());
		WriteSettings(settings);
	}
	if (repaired != nullptr) {
		*repaired = !valid;
	}
	return settings;
}

void WriteSettings(const AppSettings& settings) {
	struct Entry {
		const wchar_t* key;
		int value;
	};
	const Entry entries[] = {
		{ L"MappingEnabled", settings.mappingEnabled ? 1 : 0 },
		{ L"AltCapsLockPassThrough", settings.altCapsLockPassThrough ? 1 : 0 },
		{ L"CursorTintEnabled", settings.cursorTintEnabled ? 1 : 0 },
		{ L"CursorTintPercent", settings.cursorTintPercent },
	};
	for (const Entry& entry : entries) {
		wchar_t text[8] = {};
		swprintf_s(text, L"%d", entry.value);
		if (WritePrivateProfileStringW(kIniSection, entry.key, text, SettingsPath()) == FALSE) {
			return;  // 写不进去（只读目录之类）就算了，程序照常跑
		}
	}
}
