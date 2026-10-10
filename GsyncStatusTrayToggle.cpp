#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <process.h>
#include <utility>
#include <atomic>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>
#include "resource.h"
#include "GsyncCore.h"
#include "TrayMenu.h"

using gsync::Mode;
using gsync::State;

constexpr UINT WMAPP_TRAYICON = WM_APP + 1;
constexpr UINT WMAPP_CHANGED = WM_APP + 2;
constexpr UINT WMAPP_COMMANDDONE = WM_APP + 3;
constexpr UINT WMAPP_SHOWMENU = WM_APP + 4;
constexpr UINT WMAPP_SHOWERROR = WM_APP + 5;
constexpr UINT_PTR TrayRetryTimer = 1;
constexpr ULONGLONG ClickDelay = 5000;
constexpr wchar_t InstanceName[] = L"Local\\GsyncStatusTrayToggle.Instance";
constexpr wchar_t WindowClass[] = L"GsyncStatusTrayToggleClass";
constexpr wchar_t AppName[] = L"GsyncStatusTrayToggle";
constexpr wchar_t DriverKey[] = L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\State";
constexpr wchar_t DriverValue[] = L"0x1194f158";
constexpr wchar_t SettingsKey[] = L"Software\\GsyncStatusTrayToggle";
constexpr wchar_t SettingsValue[] = L"ToggleMode";
constexpr wchar_t StartupKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t StartupApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr DWORD StartupCommandLimit = 260;

class Handle {
public:
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { Reset(); }
    explicit operator bool() const { return value != nullptr; }

    void Reset(HANDLE h = nullptr) {
        if (*this) CloseHandle(value);
        value = h;
    }
};

struct RegKey {
    HKEY value = nullptr;
    ~RegKey() { Reset(); }

    void Reset() {
        if (value) RegCloseKey(value);
        value = nullptr;
    }
};

static HWND g_hwnd = nullptr;
static Handle g_instance;
static std::atomic<bool> g_stopping{false};
static bool g_errorDialog = false;
static bool g_exitRequested = false;
static std::wstring g_pendingError;

static UINT g_taskbarCreated = 0;
static NOTIFYICONDATAW g_nid{};
static HICON g_icons[6]{};
static bool g_lightTaskbar = false;
static bool g_iconAdded = false;
static bool g_trayRetryActive = false;
static unsigned g_trayRetries = 0;

static Mode g_preference = Mode::Fullscreen;
static bool g_preferenceErrorShown = false;

static RegKey g_driverKey;
static Handle g_watchStop;
static Handle g_watchRegistry;
static Handle g_watchThread;
static std::atomic<bool> g_refreshQueued{false};
static std::atomic<DWORD> g_watchError{ERROR_SUCCESS};

struct CommandJob {
    gsync::Action action = gsync::Action::Invalid;
    Mode preference = Mode::Fullscreen;
    gsync::Result result;
};

static CommandJob g_command;
static Handle g_commandThread;
static bool g_busy = false;
static ULONGLONG g_lastClick = 0;

static HMENU g_activeMenu = nullptr;
static traymenu::Visual* g_menuVisual = nullptr;
static traymenu::Item g_menuItems[] = {{ID_TRAY_FULLSCREEN, L"Fullscreen"},
                                       {ID_TRAY_WINDOWED, L"Fullscreen + Windowed"},
                                       {ID_TRAY_STARTUP, L"Start with Windows"},
                                       {ID_TRAY_EXIT, L"Exit"}};
static RECT g_menuIcon{};
static bool g_menuRefresh = false;

static void RefreshTray();
static void UpdateMenuChecks();

static std::wstring WindowsError(const wchar_t* action, DWORD code) {
    std::wstring message(action);
    message += L"\n\n";
    wchar_t* detail = nullptr;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<LPWSTR>(&detail), 0, nullptr) &&
        detail) {
        message += detail;
        LocalFree(detail);
        message.erase(message.find_last_not_of(L" \r\n") + 1);
    }
    message += L" (code " + std::to_wstring(code) + L")";
    return message;
}

