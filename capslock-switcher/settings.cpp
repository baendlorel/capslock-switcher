#include "settings.h"

#include "app.h"
#include "logging.h"
#include "resource.h"
#include "surface.h"

#include <shellapi.h>

#include <memory>

namespace {

constexpr wchar_t kSettingsClass[] = L"CapsLockSwitcherSettings";
constexpr wchar_t kSettingsTitle[] = L"CapsLock Switcher 设置";

// 控件 ID。
constexpr int kIdChkMapping = 101;
constexpr int kIdChkAlt = 102;
constexpr int kIdChkStartup = 103;
constexpr int kIdOpenLog = 104;

constexpr UINT_PTR kTimerRefresh = 1;
constexpr UINT kRefreshMs = 1000;

// 日志框里最多显示最后这么多行，最多往回读这么多字节。
constexpr int kLogLines = 400;
constexpr DWORD kLogTailBytes = 64 * 1024;

// 勾上"开机启动"之后要等提权副本干完，缓存才会变成新值。这段时间里不要拿旧值
// 把勾选弹回去，否则用户会看到勾上、取消、又勾上。
constexpr ULONGLONG kStartupPendingMs = 20000;

HWND g_wnd = nullptr;
HWND g_chkMapping = nullptr;
HWND g_chkAlt = nullptr;
HWND g_chkStartup = nullptr;
HWND g_btnOpenLog = nullptr;
HWND g_logView = nullptr;
HFONT g_font = nullptr;
ULONGLONG g_logSize = 0;  // 上次读到的日志文件大小
bool g_startupPending = false;
ULONGLONG g_startupPendingUntil = 0;

// 日志框显示的是日志文件的尾部：文件一变就整段重读，没变就什么都不做。
bool ReloadLogTail() {
	if (g_logView == nullptr || LogPath()[0] == L'\0') {
		return false;
	}
	const HANDLE file = CreateFileW(LogPath(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}

	LARGE_INTEGER size = {};
	if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart <= 0) {
		CloseHandle(file);
		return false;
	}
	if (static_cast<ULONGLONG>(size.QuadPart) == g_logSize) {
		CloseHandle(file);
		return false;
	}
	g_logSize = static_cast<ULONGLONG>(size.QuadPart);

	const DWORD want = static_cast<DWORD>(size.QuadPart > kLogTailBytes ? kLogTailBytes
	                                                                    : size.QuadPart);
	LARGE_INTEGER from = {};
	from.QuadPart = size.QuadPart - want;
	if (SetFilePointerEx(file, from, nullptr, FILE_BEGIN) == FALSE) {
		CloseHandle(file);
		return false;
	}
	std::unique_ptr<char[]> bytes(new (std::nothrow) char[want]);
	if (bytes == nullptr) {
		CloseHandle(file);
		return false;
	}
	DWORD got = 0;
	const bool read = ReadFile(file, bytes.get(), want, &got, nullptr) != FALSE;
	CloseHandle(file);
	if (!read || got == 0) {
		return false;
	}

	// 从文件中间读起时头一行多半是半截的，直接丢掉；从头读则跳过 UTF-8 的
	// 字节序标记（BOM）。
	size_t begin = 0;
	if (size.QuadPart > want) {
		while (begin < got && bytes[begin] != '\n') {
			++begin;
		}
		++begin;
	} else if (got >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
	           static_cast<unsigned char>(bytes[1]) == 0xBB &&
	           static_cast<unsigned char>(bytes[2]) == 0xBF) {
		begin = 3;
	}
	if (begin >= got) {
		return false;
	}

	const int count = static_cast<int>(got - begin);
	const int wideChars = MultiByteToWideChar(CP_UTF8, 0, bytes.get() + begin, count, nullptr, 0);
	if (wideChars <= 0) {
		return false;
	}
	std::unique_ptr<wchar_t[]> text(new (std::nothrow) wchar_t[static_cast<size_t>(wideChars) + 1]);
	if (text == nullptr) {
		return false;
	}
	MultiByteToWideChar(CP_UTF8, 0, bytes.get() + begin, count, text.get(), wideChars);
	text[wideChars] = L'\0';

	// 只留最后 kLogLines 行，日志再长编辑框也不会越来越笨重。
	int lines = 0;
	size_t start = static_cast<size_t>(wideChars);
	while (start > 0 && lines < kLogLines) {
		--start;
		if (text[start] == L'\n') {
			++lines;
		}
	}
	const wchar_t* shown = start > 0 ? text.get() + start + 1 : text.get();

	SetWindowTextW(g_logView, shown);
	SendMessageW(g_logView, EM_SETSEL, static_cast<WPARAM>(-1), -1);  // 滚到最后一行
	SendMessageW(g_logView, EM_SCROLLCARET, 0, 0);
	return true;
}

HFONT MakeUiFont(const UINT dpi) {
	return CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
	                   FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
	                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void ApplyFont(const HWND parent, const HFONT font) {
	SendMessageW(parent, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	for (HWND child = GetWindow(parent, GW_CHILD); child != nullptr;
	     child = GetWindow(child, GW_HWNDNEXT)) {
		SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	}
}

void CreateControls(const HWND wnd) {
	g_font = MakeUiFont(ScreenDpi(wnd, wnd));

	g_chkMapping = CreateWindowExW(
	    0, L"BUTTON", L"启用映射 (CapsLock -> Ctrl+Space)",
	    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, wnd,
	    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkMapping)), g_hInst, nullptr);
	g_chkAlt = CreateWindowExW(
	    0, L"BUTTON", L"Alt+CapsLock = 原来的大写锁定",
	    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, wnd,
	    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkAlt)), g_hInst, nullptr);
	g_chkStartup = CreateWindowExW(
	    0, L"BUTTON", L"开机启动 (管理员)",
	    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, wnd,
	    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkStartup)), g_hInst, nullptr);
	g_btnOpenLog = CreateWindowExW(
	    0, L"BUTTON", L"打开日志文件",
	    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, wnd,
	    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdOpenLog)), g_hInst, nullptr);
	g_logView = CreateWindowExW(
	    WS_EX_CLIENTEDGE, L"EDIT", L"",
	    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
	        ES_AUTOVSCROLL,
	    0, 0, 0, 0, wnd, nullptr, g_hInst, nullptr);

	ApplyFont(wnd, g_font);
}

