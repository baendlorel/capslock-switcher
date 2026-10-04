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
- ✅ 托盘菜单可随时开启/关闭映射，菜单项带勾选状态
- ✅ 每次切换在屏幕中央闪出当前输入法状态：中文显示"中"，英文显示"En"
- ✅ 出错退出时把详细错误信息写入 exe 同目录的 `capslock-switcher.log`

## 使用方法

1. 在Visual Studio中打开 `capslock-switcher.slnx` 并编译
2. 在 `capslock-switcher\x64\Debug`、`capslock-switcher\x64\Release`（x64），
   或 `capslock-switcher\Debug`、`capslock-switcher\Release`（Win32）目录找到生成的exe文件
3. 双击运行程序
4. 程序会在系统托盘右下角显示图标
5. 按下CapsLock键即可切换输入法（实际发送Ctrl+Space）
6. 切换后屏幕中央会闪出一个方块，显示当前输入法状态：中文为红底"中"，英文为蓝底"En"
   （红色 `#FF1F45`，蓝色 `#0073FF`，约半秒后自动淡出）
7. 右键点击托盘图标打开菜单：
   - `启用映射 (CapsLock -> Ctrl+Space)`：勾选/取消，用来临时开关映射。
     取消后CapsLock恢复成普通大写锁定键，程序本身仍在后台运行，随时可以再勾回来
   - `Exit`：退出程序

## 编译环境

- Visual Studio 2026（平台工具集 v145）
- Windows SDK
- C++20标准
- 编译前会用 Node.js 执行 `gen_version.mjs`，从 `package.json` 生成 `version.h`
  （该脚本按自身所在目录解析路径，在任何工作目录下调用都能正常工作）

## 技术实现

- 使用Windows低级键盘钩子（WH_KEYBOARD_LL）捕获CapsLock按键
- 钩子回调只做一次 `PostMessage`，由消息循环执行 `SendInput` 发送Ctrl+Space
- 发送后延时50毫秒（`kImeQueryDelayMs`）再读取输入法状态，等注入的快捷键先生效
- 输入法状态通过 `ImmGetDefaultIMEWnd` 取得前台窗口所在线程的 IME 窗口，
  再用 `WM_IME_CONTROL` + `IMC_GETCONVERSIONMODE`(0x0001) 读取转换模式，
  以 `IME_CMODE_NATIVE` 位判断中英文（中文模式实测为 0x401，英文为 0x0）
  - 注意**不能**用 `ImmGetContext`：IME 上下文属于线程的输入队列，
    查询属于别的线程的窗口（也就是别人的程序）只会返回 NULL，
    早期版本就是这样永远显示"En"的
- 屏幕提示是一个分层（layered）、鼠标穿透、不抢焦点的置顶小窗口，定位在
  前台窗口所在显示器的中央，显示约0.5秒后淡出：
  英文 `#0073FF` 蓝底 + 白色 `En`，中文 `#FF1F45` 红底 + 白色"中"，外加一圈白色细边
  （不透明显示，保证颜色和你指定的一致；淡出动画照旧）
- 使用Shell_NotifyIcon创建系统托盘图标
- 使用互斥体（Mutex）防止程序重复运行

### 关于 main.cpp 的编码

`main.cpp` **刻意保持纯 ASCII**，其中的中文一律写成 `\uXXXX` 转义（例如 `\u4E2D` 就是"中"）。
原因是：没有 BOM 的 UTF-8 源文件会被 MSVC 按系统代码页（中文系统是936）解释，
一个多字节字符会连带吞掉字符串结尾的引号，直接编译不过（错误 C2001/C4819）。
如果你用 Visual Studio 直接编辑并想写中文，请把文件另存为 **UTF-8 带签名(BOM)**，
两种方式都能正确编译，任选其一即可。

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
