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
- ✅ 设置页里可随时开启/关闭映射、`Alt+CapsLock` 放行和开机启动，鼠标指针的变色浓度用滑块调，随时改
- ✅ 每次切换在屏幕中央闪出当前输入法状态：中文红底"中文"，英文蓝底"English"
- ✅ 鼠标指针也跟着变色：中文红指针、英文蓝指针，和横幅同一个颜色。按 CapsLock 立刻变，
  切换窗口也会跟着变（切到中文的窗口就变红，切回英文的窗口就变蓝），不按 CapsLock
  直接用 Ctrl+Space 或点任务栏切换也能在半秒内跟上
- ✅ CapsLock 每按一下都往 `capslock-switcher.log` 写一行：切换输入法、`Alt+CapsLock` 放行
  （还会记下结果是大写还是小写）、"映射已关闭"都能看出来；设置页开着的时候会实时滚出来
- ✅ 双击托盘图标（或右键菜单里的"设置..."）打开设置页：上面是设置项，下面是实时日志
- ✅ `Alt+CapsLock` 触发原本的大写锁定（可以在设置页里关掉），放行时中央会闪一个紫色
  横幅，白色文字写着"大写"或"小写"（小写的紫色淡一半），告诉你现在是什么状态
- ✅ 可开关开机启动（设置页里勾），以计划任务方式登录时自动运行且带管理员权限
- ✅ 程序启动时在屏幕中央显示 `umbral-keys.png`，随后淡出
- ✅ 出错退出时把详细错误信息写入 exe 同目录的 `capslock-switcher.log`

## 使用方法

1. 在Visual Studio中打开 `capslock-switcher.slnx` 并编译
2. 在 `capslock-switcher\x64\Debug`、`capslock-switcher\x64\Release`（x64），
   或 `capslock-switcher\Debug`、`capslock-switcher\Release`（Win32）目录找到生成的exe文件
3. 双击运行程序
4. 程序会在系统托盘右下角显示图标
5. 按下CapsLock键即可切换输入法（实际发送Ctrl+Space），切换后屏幕中央会闪出当前的输入法
   状态：中文红底"中文"，英文蓝底"English"，约0.5秒后淡出。状态是从前台窗口所在线程的
   IME 窗口读的，读不到就沿用上一次的结果，宁可显示旧值也不闪一个错的
6. 右键点击托盘图标打开菜单；双击托盘图标直接打开设置页（菜单里也有"设置..."）。
   开关都挪到设置页了，上方是三个勾选项加一个滑块，下方是日志窗口（每秒自动刷新，显示最后 400 行）：
   - `启用映射 (CapsLock -> Ctrl+Space)`：取消后 CapsLock 恢复成普通大写锁定键，
     程序仍在后台运行，随时可以再勾回来
   - `Alt+CapsLock = 原来的大写锁定`：勾上（默认）时 `Alt+CapsLock` 等于原来的大写锁定；
     取消后 Alt 不再特殊，`Alt+CapsLock` 和 CapsLock 一样照样切换输入法
   - `鼠标指针跟着中英文变色`滑块：100%（默认）就是完整的中文红/英文蓝，0% 等于不变
     （立刻还原成你自己的指针方案），中间的数值按比例把红/蓝减淡。染的是箭头、文本 I 型、
     链接手型这三个系统指针；具体边界见下面的"注意事项"
   - `开机启动 (管理员)`：勾选时会弹**一次** UAC 提权确认，随后创建名为 `CapsLock Switcher`
     的计划任务（"登录时"触发、最高权限），之后开机自启不会再弹 UAC；升级换了目录或 exe
     名字之后，下次启动会把任务里的 exe 路径改成当前这份（日志里记一行
     `计划任务的exe路径从 x 改为 y`），这一步同样要提权，所以也会弹一次 UAC
   - 日志窗口显示 `capslock-switcher.log`（和 exe 同目录）的最后 400 行，每秒刷新：
     启动、每次 CapsLock 按键、异常都会写进去，按一下就能在窗口里看到一行
   - `打开日志文件`：用系统默认程序打开 `capslock-switcher.log`
   - 关掉设置页（或按 Esc）只是收起来，程序继续在托盘里跑