static void ShowError(const std::wstring& text) {
    if (text.empty() || g_stopping) return;
    if (g_errorDialog || g_activeMenu) {
        if (g_pendingError.empty()) g_pendingError = text;
        return;
    }
    g_errorDialog = true;
    MessageBoxW(g_hwnd, text.c_str(), AppName, MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    g_errorDialog = false;
    if (g_exitRequested) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (!g_pendingError.empty()) ShowError(std::exchange(g_pendingError, {}));
}

static bool ReadSavedMode(Mode& mode) {
    wchar_t text[32]{};
    DWORD size = sizeof(text);
    const LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER, SettingsKey, SettingsValue, RRF_RT_REG_SZ, nullptr, text, &size);
    if (rc != ERROR_SUCCESS) return false;
    return gsync::ParseMode(text, mode);
}

static bool SaveMode(Mode mode, std::wstring& error) {
    const wchar_t* text = gsync::ModeName(mode);
    const LSTATUS rc = RegSetKeyValueW(HKEY_CURRENT_USER, SettingsKey, SettingsValue, REG_SZ, text,
                                       static_cast<DWORD>((std::wcslen(text) + 1) * sizeof(wchar_t)));
    if (rc == ERROR_SUCCESS) return true;
    error = WindowsError(L"Unable to save the preferred mode in the registry.", static_cast<DWORD>(rc));
    return false;
}

static void ReportPreferenceError(const std::wstring& error) {
    if (g_preferenceErrorShown) return;
    g_preferenceErrorShown = true;
    ShowError(error + L"\n\nThe selected mode stays in use until the tray is closed.");
}

static void SetPreference(Mode mode) {
    if (mode == g_preference) return;
    g_preference = mode;
    UpdateMenuChecks();
    std::wstring error;
    if (!SaveMode(mode, error)) ReportPreferenceError(error);
}

static void ObserveActiveMode(State state) {
    if (gsync::IsOn(state)) SetPreference(gsync::StateMode(state));
}

static bool StartupDisabledByWindows() {
    BYTE data[64]{};
    DWORD size = sizeof(data);
    const LSTATUS rc =
        RegGetValueW(HKEY_CURRENT_USER, StartupApprovedKey, AppName, RRF_RT_REG_BINARY, nullptr, data, &size);
    if (rc != ERROR_SUCCESS) return false;
    return size >= 1 && (data[0] == 0x03 || data[0] == 0x07);
}

struct StartupState {
    bool known = false;
    bool enabled = false;
    std::wstring command;
};

static StartupState ReadStartup() {
    StartupState startup;
    wchar_t path[StartupCommandLimit + 1]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length > StartupCommandLimit - 2) return startup;
    startup.command = L"\"" + std::wstring(path, length) + L"\"";
    const std::wstring& command = startup.command;

    wchar_t stored[StartupCommandLimit + 1]{};
    DWORD size = sizeof(stored);
    const LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER, StartupKey, AppName, RRF_RT_REG_SZ, nullptr, stored, &size);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND || rc == ERROR_MORE_DATA ||
        rc == ERROR_UNSUPPORTED_TYPE || rc == ERROR_INVALID_DATA) {
        startup.known = true;
        return startup;
    }
    if (rc != ERROR_SUCCESS) return startup;

    const bool registered = size == (command.size() + 1) * sizeof(wchar_t) &&
                            CompareStringOrdinal(stored, static_cast<int>(command.size()), command.c_str(),
                                                 static_cast<int>(command.size()), TRUE) == CSTR_EQUAL;
    startup.known = true;
    startup.enabled = registered && !StartupDisabledByWindows();
    return startup;
}

