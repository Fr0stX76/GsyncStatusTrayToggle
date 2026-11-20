#pragma once
#include <windows.h>
#include <sddl.h>
#include <cfgmgr32.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "nvapi.h"
#include "NvApiDriverSettings.h"
#include "GsyncLogic.h"

namespace gsync
{

static_assert(SettingIds[ApplicationMode] == static_cast<std::uint32_t>(VRR_MODE_ID), "Application Mode ID");
static_assert(SettingIds[ApplicationState] == static_cast<std::uint32_t>(VRR_APP_OVERRIDE_ID), "Application State ID");
static_assert(SettingIds[GlobalFeature] == static_cast<std::uint32_t>(VRRFEATUREINDICATOR_ID), "Global Feature ID");
static_assert(SettingIds[GlobalMode] == static_cast<std::uint32_t>(VRRREQUESTSTATE_ID), "Global Mode ID");
static_assert(SettingIds[PreferredRefresh] == static_cast<std::uint32_t>(REFRESH_RATE_OVERRIDE_ID),
              "Preferred Refreshrate ID");
static_assert(SettingDefaults[ApplicationMode] == static_cast<std::uint32_t>(VRR_MODE_DEFAULT),
              "Application Mode default");
static_assert(SettingDefaults[ApplicationState] == static_cast<std::uint32_t>(VRR_APP_OVERRIDE_DEFAULT),
              "Application State default");
static_assert(SettingDefaults[GlobalFeature] == static_cast<std::uint32_t>(VRRFEATUREINDICATOR_DEFAULT),
              "Global Feature default");
static_assert(SettingDefaults[GlobalMode] == static_cast<std::uint32_t>(VRRREQUESTSTATE_DEFAULT),
              "Global Mode default");
static_assert(SettingDefaults[PreferredRefresh] == static_cast<std::uint32_t>(REFRESH_RATE_OVERRIDE_DEFAULT),
              "Preferred Refreshrate default");

// In a display model profile, the Application Mode setting holds the choice for that model: 1 allowed, 0 not.
inline constexpr NvU32 ModelChoiceSetting = static_cast<NvU32>(VRR_MODE_ID);
inline constexpr NvU32 HiddenProfileSetting = static_cast<NvU32>(CPL_HIDDEN_PROFILE_ID);
static_assert(HiddenProfileSetting == 0x106D5CFFu, "Hidden profile ID");

inline constexpr wchar_t MutexName[] = L"Global\\GsyncToggleMutex";
inline constexpr DWORD MutexTimeout = 15000;
inline constexpr DWORD ModesetWait = 1500;
inline constexpr DWORD StepWait = 200;
inline constexpr DWORD RemovalWait = 500;
inline constexpr DWORD ReturnTimeout = 4000;
inline constexpr DWORD ReturnSettle = 600;
inline constexpr DWORD RateWatch = 2500;
inline constexpr DWORD PollWait = 50;
inline constexpr DWORD ChoiceTimeout = 2000;

inline constexpr GUID MonitorInterface = {
    0xE6F07B5Fu, 0xEE97u, 0x4A90u, {0xB0u, 0x76u, 0x33u, 0xF5u, 0x7Bu, 0xF4u, 0xEAu, 0xA7u}};
inline constexpr wchar_t PartialChange[] =
    L"Part of the change was saved, then NVIDIA refused the rest. Set G-SYNC in the NVIDIA Control Panel.";
inline constexpr wchar_t LockOpenFailed[] = L"The G-SYNC lock could not be opened. Try again.";
inline constexpr wchar_t NvapiMissing[] = L"NVAPI could not be initialized. Check that the NVIDIA driver is installed.";
inline constexpr wchar_t ChoiceRejected[] = L"NVIDIA rejected the display choice. Nothing was changed.";

struct Result
{
    ExitCode code = ExitCode::Success;
    std::wstring error;
    Mode remembered = Mode::Fullscreen;
    bool hasRemembered = false;
    State state = State::Unknown;
};

inline Result Failure(ExitCode code, const wchar_t* text, NvAPI_Status status = NVAPI_OK)
{
    Result r;
    r.code = code;
    r.error = text;
    if (status != NVAPI_OK)
    {
        NvAPI_ShortString detail{};
        if (NvAPI_GetErrorMessage(status, detail) == NVAPI_OK)
        {
            r.error += L"\n\n";
            r.error.append(detail, detail + std::strlen(detail));
        }
        r.error += L" (NVIDIA code " + std::to_wstring(static_cast<int>(status)) + L")";
    }
    return r;
}

inline std::wstring WindowsError(const wchar_t* action, DWORD code)
{
    std::wstring message(action);
    if (code == ERROR_SUCCESS)
        return message;
    message += L"\n\n";
    wchar_t* detail = nullptr;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<LPWSTR>(&detail), 0, nullptr) &&
        detail)
    {
        message += detail;
        LocalFree(detail);
        message.erase(message.find_last_not_of(L" \r\n") + 1);
    }
    message += L" (code " + std::to_wstring(code) + L")";
    return message;
}