7. 右键菜单本身只有三项：版本号（灰显）、`设置...`、`Exit`——开关全在设置页里，菜单不再重复一份。
8. 想临时用一次大写锁定，按 `Alt+CapsLock` 即可（等同于原来的 CapsLock）；松手时屏幕中央
   会闪一个紫色横幅，显示"大写"或"小写"。这一条可以在设置页里关掉，关掉之后
   `Alt+CapsLock` 就和 CapsLock 一样切换输入法。另外它受"启用映射"开关控制：映射关掉时，
   单独按 CapsLock 也是普通大写锁定。

启动时屏幕中央会显示一次 `umbral-keys.png`（键帽图），停留0.5秒后在0.3秒内淡出，
用来提示程序已经起来了。

## 编译环境

- Visual Studio 2026（平台工具集 v145）
- Windows SDK
- C++20标准
- 编译前会用 Node.js 执行 `gen_version.mjs`，从 `package.json` 生成 `version.h`
  （该脚本按自身所在目录解析路径，在任何工作目录下调用都能正常工作）

## 技术实现

- 代码按职责拆成几个模块，各自一对 `.cpp/.h`：`main.cpp` 只管进程外壳（单实例、
  隐藏主窗口、消息循环），键盘钩子在 `keyboard.cpp`，托盘在 `tray.cpp`，开机启动在
  `startup.cpp`，启动画面在 `splash.cpp`，设置页在 `settings.cpp`，日志在 `logging.cpp`，
  分层窗口和 DPI 辅助在 `surface.cpp`，鼠标指针在 `cursor.cpp`
- 使用Windows低级键盘钩子（WH_KEYBOARD_LL）捕获CapsLock按键
- 切换就是把 CapsLock 换成一次 `Ctrl+Space` 注入（`SendInput`）；注入不会松开用户仍按住的
  Ctrl 或 Space，半途失败时还会把已经按下的合成键补一个抬起，不留"卡住"的修饰键。
  改输入法状态的是被注入的那个快捷键，程序自己不去设置它
- 屏幕中央的提示横幅要显示中文还是英文，读的是前台窗口所在线程的 IME 窗口：
  `EnumThreadWindows` 按类名 `IME` 找到它（`ImmGetDefaultIMEWnd` 内部做的就是这件事），
  再发 `WM_IME_CONTROL` + `IMC_GETCONVERSIONMODE`(0x0001)，用 `IME_CMODE_NATIVE` 位判断
  中英文（中文模式实测为 0x401，英文为 0x0）。**全程没有链接 imm32**：消息本身在
  `winuser.h` 里，那两个常量自己写一份就够了
- 提示横幅在屏幕中央停留约 0.5 秒后淡出：中文红底 `#FF1F45`、英文蓝底 `#0073FF`、
  大写紫底 `#9333EA`、小写用淡一半的紫 `#C999F4`，文字都是白的；圆角和逐像素 Alpha
  的做法与启动画面相同（GDI+ 画形状、GDI 画字、最后补 Alpha 蒙版）
- 鼠标指针变色（`cursor.cpp`）走的是 `SetSystemCursor`：把 `OCR_NORMAL`(32512)、
  `OCR_IBEAM`(32513)、`OCR_HAND`(32649) 三个槽换成我们自己造的 32bpp 带 Alpha 光标
  （`CreateIconIndirect`，热点从原光标继承过来，I 型的热点不在角上，丢了点击就不准；
  掩码按 Alpha 反推，免得在忽略 Alpha 的路径上画出一整块色块）。三个触发点：CapsLock
  切换时（和横幅一样先等 50ms 让注入的 Ctrl+Space 生效）、`SetWinEventHook` 的
  `EVENT_SYSTEM_FOREGROUND`（切窗口，同样去抖 50ms）、以及 500ms 一次的兜底轮询
  （覆盖同窗口内用 Ctrl+Space/Shift/点任务栏换输入法的情况，这种换法不产生前台事件）。
  染色强度由设置页的滑块给：先按上面的公式算出满色，再和用户原来的像素按百分比线性插值
  ——100% 是满色，0% 原样不动，中间越小红/蓝越淡
- 上色本身是保形的，不是涂成一块纯色：先按不透明像素的平均亮度判断本体是亮的还是暗的
  （Win10/11 默认箭头是**白体黑边**，有些方案是黑体白边，方向正好相反），然后按亮度把
  本体朝目标色 lerp，另一头的描边保持原样——白箭头变成红箭头，黑描边还是黑描边。
  经典 I 型是**单色反色光标**：`hbmColor` 是空的，`hbmMask` 高度是两倍（上半 AND 掩码、
  下半 XOR 掩码，形状在 XOR 平面上），这里把它摊平成实心形状再上色
