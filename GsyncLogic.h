#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <cwctype>
#include <initializer_list>
#include <string>

namespace gsync {

enum class State { Off, Fullscreen, Windowed, Unknown };
enum class Mode { Fullscreen, Windowed };
enum class Action { Tray, Off, Fullscreen, Windowed, Toggle, SelectFullscreen, SelectWindowed, Invalid };

inline bool IsOn(State s) {
    return s == State::Fullscreen || s == State::Windowed;
}

inline State ModeState(Mode m) {
    return m == Mode::Fullscreen ? State::Fullscreen : State::Windowed;
}

inline Mode StateMode(State s) {
    return s == State::Windowed ? Mode::Windowed : Mode::Fullscreen;
}

inline State DecodeState(std::uint32_t v) {
    switch (v) {
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

enum Setting : std::size_t {
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

inline Values MakePlan(State target) {
    const std::uint32_t mode = target == State::Fullscreen ? 1u : target == State::Windowed ? 2u : 0u;
    const bool on = IsOn(target);
    return {mode, on ? 0u : 4u, on ? 1u : 0u, mode, on ? 1u : 0u};
}

struct Decision {
    bool refuse = false;
    bool apply = false;
    State target = State::Off;
    bool hasRemembered = false;
    Mode remembered = Mode::Fullscreen;
};

inline Decision Decide(Action action, State current, Mode preference) {
    Decision d;
    const bool select = action == Action::SelectFullscreen || action == Action::SelectWindowed;
    if ((action == Action::Toggle || select) && current == State::Unknown) {
        d.refuse = true;
        return d;
    }

    const Mode selected = action == Action::SelectWindowed ? Mode::Windowed : Mode::Fullscreen;
    d.apply = true;
    switch (action) {
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
    default:
        d.target = State::Off;
        break;
    }

    if (d.apply && IsOn(d.target)) {
        d.hasRemembered = true;
        d.remembered = StateMode(d.target);
    } else if (select) {
        d.hasRemembered = true;
        d.remembered = selected;
    } else if (IsOn(current)) {
        d.hasRemembered = true;
        d.remembered = StateMode(current);
    }
    return d;
}

inline std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
    return s;
}

inline const wchar_t* ModeName(Mode m) {
    return m == Mode::Windowed ? L"Windowed" : L"Fullscreen";
}

inline bool ParseMode(const std::wstring& text, Mode& mode) {
    for (const Mode candidate : {Mode::Fullscreen, Mode::Windowed}) {
        if (text != ModeName(candidate)) continue;
        mode = candidate;
        return true;
    }
    return false;
}

inline Action ParseArguments(int argc, const wchar_t* const* argv) {
    if (argc == 1) return Action::Tray;
    if (argc != 2) return Action::Invalid;
    const std::wstring arg = Lower(argv[1]);
    if (arg == L"/off") return Action::Off;
    if (arg == L"/fs") return Action::Fullscreen;
    if (arg == L"/fsw") return Action::Windowed;
    return Action::Invalid;
}

}
