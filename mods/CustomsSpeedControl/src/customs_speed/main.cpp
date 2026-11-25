// ============================================================================
// main.cpp — CustomsSpeedControl entry point, loader ABI & F9 tab registration
// ============================================================================
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "mod_overlay.h"
#include "wrsr/loader_abi.hpp"
#include "wrsr/logger.hpp"
#include "wrsr/pe_utils.hpp"

#include "config.hpp"
#include "customs_hook.hpp"

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

char g_saveFeedback[96] = "Tip: Changes take effect immediately; click Save Config to persist.";

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
    csc::Config::get().syncFromUi();
}

void WRSR_OVERLAY_CALL onPresetClicked(void* userData)
{
    const uintptr_t encoded = reinterpret_cast<uintptr_t>(userData);
    const float     mult    = static_cast<float>(encoded);
    csc::Config::get().applyPreset(mult);
    std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                  "Applied %.0fx preset to Load & Unload (unsaved until Save Config).", mult);
}

void WRSR_OVERLAY_CALL onSaveConfig(void* /*userData*/)
{
    if (csc::Config::get().save()) {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "Saved CustomsSpeedControl.ini successfully.");
    } else {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "ERROR: Could not write CustomsSpeedControl.ini.");
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
    tab.tab_id      = "zydrahydra.customs_speed_control";
    tab.title       = "Customs Speed";
    tab.on_save     = onSaveConfig;
    tab.user_data   = nullptr;

    const int32_t tabId = RegisterModTab(&tab);
    if (tabId < 0) {
        wrsr::log::error("RegisterModTab('Customs Speed') failed");
        return;
    }

    auto& cfg = csc::Config::get();

    const uint32_t statusColor = (csc::hook::state() == csc::hook::HookState::Active)
                                     ? 0xA6D973FFu
                                     : 0xE6AD47FFu;

    ModLabelDesc statusLbl{};
    statusLbl.struct_size      = sizeof(ModLabelDesc);
    statusLbl.text             = csc::hook::statusText();
    statusLbl.separator_before = 0;
    statusLbl.rgba_color       = statusColor;
    AddLabel(tabId, &statusLbl);

    ModLabelDesc descLbl{};
    descLbl.struct_size      = sizeof(ModLabelDesc);
    descLbl.text             = "Adjust Customs House ($TYPE_CUSTOMHOUSE) cargo transfer speeds. Domestic stations and warehouses are unaffected.";
    descLbl.separator_before = 1;
    descLbl.rgba_color       = 0;
    AddLabel(tabId, &descLbl);

    ModFloatSliderDesc loadSlider{};
    loadSlider.struct_size = sizeof(ModFloatSliderDesc);
    loadSlider.label       = "Load Speed Multiplier";
    loadSlider.value       = cfg.uiLoadMultiplierPtr();
    loadSlider.min_value   = csc::kMinMultiplier;
    loadSlider.max_value   = csc::kMaxMultiplier;
    loadSlider.format      = "%.2fx";
    loadSlider.tooltip     = "Multiplier applied when vehicles load imports at a Customs House.";
    loadSlider.on_change   = onSettingChanged;
    loadSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &loadSlider);

    ModFloatSliderDesc unloadSlider{};
    unloadSlider.struct_size = sizeof(ModFloatSliderDesc);
    unloadSlider.label       = "Unload Speed Multiplier";
    unloadSlider.value       = cfg.uiUnloadMultiplierPtr();
    unloadSlider.min_value   = csc::kMinMultiplier;
    unloadSlider.max_value   = csc::kMaxMultiplier;
    unloadSlider.format      = "%.2fx";
    unloadSlider.tooltip     = "Multiplier applied when vehicles unload exports at a Customs House.";
    unloadSlider.on_change   = onSettingChanged;
    unloadSlider.user_data   = nullptr;
    AddFloatSlider(tabId, &unloadSlider);

    ModCheckboxDesc railCheck{};
    railCheck.struct_size = sizeof(ModCheckboxDesc);
    railCheck.label       = "Affect Rail (Trains & Wagons)";
    railCheck.value       = cfg.uiAffectRailPtr();
    railCheck.tooltip     = "When unchecked, only road vehicles are accelerated at Customs Houses.";
    railCheck.on_change   = onSettingChanged;
    railCheck.user_data   = nullptr;
    AddCheckbox(tabId, &railCheck);

    ModLabelDesc presetsLbl{};
    presetsLbl.struct_size      = sizeof(ModLabelDesc);
    presetsLbl.text             = "Quick Presets (sets both Load & Unload):";
    presetsLbl.separator_before = 1;
    presetsLbl.rgba_color       = 0;
    AddLabel(tabId, &presetsLbl);

    auto addPresetBtn = [&](const char* label, uintptr_t multVal, uint8_t sameLine) {
        ModButtonDesc btn{};
        btn.struct_size = sizeof(ModButtonDesc);
        btn.label       = label;
        btn.same_line   = sameLine;
        btn.tooltip     = "Set both Load and Unload multipliers immediately.";
        btn.on_click    = onPresetClicked;
        btn.user_data   = reinterpret_cast<void*>(multVal);
        AddButton(tabId, &btn);
    };

    addPresetBtn("1x",   1,   0);
    addPresetBtn("2x",   2,   1);
    addPresetBtn("5x",   5,   1);
    addPresetBtn("10x",  10,  1);
    addPresetBtn("25x",  25,  1);
    addPresetBtn("50x",  50,  1);
    addPresetBtn("100x", 100, 1);

    ModButtonDesc saveBtn{};
    saveBtn.struct_size = sizeof(ModButtonDesc);
    saveBtn.label       = "Save Config";
    saveBtn.same_line   = 0;
    saveBtn.tooltip     = "Write current settings to CustomsSpeedControl.ini (Ctrl+S).";
    saveBtn.on_click    = onSaveConfig;
    saveBtn.user_data   = nullptr;
    AddButton(tabId, &saveBtn);

    ModLabelDesc feedbackLbl{};
    feedbackLbl.struct_size      = sizeof(ModLabelDesc);
    feedbackLbl.text             = g_saveFeedback;
    feedbackLbl.separator_before = 0;
    feedbackLbl.rgba_color       = 0xAAA28AFFu;
    AddLabel(tabId, &feedbackLbl);

    wrsr::log::info("Registered 'Customs Speed' tab in F9 WRSR Tweaks overlay");
}

