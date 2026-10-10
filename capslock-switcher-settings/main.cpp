// WinUI3 设置界面。主程序是 Win32（键盘钩子、托盘、横幅都在那边），这里只负责改设置：
// 两个进程通过 exe 旁边的 capslock-switcher.ini 交换状态——这边写完再给主窗口发一条
// WM_RELOAD_SETTINGS，主程序立刻重新加载，不需要重启任何东西。
#include <windows.h>
#undef GetCurrentTime
#include <shellapi.h>

#include <chrono>
#include <fstream>
#include <string>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <microsoft.ui.xaml.window.h>

#include "../capslock-switcher/app.h"
#include "../capslock-switcher/config.h"
#include "../capslock-switcher/startup.h"
#include "../capslock-switcher/resource.h"
#include "../capslock-switcher/version.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace {

constexpr wchar_t kWindowTitle[] = L"CapsLock Switcher 设置";
constexpr wchar_t kLogFileName[] = L"capslock-switcher.log";
constexpr wchar_t kMainExeName[] = L"capslock-switcher.exe";
constexpr int kLogLines = 10;
constexpr UINT kSaveDelayMs = 250;     // 拖滑块时攒一下再写盘
constexpr UINT kRefreshMs = 1000;      // 日志刷新
constexpr ULONGLONG kStartupSettleMs = 20000;  // 等提权副本把任务改完

// exe 同目录下的某个文件。设置界面和主程序装在同一个目录里。
std::wstring AppPath(const wchar_t* fileName) {
	wchar_t exe[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameW(nullptr, exe, _countof(exe));
	wchar_t* slash = length != 0 ? wcsrchr(exe, L'\\') : nullptr;
	if (slash == nullptr) {
		return fileName;
	}
	*(slash + 1) = L'\0';
	return std::wstring(exe) + fileName;
}

// 设置界面崩了就什么都没了，至少留一份现场：写进同目录的日志，方便排查。
void Trace(const char* step) {
	std::ofstream file(AppPath(L"capslock-switcher-settings.log"), std::ios::app);
	file << step << "\n";
}

// 开机启动是计划任务，退出码 0 就说明任务在（查询不需要管理员）。
bool StartupTaskInstalled() {
	const std::wstring command =
	    L"schtasks.exe /Query /TN \"" + std::wstring(kStartupTaskName) + L"\"";
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	std::vector<wchar_t> line(command.begin(), command.end());
	line.push_back(L'\0');
	if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
	                   nullptr, &startup, &process) == FALSE) {
		return false;
	}
	WaitForSingleObject(process.hProcess, 5000);
	DWORD code = 1;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return code == 0;
}

// 装/删计划任务要管理员，交给主程序的提权副本去做，这边自己不提权。
void RequestStartupChange(const bool enable) {
	const std::wstring exe = AppPath(kMainExeName);
	const HINSTANCE launched =
	    ShellExecuteW(nullptr, L"runas", exe.c_str(),
	                  enable ? kCommandStartupEnable : kCommandStartupDisable, nullptr,
	                  SW_SHOWNORMAL);
	(void)launched;
}

// 告诉主程序"ini 变了"。主程序可能没在跑，那就算了：它下次启动自己会读。
void NotifyMainApp() {
	const HWND main = FindWindowW(kMainWindowClass, kAppTitle);
	if (main != nullptr) {
		PostMessageW(main, WM_RELOAD_SETTINGS, 0, 0);
	}
}

std::wstring ReadLogTail() {
	std::ifstream file(AppPath(kLogFileName), std::ios::binary | std::ios::ate);
	if (!file) {
		return L"(还没有日志)";
	}
	const auto size = static_cast<long long>(file.tellg());
	if (size <= 0) {
		return L"(日志是空的)";
	}
	const long long want = size > 64 * 1024 ? 64 * 1024 : size;
	file.seekg(size - want);
	std::string bytes(static_cast<size_t>(want), '\0');
	file.read(bytes.data(), want);
	bytes.resize(static_cast<size_t>(file.gcount()));

	// 从文件中间读起时头一行多半是半截的，扔掉；从头读则跳过 UTF-8 的 BOM。
	size_t begin = 0;
	if (size > want) {
		while (begin < bytes.size() && bytes[begin] != '\n') {
			++begin;
		}
		++begin;
	} else if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
	           static_cast<unsigned char>(bytes[1]) == 0xBB &&
	           static_cast<unsigned char>(bytes[2]) == 0xBF) {
		begin = 3;
	}
	if (begin >= bytes.size()) {
		return L"(日志是空的)";
	}
	const int chars = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + begin,
	                                      static_cast<int>(bytes.size() - begin), nullptr, 0);
	if (chars <= 0) {
		return L"(日志读不出来)";
	}
	std::wstring text(static_cast<size_t>(chars), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, bytes.data() + begin,
	                    static_cast<int>(bytes.size() - begin), text.data(), chars);

	// 末尾那个换行会多算一行，先去掉；然后只留最后 kLogLines 行，窗口里不至于越来越沉。
	while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
		text.pop_back();
	}
	int lines = 0;
	size_t start = text.size();
	while (start > 0 && lines < kLogLines) {
		--start;
		if (text[start] == L'\n') {
			++lines;
		}
	}
	return start > 0 ? text.substr(start + 1) : text;
}

