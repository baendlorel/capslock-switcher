#pragma once

#include <Windows.h>

// 切换提示横幅：底色跟着模式和鼠标指针共用（中文红、英文蓝、平假名 #FFC900 黄、
// 片假名 #FF8B31 橙）；日语字符是 A / あ / ア，假名那两块暖底色上写黑字。
// 只展示实际读到的状态，读不到就不显示。
void CreateBannerWindow(HINSTANCE instance);
void ShowSwitchBanner(HWND target, HKL layout);  // 注入后去抖，切走窗口/布局则不显示
void ShowCapsLockBanner(bool upper);   // Alt+CapsLock 放行后调用：紫色横幅，大写/小写
void DestroyBannerWindow();
