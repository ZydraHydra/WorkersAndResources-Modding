// ============================================================================
// main.cpp — CODispatchControl entry point, loader ABI & F9 "CO Dispatch" tab
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
#include "co_dispatch_hook.hpp"

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

// ── Overlay callbacks ─────────────────────────────────────────────────────
void WRSR_OVERLAY_CALL onSettingChanged(void* /*userData*/)
{
    cdc::Config::get().syncFromUi();
}

void WRSR_OVERLAY_CALL onPresetClicked(void* userData)
{
    const auto preset = static_cast<cdc::PresetKind>(reinterpret_cast<uintptr_t>(userData));
    cdc::Config::get().applyPreset(preset);

    const char* name = "Balanced";
    if (preset == cdc::PresetKind::Vanilla) name = "Vanilla";
    else if (preset == cdc::PresetKind::Strict) name = "Strict";

    std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                  "Applied '%s' preset (unsaved — click Save Config to persist).", name);
}

void WRSR_OVERLAY_CALL onSaveConfig(void* /*userData*/)
{
    if (cdc::Config::get().save()) {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "Saved CODispatchControl.ini successfully.");
    } else {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "ERROR: Could not write CODispatchControl.ini.");
    }
}

// ── Tab registration ──────────────────────────────────────────────────────
void registerOverlayTab()
{
    if (!ModOverlay_Initialize()) {
        wrsr::log::error("ModOverlay_Initialize failed — F9 overlay unavailable");
        return;
    }

    ModTabDesc tab{};
    tab.struct_size = sizeof(ModTabDesc);
    tab.tab_id      = "zydrahydra.co_dispatch_control";
    tab.title       = "CO Dispatch";
    tab.on_save     = onSaveConfig;
    tab.user_data   = nullptr;

    const int32_t tabId = RegisterModTab(&tab);
    if (tabId < 0) {
        wrsr::log::error("RegisterModTab('CO Dispatch') failed");
        return;
    }

    auto& cfg = cdc::Config::get();

    // ── Status label ──────────────────────────────────────────────────────
    const bool     hookActive  = (cdc::hook::state() == cdc::hook::HookState::Active);
    const uint32_t statusColor = hookActive ? 0xA6D973FFu : 0xE6AD47FFu;

    ModLabelDesc statusLbl{};
    statusLbl.struct_size      = sizeof(ModLabelDesc);
    statusLbl.text             = cdc::hook::statusText();
    statusLbl.separator_before = 0;
    statusLbl.rgba_color       = statusColor;
    AddLabel(tabId, &statusLbl);

    ModLabelDesc descLbl{};
    descLbl.struct_size      = sizeof(ModLabelDesc);
    descLbl.text             = "Matches Construction Office trucks and buses to actual site demand. "
                               "Rejects oversized vehicles when smaller ones are available in the CO, "
                               "caps simultaneous trucks per site, and never blocks mechanism/flatbed transport.";
    descLbl.separator_before = 1;
    descLbl.rgba_color       = 0;
    AddLabel(tabId, &descLbl);

    // ── Min Material Truck Fill % slider ──────────────────────────────────
    ModFloatSliderDesc fillSlider{};
    fillSlider.struct_size = sizeof(ModFloatSliderDesc);
    fillSlider.label       = "Min Material Truck Fill %";
    fillSlider.value       = cfg.uiMinMaterialFillPtr();
    fillSlider.min_value   = cdc::kMinMaterialFillPct;
    fillSlider.max_value   = cdc::kMaxMaterialFillPct;
    fillSlider.format      = "%.0f%%";
    fillSlider.tooltip     = "Rejects oversized trucks if remaining_phase_material / vehicle_capacity "
                             "is below this threshold and a smaller capable truck exists in the CO.\n"
                             "Small final top-offs (< 1.5t) are automatically exempted.";
    fillSlider.on_change   = onSettingChanged;
    fillSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &fillSlider);

    // ── Min Bus Workday Ratio slider ──────────────────────────────────────
    ModFloatSliderDesc busSlider{};
    busSlider.struct_size = sizeof(ModFloatSliderDesc);
    busSlider.label       = "Min Bus Workday Ratio";
    busSlider.value       = cfg.uiMinBusWorkdayPtr();
    busSlider.min_value   = cdc::kMinBusWorkdayRatio;
    busSlider.max_value   = cdc::kMaxBusWorkdayRatio;
    busSlider.format      = "%.2f";
    busSlider.tooltip     = "Prevents dispatching a large bus when remaining_workdays < bus_capacity * ratio\n"
                            "if a smaller bus/microbus is in the CO, letting smaller vehicles take finish-up shifts.";
    busSlider.on_change   = onSettingChanged;
    busSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &busSlider);

    // ── Max Trucks En Route per Site slider ───────────────────────────────
    ModFloatSliderDesc maxTrucksSlider{};
    maxTrucksSlider.struct_size = sizeof(ModFloatSliderDesc);
    maxTrucksSlider.label       = "Max Trucks En Route per Site";
    maxTrucksSlider.value       = cfg.uiMaxTrucksEnRoutePtr();
    maxTrucksSlider.min_value   = cdc::kMinMaxTrucksEnRoute;
    maxTrucksSlider.max_value   = cdc::kMaxMaxTrucksEnRoute;
    maxTrucksSlider.format      = "%.0f";
    maxTrucksSlider.tooltip     = "Caps simultaneous material trucks heading to the same construction site "
                                  "to prevent road-node traffic jams (10 = effectively unlimited).";
    maxTrucksSlider.on_change   = onSettingChanged;
    maxTrucksSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &maxTrucksSlider);

    // ── Quick presets ─────────────────────────────────────────────────────
    ModLabelDesc presetsLbl{};
    presetsLbl.struct_size      = sizeof(ModLabelDesc);
    presetsLbl.text             = "Quick Presets:";
    presetsLbl.separator_before = 1;
    presetsLbl.rgba_color       = 0;
    AddLabel(tabId, &presetsLbl);

    struct PresetEntry { const char* label; cdc::PresetKind kind; uint8_t sameLine; };
    static constexpr PresetEntry kPresets[] = {
        { "Vanilla",  cdc::PresetKind::Vanilla,  0 },
        { "Balanced", cdc::PresetKind::Balanced, 1 },
        { "Strict",   cdc::PresetKind::Strict,   1 },
    };
    for (const auto& p : kPresets) {
        ModButtonDesc btn{};
        btn.struct_size = sizeof(ModButtonDesc);
        btn.label       = p.label;
        btn.same_line   = p.sameLine;
        btn.tooltip     = "Apply preset values immediately.";
        btn.on_click    = onPresetClicked;
        btn.user_data   = reinterpret_cast<void*>(static_cast<uintptr_t>(p.kind));
        AddButton(tabId, &btn);
    }

    // ── Save Config button ────────────────────────────────────────────────
    ModButtonDesc saveBtn{};
    saveBtn.struct_size = sizeof(ModButtonDesc);
    saveBtn.label       = "Save Config";
    saveBtn.same_line   = 0;
    saveBtn.tooltip     = "Write current settings to CODispatchControl.ini (Ctrl+S).";
    saveBtn.on_click    = onSaveConfig;
    saveBtn.user_data   = nullptr;
    AddButton(tabId, &saveBtn);

    // ── Feedback label ────────────────────────────────────────────────────
    ModLabelDesc feedbackLbl{};
    feedbackLbl.struct_size      = sizeof(ModLabelDesc);
    feedbackLbl.text             = g_saveFeedback;
    feedbackLbl.separator_before = 0;
    feedbackLbl.rgba_color       = 0xAAA28AFFu;
    AddLabel(tabId, &feedbackLbl);

    wrsr::log::info("Registered 'CO Dispatch' tab in F9 WRSR Tweaks overlay");
}