// 所有尺寸都用 DIP 写，在这里按窗口所在显示器的 DPI 缩放。
void LayoutChildren(const HWND wnd) {
	RECT client = {};
	GetClientRect(wnd, &client);
	const int dpi = static_cast<int>(ScreenDpi(wnd, wnd));
	const int margin = MulDiv(16, dpi, 96);
	const int row = MulDiv(28, dpi, 96);
	const int gap = MulDiv(6, dpi, 96);
	const int buttonW = MulDiv(140, dpi, 96);
	const int buttonH = MulDiv(30, dpi, 96);
	const int inner = client.right - client.left - margin * 2;

	int y = margin;
	MoveWindow(g_chkMapping, margin, y, inner, row, TRUE);
	y += row;
	MoveWindow(g_chkAlt, margin, y, inner, row, TRUE);
	y += row;
	MoveWindow(g_chkStartup, margin, y, inner, row, TRUE);
	y += row + gap;
	MoveWindow(g_btnOpenLog, margin, y, buttonW, buttonH, TRUE);
	y += buttonH + gap;
	MoveWindow(g_logView, margin, y, inner, client.bottom - margin - y, TRUE);
}

// 把两个勾选同步成真实状态。BM_SETCHECK 不发通知，所以不会和用户的点击打架。
void SyncControls() {
	if (g_chkMapping != nullptr) {
		SendMessageW(g_chkMapping, BM_SETCHECK, g_enabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);
	}
	if (g_chkAlt != nullptr) {
		SendMessageW(g_chkAlt, BM_SETCHECK,
		             g_altPassThrough.load() ? BST_CHECKED : BST_UNCHECKED, 0);
	}
	if (g_chkStartup == nullptr) {
		return;
	}
	const bool shown = SendMessageW(g_chkStartup, BM_GETCHECK, 0, 0) == BST_CHECKED;
	if (g_startupPending && g_startupTaskInstalled != shown &&
	    GetTickCount64() < g_startupPendingUntil) {
		return;  // 还在等提权副本，先不要让旧值把它弹回去
	}
	g_startupPending = false;
	SendMessageW(g_chkStartup, BM_SETCHECK,
	             g_startupTaskInstalled ? BST_CHECKED : BST_UNCHECKED, 0);
}

