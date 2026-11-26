// ============================================================================
// main.cpp — AdaptiveSimSpeed entry point, loader ABI & F9 "Sim Speed" tab
// ============================================================================
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include "mod_overlay.h"
#include "wrsr/loader_abi.hpp"
#include "wrsr/logger.hpp"
#include "wrsr/pe_utils.hpp"

#include "config.hpp"
#include "sim_speed_hook.hpp"

#ifndef WRSR_MOD_VERSION
#define WRSR_MOD_VERSION "1.0.0"
#endif

#ifndef WRSR_GAME_VERSION
#define WRSR_GAME_VERSION "latest"
#endif

namespace {

std::atomic<bool>            g_workerStarted{false};
const wrsr::abi::RmlHostApi* g_rmlHost = nullptr;
const wrsr::abi::TsmHost*    g_tsmHost = nullptr;

char g_saveFeedback[128] = "Tip: Changes take effect immediately; click Save Config to persist.";

std::wstring getSelfModuleDir()
{
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&getSelfModuleDir),
            &hMod) || !hMod) {
        return {};
    }
    std::wstring path(MAX_PATH * 4, L'\0');
    const DWORD len = GetModuleFileNameW(hMod, path.data(), static_cast<DWORD>(path.size()));
    if (len == 0 || len >= path.size()) {
        return {};
    }
    path.resize(len);
    const size_t slash = path.find_last_of(L"\\/");
    return (slash != std::wstring::npos) ? path.substr(0, slash) : std::wstring{};
}

void WRSR_OVERLAY_CALL onSettingChanged(void* /*userData*/)
{
    ass::Config::get().syncFromUi();
}

void WRSR_OVERLAY_CALL onPresetClicked(void* userData)
{
    const uintptr_t encoded = reinterpret_cast<uintptr_t>(userData);
    const float     mult    = static_cast<float>(encoded);
    ass::Config::get().applyPreset(mult);
    std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                  "Applied %.0fx max speed preset (unsaved — click Save Config to persist).", mult);
}

void WRSR_OVERLAY_CALL onSaveConfig(void* /*userData*/)
{
    if (ass::Config::get().save()) {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "Saved AdaptiveSimSpeed.ini successfully.");
    } else {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "ERROR: Could not write AdaptiveSimSpeed.ini.");
    }
}