static void ToggleStartup() {
    if (g_stopping || g_errorDialog) return;
    const StartupState startup = ReadStartup();
    if (!startup.known) return;
    const std::wstring& command = startup.command;
    LSTATUS rc = startup.enabled ? RegDeleteKeyValueW(HKEY_CURRENT_USER, StartupKey, AppName)
                                 : RegSetKeyValueW(HKEY_CURRENT_USER, StartupKey, AppName, REG_SZ, command.c_str(),
                                                   static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    if (startup.enabled && (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND)) rc = ERROR_SUCCESS;
    if (rc != ERROR_SUCCESS) {
        ShowError(
            WindowsError(L"Unable to change the Windows startup entry of the application.", static_cast<DWORD>(rc)));
        return;
    }
    RegDeleteKeyValueW(HKEY_CURRENT_USER, StartupApprovedKey, AppName);
}

static State ReadDriverState() {
    for (int attempt = 0; attempt != 2; ++attempt) {
        if (!g_driverKey.value) {
            const LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, DriverKey, 0, KEY_QUERY_VALUE, &g_driverKey.value);
            if (rc != ERROR_SUCCESS) return State::Unknown;
        }
        DWORD value = 0, type = 0, size = sizeof(value);
        const LSTATUS rc =
            RegQueryValueExW(g_driverKey.value, DriverValue, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
        if ((rc == ERROR_KEY_DELETED || rc == ERROR_INVALID_HANDLE) && attempt == 0) {
            g_driverKey.Reset();
            continue;
        }
        if (rc == ERROR_FILE_NOT_FOUND) return gsync::DecodeState(gsync::SettingDefaults[gsync::ApplicationMode]);
        if (rc != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) return State::Unknown;
        return gsync::DecodeState(value);
    }
    return State::Unknown;
}

static void QueueRefresh() {
    if (g_stopping || g_refreshQueued.exchange(true)) return;
    if (!PostMessageW(g_hwnd, WMAPP_CHANGED, 0, 0)) g_refreshQueued = false;
}

static DWORD NextRetry(DWORD ms) {
    return ms >= 5000 ? 10000 : ms * 2;
}

static DWORD WINAPI WatchThreadProc(LPVOID) {
    RegKey key;
    DWORD retry = 500;
    HANDLE waits[] = {g_watchStop.value, g_watchRegistry.value};
    for (;;) {
        LSTATUS rc = ERROR_SUCCESS;
        if (!key.value) rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, DriverKey, 0, KEY_NOTIFY, &key.value);
        if (rc == ERROR_SUCCESS)
            rc = RegNotifyChangeKeyValue(key.value, FALSE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME,
                                         g_watchRegistry.value, TRUE);
        g_watchError = static_cast<DWORD>(rc);
        QueueRefresh();
        if (rc == ERROR_SUCCESS) {
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
            retry = 500;
            continue;
        }
        key.Reset();
        if (WaitForSingleObject(g_watchStop.value, retry) != WAIT_TIMEOUT) break;
        retry = NextRetry(retry);
    }
    return 0;
}

static unsigned __stdcall CommandThreadProc(void*) {
    try {
        g_command.result = gsync::Execute(g_command.action, g_command.preference);
    } catch (...) {
        g_command.result = gsync::Failure(gsync::ExitCode::InternalError, L"The G-SYNC command hit an internal error.");
    }
    while (!g_stopping && !PostMessageW(g_hwnd, WMAPP_COMMANDDONE, 0, 0)) Sleep(50);
    return 0;
}

static void FinishCommand() {
    WaitForSingleObject(g_commandThread.value, INFINITE);
    g_commandThread.Reset();
    g_busy = false;

    gsync::Result result = std::move(g_command.result);
    if (result.code == gsync::ExitCode::Success && result.hasRemembered) SetPreference(result.remembered);
    UpdateMenuChecks();
    RefreshTray();
    ShowError(result.error);
    if (g_exitRequested) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

static void StartCommand(gsync::Action action) {
    if (g_busy || g_errorDialog || g_stopping || g_exitRequested) return;
    g_command.action = action;
    g_command.preference = g_preference;
    g_command.result = {};
    g_busy = true;
    UpdateMenuChecks();

    g_commandThread.Reset(reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, CommandThreadProc, nullptr, 0, nullptr)));
    if (!g_commandThread) {
        g_busy = false;
        UpdateMenuChecks();
        ShowError(L"Unable to start the G-SYNC command: not enough resources.");
    }
}

static void RunToggle() {
    if (g_busy || g_errorDialog || g_stopping) return;
    const ULONGLONG now = GetTickCount64();
    if (g_lastClick && now - g_lastClick < ClickDelay) return;
    g_lastClick = now;
    StartCommand(gsync::Action::Toggle);
}

static HICON StateIcon(State state) {
    switch (state) {
    case State::Off:
        return g_icons[g_lightTaskbar ? 4 : 0];
    case State::Fullscreen:
        return g_icons[1];
    case State::Windowed:
        return g_icons[2];
    default:
        return g_icons[g_lightTaskbar ? 5 : 3];
    }
}

static void SetTrayState(State state) {
    const wchar_t* tip = L"G-SYNC Unknown";
    switch (state) {
    case State::Off:
        tip = L"G-SYNC Off";
        break;
    case State::Fullscreen:
        tip = L"G-SYNC Fullscreen";
        break;
    case State::Windowed:
        tip = L"G-SYNC Fullscreen + Windowed";
        break;
    default:
        break;
    }
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.hIcon = StateIcon(state);
    lstrcpynW(g_nid.szTip, tip, static_cast<int>(std::size(g_nid.szTip)));
}