LRESULT CALLBACK SettingsProc(const HWND hwnd, const UINT message, const WPARAM wParam,
                              const LPARAM lParam) {
	switch (message) {
	case WM_CREATE:
		CreateControls(hwnd);
		SyncControls();
		return 0;

	case WM_SIZE:
		LayoutChildren(hwnd);
		return 0;

	case WM_GETMINMAXINFO: {
		const int dpi = static_cast<int>(ScreenDpi(hwnd, hwnd));
		auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
		info->ptMinTrackSize.x = MulDiv(480, dpi, 96);
		info->ptMinTrackSize.y = MulDiv(380, dpi, 96);
		return 0;
	}

	case WM_DPICHANGED: {
		// 换到别的显示器就换一套字号的字体，再照系统给的位置重排一遍。
		const HFONT font = MakeUiFont(HIWORD(wParam));
		if (font != nullptr) {
			if (g_font != nullptr) {
				DeleteObject(g_font);
			}
			g_font = font;
			ApplyFont(hwnd, g_font);
		}
		const auto* suggested = reinterpret_cast<const RECT*>(lParam);
		SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
		             suggested->right - suggested->left, suggested->bottom - suggested->top,
		             SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}

	case WM_SHOWWINDOW:
		if (wParam != FALSE) {
			g_logSize = 0;  // 重新打开时整段读一遍
			ReloadLogTail();
			SyncControls();
			SetTimer(hwnd, kTimerRefresh, kRefreshMs, nullptr);
		} else {
			KillTimer(hwnd, kTimerRefresh);
		}
		return 0;

	case WM_CTLCOLORSTATIC: {
		// 只读的多行编辑框也走这条消息，所以日志框要单独给白底。其余控件都坐在窗口
		// 底色上：必须把文字背景也设成同一个颜色，不然文字会拖出一块白底。
		const HWND control = reinterpret_cast<const HWND>(lParam);
		const HDC dc = reinterpret_cast<HDC>(wParam);
		if (control == g_logView) {
			SetBkColor(dc, GetSysColor(COLOR_WINDOW));
			SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
			return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
		}
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
		return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
	}

	case WM_CTLCOLORBTN:
		// 复选框的标签走的是这条消息。
		SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
		return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));

	case WM_TIMER:
		if (wParam == kTimerRefresh) {
			ReloadLogTail();
			SyncControls();
		}
		return 0;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case kIdChkMapping:
			SetMappingEnabled(SendMessageW(g_chkMapping, BM_GETCHECK, 0, 0) == BST_CHECKED);
			return 0;
		case kIdChkAlt:
			SetAltCapsLockEnabled(SendMessageW(g_chkAlt, BM_GETCHECK, 0, 0) == BST_CHECKED);
			return 0;
		case kIdChkStartup:
			g_startupPending = true;
			g_startupPendingUntil = GetTickCount64() + kStartupPendingMs;
			SetStartupEnabled(SendMessageW(g_chkStartup, BM_GETCHECK, 0, 0) == BST_CHECKED);
			return 0;
		case IDCANCEL:  // IsDialogMessage 把 Esc 变成这个命令
			ShowWindow(hwnd, SW_HIDE);
			return 0;
		case kIdOpenLog:
			if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", LogPath(), nullptr,
			                                            nullptr, SW_SHOWNORMAL)) <= 32) {
				Log(L"settings: could not open the log file (%lu)", GetLastError());
			}
			return 0;
		}
		return 0;

	case WM_CLOSE:
		ShowWindow(hwnd, SW_HIDE);  // 关窗口只是收起来，托盘里还在跑
		return 0;

	case WM_DESTROY:
		g_wnd = nullptr;
		return 0;
	}
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // 匿名命名空间

void CreateSettingsWindow(const HINSTANCE instance) {
	WNDCLASSEX wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.lpfnWndProc = SettingsProc;
	wcex.hInstance = instance;
	wcex.lpszClassName = kSettingsClass;
	wcex.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_MAINICON));
	wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	if (RegisterClassExW(&wcex) == 0) {
		Log(L"settings: RegisterClassEx failed (%lu)", GetLastError());
		return;
	}

	const int dpi = ScreenDpi(nullptr, nullptr);
	g_wnd = CreateWindowExW(0, kSettingsClass, kSettingsTitle, WS_OVERLAPPEDWINDOW,
	                        CW_USEDEFAULT, 0, MulDiv(620, dpi, 96), MulDiv(460, dpi, 96),
	                        nullptr, nullptr, instance, nullptr);
	if (g_wnd == nullptr) {
		Log(L"settings: CreateWindowEx failed (%lu)", GetLastError());
	}
}

void OpenSettingsWindow() {
	if (g_wnd == nullptr) {
		CreateSettingsWindow(g_hInst);
	}
	if (g_wnd == nullptr) {
		return;
	}
	ShowWindow(g_wnd, SW_SHOWNORMAL);  // 最小化的话顺便还原
	SetForegroundWindow(g_wnd);
}

bool HandleSettingsMessage(MSG* message) {
	return g_wnd != nullptr && message != nullptr && IsDialogMessageW(g_wnd, message) != FALSE;
}

void DestroySettingsWindow() {
	if (g_wnd != nullptr) {
		DestroyWindow(g_wnd);  // WM_DESTROY 里已把 g_wnd 清掉
	}
	if (g_font != nullptr) {
		DeleteObject(g_font);
		g_font = nullptr;
	}
}