inline Result WindowsFailure(ExitCode code, const wchar_t* text, DWORD error)
{
    Result r;
    r.code = code;
    r.error = WindowsError(text, error);
    return r;
}

class ToggleLock final
{
    HANDLE handle_ = nullptr;
    bool owned_ = false;

public:
    ToggleLock() = default;
    ToggleLock(const ToggleLock&) = delete;
    ToggleLock& operator=(const ToggleLock&) = delete;

    ~ToggleLock()
    {
        if (owned_)
            ReleaseMutex(handle_);
        if (handle_)
            CloseHandle(handle_);
    }

    Result Acquire()
    {
        // The NVIDIA global profile is machine-wide, so the lock lives in Global\. Authenticated users get only
        // SYNCHRONIZE: enough to wait on the lock, not to change its security.
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x100000;;;AU)",
                                                                  SDDL_REVISION_1, &descriptor, nullptr))
            return WindowsFailure(ExitCode::MutexOpen, LockOpenFailed, GetLastError());
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        handle_ = CreateMutexExW(&attributes, MutexName, 0, SYNCHRONIZE);
        const DWORD error = GetLastError();
        LocalFree(descriptor);
        if (!handle_)
            return WindowsFailure(ExitCode::MutexOpen, LockOpenFailed, error);

        const DWORD wait = WaitForSingleObject(handle_, MutexTimeout);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED)
        {
            owned_ = true;
            return {};
        }
        if (wait == WAIT_TIMEOUT)
            return Failure(ExitCode::MutexTimeout,
                           L"Another G-SYNC command is still running. Try again in a few seconds.");
        return WindowsFailure(ExitCode::MutexWait, L"The G-SYNC lock could not be acquired. Try again.",
                              GetLastError());
    }
};

class DrsSession final
{
    bool initialized_ = false;

public:
    NvDRSSessionHandle session = nullptr;
    NvDRSProfileHandle profile = nullptr;
    DrsSession() = default;
    DrsSession(const DrsSession&) = delete;
    DrsSession& operator=(const DrsSession&) = delete;

    ~DrsSession()
    {
        if (session)
            NvAPI_DRS_DestroySession(session);
        if (initialized_)
            NvAPI_Unload();
    }

    Result Reload()
    {
        NvAPI_Status s = NvAPI_DRS_LoadSettings(session);
        if (s != NVAPI_OK)
            return Failure(ExitCode::NvidiaLoad, L"The NVIDIA settings could not be loaded.", s);
        s = NvAPI_DRS_GetBaseProfile(session, &profile);
        if (s != NVAPI_OK)
            return Failure(ExitCode::NvidiaProfile, L"The NVIDIA global profile could not be opened.", s);
        return {};
    }

    Result Open()
    {
        NvAPI_Status s = NvAPI_Initialize();
        if (s != NVAPI_OK)
            return Failure(ExitCode::NvidiaInitialize, NvapiMissing, s);
        initialized_ = true;
        s = NvAPI_DRS_CreateSession(&session);
        if (s != NVAPI_OK)
            return Failure(ExitCode::NvidiaSession, L"The NVIDIA settings could not be opened.", s);
        return Reload();
    }
};

inline Result ReadValues(const DrsSession& drs, Values& values)
{
    for (std::size_t i = 0; i < SettingCount; ++i)
    {
        NVDRS_SETTING setting{};
        setting.version = NVDRS_SETTING_VER;
        const NvAPI_Status s = NvAPI_DRS_GetSetting(drs.session, drs.profile, SettingIds[i], &setting);
        if (s == NVAPI_SETTING_NOT_FOUND)
        {
            values[i] = SettingDefaults[i];
            continue;
        }
        if (s != NVAPI_OK || setting.settingType != NVDRS_DWORD_TYPE)
            return Failure(ExitCode::StateUnknown, L"A G-SYNC setting could not be read. Nothing was changed.", s);
        values[i] = setting.u32CurrentValue;
    }
    return {};
}

