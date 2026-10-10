#pragma once

#include <Windows.h>

// Unknown 只表示读不到，不能据此猜测下一种模式。
enum class InputMode { Unknown, English, Chinese, Hiragana, Katakana };
constexpr const wchar_t* kInputModeNames[] = { L"未知", L"English", L"中文", L"平假名", L"片假名" };
InputMode GetInputMode(HWND target, DWORD timeoutMs = 200);
// 中文注入 Ctrl+Space；日语按实际状态循环平假名 → 全角片假名 → 半角英文。
// target/layout 来自按下 CapsLock 的那一刻；窗口或布局变了就丢弃请求。
bool SwitchInputMode(HWND target, HKL layout);
bool InstallHook();      // 起钩子线程并挂上低级键盘钩子
void UninstallHook();    // 停掉钩子线程
bool IsHookInstalled();  // 钩子句柄是否还挂着（诊断日志用）
bool RenewHook();        // 在钩子线程上换一个新的钩子；失败时排一个重试定时器
