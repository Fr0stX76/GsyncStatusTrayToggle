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
static_assert(static_cast<int>(ExitCode::DisplayNotFound) == 30, "DisplayNotFound");
static_assert(static_cast<int>(ExitCode::DisplayAmbiguous) == 31, "DisplayAmbiguous");
static_assert(static_cast<int>(ExitCode::GsyncOff) == 32, "GsyncOff");
static_assert(static_cast<int>(ExitCode::DisplayRefused) == 33, "DisplayRefused");
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
    return ParseCommand(2, argv).action;
}

static Command ParseDisplay(const wchar_t* argument, const wchar_t* display)
{
    const wchar_t* argv[] = {L"GsyncStatusTrayToggle.exe", argument, display};
    return ParseCommand(3, argv);
}

static void TestArguments()
{
    const wchar_t* exe[] = {L"GsyncStatusTrayToggle.exe"};
    Check(ParseCommand(1, exe).action == Action::Tray, "no argument starts the tray");
    Check(ParseCommand(0, nullptr).action == Action::Invalid, "empty command line is invalid");
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
    Check(ParseCommand(3, two).action == Action::Invalid, "two arguments");
}

static void TestDisplayArguments()
{
    Check(Parse(L"/list") == Action::List, "/list");
    Check(Parse(L"/LIST") == Action::List, "/LIST");
    Check(Parse(L"/allow") == Action::Invalid, "/allow without a display");
    Check(Parse(L"/disallow") == Action::Invalid, "/disallow without a display");

    const Command allow = ParseDisplay(L"/allow", L"Q90A");
    Check(allow.action == Action::Allow && allow.display == L"Q90A", "/allow Q90A");
    const Command spaced = ParseDisplay(L"/ALLOW", L"ASUS MG28U");
    Check(spaced.action == Action::Allow && spaced.display == L"ASUS MG28U", "/ALLOW with a quoted name");
    const Command disallow = ParseDisplay(L"/Disallow", L"sam713f");
    Check(disallow.action == Action::Disallow && disallow.display == L"sam713f", "/Disallow with a model code");
    Check(ParseDisplay(L"/allow", L"").action == Action::Invalid, "/allow with an empty name");
    Check(ParseDisplay(L"/list", L"Q90A").action == Action::Invalid, "/list with a name");
    Check(ParseDisplay(L"/fs", L"Q90A").action == Action::Invalid, "/fs with a name");
    Check(ParseDisplay(L"/off", L"Q90A").display.empty(), "an invalid command keeps no name");

    const wchar_t* three[] = {L"GsyncStatusTrayToggle.exe", L"/allow", L"Q90A", L"/hold"};
    Check(ParseCommand(4, three).action == Action::Invalid, "three arguments");
}

static DisplayModel Model(const wchar_t* name, std::uint16_t manufacturer, std::uint16_t product, bool vrr,
                          bool allowed)
{
    DisplayModel model;
    model.name = name;
    model.manufacturer = manufacturer;
    model.product = product;
    model.vrr = vrr;
    model.allowed = allowed;
    return model;
}

static void TestModelNames()
{
    const DisplayModel tv = Model(L"Q90A", 0x2D4C, 0x713F, true, true);
    const DisplayModel asus = Model(L"ASUS MG28U", 0x6904, 0x28A7, false, false);
    Check(ModelCode(tv) == L"SAM713F", "model code SAM713F");
    Check(ModelCode(asus) == L"ACI28A7", "model code ACI28A7");
    Check(ModelCode(0x0000, 0x0001) == L"???0001", "letters out of range");
    Check(ProfileName(tv) == L"0x2D4C:0x713F", "profile name of the Q90A");
    Check(ProfileName(asus) == L"0x6904:0x28A7", "profile name of the MG28U");
    Check(ApplicationName(tv) == L"0x2d4c:0x713f", "application name in lower case");
    Check(Hex4(0x00AF) == L"00AF" && Hex4(0xFFFF) == L"FFFF", "Hex4");
}