inline State ReadState()
{
    DrsSession drs;
    Values values{};
    if (drs.Open().code != ExitCode::Success || ReadValues(drs, values).code != ExitCode::Success)
        return State::Unknown;
    return DecodeState(values[ApplicationMode]);
}

inline bool IsNvidia(const DISPLAYCONFIG_PATH_INFO& path)
{
    DISPLAYCONFIG_ADAPTER_NAME adapter{};
    adapter.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
    adapter.header.size = sizeof(adapter);
    adapter.header.adapterId = path.targetInfo.adapterId;
    return DisplayConfigGetDeviceInfo(&adapter.header) == ERROR_SUCCESS &&
           Lower(adapter.adapterDevicePath).find(L"ven_10de") != std::wstring::npos;
}

struct DisplayConfig
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;

    bool Query()
    {
        LONG rc = ERROR_INSUFFICIENT_BUFFER;
        for (int attempt = 0; attempt < 3 && rc == ERROR_INSUFFICIENT_BUFFER; ++attempt)
        {
            UINT32 pathCount = 0, modeCount = 0;
            rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
            if (rc != ERROR_SUCCESS)
                break;
            paths.resize(pathCount);
            modes.resize(modeCount);
            rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
            paths.resize(pathCount);
            modes.resize(modeCount);
        }
        if (rc == ERROR_SUCCESS)
            return true;
        paths.clear();
        modes.clear();
        return false;
    }

    bool Apply()
    {
        if (paths.empty())
            return false;
        const UINT32 pathCount = static_cast<UINT32>(paths.size());
        const UINT32 modeCount = static_cast<UINT32>(modes.size());
        const UINT32 flags = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_NO_OPTIMIZATION;
        return SetDisplayConfig(pathCount, paths.data(), modeCount, modes.data(), flags) == ERROR_SUCCESS;
    }
};

inline bool SameTarget(const DISPLAYCONFIG_PATH_INFO& a, const DISPLAYCONFIG_PATH_INFO& b)
{
    return a.targetInfo.id == b.targetInfo.id && a.targetInfo.adapterId.LowPart == b.targetInfo.adapterId.LowPart &&
           a.targetInfo.adapterId.HighPart == b.targetInfo.adapterId.HighPart;
}

inline bool Slower(const DisplayConfig& a, const DisplayConfig& b)
{
    for (const DISPLAYCONFIG_PATH_INFO& x : a.paths)
    {
        if (!IsNvidia(x))
            continue;
        for (const DISPLAYCONFIG_PATH_INFO& y : b.paths)
        {
            const DISPLAYCONFIG_RATIONAL& p = x.targetInfo.refreshRate;
            const DISPLAYCONFIG_RATIONAL& q = y.targetInfo.refreshRate;
            if (SameTarget(x, y) && p.Denominator && q.Denominator &&
                static_cast<std::uint64_t>(p.Numerator) * q.Denominator <
                    static_cast<std::uint64_t>(q.Numerator) * p.Denominator)
                return true;
        }
    }
    return false;
}

inline bool Present(const DisplayConfig& expected)
{
    DisplayConfig now;
    if (!now.Query())
        return false;
    for (const DISPLAYCONFIG_PATH_INFO& x : expected.paths)
        if (IsNvidia(x) && std::none_of(now.paths.begin(), now.paths.end(),
                                        [&x](const DISPLAYCONFIG_PATH_INFO& y) { return SameTarget(x, y); }))
            return false;
    return true;
}

// Applying the current configuration again with SDC_NO_OPTIMIZATION redraws the displays once. The Control Panel
// toggles virtual resolution support instead, which redraws twice and can drop VRR for a moment.
inline bool RedrawDisplays(DisplayConfig& best)
{
    DisplayConfig current;
    if (!current.Query() || std::none_of(current.paths.begin(), current.paths.end(), IsNvidia))
        return true;
    if (Slower(best, current))
        std::swap(best, current);
    if (best.Apply())
        return true;
    if (Slower(current, best) || !current.Apply())
        return false;
    std::swap(best, current);
    return true;
}

