#pragma once
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <oleacc.h>
#include <dwmapi.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>

namespace traymenu
{

inline bool TaskbarIsLight()
{
    DWORD light = 0;
    DWORD size = sizeof(light);
    const LSTATUS rc =
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return rc == ERROR_SUCCESS && light != 0;
}

enum class Kind
{
    Command,
    Title,
    Separator
};

struct Item
{
    // Screen readers find MSAAMENUINFO at the start of the item data, so it stays the first member.
    MSAAMENUINFO accessible{};
    UINT id = 0;
    const wchar_t* text = L"";
    Kind kind = Kind::Command;
};

inline Item MakeItem(UINT id, const wchar_t* text, Kind kind = Kind::Command)
{
    Item item;
    item.id = id;
    item.text = text;
    item.kind = kind;
    return item;
}

// Windows does not match typed letters to owner-drawn items, so this does it as a standard menu would for items
// without an underlined letter.
inline LRESULT MenuKey(HMENU menu, const Item* items, std::size_t count, wchar_t typed)
{
    const std::wint_t key = std::towupper(static_cast<std::wint_t>(typed));
    int first = -1, next = -1, current = -1, matches = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (GetMenuState(menu, static_cast<UINT>(i), MF_BYPOSITION) & MF_HILITE)
            current = static_cast<int>(i);
    for (std::size_t i = 0; i < count; ++i)
    {
        const UINT state = GetMenuState(menu, static_cast<UINT>(i), MF_BYPOSITION);
        if (items[i].kind != Kind::Command || (state & (MF_GRAYED | MF_DISABLED)) ||
            key != std::towupper(static_cast<std::wint_t>(items[i].text[0])))
            continue;
        const int position = static_cast<int>(i);
        if (first < 0)
            first = position;
        if (position > current && next < 0)
            next = position;
        ++matches;
    }
    if (!matches)
        return MAKELRESULT(0, MNC_IGNORE);
    return MAKELRESULT(next < 0 ? first : next, matches == 1 ? MNC_EXECUTE : MNC_SELECT);
}

struct Placement
{
    POINT point{};
    RECT icon{};
};

inline bool Locate(HWND owner, UINT iconId, const POINT* shellAnchor, Placement& result)
{
    NOTIFYICONIDENTIFIER icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = owner;
    icon.uID = iconId;
    RECT rect{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&icon, &rect)) && rect.right > rect.left && rect.bottom > rect.top)
    {
        result.point = {rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2};
        result.icon = rect;
        return true;
    }

    if (shellAnchor)
    {
        result.point = *shellAnchor;
        return true;
    }
    return GetCursorPos(&result.point) != FALSE;
}

inline UINT MonitorDpi(POINT point)
{
    UINT dpiX = 0;
    UINT dpiY = 0;
    const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        return dpiX;
    return GetDpiForSystem();
}

inline BOOL CALLBACK FindMenuWindow(HWND window, LPARAM parameter)
{
    wchar_t name[16]{};
    if (!IsWindowVisible(window) || GetClassNameW(window, name, static_cast<int>(std::size(name))) <= 0 ||
        std::wcscmp(name, L"#32768") != 0)
        return TRUE;
    *reinterpret_cast<HWND*>(parameter) = window;
    return FALSE;
}

class OwnerScope
{
    HWND window_;

public:
    explicit OwnerScope(HWND hwnd) : window_(hwnd)
    {
    }
    OwnerScope(const OwnerScope&) = delete;
    OwnerScope& operator=(const OwnerScope&) = delete;

    bool Activate() const
    {
        if (!SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW))
            return false;
        return SetForegroundWindow(window_) != FALSE;
    }

    ~OwnerScope()
    {
        if (IsWindow(window_))
            ShowWindow(window_, SW_HIDE);
    }
};