void registerOverlayTab()
{
    if (!ModOverlay_Initialize()) {
        wrsr::log::error("ModOverlay_Initialize failed — F9 overlay unavailable");
        return;
    }

    ModTabDesc tab{};
    tab.struct_size = sizeof(ModTabDesc);
    tab.tab_id      = "zydrahydra.adaptive_sim_speed";
    tab.title       = "Sim Speed";
    tab.on_save     = onSaveConfig;
    tab.user_data   = nullptr;

    const int32_t tabId = RegisterModTab(&tab);
    if (tabId < 0) {
        wrsr::log::error("RegisterModTab('Sim Speed') failed");
        return;
    }

    auto& cfg = ass::Config::get();

    const uint32_t statusColor = (ass::hook::state() == ass::hook::HookState::Active)
                                     ? 0xA6D973FFu
                                     : 0xE6AD47FFu;

    ModLabelDesc statusLbl{};
    statusLbl.struct_size      = sizeof(ModLabelDesc);
    statusLbl.text             = ass::hook::statusText();
    statusLbl.separator_before = 0;
    statusLbl.rgba_color       = statusColor;
    AddLabel(tabId, &statusLbl);

    ModLabelDesc descLbl{};
    descLbl.struct_size      = sizeof(ModLabelDesc);
    descLbl.text             = "Executes extra fixed-step 1x simulation sub-steps inside unused frame headroom "
                               "without multiplying dt or dropping below your target FPS.";
    descLbl.separator_before = 1;
    descLbl.rgba_color       = 0;
    AddLabel(tabId, &descLbl);

    ModCheckboxDesc enableCheck{};
    enableCheck.struct_size = sizeof(ModCheckboxDesc);
    enableCheck.label       = "Enable Adaptive Fast-Forward";
    enableCheck.value       = cfg.uiEnabledPtr();
    enableCheck.tooltip     = "Master toggle for hardware-adaptive simulation sub-stepping.";
    enableCheck.on_change   = onSettingChanged;
    enableCheck.user_data   = nullptr;
    AddCheckbox(tabId, &enableCheck);

    ModCheckboxDesc only3xCheck{};
    only3xCheck.struct_size = sizeof(ModCheckboxDesc);
    only3xCheck.label       = "Only Boost 3x (>>) Speed";
    only3xCheck.value       = cfg.uiOnlyBoost3xPtr();
    only3xCheck.tooltip     = "Keep normal 1x Play vanilla for building/watching; only boost Fast-Forward (>>).";
    only3xCheck.on_change   = onSettingChanged;
    only3xCheck.user_data   = nullptr;
    AddCheckbox(tabId, &only3xCheck);

    ModFloatSliderDesc maxSpeedSlider{};
    maxSpeedSlider.struct_size = sizeof(ModFloatSliderDesc);
    maxSpeedSlider.label       = "Max Fast-Forward Speed";
    maxSpeedSlider.value       = cfg.uiMaxSpeedMultiplierPtr();
    maxSpeedSlider.min_value   = ass::kMinSpeedMultiplier;
    maxSpeedSlider.max_value   = ass::kMaxSpeedMultiplier;
    maxSpeedSlider.format      = "%.1fx";
    maxSpeedSlider.tooltip     = "Maximum simulation speed multiplier when CPU frame headroom allows.";
    maxSpeedSlider.on_change   = onSettingChanged;
    maxSpeedSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &maxSpeedSlider);

    ModFloatSliderDesc targetFpsSlider{};
    targetFpsSlider.struct_size = sizeof(ModFloatSliderDesc);
    targetFpsSlider.label       = "Target Min FPS";
    targetFpsSlider.value       = cfg.uiTargetMinFpsPtr();
    targetFpsSlider.min_value   = static_cast<float>(ass::kMinTargetFps);
    targetFpsSlider.max_value   = static_cast<float>(ass::kMaxTargetFps);
    targetFpsSlider.format      = "%.0f FPS";
    targetFpsSlider.tooltip     = "Minimum frame rate floor; sub-stepping scales down automatically to protect FPS.";
    targetFpsSlider.on_change   = onSettingChanged;
    targetFpsSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &targetFpsSlider);

    ModFloatSliderDesc reserveSlider{};
    reserveSlider.struct_size = sizeof(ModFloatSliderDesc);
    reserveSlider.label       = "Render Headroom Reserve (ms)";
    reserveSlider.value       = cfg.uiRenderReserveMsPtr();
    reserveSlider.min_value   = ass::kMinRenderReserveMs;
    reserveSlider.max_value   = ass::kMaxRenderReserveMs;
    reserveSlider.format      = "%.1f ms";
    reserveSlider.tooltip     = "Time reserved per frame for DX11 rendering and OS overhead.";
    reserveSlider.on_change   = onSettingChanged;
    reserveSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &reserveSlider);

    ModLabelDesc presetsLbl{};
    presetsLbl.struct_size      = sizeof(ModLabelDesc);
    presetsLbl.text             = "Quick Presets:";
    presetsLbl.separator_before = 1;
    presetsLbl.rgba_color       = 0;
    AddLabel(tabId, &presetsLbl);

    auto addPresetBtn = [&](const char* label, uintptr_t multVal, uint8_t sameLine) {
        ModButtonDesc btn{};
        btn.struct_size = sizeof(ModButtonDesc);
        btn.label       = label;
        btn.same_line   = sameLine;
        btn.tooltip     = "Set Max Fast-Forward Speed immediately.";
        btn.on_click    = onPresetClicked;
        btn.user_data   = reinterpret_cast<void*>(multVal);
        AddButton(tabId, &btn);
    };

    addPresetBtn("Vanilla 3x",   3,  0);
    addPresetBtn("Balanced 6x",  6,  1);
    addPresetBtn("Turbo 10x",    10, 1);
    addPresetBtn("Uncapped 15x", 15, 1);

    ModLabelDesc telemetryLbl{};
    telemetryLbl.struct_size      = sizeof(ModLabelDesc);
    telemetryLbl.text             = ass::hook::telemetryText();
    telemetryLbl.separator_before = 1;
    telemetryLbl.rgba_color       = 0xE6C873FFu;
    AddLabel(tabId, &telemetryLbl);

    ModButtonDesc saveBtn{};
    saveBtn.struct_size = sizeof(ModButtonDesc);
    saveBtn.label       = "Save Config";
    saveBtn.same_line   = 0;
    saveBtn.tooltip     = "Write current settings to AdaptiveSimSpeed.ini (Ctrl+S).";
    saveBtn.on_click    = onSaveConfig;
    saveBtn.user_data   = nullptr;
    AddButton(tabId, &saveBtn);

    ModLabelDesc feedbackLbl{};
    feedbackLbl.struct_size      = sizeof(ModLabelDesc);
    feedbackLbl.text             = g_saveFeedback;
    feedbackLbl.separator_before = 0;
    feedbackLbl.rgba_color       = 0xAAA28AFFu;
    AddLabel(tabId, &feedbackLbl);

    wrsr::log::info("Registered 'Sim Speed' tab in F9 WRSR Tweaks overlay");
}

