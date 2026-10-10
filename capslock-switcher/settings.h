#pragma once

#include <Windows.h>

// 设置页：上面是设置项，下面是日志。双击托盘图标，或者右键菜单里的"设置..."，
// 都能打开它。关掉只是收起来，程序继续在托盘里跑。
// 设置存在 exe 旁边的 capslock-switcher.ini 里。缺失或写坏的项在加载时用默认值顶上，
// 并把文件重写成合法内容。
void LoadSettings();
void SaveSettings();

void CreateSettingsWindow(HINSTANCE instance);
void OpenSettingsWindow();
void DestroySettingsWindow();

// 主消息循环先把它交给这里：设置页要靠 IsDialogMessage 走 Tab 键导航。
bool HandleSettingsMessage(MSG* message);
