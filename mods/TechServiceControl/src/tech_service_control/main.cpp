// ============================================================================
// main.cpp — TechServiceControl entry point, loader ABI & F9 "Tech Services" tab
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
#include "tech_service_hook.hpp"

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
    tsc::Config::get().syncFromUi();
    tsc::hook::syncThresholds();
}

void WRSR_OVERLAY_CALL onPresetClicked(void* userData)
{
    const auto preset = static_cast<tsc::PresetKind>(reinterpret_cast<uintptr_t>(userData));
    tsc::Config::get().applyPreset(preset);
    tsc::hook::syncThresholds();

    const char* name = "Fuel Saver";
    if (preset == tsc::PresetKind::Vanilla) name = "Vanilla";
    else if (preset == tsc::PresetKind::DenseCity) name = "Dense City";

    std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                  "Applied '%s' preset (unsaved — click Save Config to persist).", name);
}

void WRSR_OVERLAY_CALL onSaveConfig(void* /*userData*/)
{
    if (tsc::Config::get().save()) {
        tsc::hook::syncThresholds();
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "Saved TechServiceControl.ini successfully.");
    } else {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "ERROR: Could not write TechServiceControl.ini.");
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
    tab.tab_id      = "zydrahydra.tech_service_control";
    tab.title       = "Tech Services";
    tab.on_save     = onSaveConfig;
    tab.user_data   = nullptr;

    const int32_t tabId = RegisterModTab(&tab);
    if (tabId < 0) {
        wrsr::log::error("RegisterModTab('Tech Services') failed");
        return;
    }

    auto& cfg = tsc::Config::get();

    // ── Status label ──────────────────────────────────────────────────────
    const bool     hookActive  = (tsc::hook::state() == tsc::hook::HookState::Active);
    const uint32_t statusColor = hookActive ? 0xA6D973FFu : 0xE6AD47FFu;

    ModLabelDesc statusLbl{};
    statusLbl.struct_size      = sizeof(ModLabelDesc);
    statusLbl.text             = tsc::hook::statusText();
    statusLbl.separator_before = 0;
    statusLbl.rgba_color       = statusColor;
    AddLabel(tabId, &statusLbl);

    ModLabelDesc descLbl{};
    descLbl.struct_size      = sizeof(ModLabelDesc);
    descLbl.text             = "Customizes Technical Services dispatch thresholds: prevents snowplows "
                               "from wasting fuel on a 1% dusting while allowing earlier garbage, sewage, "
                               "and water servicing in dense districts.";
    descLbl.separator_before = 1;
    descLbl.rgba_color       = 0;
    AddLabel(tabId, &descLbl);

    // ── Enable Threshold Overrides checkbox ───────────────────────────────
    ModCheckboxDesc enableCb{};
    enableCb.struct_size = sizeof(ModCheckboxDesc);
    enableCb.label       = "Enable Threshold Overrides";
    enableCb.value       = cfg.uiOverrideEnabledPtr();
    enableCb.tooltip     = "Master toggle for all Technical Services threshold overrides.";
    enableCb.on_change   = onSettingChanged;
    enableCb.user_data   = nullptr;
    AddCheckbox(tabId, &enableCb);

    // ── Snowplow Dispatch >= % slider ─────────────────────────────────────
    ModFloatSliderDesc snowSlider{};
    snowSlider.struct_size = sizeof(ModFloatSliderDesc);
    snowSlider.label       = "Snowplow Dispatch >= %";
    snowSlider.value       = cfg.uiSnowplowMinPtr();
    snowSlider.min_value   = tsc::kMinSnowplowPct;
    snowSlider.max_value   = tsc::kMaxSnowplowPct;
    snowSlider.format      = "%.0f%%";
    snowSlider.tooltip     = "Holds snowplows in the Technical Services office until road snow depth reaches this percentage.";
    snowSlider.on_change   = onSettingChanged;
    snowSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &snowSlider);

    // ── Garbage Pickup >= % slider ────────────────────────────────────────
    ModFloatSliderDesc garbageSlider{};
    garbageSlider.struct_size = sizeof(ModFloatSliderDesc);
    garbageSlider.label       = "Garbage Pickup >= %";
    garbageSlider.value       = cfg.uiGarbageMinPtr();
    garbageSlider.min_value   = tsc::kMinGarbagePct;
    garbageSlider.max_value   = tsc::kMaxGarbagePct;
    garbageSlider.format      = "%.0f%%";
    garbageSlider.tooltip     = "Dispatches garbage trucks once waste containers or building bins reach this fill percentage (vanilla: 75% / 60%).";
    garbageSlider.on_change   = onSettingChanged;
    garbageSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &garbageSlider);

    // ── Sewage Pickup >= % slider ─────────────────────────────────────────
    ModFloatSliderDesc sewageSlider{};
    sewageSlider.struct_size = sizeof(ModFloatSliderDesc);
    sewageSlider.label       = "Sewage Pickup >= %";
    sewageSlider.value       = cfg.uiSewageMinPtr();
    sewageSlider.min_value   = tsc::kMinSewagePct;
    sewageSlider.max_value   = tsc::kMaxSewagePct;
    sewageSlider.format      = "%.0f%%";
    sewageSlider.tooltip     = "Dispatches sewage trucks once unpiped building sewage tanks reach this fill percentage (vanilla: 70%).";
    sewageSlider.on_change   = onSettingChanged;
    sewageSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &sewageSlider);

    // ── Water Delivery <= % slider ────────────────────────────────────────
    ModFloatSliderDesc waterSlider{};
    waterSlider.struct_size = sizeof(ModFloatSliderDesc);
    waterSlider.label       = "Water Delivery <= %";
    waterSlider.value       = cfg.uiWaterBelowPtr();
    waterSlider.min_value   = tsc::kMinWaterBelowPct;
    waterSlider.max_value   = tsc::kMaxWaterBelowPct;
    waterSlider.format      = "%.0f%%";
    waterSlider.tooltip     = "Dispatches water trucks once unpiped building water tanks drop below this fill percentage (vanilla: 30%).";
    waterSlider.on_change   = onSettingChanged;
    waterSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &waterSlider);

    // ── Quick presets ─────────────────────────────────────────────────────
    ModLabelDesc presetsLbl{};
    presetsLbl.struct_size      = sizeof(ModLabelDesc);
    presetsLbl.text             = "Quick Presets:";
    presetsLbl.separator_before = 1;
    presetsLbl.rgba_color       = 0;
    AddLabel(tabId, &presetsLbl);

    struct PresetEntry { const char* label; tsc::PresetKind kind; uint8_t sameLine; };
    static constexpr PresetEntry kPresets[] = {
        { "Vanilla",    tsc::PresetKind::Vanilla,   0 },
        { "Fuel Saver", tsc::PresetKind::FuelSaver, 1 },
        { "Dense City", tsc::PresetKind::DenseCity, 1 },
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
    saveBtn.tooltip     = "Write current settings to TechServiceControl.ini (Ctrl+S).";
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

    wrsr::log::info("Registered 'Tech Services' tab in F9 WRSR Tweaks overlay");
}

