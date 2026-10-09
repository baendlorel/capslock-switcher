#pragma once

#include <Windows.h>

// 启动画面：键帽 PNG 先停留片刻，再淡出。GDI+ 只在这里用得上，所以在同一个模块里
// 起停：起不来就没有启动画面，其余功能照常。
bool StartGdiplus();
void StopGdiplus();
void CreateSplashWindow(HINSTANCE instance);
bool PrepareSplash();
void ShowSplash();
void ReleaseSplash();
void DestroySplashWindow();  // 退出时销毁窗口并释放位图
