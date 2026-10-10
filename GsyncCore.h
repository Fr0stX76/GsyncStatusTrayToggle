#pragma once
#include <windows.h>
#include <sddl.h>
#include <cfgmgr32.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include "nvapi.h"
#include "NvApiDriverSettings.h"
#include "GsyncLogic.h"

namespace gsync {

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

inline constexpr wchar_t MutexName[] = L"Global\\GsyncToggleMutex";
inline constexpr DWORD MutexTimeout = 15000;
inline constexpr DWORD ModesetWait = 1500;
inline constexpr DWORD StepWait = 200;
inline constexpr DWORD RemovalWait = 500;
inline constexpr DWORD ReturnTimeout = 4000;
inline constexpr DWORD ReturnSettle = 600;
inline constexpr DWORD RateWatch = 2500;
inline constexpr DWORD PollWait = 50;

inline constexpr GUID MonitorInterface = {
    0xE6F07B5Fu, 0xEE97u, 0x4A90u, {0xB0u, 0x76u, 0x33u, 0xF5u, 0x7Bu, 0xF4u, 0xEAu, 0xA7u}};
inline constexpr wchar_t PartialChange[] =
    L"Part of the change was saved, then NVIDIA refused the rest. Set G-SYNC in the NVIDIA Control Panel.";

enum class ExitCode : int {
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
    Arguments = 64
};

struct Result {
    ExitCode code = ExitCode::Success;
    std::wstring error;
    Mode remembered = Mode::Fullscreen;
    bool hasRemembered = false;
};

inline Result Failure(ExitCode code, const wchar_t* text, NvAPI_Status status = NVAPI_OK) {
    Result r;
    r.code = code;
    r.error = text;
    if (status != NVAPI_OK) {
        NvAPI_ShortString detail{};
        if (NvAPI_GetErrorMessage(status, detail) == NVAPI_OK) {
            r.error += L"\n\n";
            r.error.append(detail, detail + std::strlen(detail));
        }
        r.error += L" (NVIDIA code " + std::to_wstring(static_cast<int>(status)) + L")";
    }
    return r;
}

class ToggleLock final {
    HANDLE handle_ = nullptr;
    bool owned_ = false;

public:
    ToggleLock() = default;
    ToggleLock(const ToggleLock&) = delete;
    ToggleLock& operator=(const ToggleLock&) = delete;

    ~ToggleLock() {
        if (owned_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }

    Result Acquire() {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x100000;;;AU)",
                                                                 SDDL_REVISION_1, &descriptor, nullptr)) {
            SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
            handle_ = CreateMutexExW(&attributes, MutexName, 0, SYNCHRONIZE);
            LocalFree(descriptor);
        }
        if (!handle_) return Failure(ExitCode::MutexOpen, L"Unable to open the G-SYNC mutex.");

        const DWORD wait = WaitForSingleObject(handle_, MutexTimeout);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            owned_ = true;
            return {};
        }
        if (wait == WAIT_TIMEOUT)
            return Failure(ExitCode::MutexTimeout,
                           L"Another G-SYNC command is still running. Try again in a few seconds.");
        return Failure(ExitCode::MutexWait, L"Unable to wait for the G-SYNC mutex.");
    }
};

class DrsSession final {
    bool initialized_ = false;

public:
    NvDRSSessionHandle session = nullptr;
    NvDRSProfileHandle profile = nullptr;
    DrsSession() = default;
    DrsSession(const DrsSession&) = delete;
    DrsSession& operator=(const DrsSession&) = delete;

    ~DrsSession() {
        if (session) NvAPI_DRS_DestroySession(session);
        if (initialized_) NvAPI_Unload();
    }

    Result Reload() {
        NvAPI_Status s = NvAPI_DRS_LoadSettings(session);
        if (s != NVAPI_OK) return Failure(ExitCode::NvidiaLoad, L"Unable to load the NVIDIA settings.", s);
        s = NvAPI_DRS_GetBaseProfile(session, &profile);
        if (s != NVAPI_OK) return Failure(ExitCode::NvidiaProfile, L"Unable to open the NVIDIA global profile.", s);
        return {};
    }

    Result Open() {
        NvAPI_Status s = NvAPI_Initialize();
        if (s != NVAPI_OK) return Failure(ExitCode::NvidiaInitialize, L"Unable to initialize NVAPI.", s);
        initialized_ = true;
        s = NvAPI_DRS_CreateSession(&session);
        if (s != NVAPI_OK) return Failure(ExitCode::NvidiaSession, L"Unable to open the NVIDIA settings.", s);
        return Reload();
    }
};

inline Result ReadValues(const DrsSession& drs, Values& values) {
    for (std::size_t i = 0; i < SettingCount; ++i) {
        NVDRS_SETTING setting{};
        setting.version = NVDRS_SETTING_VER;
        const NvAPI_Status s = NvAPI_DRS_GetSetting(drs.session, drs.profile, SettingIds[i], &setting);
        if (s == NVAPI_SETTING_NOT_FOUND) {
            values[i] = SettingDefaults[i];
            continue;
        }
        if (s != NVAPI_OK || setting.settingType != NVDRS_DWORD_TYPE)
            return Failure(ExitCode::StateUnknown, L"Unable to read a G-SYNC setting. Nothing was changed.", s);
        values[i] = setting.u32CurrentValue;
    }
    return {};
}

