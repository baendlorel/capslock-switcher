#pragma once

#include <Windows.h>

// 设置页：四个勾选项，鼠标颜色开关右边就是浓度滑块。双击托盘图标，或者右键菜单里的
// "设置..."都能打开它；关掉只是收起来，程序继续在托盘里跑。改动立刻写进 exe 旁边的
// capslock-switcher.ini 并当场生效，不需要重启任何东西。
void OpenSettingsWindow();
void DestroySettingsWindow();

// 主消息循环先把它交给这里：设置页要靠 IsDialogMessage 走 Tab 键导航。
bool HandleSettingsMessage(MSG* message);