class ReturnWatch final
{
    std::vector<std::wstring> monitors_;
    HANDLE removed_ = nullptr;
    HANDLE arrived_ = nullptr;
    HCMNOTIFICATION notification_ = nullptr;
    static DWORD CALLBACK Notify(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA data,
                                 DWORD) noexcept
    {
        // Runs on a system thread pool thread. monitors_ is complete before the registration and never changes.
        const ReturnWatch* self = static_cast<const ReturnWatch*>(context);
        const wchar_t* link = data->u.DeviceInterface.SymbolicLink;
        if (std::none_of(self->monitors_.begin(), self->monitors_.end(), [link](const std::wstring& monitor)
                         { return CompareStringOrdinal(monitor.c_str(), -1, link, -1, TRUE) == CSTR_EQUAL; }))
            return ERROR_SUCCESS;

        if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
            static_cast<void>(SetEvent(self->removed_));
        else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL)
            static_cast<void>(SetEvent(self->arrived_));
        return ERROR_SUCCESS;
    }

public:
    explicit ReturnWatch(const DisplayConfig& config)
    {
        for (const DISPLAYCONFIG_PATH_INFO& path : config.paths)
        {
            if (!IsNvidia(path))
                continue;
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorDevicePath[0])
                monitors_.emplace_back(target.monitorDevicePath);
        }

        if (monitors_.empty())
            return;
        removed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        arrived_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!removed_ || !arrived_)
            return;

        CM_NOTIFY_FILTER filter{};
        filter.cbSize = sizeof(filter);
        filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        filter.u.DeviceInterface.ClassGuid = MonitorInterface;
        if (CM_Register_Notification(&filter, this, Notify, &notification_) != CR_SUCCESS)
            notification_ = nullptr;
    }

    ReturnWatch(const ReturnWatch&) = delete;
    ReturnWatch& operator=(const ReturnWatch&) = delete;

    ~ReturnWatch()
    {
        if (notification_)
            CM_Unregister_Notification(notification_);
        if (removed_)
            CloseHandle(removed_);
        if (arrived_)
            CloseHandle(arrived_);
    }

    bool Removed(DWORD timeout) const
    {
        return notification_ && WaitForSingleObject(removed_, timeout) == WAIT_OBJECT_0;
    }

    void Arrived(DWORD timeout) const
    {
        static_cast<void>(WaitForSingleObject(arrived_, timeout));
    }
};

inline ULONGLONG WaitForReturn(const ReturnWatch& watch, const DisplayConfig& expected)
{
    // No removal means the driver kept the monitor during the mode change, or notifications are unavailable.
    // A fixed wait covers both cases.
    if (!watch.Removed(RemovalWait))
    {
        Sleep(ModesetWait);
        return 0;
    }

    const ULONGLONG deadline = GetTickCount64() + ReturnTimeout;
    watch.Arrived(ReturnTimeout);
    while (!Present(expected) && GetTickCount64() < deadline)
        Sleep(PollWait);
    const ULONGLONG returned = GetTickCount64();
    Sleep(ReturnSettle);
    return returned;
}

// The driver can bring a display back at a lower refresh rate shortly after the reconnect. This polls because the
// command-line mode has no window to receive WM_DISPLAYCHANGE.
inline void KeepRate(DisplayConfig& best, ULONGLONG until, bool& redrawn)
{
    while (GetTickCount64() < until)
    {
        Sleep(PollWait);
        DisplayConfig now;
        if (now.Query() && Slower(now, best))
        {
            Sleep(ReturnSettle);
            redrawn = best.Apply();
            return;
        }
    }
}