inline bool IsNvidia(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_ADAPTER_NAME adapter{};
    adapter.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
    adapter.header.size = sizeof(adapter);
    adapter.header.adapterId = path.targetInfo.adapterId;
    return DisplayConfigGetDeviceInfo(&adapter.header) == ERROR_SUCCESS &&
           Lower(adapter.adapterDevicePath).find(L"ven_10de") != std::wstring::npos;
}

struct DisplayConfig {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;

    bool Query() {
        LONG rc = ERROR_INSUFFICIENT_BUFFER;
        for (int attempt = 0; attempt < 3 && rc == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
            UINT32 pathCount = 0, modeCount = 0;
            rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
            if (rc != ERROR_SUCCESS) break;
            paths.resize(pathCount);
            modes.resize(modeCount);
            rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
            paths.resize(pathCount);
            modes.resize(modeCount);
        }
        if (rc == ERROR_SUCCESS) return true;
        paths.clear();
        modes.clear();
        return false;
    }

    bool Apply() {
        if (paths.empty()) return false;
        const UINT32 pathCount = static_cast<UINT32>(paths.size());
        const UINT32 modeCount = static_cast<UINT32>(modes.size());
        const UINT32 flags = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_NO_OPTIMIZATION;
        return SetDisplayConfig(pathCount, paths.data(), modeCount, modes.data(), flags) == ERROR_SUCCESS;
    }
};

inline bool SameTarget(const DISPLAYCONFIG_PATH_INFO& a, const DISPLAYCONFIG_PATH_INFO& b) {
    return a.targetInfo.id == b.targetInfo.id && a.targetInfo.adapterId.LowPart == b.targetInfo.adapterId.LowPart &&
           a.targetInfo.adapterId.HighPart == b.targetInfo.adapterId.HighPart;
}

inline bool Slower(const DisplayConfig& a, const DisplayConfig& b) {
    for (const DISPLAYCONFIG_PATH_INFO& x : a.paths) {
        if (!IsNvidia(x)) continue;
        for (const DISPLAYCONFIG_PATH_INFO& y : b.paths) {
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

inline bool Present(const DisplayConfig& expected) {
    DisplayConfig now;
    if (!now.Query()) return false;
    for (const DISPLAYCONFIG_PATH_INFO& x : expected.paths)
        if (IsNvidia(x) && std::none_of(now.paths.begin(), now.paths.end(),
                                        [&x](const DISPLAYCONFIG_PATH_INFO& y) { return SameTarget(x, y); }))
            return false;
    return true;
}

inline bool RedrawDisplays(DisplayConfig& best) {
    DisplayConfig current;
    if (!current.Query() || std::none_of(current.paths.begin(), current.paths.end(), IsNvidia)) return true;
    if (Slower(best, current)) std::swap(best, current);
    if (best.Apply()) return true;
    if (Slower(current, best) || !current.Apply()) return false;
    std::swap(best, current);
    return true;
}

class ReturnWatch final {
    std::vector<std::wstring> monitors_;
    HANDLE removed_ = nullptr;
    HANDLE arrived_ = nullptr;
    HCMNOTIFICATION notification_ = nullptr;
    static DWORD CALLBACK Notify(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA data,
                                 DWORD) {
        const ReturnWatch* self = static_cast<const ReturnWatch*>(context);
        const wchar_t* link = data->u.DeviceInterface.SymbolicLink;
        if (std::none_of(self->monitors_.begin(), self->monitors_.end(), [link](const std::wstring& monitor) {
                return CompareStringOrdinal(monitor.c_str(), -1, link, -1, TRUE) == CSTR_EQUAL;
            }))
            return ERROR_SUCCESS;

        if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
            SetEvent(self->removed_);
        else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL)
            SetEvent(self->arrived_);
        return ERROR_SUCCESS;
    }

public:
    explicit ReturnWatch(const DisplayConfig& config) {
        for (const DISPLAYCONFIG_PATH_INFO& path : config.paths) {
            if (!IsNvidia(path)) continue;
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorDevicePath[0])
                monitors_.emplace_back(target.monitorDevicePath);
        }

        if (monitors_.empty()) return;
        removed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        arrived_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!removed_ || !arrived_) return;

        CM_NOTIFY_FILTER filter{};
        filter.cbSize = sizeof(filter);
        filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        filter.u.DeviceInterface.ClassGuid = MonitorInterface;
        if (CM_Register_Notification(&filter, this, Notify, &notification_) != CR_SUCCESS) notification_ = nullptr;
    }

    ReturnWatch(const ReturnWatch&) = delete;
    ReturnWatch& operator=(const ReturnWatch&) = delete;

    ~ReturnWatch() {
        if (notification_) CM_Unregister_Notification(notification_);
        if (removed_) CloseHandle(removed_);
        if (arrived_) CloseHandle(arrived_);
    }

    bool Removed(DWORD timeout) const {
        return notification_ && WaitForSingleObject(removed_, timeout) == WAIT_OBJECT_0;
    }

    void Arrived(DWORD timeout) const { WaitForSingleObject(arrived_, timeout); }
};

