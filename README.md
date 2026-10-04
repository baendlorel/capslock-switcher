# CapsLock切换器 (CapsLock Switcher)

将CapsLock键映射为Ctrl+Space的Windows工具，方便切换中英文输入法。

## 功能特性

- ✅ 将CapsLock键映射为Ctrl+Space组合键
- ✅ 后台运行，无窗口界面
- ✅ 系统托盘图标显示运行状态
- ✅ 右键托盘图标可退出程序
- ✅ 防止程序重复运行
- ✅ 退出后CapsLock恢复原有功能
- ✅ 长时间运行自愈：键盘钩子失效后自动重装，Explorer重启后自动恢复托盘图标
- ✅ 托盘图标创建失败时自动重试，不再直接退出

## 使用方法

1. 在Visual Studio中打开 `capslock-switcher.slnx` 并编译
2. 在 `capslock-switcher\x64\Debug`、`capslock-switcher\x64\Release`（x64），
   或 `capslock-switcher\Debug`、`capslock-switcher\Release`（Win32）目录找到生成的exe文件
3. 双击运行程序
4. 程序会在系统托盘右下角显示图标
5. 按下CapsLock键即可切换输入法（实际发送Ctrl+Space）
6. 右键点击托盘图标，选择"Exit"退出程序

## 编译环境

- Visual Studio 2026（平台工具集 v145）
- Windows SDK
- C++20标准
- 编译前会用 Node.js 执行 `gen_version.mjs`，从 `package.json` 生成 `version.h`
  （该脚本按自身所在目录解析路径，在任何工作目录下调用都能正常工作）

## 技术实现

- 使用Windows低级键盘钩子（WH_KEYBOARD_LL）捕获CapsLock按键
- 钩子回调只做一次 `PostMessage`，由消息循环执行 `SendInput` 发送Ctrl+Space
- 使用Shell_NotifyIcon创建系统托盘图标
- 使用互斥体（Mutex）防止程序重复运行

## 注意事项

- 在某些受保护的应用中可能需要管理员权限
- 退出程序后CapsLock键恢复原有大小写锁定功能
- 如需开机自启动，可将程序快捷方式放入Windows启动文件夹：
  `C:\Users\你的用户名\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup`
- 程序已在运行时再次启动，会提示"Already running"，并顺带要求已在运行的实例
  重新显示托盘图标（因为它可能已经因为Explorer重启而不可见）

## 稳定性设计说明

程序长时间运行后曾出现「CapsLock没反应、托盘图标也没了」的现象。这两件事各有原因，
而且进程通常并没有真的崩溃：

1. **CapsLock失效**：在低级键盘钩子回调里直接调用 `SendInput` 会拖长回调耗时。一旦超过
   `LowLevelHooksTimeout`，Windows 7 以上会**静默卸载该钩子**，且没有任何通知或查询接口。
   现在回调只投递一条消息，注入动作在消息循环中完成；此外每60秒重新挂一次钩子
   （先挂新的再摘旧的，因此不存在「没有钩子」的空档，也不会重复注入），
   即使钩子仍被系统摘除也能自行恢复。
2. **托盘图标消失**：Explorer 重启会销毁任务栏上的所有图标。程序现在处理
   `TaskbarCreated` 消息，任务栏重建后自动把图标加回去。

另外，启动时钩子或托盘图标创建失败（例如开机自启时外壳尚未就绪）不再导致程序退出，
而是每2秒重试一次。

## 项目结构

```
capslock-switcher/
├── capslock-switcher/
│   ├── main.cpp                    # 主程序源代码
│   ├── resource.h                  # 资源ID头文件
│   ├── version.h                   # 由 gen_version.mjs 生成的版本号
│   ├── gen_version.mjs             # 从 package.json 生成 version.h
│   ├── app.ico                     # 程序/托盘图标（16/24/32/48/256）
│   ├── capslock-switcher.rc        # 资源脚本
│   └── capslock-switcher.vcxproj   # 项目文件
├── capslock-switcher.slnx          # 解决方案
├── package.json                    # 名称与版本号来源
└── README.md                       # 本文件
```