inline Result Execute(Action action, Mode preference = Mode::Fullscreen)
{
    ToggleLock lock;
    Result result = lock.Acquire();
    if (result.code != ExitCode::Success)
        return result;

    DrsSession drs;
    result = drs.Open();
    if (result.code != ExitCode::Success)
        return result;

    Values before{};
    result = ReadValues(drs, before);
    if (result.code != ExitCode::Success)
        return result;

    const State current = DecodeState(before[ApplicationMode]);
    const Decision decision = Decide(action, current, preference);
    if (decision.refuse)
        return Failure(ExitCode::StateUnknown, L"Application Mode (Enable G-SYNC globally) has an unknown value. "
                                               L"Set G-SYNC in the NVIDIA Control Panel, or use /off, /fs or /fsw.");
    result.hasRemembered = decision.hasRemembered;
    result.remembered = decision.remembered;
    result.state = current;
    if (!decision.apply)
        return result;

    const bool onOff = IsOn(current) != IsOn(decision.target);
    Values plan = MakePlan(decision.target);
    if (IsOn(current) && IsOn(decision.target))
        for (const std::size_t i : {ApplicationState, GlobalFeature, PreferredRefresh})
            plan[i] = before[i];
    if (before == plan)
        return result;

    bool savedAny = false;
    Result failure;
    const auto save = [&](std::initializer_list<std::size_t> group)
    {
        bool changed = false;
        for (const std::size_t i : group)
        {
            if (before[i] == plan[i])
                continue;
            NVDRS_SETTING setting{};
            setting.version = NVDRS_SETTING_VER;
            setting.settingId = SettingIds[i];
            setting.settingType = NVDRS_DWORD_TYPE;
            setting.u32CurrentValue = plan[i];
            const NvAPI_Status s = NvAPI_DRS_SetSetting(drs.session, drs.profile, &setting);
            if (s != NVAPI_OK)
            {
                failure =
                    Failure(ExitCode::NvidiaSetting,
                            savedAny ? PartialChange : L"NVIDIA rejected the change. The settings were not saved.", s);
                return false;
            }
            changed = true;
        }
        if (!changed)
            return true;
        const NvAPI_Status s = NvAPI_DRS_SaveSettings(drs.session);
        if (s != NVAPI_OK)
        {
            failure =
                Failure(ExitCode::NvidiaSave, savedAny ? PartialChange : L"The NVIDIA settings could not be saved.", s);
            return false;
        }
        savedAny = true;
        return true;
    };

    DisplayConfig previous;
    if (onOff)
        static_cast<void>(previous.Query());
    // Same three saves, in the same order, as the NVIDIA Control Panel. The first one makes the driver reconnect
    // the display, and the next ones must wait until it is back.
    const ReturnWatch watch(previous);
    if (!save({ApplicationMode, ApplicationState}))
        return failure;
    const ULONGLONG returned = onOff ? WaitForReturn(watch, previous) : 0;
    if (!save({GlobalFeature}))
        return failure;
    bool redrawn = true;
    if (onOff)
    {
        redrawn = RedrawDisplays(previous);
        Sleep(StepWait);
    }
    if (!save({PreferredRefresh, GlobalMode}))
        return failure;
    if (returned)
        KeepRate(previous, returned + RateWatch, redrawn);

    Values after{};
    Result check = drs.Reload();
    if (check.code == ExitCode::Success)
        check = ReadValues(drs, after);
    result.state = check.code == ExitCode::Success ? DecodeState(after[ApplicationMode]) : State::Unknown;
    if (check.code != ExitCode::Success || after != plan)
    {
        result.code = ExitCode::Verification;
        result.error = L"The NVIDIA settings were saved, but the result could not be confirmed. "
                       L"Another program may have changed them. Check the state before toggling again.";
    }
    else if (!redrawn)
    {
        result.code = ExitCode::Verification;
        result.error = L"G-SYNC was changed, but Windows could not redraw the displays. "
                       L"If a display stays black or at a lower refresh rate, turn it off and back on.";
    }
    return result;
}

class NvapiScope final
{
    bool loaded_ = false;

public:
    NvapiScope() = default;
    NvapiScope(const NvapiScope&) = delete;
    NvapiScope& operator=(const NvapiScope&) = delete;

    ~NvapiScope()
    {
        if (loaded_)
            NvAPI_Unload();
    }

    Result Open()
    {
        const NvAPI_Status s = NvAPI_Initialize();
        if (s != NVAPI_OK)
            return Failure(ExitCode::NvidiaInitialize, NvapiMissing, s);
        loaded_ = true;
        return {};
    }
};

struct DisplayTarget
{
    NvU32 id = 0;
    DisplayModel model;
    NV_GET_ADAPTIVE_SYNC_DATA sync{};
};

inline bool ReadSync(NvU32 id, NV_GET_ADAPTIVE_SYNC_DATA& sync)
{
    sync = {};
    sync.version = NV_GET_ADAPTIVE_SYNC_DATA_VER;
    return NvAPI_DISP_GetAdaptiveSyncData(id, &sync) == NVAPI_OK;
}

