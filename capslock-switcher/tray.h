#pragma once

#include <Windows.h>

// 托盘图标与右键菜单。
extern bool g_trayIconAdded;  // 外壳那边现在有没有我们的图标

bool AddTrayIcon(HWND hwnd);
void RefreshTrayIcon(HWND hwnd);
void RemoveTrayIcon();
void ShowBalloon(const wchar_t* text);
void ShowTrayMenu(HWND hwnd);
void UpdateTrayTooltip();  // 映射开关变了之后刷新悬停提示
