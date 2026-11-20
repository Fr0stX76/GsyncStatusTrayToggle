#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cwctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace gsync
{

enum class State
{
    Off,
    Fullscreen,
    Windowed,
    Unknown
};
enum class Mode
{
    Fullscreen,
    Windowed
};
enum class Action
{
    Tray,
    Off,
    Fullscreen,
    Windowed,
    Toggle,
    SelectFullscreen,
    SelectWindowed,
    Allow,
    Disallow,
    List,
    Invalid
};

enum class ExitCode : int
{
    Success = 0,
    MutexOpen = 1,
    MutexTimeout = 2,
    MutexWait = 3,
    NvidiaInitialize = 10,
    NvidiaSession = 11,
    NvidiaLoad = 12,
    NvidiaProfile = 13,
    StateUnknown = 14,
    NvidiaSetting = 15,
    NvidiaSave = 16,
    Verification = 17,
    InternalError = 20,
    DisplayNotFound = 30,
    DisplayAmbiguous = 31,
    GsyncOff = 32,
    DisplayRefused = 33,
    Arguments = 64
};

inline bool IsOn(State s)
{
    return s == State::Fullscreen || s == State::Windowed;
}

inline State ModeState(Mode m)
{
    return m == Mode::Fullscreen ? State::Fullscreen : State::Windowed;
}

inline Mode StateMode(State s)
{
    return s == State::Windowed ? Mode::Windowed : Mode::Fullscreen;
}

inline State DecodeState(std::uint32_t v)
{
    switch (v)
    {
    case 0:
        return State::Off;
    case 1:
        return State::Fullscreen;
    case 2:
        return State::Windowed;
    default:
        return State::Unknown;
    }
}

enum Setting : std::size_t
{
    ApplicationMode,
    ApplicationState,
    GlobalFeature,
    GlobalMode,
    PreferredRefresh,
    SettingCount
};

using Values = std::array<std::uint32_t, SettingCount>;
constexpr Values SettingIds = {0x1194F158u, 0x10A879CFu, 0x1094F157u, 0x1094F1F7u, 0x0064B541u};
constexpr Values SettingDefaults = {1u, 0u, 1u, 1u, 0u};

inline Values MakePlan(State target)
{
    const std::uint32_t mode = target == State::Fullscreen ? 1u : target == State::Windowed ? 2u : 0u;
    const bool on = IsOn(target);
    return {mode, on ? 0u : 4u, on ? 1u : 0u, mode, on ? 1u : 0u};
}

struct Decision
{
    bool refuse = false;
    bool apply = false;
    State target = State::Off;
    bool hasRemembered = false;
    Mode remembered = Mode::Fullscreen;
};

inline Decision Decide(Action action, State current, Mode preference)
{
    Decision d;
    const bool select = action == Action::SelectFullscreen || action == Action::SelectWindowed;
    if ((action == Action::Toggle || select) && current == State::Unknown)
    {
        d.refuse = true;
        return d;
    }

    const Mode selected = action == Action::SelectWindowed ? Mode::Windowed : Mode::Fullscreen;
    d.apply = true;
    switch (action)
    {
    case Action::SelectFullscreen:
    case Action::SelectWindowed:
        d.apply = current != State::Off;
        d.target = ModeState(selected);
        break;
    case Action::Toggle:
        d.target = IsOn(current) ? State::Off : ModeState(preference);
        break;
    case Action::Fullscreen:
        d.target = State::Fullscreen;
        break;
    case Action::Windowed:
        d.target = State::Windowed;
        break;
    case Action::Off:
        d.target = State::Off;
        break;
    default:
        d.refuse = true;
        d.apply = false;
        return d;
    }

    if (d.apply && IsOn(d.target))
    {
        d.hasRemembered = true;
        d.remembered = StateMode(d.target);
    }
    else if (select)
    {
        d.hasRemembered = true;
        d.remembered = selected;
    }
    else if (IsOn(current))
    {
        d.hasRemembered = true;
        d.remembered = StateMode(current);
    }
    return d;
}

inline std::wstring Lower(std::wstring s)
{
    for (auto& c : s)
        c = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
    return s;
}

inline bool SameTextIgnoringCase(std::wstring_view a, std::wstring_view b)
{
    return std::equal(
        a.begin(), a.end(), b.begin(), b.end(), [](wchar_t x, wchar_t y)
        { return std::towlower(static_cast<std::wint_t>(x)) == std::towlower(static_cast<std::wint_t>(y)); });
}

inline bool IsDriverSettingsFile(std::wstring_view name)
{
    for (const wchar_t* file : {L"nvdrsdb0.bin", L"nvdrsdb1.bin", L"nvdrssel.bin"})
        if (SameTextIgnoringCase(name, file))
            return true;
    return false;
}

inline const wchar_t* ModeName(Mode m)
{
    return m == Mode::Windowed ? L"Windowed" : L"Fullscreen";
}

inline bool ParseMode(const std::wstring& text, Mode& mode)
{
    for (const Mode candidate : {Mode::Fullscreen, Mode::Windowed})
    {
        if (text != ModeName(candidate))
            continue;
        mode = candidate;
        return true;
    }
    return false;
}

struct Command
{
    Action action = Action::Invalid;
    std::wstring display;
};

inline Command ParseCommand(int argc, const wchar_t* const* argv)
{
    Command command;
    if (argc == 1)
    {
        command.action = Action::Tray;
        return command;
    }
    if (argc != 2 && argc != 3)
        return command;
    const std::wstring arg = Lower(argv[1]);
    if (argc == 3)
    {
        if ((arg == L"/allow" || arg == L"/disallow") && argv[2][0] != L'\0')
        {
            command.action = arg == L"/allow" ? Action::Allow : Action::Disallow;
            command.display = argv[2];
        }
        return command;
    }
    if (arg == L"/off")
        command.action = Action::Off;
    else if (arg == L"/fs")
        command.action = Action::Fullscreen;
    else if (arg == L"/fsw")
        command.action = Action::Windowed;
    else if (arg == L"/list")
        command.action = Action::List;
    return command;
}

// NVIDIA keeps one choice per EDID manufacturer and product code, so identical displays are grouped into one model.
struct DisplayModel
{
    std::wstring name;
    std::uint16_t manufacturer = 0;
    std::uint16_t product = 0;
    bool vrr = false;
    bool allowed = false;
};

inline bool SameModel(const DisplayModel& a, const DisplayModel& b)
{
    return a.manufacturer == b.manufacturer && a.product == b.product;
}

inline std::wstring Hex4(std::uint16_t value)
{
    std::wstring text;
    for (const unsigned shift : {12u, 8u, 4u, 0u})
        text.push_back(L"0123456789ABCDEF"[(value >> shift) & 0xFu]);
    return text;
}

// Windows reports the EDID manufacturer bytes swapped. The three letters take five bits each, 1 for A.
inline std::wstring ModelCode(std::uint16_t manufacturer, std::uint16_t product)
{
    const unsigned packed = ((manufacturer & 0xFFu) << 8) | (manufacturer >> 8);
    std::wstring code;
    for (const unsigned shift : {10u, 5u, 0u})
    {
        const unsigned letter = (packed >> shift) & 0x1Fu;
        code.push_back(letter >= 1 && letter <= 26 ? static_cast<wchar_t>(L'A' + letter - 1) : L'?');
    }
    return code + Hex4(product);
}

inline std::wstring ModelCode(const DisplayModel& model)
{
    return ModelCode(model.manufacturer, model.product);
}

// The NVIDIA Control Panel keeps the choice for a display model in a hidden profile with this name, and attaches
// to it an application with the same name in lower case.
inline std::wstring ProfileName(const DisplayModel& model)
{
    return L"0x" + Hex4(model.manufacturer) + L":0x" + Hex4(model.product);
}

inline std::wstring ApplicationName(const DisplayModel& model)
{
    return Lower(ProfileName(model));
}

inline std::vector<DisplayModel> GroupModels(const std::vector<DisplayModel>& displays)
{
    std::vector<DisplayModel> models;
    for (const DisplayModel& display : displays)
    {
        const auto same = std::find_if(models.begin(), models.end(),
                                       [&display](const DisplayModel& model) { return SameModel(model, display); });
        if (same == models.end())
        {
            models.push_back(display);
            continue;
        }
        // Identical displays share one NVIDIA profile. The model counts as allowed only when every display that
        // offers VRR allows G-SYNC; displays without VRR count only when none offers it.
        if (display.vrr && !same->vrr)
            same->allowed = display.allowed;
        else if (display.vrr == same->vrr)
            same->allowed = same->allowed && display.allowed;
        same->vrr = same->vrr || display.vrr;
        if (same->name.empty())
            same->name = display.name;
    }
    return models;
}

inline std::vector<std::size_t> MatchModels(const std::vector<DisplayModel>& models, std::wstring_view wanted)
{
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < models.size(); ++i)
        if (SameTextIgnoringCase(models[i].name, wanted) || SameTextIgnoringCase(ModelCode(models[i]), wanted))
            found.push_back(i);
    return found;
}