class Visual
{
    HFONT font_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HWND menuWindow_ = nullptr;
    int dpi_ = 96;
    COLORREF background_ = 0, foreground_ = 0, highlight_ = 0, selectedText_ = 0, disabledText_ = 0, line_ = 0;
    UINT width_ = 0, height_ = 0, separatorHeight_ = 0;
    DWORD error_ = ERROR_SUCCESS;
    int Scale(int value) const
    {
        return MulDiv(value, dpi_, 96);
    }

    bool Fail()
    {
        error_ = GetLastError();
        return false;
    }

public:
    Visual() = default;
    Visual(const Visual&) = delete;
    Visual& operator=(const Visual&) = delete;

    ~Visual()
    {
        if (font_)
            DeleteObject(font_);
        if (backgroundBrush_)
            DeleteObject(backgroundBrush_);
    }

    bool Prepare(HWND owner, UINT menuDpi, HMENU menu, Item* items, std::size_t count)
    {
        dpi_ = static_cast<int>(menuDpi);
        SetLastError(ERROR_SUCCESS);

        HIGHCONTRASTW contrast{};
        contrast.cbSize = sizeof(contrast);
        const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                                  (contrast.dwFlags & HCF_HIGHCONTRASTON);
        const bool dark = !highContrast && !TaskbarIsLight();
        background_ = highContrast ? GetSysColor(COLOR_MENU) : dark ? RGB(32, 32, 32) : RGB(249, 249, 249);
        foreground_ = highContrast ? GetSysColor(COLOR_MENUTEXT) : dark ? RGB(240, 240, 240) : RGB(26, 26, 26);
        highlight_ = highContrast ? GetSysColor(COLOR_HIGHLIGHT) : dark ? RGB(55, 55, 55) : RGB(230, 230, 230);
        selectedText_ = highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : foreground_;
        disabledText_ = highContrast ? GetSysColor(COLOR_GRAYTEXT) : dark ? RGB(140, 140, 140) : RGB(118, 118, 118);
        line_ = highContrast ? GetSysColor(COLOR_GRAYTEXT) : dark ? RGB(64, 64, 64) : RGB(215, 215, 215);

        backgroundBrush_ = CreateSolidBrush(background_);
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (!backgroundBrush_ ||
            !SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, menuDpi))
            return Fail();
        font_ = CreateFontIndirectW(&metrics.lfMenuFont);
        if (!font_)
            return Fail();

        HDC dc = GetDC(owner);
        if (!dc)
            return Fail();
        HGDIOBJ previous = SelectObject(dc, font_);
        LONG textWidth = 0, textHeight = 0;
        bool measured = previous != nullptr;
        for (std::size_t i = 0; measured && i < count; ++i)
        {
            SIZE extent{};
            measured = GetTextExtentPoint32W(dc, items[i].text, lstrlenW(items[i].text), &extent) != FALSE;
            textWidth = (std::max)(textWidth, extent.cx);
            textHeight = (std::max)(textHeight, extent.cy);
        }
        if (!measured)
            error_ = GetLastError();
        if (previous)
            SelectObject(dc, previous);
        ReleaseDC(owner, dc);
        if (!measured)
            return false;

        width_ = static_cast<UINT>(textWidth + Scale(40));
        height_ = static_cast<UINT>((std::max)(static_cast<int>(textHeight) + Scale(10), Scale(28)));
        separatorHeight_ = static_cast<UINT>(Scale(9));

        MENUINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = MIM_BACKGROUND | MIM_STYLE;
        info.hbrBack = backgroundBrush_;
        info.dwStyle = MNS_NOCHECK;
        if (!SetMenuInfo(menu, &info))
            return Fail();

