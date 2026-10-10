#pragma once

#include <Windows.h>

// 鼠标指针跟着中英文状态变色：中文红、英文蓝，跟中央横幅同一份颜色；滑块控制程度，
// 100% 是完整颜色，0% 就是不变。
//
// SetSystemCursor 替换的是会话级的箭头、I 型和手型，所有使用系统光标的应用都会受影响。
// 原图只读注册表方案指向的 .cur 文件：按用户基础大小 × 当前显示器 DPI 选原生帧，
// 直接用 CreateIconFromResourceEx 解码，避免 LoadCursor/LoadImage 取小帧再放大。
// 滑块只改预乘 RGB，alpha/热点不动；跨屏 DPI 变化时从文件重建，不加工已染色的系统槽。
// 提交用两帧完全相同的 ANI（视觉静止），绕开 SetSystemCursor 对静态光标的二次缩放。
// 正常退出、滑到 0%：原 .cur 字节同样双帧提交，保留掩码/热点；每 10 秒补刷颜色。
// 强杀不能自动还原，但下次启动仍从干净文件读源，不会累积染色。
// .ani、没有可读静态文件的槽和自带光标的应用保持原样；高对比度下不染。
void InitializeCursorTint();     // WM_CREATE：读方案原图 + 挂前台事件 + 首次上色
void CursorSwitchSettle();       // WM_SWITCH_IME：起 50ms 去抖，等 Ctrl+Space 先生效
void CursorSettleTick();         // 主窗口 WM_TIMER(kTimerCursorSettle)
void CursorPollTick();           // 主窗口 WM_TIMER(kTimerCursorPoll)
void OnSystemCursorsChanged();   // WM_SETTINGCHANGE(SPI_SETCURSORS / SPI_SETHIGHCONTRAST)
void DestroyCursorTint();        // WM_DESTROY：还原 + 摘钩子 + 清缓存
