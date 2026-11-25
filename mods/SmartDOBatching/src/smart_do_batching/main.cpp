// ============================================================================
// main.cpp — SmartDOBatching entry point, loader ABI & F9 "DO Batching" tab
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
#include "batch_hook.hpp"

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
    sdb::Config::get().syncFromUi();
}

void WRSR_OVERLAY_CALL onPresetClicked(void* userData)
{
    // userData encodes the preset percentage as (uintptr_t)(int pct)
    const int   pct  = static_cast<int>(reinterpret_cast<uintptr_t>(userData));
    const float fPct = static_cast<float>(pct);
    sdb::Config::get().applyPreset(fPct);
    std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                  "Applied %.0f%% preset to Truck & Train fill thresholds (unsaved).", fPct);
}

void WRSR_OVERLAY_CALL onSaveConfig(void* /*userData*/)
{
    if (sdb::Config::get().save()) {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "Saved SmartDOBatching.ini successfully.");
    } else {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "ERROR: Could not write SmartDOBatching.ini.");
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
    tab.tab_id      = "zydrahydra.smart_do_batching";
    tab.title       = "DO Batching";
    tab.on_save     = onSaveConfig;
    tab.user_data   = nullptr;

    const int32_t tabId = RegisterModTab(&tab);
    if (tabId < 0) {
        wrsr::log::error("RegisterModTab('DO Batching') failed");
        return;
    }

    auto& cfg = sdb::Config::get();

    // ── Status label ──────────────────────────────────────────────────────
    const bool      hookActive  = (sdb::hook::state() == sdb::hook::HookState::Active);
    const uint32_t  statusColor = hookActive ? 0xA6D973FFu : 0xE6AD47FFu;

    ModLabelDesc statusLbl{};
    statusLbl.struct_size      = sizeof(ModLabelDesc);
    statusLbl.text             = sdb::hook::statusText();
    statusLbl.separator_before = 0;
    statusLbl.rgba_color       = statusColor;
    AddLabel(tabId, &statusLbl);

    ModLabelDesc descLbl{};
    descLbl.struct_size      = sizeof(ModLabelDesc);
    descLbl.text             = "Prevents Distribution Offices from dispatching near-empty trucks or trains. "
                               "A vehicle is only sent when the available cargo is at least the configured "
                               "percentage of its capacity. Essential goods (food, coal, water, fuel, etc.) "
                               "are exempt by default so towns never starve or freeze.";
    descLbl.separator_before = 1;
    descLbl.rgba_color       = 0;
    AddLabel(tabId, &descLbl);

    // ── Min Truck Fill % slider ───────────────────────────────────────────
    ModFloatSliderDesc truckSlider{};
    truckSlider.struct_size = sizeof(ModFloatSliderDesc);
    truckSlider.label       = "Min Truck Fill %";
    truckSlider.value       = cfg.uiMinTruckFillPtr();
    truckSlider.min_value   = sdb::kMinFillPct;
    truckSlider.max_value   = sdb::kMaxFillPct;
    truckSlider.format      = "%.0f%%";
    truckSlider.tooltip     = "Minimum cargo percentage of truck capacity required before a Distribution "
                              "Office road vehicle is dispatched. 0% = vanilla behaviour.";
    truckSlider.on_change   = onSettingChanged;
    truckSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &truckSlider);

    // ── Min Train Fill % slider ───────────────────────────────────────────
    ModFloatSliderDesc trainSlider{};
    trainSlider.struct_size = sizeof(ModFloatSliderDesc);
    trainSlider.label       = "Min Train Fill %";
    trainSlider.value       = cfg.uiMinTrainFillPtr();
    trainSlider.min_value   = sdb::kMinFillPct;
    trainSlider.max_value   = sdb::kMaxFillPct;
    trainSlider.format      = "%.0f%%";
    trainSlider.tooltip     = "Minimum cargo percentage of train capacity required before a Distribution "
                              "Office rail consist is dispatched. 0% = vanilla behaviour.";
    trainSlider.on_change   = onSettingChanged;
    trainSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &trainSlider);

    // ── Exempt Essential Goods checkbox ───────────────────────────────────
    ModCheckboxDesc exemptCheck{};
    exemptCheck.struct_size = sizeof(ModCheckboxDesc);
    exemptCheck.label       = "Exempt Essential Goods";
    exemptCheck.value       = cfg.uiExemptEssentialsPtr();
    exemptCheck.tooltip     = "When enabled, batch gating is skipped for critical survival resources:\n"
                              "Food, Meat, Alcohol, Medicine/Chemicals, Coal, Water, and Fuel.\n"
                              "Keeps towns fed, warm, and supplied regardless of batch thresholds.";
    exemptCheck.on_change   = onSettingChanged;
    exemptCheck.user_data   = nullptr;
    AddCheckbox(tabId, &exemptCheck);

    // ── Prevent Small-Building Deadlock checkbox ──────────────────────────
    ModCheckboxDesc deadlockCheck{};
    deadlockCheck.struct_size = sizeof(ModCheckboxDesc);
    deadlockCheck.label       = "Prevent Small-Building Deadlock";
    deadlockCheck.value       = cfg.uiPreventDeadlockPtr();
    deadlockCheck.tooltip     = "When enabled, the minimum batch size is capped at 85% of the destination\n"
                                "building's total storage capacity. Prevents a 12t truck assigned to a 4t\n"
                                "grocery store or small heating plant from waiting forever for a batch that\n"
                                "can never fit in the destination.";
    deadlockCheck.on_change   = onSettingChanged;
    deadlockCheck.user_data   = nullptr;
    AddCheckbox(tabId, &deadlockCheck);

    // ── Quick presets ─────────────────────────────────────────────────────
    ModLabelDesc presetsLbl{};
    presetsLbl.struct_size      = sizeof(ModLabelDesc);
    presetsLbl.text             = "Quick Presets (applies same % to Truck & Train):";
    presetsLbl.separator_before = 1;
    presetsLbl.rgba_color       = 0;
    AddLabel(tabId, &presetsLbl);

    struct PresetEntry { const char* label; int pct; uint8_t sameLine; };
    static constexpr PresetEntry kPresets[] = {
        { "Vanilla 0%",  0,  0 },
        { "50%",        50,  1 },
        { "75%",        75,  1 },
        { "90%",        90,  1 },
    };
    for (const auto& p : kPresets) {
        ModButtonDesc btn{};
        btn.struct_size = sizeof(ModButtonDesc);
        btn.label       = p.label;
        btn.same_line   = p.sameLine;
        btn.tooltip     = "Apply preset fill threshold to both Truck and Train sliders immediately.";
        btn.on_click    = onPresetClicked;
        btn.user_data   = reinterpret_cast<void*>(static_cast<uintptr_t>(p.pct));
        AddButton(tabId, &btn);
    }

    // ── Save Config button ────────────────────────────────────────────────
    ModButtonDesc saveBtn{};
    saveBtn.struct_size = sizeof(ModButtonDesc);
    saveBtn.label       = "Save Config";
    saveBtn.same_line   = 0;
    saveBtn.tooltip     = "Write current settings to SmartDOBatching.ini (Ctrl+S).";
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

    wrsr::log::info("Registered 'DO Batching' tab in F9 WRSR Tweaks overlay");
}

