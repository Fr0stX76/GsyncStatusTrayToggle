#include <cstdio>
#include "../GsyncLogic.h"

using namespace gsync;

static_assert(static_cast<int>(ExitCode::Success) == 0, "Success");
static_assert(static_cast<int>(ExitCode::MutexOpen) == 1, "MutexOpen");
static_assert(static_cast<int>(ExitCode::MutexTimeout) == 2, "MutexTimeout");
static_assert(static_cast<int>(ExitCode::MutexWait) == 3, "MutexWait");
static_assert(static_cast<int>(ExitCode::NvidiaInitialize) == 10, "NvidiaInitialize");
static_assert(static_cast<int>(ExitCode::NvidiaSession) == 11, "NvidiaSession");
static_assert(static_cast<int>(ExitCode::NvidiaLoad) == 12, "NvidiaLoad");
static_assert(static_cast<int>(ExitCode::NvidiaProfile) == 13, "NvidiaProfile");
static_assert(static_cast<int>(ExitCode::StateUnknown) == 14, "StateUnknown");
static_assert(static_cast<int>(ExitCode::NvidiaSetting) == 15, "NvidiaSetting");
static_assert(static_cast<int>(ExitCode::NvidiaSave) == 16, "NvidiaSave");
static_assert(static_cast<int>(ExitCode::Verification) == 17, "Verification");
static_assert(static_cast<int>(ExitCode::InternalError) == 20, "InternalError");
static_assert(static_cast<int>(ExitCode::Arguments) == 64, "Arguments");

static_assert(ApplicationMode == 0 && ApplicationState == 1 && GlobalFeature == 2 && GlobalMode == 3 &&
                  PreferredRefresh == 4 && SettingCount == 5,
              "Setting order");

static int g_failures = 0;
static int g_checks = 0;

static void Check(bool ok, const char* name)
{
    ++g_checks;
    if (ok)
        return;
    ++g_failures;
    std::printf("FAIL: %s\n", name);
}

static Action Parse(const wchar_t* argument)
{
    const wchar_t* argv[] = {L"GsyncStatusTrayToggle.exe", argument};
    return ParseArguments(2, argv);
}

static void TestArguments()
{
    const wchar_t* exe[] = {L"GsyncStatusTrayToggle.exe"};
    Check(ParseArguments(1, exe) == Action::Tray, "no argument starts the tray");
    Check(ParseArguments(0, nullptr) == Action::Invalid, "empty command line is invalid");
    Check(Parse(L"/off") == Action::Off, "/off");
    Check(Parse(L"/fs") == Action::Fullscreen, "/fs");
    Check(Parse(L"/fsw") == Action::Windowed, "/fsw");
    Check(Parse(L"/OFF") == Action::Off, "/OFF");
    Check(Parse(L"/Fs") == Action::Fullscreen, "/Fs");
    Check(Parse(L"/FSW") == Action::Windowed, "/FSW");
    Check(Parse(L"off") == Action::Invalid, "off without slash");
    Check(Parse(L"-fs") == Action::Invalid, "-fs");
    Check(Parse(L"/on") == Action::Invalid, "/on");
    Check(Parse(L"/fs ") == Action::Invalid, "/fs with a space");
    Check(Parse(L"") == Action::Invalid, "empty argument");

    const wchar_t* two[] = {L"GsyncStatusTrayToggle.exe", L"/fs", L"/off"};
    Check(ParseArguments(3, two) == Action::Invalid, "two arguments");
}

static void TestSettings()
{
    Check(SettingIds == Values{0x1194F158u, 0x10A879CFu, 0x1094F157u, 0x1094F1F7u, 0x0064B541u}, "setting IDs");
    Check(SettingDefaults == Values{1u, 0u, 1u, 1u, 0u}, "setting defaults");
    Check(MakePlan(State::Off) == Values{0u, 4u, 0u, 0u, 0u}, "plan Off");
    Check(MakePlan(State::Fullscreen) == Values{1u, 0u, 1u, 1u, 1u}, "plan Fullscreen");
    Check(MakePlan(State::Windowed) == Values{2u, 0u, 1u, 2u, 1u}, "plan Fullscreen + Windowed");
}

static void TestStates()
{
    Check(DecodeState(0) == State::Off, "decode 0");
    Check(DecodeState(1) == State::Fullscreen, "decode 1");
    Check(DecodeState(2) == State::Windowed, "decode 2");
    Check(DecodeState(3) == State::Unknown, "decode 3");
    Check(DecodeState(0xFFFFFFFFu) == State::Unknown, "decode max");
    Check(!IsOn(State::Off) && IsOn(State::Fullscreen) && IsOn(State::Windowed) && !IsOn(State::Unknown), "IsOn");
    Check(ModeState(Mode::Fullscreen) == State::Fullscreen && ModeState(Mode::Windowed) == State::Windowed,
          "ModeState");
    Check(StateMode(State::Fullscreen) == Mode::Fullscreen && StateMode(State::Windowed) == Mode::Windowed,
          "StateMode");
}

static void TestModes()
{
    Mode mode = Mode::Windowed;
    Check(ParseMode(L"Fullscreen", mode) && mode == Mode::Fullscreen, "parse Fullscreen");
    Check(ParseMode(L"Windowed", mode) && mode == Mode::Windowed, "parse Windowed");
    mode = Mode::Fullscreen;
    Check(!ParseMode(L"windowed", mode) && mode == Mode::Fullscreen, "saved mode is case-sensitive");
    Check(!ParseMode(L"", mode) && mode == Mode::Fullscreen, "empty saved mode");
    Check(!ParseMode(L"Fullscreen + Windowed", mode) && mode == Mode::Fullscreen, "menu text is not a saved mode");
    Check(std::wstring(ModeName(Mode::Fullscreen)) == L"Fullscreen", "name Fullscreen");
    Check(std::wstring(ModeName(Mode::Windowed)) == L"Windowed", "name Windowed");
    Check(Lower(L"/FSW") == L"/fsw", "Lower");
}