// A display whose EDID IDs or NVIDIA data cannot be read is left out: no model profile can be named for it.
inline bool Describe(const DISPLAYCONFIG_PATH_INFO& path, DisplayTarget& target)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME monitor{};
    monitor.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    monitor.header.size = sizeof(monitor);
    monitor.header.adapterId = path.targetInfo.adapterId;
    monitor.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&monitor.header) != ERROR_SUCCESS || !monitor.flags.edidIdsValid)
        return false;

    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    char gdiName[CCHDEVICENAME]{};
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
        !WideCharToMultiByte(CP_ACP, 0, source.viewGdiDeviceName, -1, gdiName, static_cast<int>(sizeof(gdiName)),
                             nullptr, nullptr) ||
        NvAPI_DISP_GetDisplayIdByDisplayName(gdiName, &target.id) != NVAPI_OK || !ReadSync(target.id, target.sync))
        return false;

    // supportVRR follows the VRR switch in the monitor's own menu. currentlyCapableOfVRR also follows the choice
    // made here, so it cannot tell whether the display belongs in the list.
    NV_MONITOR_CAPABILITIES caps{};
    caps.version = NV_MONITOR_CAPABILITIES_VER;
    caps.size = static_cast<NvU16>(sizeof(caps));
    caps.infoType = NV_MONITOR_CAPS_TYPE_GENERIC;
    target.model.vrr = NvAPI_DISP_GetMonitorCapabilities(target.id, &caps) == NVAPI_OK && caps.bIsValidInfo &&
                       caps.data.caps.supportVRR;
    target.model.name = monitor.monitorFriendlyDeviceName;
    target.model.manufacturer = monitor.edidManufactureId;
    target.model.product = monitor.edidProductCodeId;
    target.model.allowed = !target.sync.bDisableAdaptiveSync;
    return true;
}

inline bool ReadTargets(std::vector<DisplayTarget>& targets)
{
    targets.clear();
    DisplayConfig config;
    if (!config.Query())
        return false;
    for (const DISPLAYCONFIG_PATH_INFO& path : config.paths)
    {
        DisplayTarget target;
        // Cloned displays share one source, so NVAPI returns the same display ID for each of them.
        if (IsNvidia(path) && Describe(path, target) &&
            std::none_of(targets.begin(), targets.end(),
                         [&target](const DisplayTarget& known) { return known.id == target.id; }))
            targets.push_back(std::move(target));
    }
    return true;
}

inline std::vector<DisplayModel> ModelsOf(const std::vector<DisplayTarget>& targets)
{
    std::vector<DisplayModel> displays;
    for (const DisplayTarget& target : targets)
        displays.push_back(target.model);
    return GroupModels(displays);
}

// The caller keeps NVAPI loaded.
inline bool ReadModels(std::vector<DisplayModel>& models)
{
    std::vector<DisplayTarget> targets;
    if (!ReadTargets(targets))
        return false;
    models = ModelsOf(targets);
    return true;
}

inline Result ListModels(std::vector<DisplayModel>& models)
{
    NvapiScope nvapi;
    Result result = nvapi.Open();
    if (result.code != ExitCode::Success)
        return result;
    if (!ReadModels(models))
        return Failure(ExitCode::DisplayRefused, L"The active displays could not be read.");
    if (models.empty())
        return Failure(ExitCode::DisplayNotFound, L"No active NVIDIA display.");
    return result;
}

inline void ToUnicode(const std::wstring& text, NvAPI_UnicodeString& out)
{
    std::fill(std::begin(out), std::end(out), static_cast<NvU16>(0));
    const std::size_t length = (std::min)(text.size(), std::size(out) - 1);
    for (std::size_t i = 0; i < length; ++i)
        out[i] = static_cast<NvU16>(text[i]);
}

// NvAPI_DRS_GetSetting also returns values inherited from the global profile, so only a value stored in this
// profile counts.
inline bool HoldsDword(NvDRSSessionHandle session, NvDRSProfileHandle profile, NvU32 id, NvU32 value)
{
    NVDRS_SETTING setting{};
    setting.version = NVDRS_SETTING_VER;
    return NvAPI_DRS_GetSetting(session, profile, id, &setting) == NVAPI_OK &&
           setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION && setting.settingType == NVDRS_DWORD_TYPE &&
           setting.u32CurrentValue == value;
}

inline NvAPI_Status WriteDword(NvDRSSessionHandle session, NvDRSProfileHandle profile, NvU32 id, NvU32 value)
{
    NVDRS_SETTING setting{};
    setting.version = NVDRS_SETTING_VER;
    setting.settingId = id;
    setting.settingType = NVDRS_DWORD_TYPE;
    setting.u32CurrentValue = value;
    return NvAPI_DRS_SetSetting(session, profile, &setting);
}

