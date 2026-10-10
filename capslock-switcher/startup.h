#pragma once

#include <Windows.h>

// 开机启动：一条"登录时"计划任务，由任务计划程序以最高权限拉起，所以登录不会弹
// UAC。创建这条任务本身需要管理员权限，这正是那个勾选项要提权重新启动本程序的原因。
// 这两个参数由提权后的副本解析，字面量必须和 WinMain 里的一致。
constexpr wchar_t kCommandStartupEnable[] = L"--startup-enable";
constexpr wchar_t kCommandStartupDisable[] = L"--startup-disable";

bool QueryStartupTask();
bool IsProcessElevated();
void SyncStartupTaskPath();  // 任务记的还是老路径时，把它改指当前这份 exe
int RunStartupHelper(bool enable);  // 提权副本：干完活就退出，不创建窗口