static void TestDriverSettingsFiles()
{
    Check(IsDriverSettingsFile(L"nvdrsdb0.bin"), "nvdrsdb0.bin");
    Check(IsDriverSettingsFile(L"nvdrsdb1.bin"), "nvdrsdb1.bin");
    Check(IsDriverSettingsFile(L"nvdrssel.bin"), "nvdrssel.bin");
    Check(IsDriverSettingsFile(L"NVDRSDB0.BIN"), "settings file names ignore case");
    Check(!IsDriverSettingsFile(L"nvAppTimestamps"), "nvAppTimestamps is ignored");
    Check(!IsDriverSettingsFile(L"update.bin"), "update.bin is ignored");
    Check(!IsDriverSettingsFile(L"nvdrswr.lk"), "nvdrswr.lk is ignored");
    Check(!IsDriverSettingsFile(L"nvdrsdb2.bin"), "nvdrsdb2.bin is ignored");
    Check(!IsDriverSettingsFile(L"nvdrsdb0.bin.tmp"), "longer name is ignored");
    Check(!IsDriverSettingsFile(L"nvdrsdb0.bi"), "shorter name is ignored");
    Check(!IsDriverSettingsFile(L""), "empty name is ignored");
    const wchar_t record[] = L"nvdrssel.binXYZ";
    Check(IsDriverSettingsFile(std::wstring_view(record, 12)), "name without terminator");
}

struct DecideCase
{
    const char* name;
    Action action;
    State current;
    Mode preference;
    bool refuse;
    bool apply;
    State target;
    bool hasRemembered;
    Mode remembered;
};

static void TestDecide()
{
    constexpr Mode F = Mode::Fullscreen;
    constexpr Mode W = Mode::Windowed;
    const DecideCase cases[] = {
        {"/off from Off", Action::Off, State::Off, F, false, true, State::Off, false, F},
        {"/off from Fullscreen", Action::Off, State::Fullscreen, W, false, true, State::Off, true, F},
        {"/off from Windowed", Action::Off, State::Windowed, F, false, true, State::Off, true, W},
        {"/off from Unknown", Action::Off, State::Unknown, F, false, true, State::Off, false, F},
        {"/fs from Off", Action::Fullscreen, State::Off, W, false, true, State::Fullscreen, true, F},
        {"/fs from Windowed", Action::Fullscreen, State::Windowed, W, false, true, State::Fullscreen, true, F},
        {"/fs from Unknown", Action::Fullscreen, State::Unknown, W, false, true, State::Fullscreen, true, F},
        {"/fsw from Off", Action::Windowed, State::Off, F, false, true, State::Windowed, true, W},
        {"/fsw from Fullscreen", Action::Windowed, State::Fullscreen, F, false, true, State::Windowed, true, W},
        {"/fsw from Unknown", Action::Windowed, State::Unknown, F, false, true, State::Windowed, true, W},
        {"click from Off, Fullscreen", Action::Toggle, State::Off, F, false, true, State::Fullscreen, true, F},
        {"click from Off, Windowed", Action::Toggle, State::Off, W, false, true, State::Windowed, true, W},
        {"click from Fullscreen", Action::Toggle, State::Fullscreen, W, false, true, State::Off, true, F},
        {"click from Windowed", Action::Toggle, State::Windowed, F, false, true, State::Off, true, W},
        {"click from Unknown", Action::Toggle, State::Unknown, F, true, false, State::Off, false, F},
        {"menu Fullscreen while Off", Action::SelectFullscreen, State::Off, W, false, false, State::Fullscreen, true,
         F},
        {"menu Windowed while Off", Action::SelectWindowed, State::Off, F, false, false, State::Windowed, true, W},
        {"menu Fullscreen while Windowed", Action::SelectFullscreen, State::Windowed, W, false, true, State::Fullscreen,
         true, F},
        {"menu Windowed while Fullscreen", Action::SelectWindowed, State::Fullscreen, F, false, true, State::Windowed,
         true, W},
        {"menu Fullscreen while Unknown", Action::SelectFullscreen, State::Unknown, F, true, false, State::Off, false,
         F},
        {"menu Windowed while Unknown", Action::SelectWindowed, State::Unknown, F, true, false, State::Off, false, F},
        {"tray is not a command", Action::Tray, State::Fullscreen, F, true, false, State::Off, false, F},
        {"invalid is not a command", Action::Invalid, State::Fullscreen, F, true, false, State::Off, false, F},
    };
    for (const DecideCase& c : cases)
    {
        const Decision d = Decide(c.action, c.current, c.preference);
        bool ok = d.refuse == c.refuse && d.apply == c.apply;
        if (ok && !c.refuse)
            ok = d.target == c.target && d.hasRemembered == c.hasRemembered &&
                 (!c.hasRemembered || d.remembered == c.remembered);
        Check(ok, c.name);
    }
}

int main()
{
    TestArguments();
    TestSettings();
    TestStates();
    TestModes();
    TestDriverSettingsFiles();
    TestDecide();
    if (g_failures)
    {
        std::printf("%d of %d checks failed.\n", g_failures, g_checks);
        return 1;
    }
    std::printf("All %d checks passed.\n", g_checks);
    return 0;
}
