#pragma once

#include <Windows.h>

// 键盘钩子与按键注入。
bool SendCtrlSpace();    // 注入一次 Ctrl+Space，失败时把自己按下的键补抬起
bool InstallHook();      // 起钩子线程并挂上低级键盘钩子
void UninstallHook();    // 停掉钩子线程
bool IsHookInstalled();  // 钩子句柄是否还挂着（诊断日志用）
bool RenewHook();        // 在钩子线程上换一个新的钩子；失败时排一个重试定时器