inline ULONGLONG WaitForReturn(const ReturnWatch& watch, const DisplayConfig& expected) {
    if (!watch.Removed(RemovalWait)) {
        Sleep(ModesetWait);
        return 0;
    }

    const ULONGLONG deadline = GetTickCount64() + ReturnTimeout;
    watch.Arrived(ReturnTimeout);
    while (!Present(expected) && GetTickCount64() < deadline) Sleep(PollWait);
    const ULONGLONG returned = GetTickCount64();
    Sleep(ReturnSettle);
    return returned;
}

inline void KeepRate(DisplayConfig& best, ULONGLONG until, bool& redrawn) {
    while (GetTickCount64() < until) {
        Sleep(PollWait);
        DisplayConfig now;
        if (now.Query() && Slower(now, best)) {
            Sleep(ReturnSettle);
            redrawn = best.Apply();
            return;
        }
    }
}

inline Result Execute(Action action, Mode preference = Mode::Fullscreen) {
    ToggleLock lock;
    Result result = lock.Acquire();
    if (result.code != ExitCode::Success) return result;

    DrsSession drs;
    result = drs.Open();
    if (result.code != ExitCode::Success) return result;

    Values before{};
    result = ReadValues(drs, before);
    if (result.code != ExitCode::Success) return result;

    const State current = DecodeState(before[ApplicationMode]);
    const Decision decision = Decide(action, current, preference);
    if (decision.refuse)
        return Failure(
            ExitCode::StateUnknown,
            L"Application Mode (Enable G-SYNC globally) has an unknown value. Set G-SYNC in the NVIDIA Control Panel, or use /off, /fs or /fsw.");
    result.hasRemembered = decision.hasRemembered;
    result.remembered = decision.remembered;
    if (!decision.apply) return result;

    const bool onOff = IsOn(current) != IsOn(decision.target);
    Values plan = MakePlan(decision.target);
    if (IsOn(current) && IsOn(decision.target))
        for (const std::size_t i : {ApplicationState, GlobalFeature, PreferredRefresh}) plan[i] = before[i];
    if (before == plan) return result;

    bool savedAny = false;
    Result failure;
    const auto save = [&](std::initializer_list<std::size_t> group) {
        bool changed = false;
        for (const std::size_t i : group) {
            if (before[i] == plan[i]) continue;
            NVDRS_SETTING setting{};
            setting.version = NVDRS_SETTING_VER;
            setting.settingId = SettingIds[i];
            setting.settingType = NVDRS_DWORD_TYPE;
            setting.u32CurrentValue = plan[i];
            const NvAPI_Status s = NvAPI_DRS_SetSetting(drs.session, drs.profile, &setting);
            if (s != NVAPI_OK) {
                failure =
                    Failure(ExitCode::NvidiaSetting,
                            savedAny ? PartialChange : L"NVIDIA rejected the change. The settings were not saved.", s);
                return false;
            }
            changed = true;
        }
        if (!changed) return true;
        const NvAPI_Status s = NvAPI_DRS_SaveSettings(drs.session);
        if (s != NVAPI_OK) {
            failure =
                Failure(ExitCode::NvidiaSave, savedAny ? PartialChange : L"Unable to save the NVIDIA settings.", s);
            return false;
        }
        savedAny = true;
        return true;
    };

    DisplayConfig previous;
    if (onOff) previous.Query();
    const ReturnWatch watch(previous);
    if (!save({ApplicationMode, ApplicationState})) return failure;
    const ULONGLONG returned = onOff ? WaitForReturn(watch, previous) : 0;
    if (!save({GlobalFeature})) return failure;
    bool redrawn = true;
    if (onOff) {
        redrawn = RedrawDisplays(previous);
        Sleep(StepWait);
    }
    if (!save({PreferredRefresh, GlobalMode})) return failure;
    if (returned) KeepRate(previous, returned + RateWatch, redrawn);

    Values after{};
    Result check = drs.Reload();
    if (check.code == ExitCode::Success) check = ReadValues(drs, after);
    if (check.code != ExitCode::Success || after != plan) {
        result.code = ExitCode::Verification;
        result.error =
            L"The NVIDIA settings were saved, but the result could not be confirmed. Another program may have changed them. Check the state before toggling again.";
    } else if (!redrawn) {
        result.code = ExitCode::Verification;
        result.error =
            L"G-SYNC was changed, but Windows could not redraw the displays. If a display stays black or at a lower refresh rate, turn it off and back on.";
    }
    return result;
}

}