inline NvAPI_Status CreateModelProfile(NvDRSSessionHandle session, const DisplayModel& model,
                                       NvDRSProfileHandle& profile)
{
    NVDRS_PROFILE info{};
    info.version = NVDRS_PROFILE_VER;
    ToUnicode(ProfileName(model), info.profileName);
    return NvAPI_DRS_CreateProfile(session, &info, &profile);
}

inline NvAPI_Status AddModelApplication(NvDRSSessionHandle session, NvDRSProfileHandle profile,
                                        const DisplayModel& model, bool& added)
{
    NvAPI_UnicodeString name{};
    ToUnicode(ApplicationName(model), name);
    // Each NVDRS_APPLICATION holds four 4 KB strings, so it is zeroed on the heap rather than on the stack.
    const auto found = std::make_unique<NVDRS_APPLICATION>();
    found->version = NVDRS_APPLICATION_VER;
    const NvAPI_Status s = NvAPI_DRS_GetApplicationInfo(session, profile, name, found.get());
    if (s != NVAPI_EXECUTABLE_NOT_FOUND)
        return s;

    const auto application = std::make_unique<NVDRS_APPLICATION>();
    application->version = NVDRS_APPLICATION_VER;
    ToUnicode(ApplicationName(model), application->appName);
    ToUnicode(ProfileName(model), application->userFriendlyName);
    const NvAPI_Status created = NvAPI_DRS_CreateApplication(session, profile, application.get());
    added = created == NVAPI_OK;
    return created;
}

// The same profile as the NVIDIA Control Panel's: hidden from its program list, with one application named after
// the profile, so the Control Panel shows and edits this choice as its own. Nothing reaches the NVIDIA settings
// unless every step succeeds, since only NvAPI_DRS_SaveSettings stores the session.
inline Result SaveModelChoice(const DrsSession& drs, const DisplayModel& model, bool allow)
{
    NvAPI_UnicodeString name{};
    ToUnicode(ProfileName(model), name);
    NvDRSProfileHandle profile = nullptr;
    bool changed = false;
    NvAPI_Status s = NvAPI_DRS_FindProfileByName(drs.session, name, &profile);
    if (s == NVAPI_PROFILE_NOT_FOUND)
    {
        s = CreateModelProfile(drs.session, model, profile);
        changed = true;
    }
    if (s != NVAPI_OK)
        return Failure(ExitCode::NvidiaSetting, ChoiceRejected, s);

    bool added = false;
    s = AddModelApplication(drs.session, profile, model, added);
    if (s == NVAPI_EXECUTABLE_ALREADY_IN_USE)
        return Failure(ExitCode::NvidiaSetting, (L"Another NVIDIA profile already uses the application " +
                                                 ApplicationName(model) + L". Nothing was changed.")
                                                    .c_str());
    if (s != NVAPI_OK)
        return Failure(ExitCode::NvidiaSetting, ChoiceRejected, s);
    changed = changed || added;

    struct Wanted
    {
        NvU32 id;
        NvU32 value;
    };
    for (const Wanted wanted : {Wanted{HiddenProfileSetting, 1u}, Wanted{ModelChoiceSetting, allow ? 1u : 0u}})
    {
        if (HoldsDword(drs.session, profile, wanted.id, wanted.value))
            continue;
        s = WriteDword(drs.session, profile, wanted.id, wanted.value);
        if (s != NVAPI_OK)
            return Failure(ExitCode::NvidiaSetting, ChoiceRejected, s);
        changed = true;
    }
    if (!changed)
        return {};
    s = NvAPI_DRS_SaveSettings(drs.session);
    if (s != NVAPI_OK)
        return Failure(ExitCode::NvidiaSave, L"The NVIDIA settings could not be saved. Nothing was changed.", s);
    return {};
}

inline bool ModelShows(const std::vector<DisplayTarget>& targets, const DisplayModel& model, NvU32 disabled)
{
    for (const DisplayTarget& target : targets)
    {
        NV_GET_ADAPTIVE_SYNC_DATA sync{};
        if (SameModel(target.model, model) && target.model.vrr &&
            (!ReadSync(target.id, sync) || sync.bDisableAdaptiveSync != disabled))
            return false;
    }
    return true;
}

