#pragma once

#include <Windows.h>

// 设置存在 exe 旁边的 capslock-switcher.ini 里。启动时和设置页每次改动都读这一份：
// 键名、取值范围、坏值处理只有一处，不会两边各写一套。
struct AppSettings {
	bool mappingEnabled;         // 中文中/英、日语平/片/英
	bool altCapsLockPassThrough; // Alt+CapsLock = 原来的大写锁定
	bool cursorTintEnabled;      // 鼠标指针跟着中英文变色
	int cursorTintPercent;       // 0~100
};

// ini 缺失、写坏或读不出来时回到这几个默认值。
constexpr bool kDefaultMappingEnabled = true;
constexpr bool kDefaultAltCapsLockPassThrough = true;
constexpr bool kDefaultCursorTintEnabled = true;
constexpr int kDefaultCursorTintPercent = 100;

// exe 旁边的 capslock-switcher.ini。拼不出来（路径太长之类）时返回空串，
// 这时读写都会失败，设置只是不落盘。
const wchar_t* SettingsPath();

// 逐项读：缺失或写坏的项退回默认值。任何一项不合法都会把整份文件重写成合法内容，
// 好让手改坏的文件自愈。repaired 非空时，用它回报这次是否触发了重写。
AppSettings ReadSettings(bool* repaired = nullptr);

void WriteSettings(const AppSettings& settings);