inline std::wstring ModelLabel(const std::vector<DisplayModel>& models, std::size_t index)
{
    const DisplayModel& model = models[index];
    if (model.name.empty())
        return ModelCode(model);
    for (std::size_t i = 0; i < models.size(); ++i)
        if (i != index && SameTextIgnoringCase(models[i].name, model.name))
            return model.name + L" (" + ModelCode(model) + L")";
    return model.name;
}

inline std::wstring ListText(const std::vector<DisplayModel>& models)
{
    if (models.empty())
        return L"No active NVIDIA display.\r\n";
    struct Row
    {
        std::wstring name, code, gsync, vrr;
    };
    std::vector<Row> rows = {{L"Display", L"Model", L"G-SYNC", L"VRR"}};
    for (std::size_t i = 0; i < models.size(); ++i)
        rows.push_back({ModelLabel(models, i), ModelCode(models[i]), models[i].allowed ? L"allowed" : L"not allowed",
                        models[i].vrr ? L"available" : L"not available"});
    std::size_t nameWidth = 0, codeWidth = 0, gsyncWidth = 0;
    for (const Row& row : rows)
    {
        nameWidth = (std::max)(nameWidth, row.name.size());
        codeWidth = (std::max)(codeWidth, row.code.size());
        gsyncWidth = (std::max)(gsyncWidth, row.gsync.size());
    }
    std::wstring text;
    for (const Row& row : rows)
    {
        text += row.name + std::wstring(nameWidth - row.name.size() + 2, L' ');
        text += row.code + std::wstring(codeWidth - row.code.size() + 2, L' ');
        text += row.gsync + std::wstring(gsyncWidth - row.gsync.size() + 2, L' ');
        text += row.vrr + L"\r\n";
    }
    return text;
}

constexpr std::size_t TrayTipLength = 127;

inline std::wstring TrayTip(State state, const std::vector<DisplayModel>& models)
{
    std::wstring tip = state == State::Off          ? L"G-SYNC Off"
                       : state == State::Fullscreen ? L"G-SYNC Fullscreen"
                       : state == State::Windowed   ? L"G-SYNC Fullscreen + Windowed"
                                                    : L"G-SYNC Unknown";
    if (!IsOn(state))
        return tip;
    std::wstring blocked;
    for (std::size_t i = 0; i < models.size(); ++i)
        if (models[i].vrr && !models[i].allowed)
            blocked += (blocked.empty() ? L"" : L", ") + ModelLabel(models, i);
    if (!blocked.empty())
        tip += L"\nNot allowed on " + blocked;
    if (tip.size() > TrayTipLength)
        tip = tip.substr(0, TrayTipLength - 3) + L"...";
    return tip;
}

}
