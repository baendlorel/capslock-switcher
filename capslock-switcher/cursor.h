#pragma once

#include <Windows.h>

// 鼠标指针跟着中英文状态变色：中文红、英文蓝，跟中央横幅同一份颜色。
//
// 实现是 SetSystemCursor 逐个替换系统光标槽（箭头、I 型、手型）。那是**会话级的
// 全局修改**，不是只改我们自己的窗口：所有程序都会看到新指针，而且进程被强杀时
// 不会自动还原。所以这里有一套兜底：
//   - 正常退出、关掉开关：立刻把系统光标还原成用户自己的方案；
//   - 启动时先重载一遍用户方案，把上一次强杀留下的颜色冲掉；
//   - 每 10 秒无条件重刷一次（Windows 偶尔会自己重载光标方案，把颜色冲掉）。
// 已知边界：自带光标的应用（Photoshop、游戏之类）盖不住；开了高对比度时不染。
void InitializeCursorTint();     // WM_CREATE：自愈 + 解码光标 + 挂前台事件 + 首次上色
void CursorSwitchSettle();       // WM_SWITCH_IME：起 50ms 去抖，等 Ctrl+Space 先生效
void CursorSettleTick();         // 主窗口 WM_TIMER(kTimerCursorSettle)
void CursorPollTick();           // 主窗口 WM_TIMER(kTimerCursorPoll)
void OnSystemCursorsChanged();   // WM_SETTINGCHANGE(SPI_SETCURSORS / SPI_SETHIGHCONTRAST)
void DestroyCursorTint();        // WM_DESTROY：还原 + 摘钩子 + 清缓存