- 会话级的光标改完不会自己回去，所以有两条兜底：正常退出和取消勾选时用
  `SPI_SETCURSORS` 还原（**不带** `SPIF_UPDATEINIFILE`，那会把染色写进用户配置），
  启动时先重载一遍用户自己的方案再解码——解码必须发生在没染色的状态下，否则
  `LoadCursor` 读回来的是我们自己的颜色，会越染越深
- 大写锁定现在开着还是关着，是钩子自己数出来的：系统的开关位（`GetKeyState(VK_CAPITAL)`
  的低位）只在有焦点的线程输入队列里更新，本程序没有焦点，读到的会是过期值。所以程序在
  钩子线程起来时读一次（那时队列刚建好，拿到的是系统当前状态），之后每放行一次 CapsLock
  （记在按键抬起时）就翻一次；别的程序注入的 CapsLock 也会跟着数，免得慢慢跑偏
- 钩子运行在独立线程，回调只在每次 CapsLock 首次按下时投递 `PostMessage`，
  由主线程执行 `SendInput`；长按不会反复切换，耗时操作也不会阻塞钩子线程。
  如果前台窗口已改变，则丢弃排队的切换请求
- 进程显式声明 **Per-Monitor V2 DPI 感知**（`SetProcessDpiAwarenessContext`，
  并带 Win8.1 / Vista 两级回退）。不声明的话 Windows 会把窗口渲染进一张更小的
  虚拟画布再拉伸到屏幕，启动图就会像"被强行放大"一样发虚
- `Alt+CapsLock`：以 CapsLock 首次按下时的 Alt 状态决定是否放行，并保持到该键抬起；
  中途松开 Alt 或改变映射开关，都不会拆散按下/抬起事件
- 启动画面 `umbral-keys.png` 以 RCDATA 资源嵌进 exe（见 `capslock-switcher.rc`），
  用 GDI+ 解码后画进一张**预乘 Alpha（PARGB）**的 DIB，再交给 `UpdateLayeredWindow`
  ——所以它是真正的逐像素透明，键帽边缘不会出现黑框。
  淡出就是拿同一个 DIB 反复调用 `UpdateLayeredWindow` 并逐级降低 `SourceConstantAlpha`，
  不需要重新解码或重绘
- 开机启动用计划任务实现（`schtasks /SC ONLOGON /RL HIGHEST`），因为"登录时以管理员身份
  启动且不弹 UAC"只有计划任务的"最高权限"能做到，启动文件夹的快捷方式做不到。
  创建任务本身需要管理员权限，所以菜单项会用 `ShellExecuteW(..., "runas", ...)`
  以 `--startup-enable` / `--startup-disable` 参数重新拉起自己；这个提权副本
  在抢互斥体之前就完成工作并退出，不创建任何窗口，结果写进日志
- 任务名固定（不带版本号），所以每次启动都会用 `schtasks /Query /TN "CapsLock Switcher" /XML`
  看一眼任务动作里记的 exe 路径：和当前这份 exe 不一致（换过目录、改过文件名）就用 `/F`
  重建同一条任务改指过来，并记下 `计划任务的exe路径从 x 改为 y`；任务不存在、或者路径已经
  一致时什么都不做
- 使用Shell_NotifyIcon创建系统托盘图标
- 使用互斥体（Mutex）防止程序重复运行

### 关于源文件的编码

`main.cpp` 和 `tests/regression.cpp` 都保存为 **UTF-8 带签名（BOM）**，注释是中文。
BOM 是必需的：没有 BOM 的 UTF-8 源文件会被 MSVC 按系统代码页（中文系统是936）解释，
一个多字节字符会连带吞掉字符串结尾的引号，直接编译不过（错误 C2001/C4819）。
字符串里的中文仍然写成 `\uXXXX` 转义（例如 `\u542F\u7528` 就是"启用"），
这样即使文件哪天被重新存成不带 BOM，也不会波及这些字面量；新写的代码（比如 `settings.cpp`）
直接写中文即可，BOM 已经保证了正确性。

## 注意事项