static void TestGroupModels()
{
    const std::vector<DisplayModel> twins =
        GroupModels({Model(L"Q90A", 0x2D4C, 0x713F, true, true), Model(L"Q90A", 0x2D4C, 0x713F, true, false),
                     Model(L"ASUS MG28U", 0x6904, 0x28A7, false, false)});
    Check(twins.size() == 2, "identical displays share one model");
    Check(twins[0].vrr && !twins[0].allowed, "a model is allowed only when every display is");
    Check(twins[1].name == L"ASUS MG28U", "order of first appearance");

    const std::vector<DisplayModel> mixed =
        GroupModels({Model(L"", 0x2D4C, 0x713F, false, false), Model(L"Q90A", 0x2D4C, 0x713F, true, true)});
    Check(mixed.size() == 1 && mixed[0].vrr && mixed[0].allowed, "a display without VRR does not count");
    Check(mixed[0].name == L"Q90A", "a missing name is taken from an identical display");

    const std::vector<DisplayModel> later =
        GroupModels({Model(L"Q90A", 0x2D4C, 0x713F, true, true), Model(L"Q90A", 0x2D4C, 0x713F, false, false)});
    Check(later.size() == 1 && later[0].allowed, "a later display without VRR does not count");
}

static void TestMatchModels()
{
    const std::vector<DisplayModel> models = {
        Model(L"Q90A", 0x2D4C, 0x713F, true, true), Model(L"ASUS MG28U", 0x6904, 0x28A7, true, false),
        Model(L"Monitor", 0x6D1E, 0x5B09, true, false), Model(L"monitor", 0xAC10, 0x4194, false, false)};
    Check(MatchModels(models, L"q90a") == std::vector<std::size_t>{0}, "name ignores case");
    Check(MatchModels(models, L"ASUS MG28U") == std::vector<std::size_t>{1}, "name with a space");
    Check(MatchModels(models, L"sam713f") == std::vector<std::size_t>{0}, "model code ignores case");
    Check(MatchModels(models, L"Monitor").size() == 2, "same name on two models");
    Check(MatchModels(models, L"Q90").empty(), "no partial match");
    Check(MatchModels(models, L"").empty(), "empty name");
    Check(ModelLabel(models, 0) == L"Q90A", "label is the name");
    Check(ModelLabel(models, 2) == L"Monitor (GSM5B09)", "label of a shared name adds the model code");
    Check(ModelLabel(models, 3) == L"monitor (DEL4194)", "every model with a shared name gets its code");
    Check(ModelLabel({Model(L"", 0x2D4C, 0x713F, true, true)}, 0) == L"SAM713F", "label without a name");
}

static void TestListText()
{
    const std::vector<DisplayModel> models = {Model(L"Q90A", 0x2D4C, 0x713F, true, true),
                                              Model(L"ASUS MG28U", 0x6904, 0x28A7, false, false)};
    Check(ListText(models) == L"Display     Model    G-SYNC       VRR\r\n"
                              L"Q90A        SAM713F  allowed      available\r\n"
                              L"ASUS MG28U  ACI28A7  not allowed  not available\r\n",
          "list text");
    Check(ListText({}) == L"No active NVIDIA display.\r\n", "empty list");
}

static void TestTrayTip()
{
    const std::vector<DisplayModel> allowed = {Model(L"Q90A", 0x2D4C, 0x713F, true, true),
                                               Model(L"ASUS MG28U", 0x6904, 0x28A7, false, false)};
    Check(TrayTip(State::Off, allowed) == L"G-SYNC Off", "tip Off");
    Check(TrayTip(State::Fullscreen, allowed) == L"G-SYNC Fullscreen", "display without VRR gives no warning");
    Check(TrayTip(State::Windowed, {}) == L"G-SYNC Fullscreen + Windowed", "tip Fullscreen + Windowed");
    Check(TrayTip(State::Unknown, {}) == L"G-SYNC Unknown", "tip Unknown");

    const std::vector<DisplayModel> blocked = {Model(L"Q90A", 0x2D4C, 0x713F, true, false),
                                               Model(L"ASUS MG28U", 0x6904, 0x28A7, true, false)};
    Check(TrayTip(State::Fullscreen, blocked) == L"G-SYNC Fullscreen\nNot allowed on Q90A, ASUS MG28U",
          "warning lists every display");
    Check(TrayTip(State::Off, blocked) == L"G-SYNC Off", "no warning while G-SYNC is off");

    std::vector<DisplayModel> many;
    for (std::uint16_t i = 0; i < 12; ++i)
        many.push_back(Model(L"Display name", 0x2D4C, static_cast<std::uint16_t>(0x7000 + i), true, false));
    const std::wstring tip = TrayTip(State::Fullscreen, many);
    Check(tip.size() == TrayTipLength && tip.substr(tip.size() - 3) == L"...", "long warning is shortened");
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
    TestDisplayArguments();
    TestModelNames();
    TestGroupModels();
    TestMatchModels();
    TestListText();
    TestTrayTip();
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
