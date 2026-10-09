#pragma once

#include <Windows.h>

// 切换提示横幅：按下 CapsLock 之后在屏幕中央闪一下，显示当前落到中文还是英文。
// 它只读输入法状态，用的是前台线程的 IME 窗口 + WM_IME_CONTROL 消息，
// 没有链接 imm32（见 banner.cpp 里的说明）。
void CreateBannerWindow(HINSTANCE instance);
void ShowSwitchBanner();              // 注入完 Ctrl+Space 后调用，内部会等它先生效
void ShowCapsLockBanner(bool upper);   // Alt+CapsLock 放行后调用：紫色横幅，大写/小写
void DestroyBannerWindow();

// 前台线程的输入法是不是停在中文（native）模式。读不到时沿用上一次成功读到的值
// （宁可显示旧值，也不闪一个错的）。timeoutMs 是等 IME 窗口回消息的上限：
// 键盘切换那条路给足，鼠标指针的轮询路径给小值，免得把主线程钉住。
bool CurrentInputIsChinese(DWORD timeoutMs = 200);