// ── Worker thread ─────────────────────────────────────────────────────────
DWORD WINAPI workerThread(LPVOID)
{
    ModOverlay_WaitForHostReady(500);

    wrsr::log::info("TechServiceControl v%s (WRSR target: %s) — worker thread started",
                    WRSR_MOD_VERSION, WRSR_GAME_VERSION);

    const std::wstring dllDir = getSelfModuleDir();
    tsc::Config::get().load(dllDir);

    const auto exe = wrsr::pe::getMainModuleRange();
    if (!exe.base || exe.size == 0) {
        wrsr::log::error("Failed to locate SOVIET64.exe image range — aborting");
        return 1;
    }

    wrsr::log::info("SOVIET64.exe base=0x%p size=0x%zX", exe.base, exe.size);
    tsc::hook::install(exe.base, exe.size);

    registerOverlayTab();

    wrsr::log::info("TechServiceControl initialization complete");
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
        wrsr::log::init(L"TechServiceControl.log");
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
    out->id                 = "zydrahydra.tech_service_control";
    out->name               = "TechServiceControl";
    out->version            = WRSR_MOD_VERSION;
    out->author             = "ZydraHydra";
    return (hostApiVersion >= 1) ? 1 : 0;
}

WRSR_ABI_EXPORT int WRSR_ABI_CALL RmlPluginLoad(const wrsr::abi::RmlHostApi* host)
{
    g_rmlHost = host;
    if (host && host->log) {
        host->log(host->plugin_context, wrsr::abi::RML_LOG_INFO,
                  "TechServiceControl: RmlPluginLoad initialized");

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
        info->name    = "TechServiceControl";
        info->version = WRSR_MOD_VERSION;
    }
    if (host && host->log) {
        host->log("TechServiceControl  TsmPluginInit (host API %u)", host->apiVersion);
    }
    ensureWorkerStarted();
    return 0;
}

WRSR_ABI_EXPORT int TsmPluginStart(void)
{
    if (g_tsmHost && g_tsmHost->log) {
        g_tsmHost->log("TechServiceControl  TsmPluginStart");
    }
    return 0;
}