DWORD WINAPI workerThread(LPVOID)
{
    Sleep(500);

    wrsr::log::info("CustomsSpeedControl v%s (WRSR target: %s) — worker thread started",
                    WRSR_MOD_VERSION, WRSR_GAME_VERSION);

    const std::wstring dllDir = getSelfModuleDir();
    csc::Config::get().load(dllDir);

    const auto exe = wrsr::pe::getMainModuleRange();
    if (!exe.base || exe.size == 0) {
        wrsr::log::error("Failed to locate SOVIET64.exe image range — aborting");
        return 1;
    }

    wrsr::log::info("SOVIET64.exe base=0x%p size=0x%zX", exe.base, exe.size);
    csc::hook::install(exe.base, exe.size);

    registerOverlayTab();

    wrsr::log::info("CustomsSpeedControl initialization complete");
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
        wrsr::log::init(L"CustomsSpeedControl.log");
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
    out->id                 = "zydrahydra.customs_speed_control";
    out->name               = "CustomsSpeedControl";
    out->version            = WRSR_MOD_VERSION;
    out->author             = "ZydraHydra";
    return (hostApiVersion >= 1) ? 1 : 0;
}

WRSR_ABI_EXPORT int WRSR_ABI_CALL RmlPluginLoad(const wrsr::abi::RmlHostApi* host)
{
    g_rmlHost = host;
    if (host && host->log) {
        host->log(host->plugin_context, wrsr::abi::RML_LOG_INFO,
                  "CustomsSpeedControl: RmlPluginLoad initialized");

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
        info->name    = "CustomsSpeedControl";
        info->version = WRSR_MOD_VERSION;
    }
    if (host && host->log) {
        host->log("CustomsSpeedControl  TsmPluginInit (host API %u)", host->apiVersion);
    }
    ensureWorkerStarted();
    return 0;
}

WRSR_ABI_EXPORT int TsmPluginStart(void)
{
    if (g_tsmHost && g_tsmHost->log) {
        g_tsmHost->log("CustomsSpeedControl  TsmPluginStart");
    }
    return 0;
}