// ── Worker thread ─────────────────────────────────────────────────────────
DWORD WINAPI workerThread(LPVOID)
{
    ModOverlay_WaitForHostReady(500);

    wrsr::log::info("CODispatchControl v%s (WRSR target: %s) — worker thread started",
                    WRSR_MOD_VERSION, WRSR_GAME_VERSION);

    const std::wstring dllDir = getSelfModuleDir();
    cdc::Config::get().load(dllDir);

    const auto exe = wrsr::pe::getMainModuleRange();
    if (!exe.base || exe.size == 0) {
        wrsr::log::error("Failed to locate SOVIET64.exe image range — aborting");
        return 1;
    }

    wrsr::log::info("SOVIET64.exe base=0x%p size=0x%zX", exe.base, exe.size);
    cdc::hook::install(exe.base, exe.size);

    registerOverlayTab();

    wrsr::log::info("CODispatchControl initialization complete");
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
        wrsr::log::init(L"CODispatchControl.log");
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
    out->id                 = "zydrahydra.co_dispatch_control";
    out->name               = "CODispatchControl";
    out->version            = WRSR_MOD_VERSION;
    out->author             = "ZydraHydra";
    return (hostApiVersion >= 1) ? 1 : 0;
}

WRSR_ABI_EXPORT int WRSR_ABI_CALL RmlPluginLoad(const wrsr::abi::RmlHostApi* host)
{
    g_rmlHost = host;
    if (host && host->log) {
        host->log(host->plugin_context, wrsr::abi::RML_LOG_INFO,
                  "CODispatchControl: RmlPluginLoad initialized");

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
WRSR_ABI_EXPORT const char TsmPluginTargetVersion[] = "0.4.6";

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
        info->name    = "CODispatchControl";
        info->version = WRSR_MOD_VERSION;
    }
    if (host && host->log) {
        host->log("CODispatchControl  TsmPluginInit (host API %u)", host->apiVersion);
    }
    ensureWorkerStarted();
    return 0;
}

WRSR_ABI_EXPORT int TsmPluginStart(void)
{
    if (g_tsmHost && g_tsmHost->log) {
        g_tsmHost->log("CODispatchControl  TsmPluginStart");
    }
    return 0;
}