struct SettingsApp : ApplicationT<SettingsApp, Markup::IXamlMetadataProvider> {
	// 纯 C++ 界面没有 XAML 编译器生成的类型映射，应用必须提供框架的元数据。
	Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type) {
		return m_metadata.GetXamlType(type);
	}
	Markup::IXamlType GetXamlType(hstring const& name) {
		return m_metadata.GetXamlType(name);
	}
	com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() {
		return m_metadata.GetXmlnsDefinitions();
	}

	SettingsApp() {
		// 出问题时不至于无声无息地消失：把信息落到跟踪文件里。
		UnhandledException([](auto&&, UnhandledExceptionEventArgs const& args) {
			Trace("UnhandledException");
			Trace(winrt::to_string(args.Message()).c_str());
		});
	}

	void OnLaunched(LaunchActivatedEventArgs const&) {
		// 没有这份资源字典，ToggleSwitch/Slider 这些控件拿不到默认模板。
		Resources().MergedDictionaries().Append(XamlControlsResources());
		m_window = Window();
		m_window.Title(kWindowTitle);

		m_window.ExtendsContentIntoTitleBar(true);
		BuildUi();
		m_window.SetTitleBar(m_titleBar);
		SetupWindow();
		LoadFromIni();
		m_window.Activate();
		StartTimers();
		m_window.Closed([](auto&&, auto&&) {
			Application::Current().Exit();
		});
	}

  private:
	void BuildUi() {
		m_startup = ToggleSwitch();
		m_mapping = ToggleSwitch();
		m_alt = ToggleSwitch();
		m_cursor = ToggleSwitch();
		m_percent = Slider();
		m_percentText = TextBlock();
		m_log = TextBox();
		m_titleBar = Grid();
		StackPanel root;
		root.Padding(Thickness{ 28, 40, 28, 24 });
		root.Spacing(14);

		TextBlock title;
		title.Text(L"CapsLock Switcher");
		title.FontSize(26);
		title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
		TextBlock version;
		wchar_t versionLabel[32] = {};
		swprintf_s(versionLabel, L"v%hs", APP_VERSION);
		version.Text(versionLabel);
		version.Opacity(0.6);
		version.VerticalAlignment(VerticalAlignment::Bottom);
		version.Margin(Thickness{ 0, 0, 0, 4 });
		StackPanel titleRow;
		titleRow.Orientation(Orientation::Horizontal);
		titleRow.Spacing(10);
		titleRow.Children().Append(title);
		titleRow.Children().Append(version);
		root.Children().Append(titleRow);

		m_startup.OnContent(box_value(L"开机启动 (管理员)"));
		m_startup.OffContent(box_value(L"开机启动 (管理员)"));
		m_startup.Toggled([this](auto&&, auto&&) {
			if (m_loading) {
				return;
			}
			RequestStartupChange(m_startup.IsOn());
			m_startupPending = true;
			m_startupSettleUntil = GetTickCount64() + kStartupSettleMs;
		});
		root.Children().Append(m_startup);

		m_mapping.OnContent(box_value(L"启用映射 (CapsLock -> Ctrl+Space)"));
		m_mapping.OffContent(box_value(L"启用映射 (CapsLock -> Ctrl+Space)"));
		m_mapping.Toggled([this](auto&&, auto&&) { SaveFromControls(); });
		root.Children().Append(m_mapping);

		m_alt.OnContent(box_value(L"Alt+CapsLock = 原来的大写锁定"));
		m_alt.OffContent(box_value(L"Alt+CapsLock = 原来的大写锁定"));
		m_alt.Toggled([this](auto&&, auto&&) { SaveFromControls(); });
		root.Children().Append(m_alt);

		m_cursor.OnContent(box_value(L"鼠标指针跟着中英文变色"));
		m_cursor.OffContent(box_value(L"鼠标指针跟着中英文变色"));
		m_cursor.Toggled([this](auto&&, auto&&) {
			m_percent.IsEnabled(m_cursor.IsOn());  // 关掉时滑块变灰、点不动
			SaveFromControls();
		});
		StackPanel cursorRow;
		cursorRow.Orientation(Orientation::Horizontal);
		cursorRow.Spacing(16);
		cursorRow.Children().Append(m_cursor);
		m_percentText.VerticalAlignment(VerticalAlignment::Center);
		m_percentText.MinWidth(64);
		cursorRow.Children().Append(m_percentText);
		m_percent.Minimum(0);
		m_percent.Maximum(100);
		m_percent.StepFrequency(1);
		m_percent.Width(200);
		m_percent.VerticalAlignment(VerticalAlignment::Center);
		m_percent.Margin(Thickness{ 0, 4, 0, 0 });
		m_percent.ValueChanged([this](auto&&, auto&&) {
			UpdatePercentText();
			ScheduleSave();  // 拖动时攒一下，别每一格都写盘
		});
		cursorRow.Children().Append(m_percent);
		root.Children().Append(cursorRow);

		StackPanel logHeader;
		logHeader.Orientation(Orientation::Horizontal);
		logHeader.Spacing(12);
		TextBlock logTitle;
		wchar_t logLabel[64] = {};
		swprintf_s(logLabel, L"日志（最后 %d 行）", kLogLines);
		logTitle.Text(logLabel);
		logTitle.VerticalAlignment(VerticalAlignment::Center);
		logHeader.Children().Append(logTitle);
		Button openLog;
		openLog.Content(box_value(L"打开日志文件"));
		openLog.Click([](auto&&, auto&&) {
			const std::wstring path = AppPath(kLogFileName);
			ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		});
		logHeader.Children().Append(openLog);
		root.Children().Append(logHeader);

		m_log.IsReadOnly(true);
		m_log.AcceptsReturn(true);
		m_log.TextWrapping(TextWrapping::NoWrap);
		m_log.FontFamily(FontFamily(L"Consolas"));
		m_log.FontSize(12);
		m_log.Height(190); 
		m_log.VerticalAlignment(VerticalAlignment::Stretch);
		ScrollViewer::SetVerticalScrollBarVisibility(m_log, ScrollBarVisibility::Auto);
		root.Children().Append(m_log);

		ScrollViewer scroll;
		scroll.Content(root);
		scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);

		// 顶上这条空带就是标题栏的拖拽区，正文从它下面开始。
		m_titleBar.Height(32);
		m_titleBar.VerticalAlignment(VerticalAlignment::Top);
		m_titleBar.Background(SolidColorBrush(winrt::Windows::UI::Color{ 0, 0, 0, 0 }));

		// Win10 / 不支持 Mica 时使用随浅色、深色和高对比度主题更新的实体背景。
		auto page = Markup::XamlReader::Load(
		    LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
		             Background="{ThemeResource ApplicationPageBackgroundThemeBrush}" />)").as<Grid>();
		if (winrt::Microsoft::UI::Composition::SystemBackdrops::MicaController::IsSupported()) {
			m_window.SystemBackdrop(MicaBackdrop());
			page.Background(nullptr);
		}
		page.Children().Append(scroll);
		page.Children().Append(m_titleBar);
		m_window.Content(page);
	}

	void SetupWindow() {
		const HWND hwnd = GetHwnd();
		const UINT dpi = GetDpiForWindow(hwnd);
		const int width = MulDiv(660, static_cast<int>(dpi), 96);
		const int height = MulDiv(570, static_cast<int>(dpi), 96);
		if (const auto appWindow = m_window.AppWindow(); appWindow != nullptr) {
			appWindow.Resize({ width, height });
			// SDK 1.7 在 Win10 也支持自定义标题栏；按钮颜色在 Win10 上被系统忽略。
			const auto titleBar = appWindow.TitleBar();
			const winrt::Windows::UI::Color transparent{ 0, 0, 0, 0 };
			titleBar.ButtonBackgroundColor(transparent);
			titleBar.ButtonInactiveBackgroundColor(transparent);
		}
		// app.ico 已经嵌进这个 exe；大小图标都设，标题栏和任务栏才都认得。
		const HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_MAINICON));
		SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
		SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
	}

	HWND GetHwnd() const {
		const auto native = m_window.as<IWindowNative>();
		HWND hwnd = nullptr;
		if (native != nullptr) {
			native->get_WindowHandle(&hwnd);
		}
		return hwnd;
	}

	void StartTimers() {
		m_saveTimer = DispatcherTimer();
		m_saveTimer.Interval(std::chrono::milliseconds(kSaveDelayMs));
		m_saveTimer.Tick([this](auto&&, auto&&) {
			m_saveTimer.Stop();
			SaveFromControls();
		});

		m_refreshTimer = DispatcherTimer();
		m_refreshTimer.Interval(std::chrono::milliseconds(kRefreshMs));
		m_refreshTimer.Tick([this](auto&&, auto&&) { RefreshLiveState(); });
		m_refreshTimer.Start();
		RefreshLiveState();
	}

	void LoadFromIni() {
		m_loading = true;
		const AppSettings settings = ReadSettings();
		m_mapping.IsOn(settings.mappingEnabled);
		m_alt.IsOn(settings.altCapsLockPassThrough);
		m_cursor.IsOn(settings.cursorTintEnabled);
		m_percent.Value(settings.cursorTintPercent);
		m_percent.IsEnabled(settings.cursorTintEnabled);
		UpdatePercentText();
		m_startup.IsOn(StartupTaskInstalled());
		m_loading = false;
	}

	AppSettings CurrentSettings() const {
		AppSettings settings = {};
		settings.mappingEnabled = m_mapping.IsOn();
		settings.altCapsLockPassThrough = m_alt.IsOn();
		settings.cursorTintEnabled = m_cursor.IsOn();
		settings.cursorTintPercent = static_cast<int>(m_percent.Value());
		return settings;
	}

	void ScheduleSave() {
		if (m_loading || m_saveTimer == nullptr) {
			return;
		}
		m_saveTimer.Stop();
		m_saveTimer.Start();
	}

	void SaveFromControls() {
		if (m_loading) {
			return;
		}
		WriteSettings(CurrentSettings());
		NotifyMainApp();
	}

	void UpdatePercentText() {
		wchar_t text[64] = {};
		swprintf_s(text, L"浓度 %d%%", static_cast<int>(m_percent.Value()));
		m_percentText.Text(text);
	}

	void RefreshLiveState() {
		const std::wstring log = ReadLogTail();
		if (m_log.Text() != log) {
			m_log.Text(log);
		}
		if (!m_startupPending) {
			return;
		}
		// 提权副本可能还在跑，也可能被拒绝；状态对上了就停止等待，超时也停。
		const bool installed = StartupTaskInstalled();
		if (installed == m_startup.IsOn() || GetTickCount64() >= m_startupSettleUntil) {
			m_startupPending = false;
			m_loading = true;
			m_startup.IsOn(installed);
			m_loading = false;
		}
	}

	XamlTypeInfo::XamlControlsXamlMetaDataProvider m_metadata;
	Window m_window{ nullptr };
	Grid m_titleBar{ nullptr };
	ToggleSwitch m_mapping{ nullptr };
	ToggleSwitch m_alt{ nullptr };
	ToggleSwitch m_cursor{ nullptr };
	ToggleSwitch m_startup{ nullptr };
	Slider m_percent{ nullptr };
	TextBlock m_percentText{ nullptr };
	TextBox m_log{ nullptr };
	DispatcherTimer m_saveTimer{ nullptr };
	DispatcherTimer m_refreshTimer{ nullptr };
	bool m_loading = true;
	bool m_startupPending = false;
	ULONGLONG m_startupSettleUntil = 0;
};

}  // 匿名命名空间

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	winrt::init_apartment(winrt::apartment_type::single_threaded);

	// 已经开着一个设置窗口就把它带到前面，别开第二个。
	const HWND existing = FindWindowW(L"WinUIDesktopWin32WindowClass", kWindowTitle);
	if (existing != nullptr) {
		ShowWindow(existing, SW_RESTORE);
		SetForegroundWindow(existing);
		return 0;
	}

	Application::Start([](auto&&) { make<SettingsApp>(); });
	return 0;
}