// The driver reads a saved choice only when the display reconnects, so the choice is also applied to each display
// now, as the Control Panel does. Applying it can itself reconnect the display, and a value read during the
// reconnect is the old one, so the check waits until the display is back.
inline Result ApplyModelChoice(const std::vector<DisplayTarget>& targets, const DisplayModel& model, bool allow)
{
    const NvU32 disabled = allow ? 0u : 1u;
    DisplayConfig config;
    static_cast<void>(config.Query());
    const ReturnWatch watch(config);
    bool changed = false;
    for (const DisplayTarget& target : targets)
    {
        if (!SameModel(target.model, model) || !target.model.vrr || target.sync.bDisableAdaptiveSync == disabled)
            continue;
        // Set replaces every field, so the values just read are sent back and only the choice changes.
        NV_SET_ADAPTIVE_SYNC_DATA set{};
        set.version = NV_SET_ADAPTIVE_SYNC_DATA_VER;
        set.maxFrameInterval = target.sync.maxFrameInterval;
        set.maxFrameIntervalNs = static_cast<NvU64>(target.sync.maxFrameInterval) * 1000u;
        set.bDisableAdaptiveSync = disabled;
        set.bDisableFrameSplitting = target.sync.bDisableFrameSplitting;
        const NvAPI_Status s = NvAPI_DISP_SetAdaptiveSyncData(target.id, &set);
        if (s != NVAPI_OK)
            return Failure(ExitCode::DisplayRefused,
                           L"The choice was saved, but NVIDIA could not apply it to the display now.", s);
        changed = true;
    }
    if (!changed)
        return {};

    if (watch.Removed(RemovalWait))
    {
        const ULONGLONG back = GetTickCount64() + ReturnTimeout;
        watch.Arrived(ReturnTimeout);
        while (!Present(config) && GetTickCount64() < back)
            Sleep(PollWait);
        Sleep(ReturnSettle);
    }
    const ULONGLONG until = GetTickCount64() + ChoiceTimeout;
    while (!ModelShows(targets, model, disabled))
    {
        if (GetTickCount64() >= until)
            return Failure(ExitCode::Verification, L"The choice was saved, but the display did not confirm it. "
                                                   L"Check the option in the NVIDIA Control Panel.");
        Sleep(PollWait);
    }
    return {};
}

inline Result ChooseModel(const DrsSession& drs, const std::wstring& wanted, bool allow)
{
    std::vector<DisplayTarget> targets;
    if (!ReadTargets(targets))
        return Failure(ExitCode::DisplayRefused, L"The active displays could not be read. Nothing was changed.");
    const std::vector<DisplayModel> models = ModelsOf(targets);
    const std::vector<std::size_t> found = MatchModels(models, wanted);
    if (found.empty())
        return Failure(ExitCode::DisplayNotFound,
                       (L"No active NVIDIA display is named " + wanted + L". Nothing was changed.").c_str());
    if (found.size() > 1)
        return Failure(ExitCode::DisplayAmbiguous, (L"Several display models are named " + wanted +
                                                    L". Use the model code shown by /list. Nothing was changed.")
                                                       .c_str());
    const DisplayModel& model = models[found.front()];
    const Result saved = SaveModelChoice(drs, model, allow);
    if (saved.code != ExitCode::Success)
        return saved;
    return ApplyModelChoice(targets, model, allow);
}

inline Result ChooseForDisplay(const std::wstring& wanted, bool allow)
{
    ToggleLock lock;
    Result result = lock.Acquire();
    if (result.code != ExitCode::Success)
        return result;
    DrsSession drs;
    result = drs.Open();
    if (result.code != ExitCode::Success)
        return result;
    Values values{};
    result = ReadValues(drs, values);
    if (result.code != ExitCode::Success)
        return result;

    const State state = DecodeState(values[ApplicationMode]);
    if (state == State::Unknown)
        return Failure(ExitCode::StateUnknown, L"Application Mode (Enable G-SYNC globally) has an unknown value. "
                                               L"Nothing was changed.");
    // NVIDIA refuses the change while G-SYNC is off, and the Control Panel grays the option out.
    result = state == State::Off ? Failure(ExitCode::GsyncOff, L"G-SYNC is off. Turn it on, then allow or disallow "
                                                               L"the display again. Nothing was changed.")
                                 : ChooseModel(drs, wanted, allow);
    result.state = state;
    return result;
}

}