// ── Worker thread ─────────────────────────────────────────────────────────
DWORD WINAPI workerThread(LPVOID)
{
    Sleep(500);

    wrsr::log::info("SmartDOBatching v%s (WRSR target: %s) — worker thread started",
                    WRSR_MOD_VERSION, WRSR_GAME_VERSION);

    const std::wstring dllDir = getSelfModuleDir();
    sdb::Config::get().load(dllDir);

    const auto exe = wrsr::pe::getMainModuleRange();
    if (!exe.base || exe.size == 0) {
        wrsr::log::error("Failed to locate SOVIET64.exe image range — aborting");
        return 1;
    }

    wrsr::log::info("SOVIET64.exe base=0x%p size=0x%zX", exe.base, exe.size);
    sdb::hook::install(exe.base, exe.size);

    registerOverlayTab();

    wrsr::log::info("SmartDOBatching initialization complete");
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
        wrsr::log::init(L"SmartDOBatching.log");
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
    out->id                 = "zydrahydra.smart_do_batching";
    out->name               = "SmartDOBatching";
    out->version            = WRSR_MOD_VERSION;
    out->author             = "ZydraHydra";
    return (hostApiVersion >= 1) ? 1 : 0;
}

WRSR_ABI_EXPORT int WRSR_ABI_CALL RmlPluginLoad(const wrsr::abi::RmlHostApi* host)
{
    g_rmlHost = host;
    if (host && host->log) {
        host->log(host->plugin_context, wrsr::abi::RML_LOG_INFO,
                  "SmartDOBatching: RmlPluginLoad initialized");

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
        info->name    = "SmartDOBatching";
        info->version = WRSR_MOD_VERSION;
    }
    if (host && host->log) {
        host->log("SmartDOBatching  TsmPluginInit (host API %u)", host->apiVersion);
    }
    ensureWorkerStarted();
    return 0;
}

WRSR_ABI_EXPORT int TsmPluginStart(void)
{
    if (g_tsmHost && g_tsmHost->log) {
        g_tsmHost->log("SmartDOBatching  TsmPluginStart");
    }
    return 0;
}