static void StopTrayRetry() {
    if (g_trayRetryActive) KillTimer(g_hwnd, TrayRetryTimer);
    g_trayRetryActive = false;
    g_trayRetries = 0;
}

static void ScheduleTrayRetry() {
    if (g_trayRetryActive || g_stopping) return;
    if (!SetTimer(g_hwnd, TrayRetryTimer, 1000, nullptr)) {
        ShowError(WindowsError(L"Unable to schedule the tray icon restore.", GetLastError()));
        return;
    }
    g_trayRetryActive = true;
}

static bool AddTrayIcon(State state) {
    SetTrayState(state);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid) && !Shell_NotifyIconW(NIM_MODIFY, &g_nid)) return false;
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &g_nid)) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        return false;
    }
    g_iconAdded = true;
    StopTrayRetry();
    return true;
}

static void RefreshTray() {
    if (g_stopping) return;
    const State state = g_watchError.load() == ERROR_SUCCESS ? ReadDriverState() : State::Unknown;
    if (!g_iconAdded) {
        if (!AddTrayIcon(state)) ScheduleTrayRetry();
    } else if (StateIcon(state) != g_nid.hIcon) {
        SetTrayState(state);
        if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid)) {
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            g_iconAdded = false;
            if (!AddTrayIcon(state)) ScheduleTrayRetry();
        }
    }

    if (!g_busy) ObserveActiveMode(state);
}

static void RemoveTrayIcon() {
    StopTrayRetry();
    if (g_iconAdded) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_iconAdded = false;
}

static void UpdateMenuChecks() {
    if (!g_activeMenu) return;
    const UINT selected = g_preference == Mode::Fullscreen ? ID_TRAY_FULLSCREEN : ID_TRAY_WINDOWED;
    for (const UINT id : {static_cast<UINT>(ID_TRAY_FULLSCREEN), static_cast<UINT>(ID_TRAY_WINDOWED)}) {
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_STATE;
        item.fState =
            static_cast<UINT>((id == selected ? MFS_CHECKED : MFS_UNCHECKED) | (g_busy ? MFS_GRAYED : MFS_ENABLED));
        const UINT previous = GetMenuState(g_activeMenu, id, MF_BYCOMMAND);
        if (previous != static_cast<UINT>(-1)) item.fState |= previous & MFS_HILITE;
        SetMenuItemInfoW(g_activeMenu, id, FALSE, &item);
    }
}

static void DispatchMenuCommand(UINT command) {
    if (command == ID_TRAY_EXIT)
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    else if (command == ID_TRAY_FULLSCREEN)
        StartCommand(gsync::Action::SelectFullscreen);
    else if (command == ID_TRAY_WINDOWED)
        StartCommand(gsync::Action::SelectWindowed);
    else if (command == ID_TRAY_STARTUP)
        ToggleStartup();
}

static bool IsRightButtonMessage(UINT message) {
    switch (message) {
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONUP:
    case WM_NCRBUTTONDBLCLK:
        return true;
    default:
        return false;
    }
}

