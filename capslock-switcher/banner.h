#pragma once

#include <Windows.h>

// 切换提示横幅：按下 CapsLock 之后在屏幕中央闪一下，显示当前落到中文还是英文。
// 它只读输入法状态，用的是前台线程的 IME 窗口 + WM_IME_CONTROL 消息，
// 没有链接 imm32（见 banner.cpp 里的说明）。
void CreateBannerWindow(HINSTANCE instance);
void ShowSwitchBanner();              // 注入完 Ctrl+Space 后调用，内部会等它先生效
void ShowCapsLockBanner(bool upper);   // Alt+CapsLock 放行后调用：紫色横幅，大写/小写
void DestroyBannerWindow();