DWORD WINAPI workerThread(LPVOID)
{
    Sleep(500);

    wrsr::log::info("AdaptiveSimSpeed v%s (WRSR target: %s) — worker thread started",
                    WRSR_MOD_VERSION, WRSR_GAME_VERSION);

    const std::wstring dllDir = getSelfModuleDir();
    ass::Config::get().load(dllDir);

    const auto exe = wrsr::pe::getMainModuleRange();
    if (!exe.base || exe.size == 0) {
        wrsr::log::error("Failed to locate SOVIET64.exe image range — aborting");
        return 1;
    }

    wrsr::log::info("SOVIET64.exe base=0x%p size=0x%zX", exe.base, exe.size);
    ass::hook::install(exe.base, exe.size);

    registerOverlayTab();

    wrsr::log::info("AdaptiveSimSpeed initialization complete");
    return 0;
}

void ensureWorkerStarted()
{
    if (!g_workerStarted.exchange(true)) {
        HANDLE hThread = CreateThread(nullptr, 0, workerThread, nullptr, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            wrsr::log::error("CreateThread failed: %lu", GetLastError());
        }
    }
}

} // namespace

// ============================================================================
// DLL Entry Point
// ============================================================================
BOOL WINAPI DllMain(HINSTANCE hDll, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hDll);
        wrsr::log::init(L"AdaptiveSimSpeed.log");
        wrsr::log::info("DllMain DLL_PROCESS_ATTACH");
        ensureWorkerStarted();
    }
    return TRUE;
}

// ============================================================================
// Republic Mod Loader (RML) — Native Plugin Exports (API v1)
// ============================================================================
WRSR_ABI_EXPORT int WRSR_ABI_CALL RmlPluginQuery(
    uint32_t hostApiVersion,
    wrsr::abi::RmlPluginDescriptor* out)
{
    if (!out) {
        return 0;
    }
    out->struct_size        = static_cast<uint32_t>(sizeof(wrsr::abi::RmlPluginDescriptor));
    out->descriptor_version = wrsr::abi::kRmlPluginDescriptorVersion;
    out->api_min            = 1;
    out->api_max            = wrsr::abi::kRmlApiVersion;
    out->id                 = "zydrahydra.adaptive_sim_speed";
    out->name               = "AdaptiveSimSpeed";
    out->version            = WRSR_MOD_VERSION;
    out->author             = "ZydraHydra";
    return (hostApiVersion >= 1) ? 1 : 0;
}

WRSR_ABI_EXPORT int WRSR_ABI_CALL RmlPluginLoad(const wrsr::abi::RmlHostApi* host)
{
    g_rmlHost = host;
    if (host && host->log) {
        host->log(host->plugin_context, wrsr::abi::RML_LOG_INFO,
                  "AdaptiveSimSpeed: RmlPluginLoad initialized");

        if (host->get_game_info) {
            wrsr::abi::RmlGameInfo gi{};
            gi.struct_size = static_cast<uint32_t>(sizeof(wrsr::abi::RmlGameInfo));
            if (host->get_game_info(host->plugin_context, &gi) && gi.version) {
                wrsr::log::info("RML reported WRSR version: %s (base=0x%llX)",
                                gi.version, static_cast<unsigned long long>(gi.image_base));
            }
        }
    }
    ensureWorkerStarted();
    return 1;
}

// ============================================================================
// TesmioLoader — Compatibility Exports (API v4, Target v0.4.5)
// ============================================================================
WRSR_ABI_EXPORT const char TsmPluginTargetVersion[] = "0.4.5";

WRSR_ABI_EXPORT unsigned TsmPluginApiVersion(void)
{
    return wrsr::abi::kTsmApiVersion;
}

WRSR_ABI_EXPORT int TsmPluginInit(
    const wrsr::abi::TsmHost* host,
    wrsr::abi::TsmPluginInfo* info)
{
    g_tsmHost = host;
    if (info) {
        info->name    = "AdaptiveSimSpeed";
        info->version = WRSR_MOD_VERSION;
    }
    if (host && host->log) {
        host->log("AdaptiveSimSpeed  TsmPluginInit (host API %u)", host->apiVersion);
    }
    ensureWorkerStarted();
    return 0;
}

WRSR_ABI_EXPORT int TsmPluginStart(void)
{
    if (g_tsmHost && g_tsmHost->log) {
        g_tsmHost->log("AdaptiveSimSpeed  TsmPluginStart");
    }
    return 0;
}
