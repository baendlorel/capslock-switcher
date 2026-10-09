#pragma once

#include <Windows.h>

// 分层窗口与 DPI 辅助：程序里所有尺寸都用 DIP 表示，在这里统一缩放。
RECT WorkAreaFor(HWND reference);
int ScreenDpi(HWND reference = nullptr, HWND surfaceWindow = nullptr);
void EnableDpiAwareness();
bool CreateArgbSurface(int width, int height, HBITMAP& bitmap, HDC& memoryDc, void*& bits);
void DestroyArgbSurface(HBITMAP& bitmap, HDC& memoryDc, void*& bits);
bool PushLayeredSurface(HWND hwnd, HDC sourceDc, int width, int height, int alpha,
                        HWND monitorReference);
