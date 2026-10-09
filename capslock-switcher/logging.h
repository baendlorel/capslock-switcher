#pragma once

#include <Windows.h>

// 诊断日志。写不进日志绝不能妨碍程序启动或干净退出，所以实现里所有失败都故意吞掉。
void BuildPaths();  // 记下可执行文件目录并据此推出日志路径
void Log(const wchar_t* format, ...);
const wchar_t* ExePath();
const wchar_t* LogPath();
const wchar_t* ErrorText(DWORD code, wchar_t (&buffer)[512]);
void LogEnvironment();
