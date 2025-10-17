# CapsLock切换器 (CapsLock Switcher)

将CapsLock键映射为Ctrl+Space的Windows工具，方便切换中英文输入法。

## 功能特性

- ✅ 将CapsLock键映射为Ctrl+Space组合键
- ✅ 后台运行，无窗口界面
- ✅ 系统托盘图标显示运行状态
- ✅ 右键托盘图标可退出程序
- ✅ 防止程序重复运行
- ✅ 退出后CapsLock恢复原有功能

## 使用方法

1. 在Visual Studio中打开项目并编译（生成解决方案）
2. 在 `capslock-switcher\x64\Debug` 或 `capslock-switcher\x64\Release` 目录找到生成的exe文件
3. 双击运行程序
4. 程序会在系统托盘右下角显示图标
5. 按下CapsLock键即可切换输入法（实际发送Ctrl+Space）
6. 右键点击托盘图标，选择"Exit"退出程序

## 编译环境

- Visual Studio 2019或更高版本
- Windows SDK
- C++20标准

## 技术实现

- 使用Windows低级键盘钩子（WH_KEYBOARD_LL）捕获CapsLock按键
- 使用SendInput API模拟Ctrl+Space组合键
- 使用Shell_NotifyIcon创建系统托盘图标
- 使用互斥体（Mutex）防止程序重复运行

## 注意事项

- 在某些受保护的应用中可能需要管理员权限
- 退出程序后CapsLock键恢复原有大小写锁定功能
- 如需开机自启动，可将程序快捷方式放入Windows启动文件夹：
  `C:\Users\你的用户名\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup`

## 项目结构

```
capslock-switcher/
├── capslock-switcher/
│   ├── main.cpp              # 主程序源代码
│   ├── resource.h            # 资源头文件
│   └── capslock-switcher.vcxproj  # 项目文件
└── README.md                 # 本文件