static LRESULT CALLBACK MenuFilterProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == MSGF_MENU && g_menuVisual) {
        const MSG& msg = *reinterpret_cast<const MSG*>(lParam);
        if (IsRightButtonMessage(msg.message)) {
            if (g_menuVisual->Contains(msg.pt)) return TRUE;
            if (PtInRect(&g_menuIcon, msg.pt)) {
                const bool pressed = msg.message != WM_RBUTTONUP && msg.message != WM_NCRBUTTONUP;
                if (pressed && !g_menuRefresh) {
                    g_menuRefresh = true;
                    EndMenu();
                }
                return TRUE;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static void ShowTrayMenu(const POINT* shellAnchor) {
    if (g_errorDialog || g_activeMenu || g_stopping) return;

    traymenu::Placement placement;
    if (!traymenu::Locate(g_hwnd, g_nid.uID, shellAnchor, placement)) return;
    g_menuIcon = placement.icon;
    if (!SetWindowPos(g_hwnd, nullptr, placement.point.x, placement.point.y, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE))
        return;
    RefreshTray();
    if (g_stopping || g_exitRequested) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        ShowError(WindowsError(L"Unable to open the menu.", GetLastError()));
        return;
    }
    traymenu::Visual visual;
    if (!visual.Prepare(g_hwnd, traymenu::MonitorDpi(placement.point), menu, g_menuItems, std::size(g_menuItems))) {
        DestroyMenu(menu);
        ShowError(L"Unable to prepare the menu.");
        return;
    }

    const StartupState startup = ReadStartup();
    MENUITEMINFOW startupItem{};
    startupItem.cbSize = sizeof(startupItem);
    startupItem.fMask = MIIM_STATE;
    startupItem.fState =
        static_cast<UINT>((startup.enabled ? MFS_CHECKED : MFS_UNCHECKED) | (startup.known ? MFS_ENABLED : MFS_GRAYED));
    if (!SetMenuItemInfoW(menu, ID_TRAY_STARTUP, FALSE, &startupItem)) {
        const DWORD code = GetLastError();
        DestroyMenu(menu);
        ShowError(WindowsError(L"Unable to prepare the Start with Windows option.", code));
        return;
    }

    g_activeMenu = menu;
    g_menuVisual = &visual;
    UpdateMenuChecks();

    BOOL shown = FALSE;
    bool activated = false;
    {
        traymenu::OwnerScope owner(g_hwnd);
        activated = owner.Activate();
        if (activated) {
            g_menuRefresh = false;
            const HHOOK filter = SetWindowsHookExW(WH_MSGFILTER, MenuFilterProc, nullptr, GetCurrentThreadId());
            shown = TrackPopupMenuEx(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, placement.point.x, placement.point.y,
                                     g_hwnd, nullptr);
            if (filter) UnhookWindowsHookEx(filter);
        }
    }

    const bool refresh = g_menuRefresh && !g_stopping && !g_exitRequested && g_pendingError.empty();
    g_menuRefresh = false;
    g_menuVisual = nullptr;
    g_activeMenu = nullptr;
    DestroyMenu(menu);
    if (refresh) {
        PostMessageW(g_hwnd, WMAPP_SHOWMENU, 0, 0);
        return;
    }

    Shell_NotifyIconW(NIM_SETFOCUS, &g_nid);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    if (activated && !shown) ShowError(L"Unable to show the menu.");
    if (!g_pendingError.empty()) PostMessageW(g_hwnd, WMAPP_SHOWERROR, 0, 0);
}

static void Shutdown(HWND hwnd) {
    g_stopping = true;
    RemoveTrayIcon();
    DestroyWindow(hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_taskbarCreated) {
        g_iconAdded = false;
        StopTrayRetry();
        RefreshTray();
        return 0;
    }
    switch (msg) {
    case WM_ENTERIDLE:
        if (wParam == MSGF_MENU && g_menuVisual) {
            g_menuVisual->RoundCorners();
            return 0;
        }
        break;
    case WM_DPICHANGED:
        if (g_activeMenu) EndMenu();
        return 0;
    case WM_SETTINGCHANGE:
        if (traymenu::TaskbarIsLight() != g_lightTaskbar) {
            g_lightTaskbar = !g_lightTaskbar;
            if (g_iconAdded) RefreshTray();
        }
        return 0;
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (g_menuVisual && item->CtlType == ODT_MENU) {
            g_menuVisual->Measure(*item);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (g_menuVisual && item->CtlType == ODT_MENU) return g_menuVisual->Draw(*item) ? TRUE : FALSE;
        break;
    }
    case WM_MENUCHAR: {
        if (!g_activeMenu || reinterpret_cast<HMENU>(lParam) != g_activeMenu) break;
        wchar_t key = static_cast<wchar_t>(LOWORD(wParam));
        if (key >= L'a' && key <= L'z') key = static_cast<wchar_t>(key - L'a' + L'A');
        int first = -1, next = -1, current = -1, matches = 0;
        const int count = static_cast<int>(std::size(g_menuItems));
        for (int i = 0; i < count; ++i)
            if (GetMenuState(g_activeMenu, g_menuItems[i].id, MF_BYCOMMAND) & MF_HILITE) current = i;
        for (int i = 0; i < count; ++i) {
            const UINT state = GetMenuState(g_activeMenu, g_menuItems[i].id, MF_BYCOMMAND);
            if ((state & (MF_GRAYED | MF_DISABLED)) || key != g_menuItems[i].text[0]) continue;
            if (first < 0) first = i;
            if (i > current && next < 0) next = i;
            ++matches;
        }
        if (!matches) return MAKELRESULT(0, MNC_IGNORE);
        return MAKELRESULT(next < 0 ? first : next, matches == 1 ? MNC_EXECUTE : MNC_SELECT);
    }
    case WMAPP_TRAYICON: {
        const UINT event = LOWORD(lParam);
        if (event == NIN_SELECT || event == NIN_KEYSELECT) {
            RunToggle();
        } else if (event == WM_CONTEXTMENU) {
            const POINT anchor{static_cast<short>(LOWORD(wParam)), static_cast<short>(HIWORD(wParam))};
            ShowTrayMenu(&anchor);
        }
        return 0;
    }
    case WMAPP_CHANGED:
        g_refreshQueued = false;
        RefreshTray();
        return 0;
    case WM_TIMER:
        if (wParam == TrayRetryTimer && g_trayRetryActive) {
            ++g_trayRetries;
            RefreshTray();
            if (g_trayRetryActive && g_trayRetries >= 10) {
                StopTrayRetry();
                ShowError(
                    L"Unable to restore the tray icon. Restart GsyncStatusTrayToggle once the taskbar is available.");
            }
        }
        return 0;
    case WMAPP_SHOWMENU:
        ShowTrayMenu(nullptr);
        return 0;
    case WMAPP_SHOWERROR:
        ShowError(std::exchange(g_pendingError, {}));
        return 0;
    case WMAPP_COMMANDDONE:
        FinishCommand();
        return 0;
    case WM_COMMAND:
        DispatchMenuCommand(LOWORD(wParam));
        return 0;
    case WM_ENDSESSION:
        if (wParam && !g_stopping) {
            Shutdown(hwnd);
            if (g_commandThread) WaitForSingleObject(g_commandThread.value, INFINITE);
        }
        return 0;
    case WM_CLOSE:
        if (g_busy || g_errorDialog)
            g_exitRequested = true;
        else
            Shutdown(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool StartWatcher(std::wstring& error) {
    g_watchStop.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    g_watchRegistry.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!g_watchStop || !g_watchRegistry) {
        error = WindowsError(L"Unable to prepare the change monitoring.", GetLastError());
        return false;
    }
    g_watchThread.Reset(CreateThread(nullptr, 0, WatchThreadProc, nullptr, 0, nullptr));
    if (!g_watchThread) {
        error = WindowsError(L"Unable to start the change monitoring.", GetLastError());
        return false;
    }
    SetThreadPriority(g_watchThread.value, THREAD_PRIORITY_LOWEST);
    return true;
}

static void Cleanup() {
    g_stopping = true;
    if (g_commandThread) WaitForSingleObject(g_commandThread.value, INFINITE);
    if (g_watchStop) SetEvent(g_watchStop.value);
    if (g_watchThread) WaitForSingleObject(g_watchThread.value, INFINITE);

    g_commandThread.Reset();
    g_watchThread.Reset();
    g_watchStop.Reset();
    g_watchRegistry.Reset();
    g_driverKey.Reset();

    if (g_hwnd && IsWindow(g_hwnd)) {
        RemoveTrayIcon();
        DestroyWindow(g_hwnd);
    }
    g_hwnd = nullptr;

    for (HICON& icon : g_icons) {
        if (icon) DestroyIcon(icon);
        icon = nullptr;
    }
    g_instance.Reset();
}

static bool RegistryNeedsAdmin() {
    RegKey key;
    const LSTATUS rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, DriverKey, 0, KEY_QUERY_VALUE | KEY_NOTIFY, &key.value);
    return rc == ERROR_ACCESS_DENIED;
}

static bool ReadElevation(TOKEN_ELEVATION_TYPE& type, std::wstring& error) {
    Handle token;
    DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) ||
        !GetTokenInformation(token.value, TokenElevationType, &type, sizeof(type), &size)) {
        error = WindowsError(L"Unable to check the program's privileges.", GetLastError());
        return false;
    }
    return true;
}

static bool RelaunchElevated(std::wstring& error) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        error = L"Unable to locate the application to restart it as administrator.";
        return false;
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = path.data();
    info.nShow = SW_SHOWNORMAL;
    const BOOL launched = ShellExecuteExW(&info);
    const DWORD code = launched ? ERROR_SUCCESS : GetLastError();
    if (SUCCEEDED(com)) CoUninitialize();
    if (!launched && code != ERROR_CANCELLED)
        error = WindowsError(L"Unable to restart the tray as administrator.", code);
    return launched != FALSE;
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return static_cast<int>(gsync::ExitCode::Arguments);
    const gsync::Action action = gsync::ParseArguments(argc, argv);
    LocalFree(argv);

    if (action == gsync::Action::Invalid) return static_cast<int>(gsync::ExitCode::Arguments);
    if (action != gsync::Action::Tray) {
        try {
            return static_cast<int>(gsync::Execute(action).code);
        } catch (...) {
            return static_cast<int>(gsync::ExitCode::InternalError);
        }
    }

    {
        Handle existing(OpenMutexW(SYNCHRONIZE, FALSE, InstanceName));
        if (existing || GetLastError() == ERROR_ACCESS_DENIED) return 0;
    }

    std::wstring error;
    if (RegistryNeedsAdmin()) {
        TOKEN_ELEVATION_TYPE elevation = TokenElevationTypeDefault;
        if (!ReadElevation(elevation, error)) {
            ShowError(error);
            return 1;
        }
        if (elevation == TokenElevationTypeLimited) {
            const bool launched = RelaunchElevated(error);
            ShowError(error);
            return launched ? 0 : 1;
        }
        if (elevation == TokenElevationTypeDefault) {
            ShowError(
                L"The G-SYNC state can only be read with administrator rights, and this account can't get them. Start the tray from an administrator account.");
            return 1;
        }
    }

    g_instance.Reset(CreateMutexExW(nullptr, InstanceName, 0, SYNCHRONIZE));
    const DWORD instanceError = GetLastError();
    if (instanceError == ERROR_ALREADY_EXISTS || (!g_instance && instanceError == ERROR_ACCESS_DENIED)) return 0;
    if (!g_instance) {
        ShowError(WindowsError(L"Unable to check whether the tray is already running.", instanceError));
        return 1;
    }

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (!g_taskbarCreated) {
        ShowError(WindowsError(L"Unable to prepare the tray icon restore.", GetLastError()));
        return 1;
    }

    const int ids[] = {IDI_ICON_OFF,     IDI_ICON_FULLSCREEN, IDI_ICON_WINDOWED,
                       IDI_ICON_UNKNOWN, IDI_ICON_OFF_LIGHT,  IDI_ICON_UNKNOWN_LIGHT};
    static_assert(std::size(ids) == std::size(g_icons), "One resource per icon");
    const int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    for (std::size_t i = 0; i < std::size(ids); ++i) {
        g_icons[i] = static_cast<HICON>(
            LoadImageW(instance, MAKEINTRESOURCEW(ids[i]), IMAGE_ICON, iconSize, iconSize, LR_DEFAULTCOLOR));
        if (!g_icons[i]) {
            ShowError(WindowsError(L"Unable to load an icon.", GetLastError()));
            return 1;
        }
    }

    g_lightTaskbar = traymenu::TaskbarIsLight();
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = WindowClass;
    if (!RegisterClassW(&wc)) {
        ShowError(WindowsError(L"Unable to prepare the tray window.", GetLastError()));
        return 1;
    }

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, WindowClass, AppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                             nullptr);
    if (!g_hwnd) {
        ShowError(WindowsError(L"Unable to create the tray window.", GetLastError()));
        return 1;
    }

    if (!ChangeWindowMessageFilterEx(g_hwnd, g_taskbarCreated, MSGFLT_ALLOW, nullptr)) {
        const DWORD code = GetLastError();
        TOKEN_ELEVATION_TYPE elevation = TokenElevationTypeDefault;
        std::wstring ignored;
        if (ReadElevation(elevation, ignored) && elevation == TokenElevationTypeFull)
            ShowError(WindowsError(
                L"Windows denied the permission used to restore the tray icon after Explorer restarts. The tray keeps running, but the icon may not come back in that case.",
                code));
    }

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WMAPP_TRAYICON;

    ReadSavedMode(g_preference);
    if (!StartWatcher(error)) {
        ShowError(error);
        Cleanup();
        return 1;
    }
    RefreshTray();

    int exitCode = 0;
    MSG msg{};
    for (;;) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) break;
        if (got == -1) {
            ShowError(WindowsError(L"Error in the Windows message loop.", GetLastError()));
            exitCode = 1;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Cleanup();
    UnregisterClassW(WindowClass, instance);
    return exitCode;
}