- 在某些受保护的应用中可能需要管理员权限
- 退出程序后CapsLock键恢复原有大小写锁定功能
- **鼠标指针变色的边界**（用的是 `SetSystemCursor`，改的是当前登录会话的系统光标）：
  - 自带光标的应用盖不住：Photoshop、游戏、网页里自定义 `cursor` 的地方仍然是原来的指针
  - 程序被任务管理器**强杀**时来不及还原，指针颜色会保留到注销为止。下次启动本程序会
    自动重载一遍你自己的光标方案把残留冲掉（正常退出、滑块拖到 0% 都会立刻还原）
  - 开了高对比度时不染色（那套指针是专门挑的高可见度方案，不该乱动）
  - 你自己在"鼠标设置"里换指针方案或改大小，程序会重新读一遍再接着染色
- 如需开机自启动，可将程序快捷方式放入Windows启动文件夹：
  `C:\Users\你的用户名\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup`
- 程序已在运行时再次启动，会提示"Already running"，并顺带要求已在运行的实例
  重新显示托盘图标（因为它可能已经因为Explorer重启而不可见）

## 稳定性设计说明

程序长时间运行后曾出现「CapsLock没反应、托盘图标也没了」的现象。这两件事各有原因，
而且进程通常并没有真的崩溃：

1. **CapsLock失效**：在低级键盘钩子回调里直接调用 `SendInput` 会拖长回调耗时。一旦超过
   `LowLevelHooksTimeout`，Windows 7 以上会**静默卸载该钩子**，且没有任何通知或查询接口。
   现在钩子使用独立消息线程，回调只投递一条消息，注入动作在主线程完成；
   主线程创建计划任务时，钩子仍能处理输入。此外每60秒重新挂一次钩子
   （先挂新的再摘旧的，因此不存在「没有钩子」的空档，也不会重复注入），
   即使钩子仍被系统摘除也能自行恢复。
2. **托盘图标消失**：Explorer 重启会销毁任务栏上的所有图标。程序现在处理
   `TaskbarCreated` 消息，任务栏重建后自动把图标加回去。

另外，启动时钩子或托盘图标创建失败（例如开机自启时外壳尚未就绪）不再导致程序退出，
而是每2秒重试一次。

## 项目结构

回归检查可在 PowerShell 中运行 `./tests/run.ps1`。脚本自动加载 Visual Studio C++ 环境，
验证按键配对、长按、修饰键、注入失败、钩子线程生命周期，启动图的解码与释放，
以及指针变色的浓度公式（0%/50%/100%）、热点保留和"解码→上色→还原"整条状态机。
各模块的 `.cpp` 会编进同一个翻译单元，键盘 API 换成替身，所以替身对所有模块都生效。
测试使用模拟键盘 API，不会安装全局键盘钩子或向其他程序发送按键。

```
capslock-switcher/
├── capslock-switcher/
│   ├── main.cpp                    # 进程外壳：单实例、隐藏主窗口、消息循环
│   ├── app.h                       # 共享状态与跨模块入口
│   ├── keyboard.cpp / keyboard.h   # 键盘钩子与 Ctrl+Space 注入
│   ├── banner.cpp / banner.h       # 切换提示横幅（屏幕中央的中/英文）
│   ├── cursor.cpp / cursor.h       # 鼠标指针跟着中英文变色
│   ├── tray.cpp / tray.h           # 托盘图标与右键菜单
│   ├── startup.cpp / startup.h     # 开机启动（计划任务 + 提权副本）
│   ├── splash.cpp / splash.h       # 启动画面
│   ├── settings.cpp / settings.h   # 设置页（设置项 + 日志）
│   ├── logging.cpp / logging.h     # 诊断日志
│   ├── surface.cpp / surface.h     # 分层窗口与 DPI 辅助
│   ├── resource.h                  # 资源ID头文件
│   ├── version.h                   # 由 gen_version.mjs 生成的版本号
│   ├── gen_version.mjs             # 从 package.json 生成 version.h
│   ├── app.ico                     # 程序/托盘图标（16/24/32/48/256）
│   ├── umbral-keys.png             # 启动画面（以 RCDATA 嵌进 exe）
│   ├── capslock-switcher.rc        # 资源脚本
│   └── capslock-switcher.vcxproj   # 项目文件
├── capslock-switcher.slnx          # 解决方案
├── tests/
│   ├── regression.cpp              # 回归检查
│   └── run.ps1                     # 跑回归检查（自动加载 VS C++ 环境）
├── package.json                    # 名称与版本号来源
└── README.md                       # 本文件
```