        for (std::size_t i = 0; i < count; ++i)
        {
            Item& item = items[i];
            const bool separator = item.kind == Kind::Separator;
            item.accessible = {MSAA_MENU_SIG, static_cast<DWORD>(lstrlenW(item.text)), const_cast<LPWSTR>(item.text)};
            if (!AppendMenuW(menu, static_cast<UINT>(separator ? MF_SEPARATOR : MF_STRING), item.id,
                             separator ? nullptr : item.text))
                return Fail();
            // A system-drawn separator ignores the menu colors, so separators are drawn like the other items.
            MENUITEMINFOW entry{};
            entry.cbSize = sizeof(entry);
            entry.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STATE;
            entry.fType = static_cast<UINT>(MFT_OWNERDRAW | (separator ? MFT_SEPARATOR : 0));
            entry.fState = static_cast<UINT>(item.kind == Kind::Title ? MFS_DISABLED : MFS_ENABLED);
            entry.dwItemData = reinterpret_cast<ULONG_PTR>(&item);
            if (!SetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &entry))
                return Fail();
        }
        return true;
    }

    DWORD Error() const
    {
        return error_;
    }

    HWND Window()
    {
        if (!menuWindow_)
        {
            HWND found = nullptr;
            static_cast<void>(
                EnumThreadWindows(GetCurrentThreadId(), FindMenuWindow, reinterpret_cast<LPARAM>(&found)));
            if (!found)
                return nullptr;
            menuWindow_ = found;
            const DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUNDSMALL;
            // Windows 10 has no corner preference. The call fails there and the menu keeps square corners.
            static_cast<void>(
                DwmSetWindowAttribute(found, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference)));
        }
        return IsWindow(menuWindow_) ? menuWindow_ : nullptr;
    }

    bool Contains(POINT point)
    {
        const HWND window = Window();
        RECT rect{};
        return window && GetWindowRect(window, &rect) && PtInRect(&rect, point);
    }

    void RoundCorners()
    {
        static_cast<void>(Window());
    }

    void Measure(MEASUREITEMSTRUCT& item) const
    {
        const auto* entry = reinterpret_cast<const Item*>(item.itemData);
        item.itemWidth = width_;
        item.itemHeight = entry && entry->kind == Kind::Separator ? separatorHeight_ : height_;
    }

    bool Draw(const DRAWITEMSTRUCT& item) const
    {
        const auto* entry = reinterpret_cast<const Item*>(item.itemData);
        if (!entry)
            return false;
        const int saved = SaveDC(item.hDC);
        if (!saved)
            return false;

        const bool disabled = (item.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
        const bool selected = (item.itemState & ODS_SELECTED) != 0 && entry->kind == Kind::Command;
        const COLORREF ink = disabled ? disabledText_ : selected ? selectedText_ : foreground_;
        SetDCBrushColor(item.hDC, selected ? highlight_ : background_);
        FillRect(item.hDC, &item.rcItem, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        if (entry->kind == Kind::Separator)
        {
            const LONG middle = item.rcItem.top + (item.rcItem.bottom - item.rcItem.top) / 2;
            const RECT line = {item.rcItem.left + Scale(4), middle, item.rcItem.right - Scale(4),
                               middle + (std::max)(1, Scale(1))};
            SetDCBrushColor(item.hDC, line_);
            FillRect(item.hDC, &line, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            RestoreDC(item.hDC, saved);
            return true;
        }
        SelectObject(item.hDC, font_);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, ink);

        RECT label = item.rcItem;
        label.left += Scale(32);
        label.right -= Scale(8);
        DrawTextW(item.hDC, entry->text, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        HPEN pen = nullptr;
        if (item.itemState & ODS_CHECKED)
        {
            pen = CreatePen(PS_SOLID, (std::max)(1, Scale(1)), ink);
            if (pen)
            {
                SelectObject(item.hDC, pen);
                const LONG x = item.rcItem.left + Scale(8);
                const LONG y = item.rcItem.top + (item.rcItem.bottom - item.rcItem.top - Scale(16)) / 2;
                const POINT points[] = {
                    {x + Scale(2), y + Scale(8)}, {x + Scale(6), y + Scale(12)}, {x + Scale(14), y + Scale(3)}};
                Polyline(item.hDC, points, 3);
            }
        }

        RestoreDC(item.hDC, saved);
        if (pen)
            DeleteObject(pen);
        return true;
    }
};

}
