// ============================================================================
// main.cpp — SmartBayAccess entry point, loader ABI & F9 "Bay Access" tab
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

#include "bay_access_hook.hpp"
#include "config.hpp"

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
void WRSR_OVERLAY_CALL onModeClicked(void* userData)
{
    const auto mode = static_cast<sba::BayAccessMode>(reinterpret_cast<intptr_t>(userData));
    sba::Config::get().setMode(mode);
    std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                  "Switched to '%s' (unsaved — click Save Config to persist).",
                  sba::Config::modeName(mode));
}

void WRSR_OVERLAY_CALL onResetTracking(void* /*userData*/)
{
    sba::hook::resetTracking();
    std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                  "Cleared active building apron tracking map.");
}

void WRSR_OVERLAY_CALL onSaveConfig(void* /*userData*/)
{
    if (sba::Config::get().save()) {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "Saved SmartBayAccess.ini successfully.");
    } else {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "ERROR: Could not write SmartBayAccess.ini.");
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
    tab.tab_id      = "zydrahydra.smart_bay_access";
    tab.title       = "Bay Access";
    tab.on_save     = onSaveConfig;
    tab.user_data   = nullptr;

    const int32_t tabId = RegisterModTab(&tab);
    if (tabId < 0) {
        wrsr::log::error("RegisterModTab('Bay Access') failed");
        return;
    }

    auto& cfg = sba::Config::get();

    // ── Status label ──────────────────────────────────────────────────────
    const bool     hookActive  = (sba::hook::state() == sba::hook::HookState::Active);
    const uint32_t statusColor = hookActive ? 0xA6D973FFu : 0xE6AD47FFu;

    ModLabelDesc statusLbl{};
    statusLbl.struct_size      = sizeof(ModLabelDesc);
    statusLbl.text             = sba::hook::statusText();
    statusLbl.separator_before = 0;
    statusLbl.rgba_color       = statusColor;
    AddLabel(tabId, &statusLbl);

    ModLabelDesc descLbl{};
    descLbl.struct_size      = sizeof(ModLabelDesc);
    descLbl.text             = "Eliminates single-vehicle courtyard bottlenecks at multi-bay road "
                               "buildings (cargo stations, depots, Customs Houses, bus platforms) "
                               "while preserving per-bay reservations and 1-bay / rail behavior.";
    descLbl.separator_before = 1;
    descLbl.rgba_color       = 0;
    AddLabel(tabId, &descLbl);

    // ── Mode readout & preset buttons ─────────────────────────────────────
    ModLabelDesc modeLbl{};
    modeLbl.struct_size      = sizeof(ModLabelDesc);
    modeLbl.text             = cfg.modeStatusText();
    modeLbl.separator_before = 1;
    modeLbl.rgba_color       = 0xE6C873FFu;
    AddLabel(tabId, &modeLbl);

    struct ModePreset { const char* label; sba::BayAccessMode mode; uint8_t sameLine; const char* tip; };
    static constexpr ModePreset kPresets[] = {
        { "Vanilla",                 sba::BayAccessMode::Vanilla,       0, "Restore vanilla single-mover apron lock (mode = 0)." },
        { "Same-Direction Batching", sba::BayAccessMode::SameDirection, 1, "Allow simultaneous entries while none exiting, and exits while none entering (mode = 1)." },
        { "Unrestricted",            sba::BayAccessMode::Unrestricted,  1, "Allow simultaneous entry and exit whenever a free bay exists (mode = 2)." },
    };
    for (const auto& p : kPresets) {
        ModButtonDesc btn{};
        btn.struct_size = sizeof(ModButtonDesc);
        btn.label       = p.label;
        btn.same_line   = p.sameLine;
        btn.tooltip     = p.tip;
        btn.on_click    = onModeClicked;
        btn.user_data   = reinterpret_cast<void*>(static_cast<intptr_t>(p.mode));
        AddButton(tabId, &btn);
    }

    // ── Live diagnostics ──────────────────────────────────────────────────
    ModLabelDesc trackedLbl{};
    trackedLbl.struct_size      = sizeof(ModLabelDesc);
    trackedLbl.text             = sba::hook::trackedBuildingsText();
    trackedLbl.separator_before = 1;
    trackedLbl.rgba_color       = 0;
    AddLabel(tabId, &trackedLbl);

    ModLabelDesc batchedLbl{};
    batchedLbl.struct_size      = sizeof(ModLabelDesc);
    batchedLbl.text             = sba::hook::batchedEntriesText();
    batchedLbl.separator_before = 0;
    batchedLbl.rgba_color       = 0;
    AddLabel(tabId, &batchedLbl);

    // ── Action buttons ────────────────────────────────────────────────────
    ModButtonDesc resetBtn{};
    resetBtn.struct_size = sizeof(ModButtonDesc);
    resetBtn.label       = "Reset Tracking";
    resetBtn.same_line   = 0;
    resetBtn.tooltip     = "Flush the active building apron tracking map immediately.";
    resetBtn.on_click    = onResetTracking;
    resetBtn.user_data   = nullptr;
    AddButton(tabId, &resetBtn);

    ModButtonDesc saveBtn{};
    saveBtn.struct_size = sizeof(ModButtonDesc);
    saveBtn.label       = "Save Config";
    saveBtn.same_line   = 1;
    saveBtn.tooltip     = "Write current mode to SmartBayAccess.ini (Ctrl+S).";
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

    wrsr::log::info("Registered 'Bay Access' tab in F9 WRSR Tweaks overlay");
}

// ── Worker thread ─────────────────────────────────────────────────────────
DWORD WINAPI workerThread(LPVOID)
{
    ModOverlay_WaitForHostReady(500);

    wrsr::log::info("SmartBayAccess v%s (WRSR target: %s) — worker thread started",
                    WRSR_MOD_VERSION, WRSR_GAME_VERSION);

    const std::wstring dllDir = getSelfModuleDir();
    sba::Config::get().load(dllDir);

    const auto exe = wrsr::pe::getMainModuleRange();
    if (!exe.base || exe.size == 0) {
        wrsr::log::error("Failed to locate SOVIET64.exe image range — aborting");
        return 1;
    }

    wrsr::log::info("SOVIET64.exe base=0x%p size=0x%zX", exe.base, exe.size);
    sba::hook::install(exe.base, exe.size);

    registerOverlayTab();

    wrsr::log::info("SmartBayAccess initialization complete");
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
        wrsr::log::init(L"SmartBayAccess.log");
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
    out->id                 = "zydrahydra.smart_bay_access";
    out->name               = "SmartBayAccess";
    out->version            = WRSR_MOD_VERSION;
    out->author             = "ZydraHydra";
    return (hostApiVersion >= 1) ? 1 : 0;
}

WRSR_ABI_EXPORT int WRSR_ABI_CALL RmlPluginLoad(const wrsr::abi::RmlHostApi* host)
{
    g_rmlHost = host;
    if (host && host->log) {
        host->log(host->plugin_context, wrsr::abi::RML_LOG_INFO,
                  "SmartBayAccess: RmlPluginLoad initialized");

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
        info->name    = "SmartBayAccess";
        info->version = WRSR_MOD_VERSION;
    }
    if (host && host->log) {
        host->log("SmartBayAccess  TsmPluginInit (host API %u)", host->apiVersion);
    }
    ensureWorkerStarted();
    return 0;
}

WRSR_ABI_EXPORT int TsmPluginStart(void)
{
    if (g_tsmHost && g_tsmHost->log) {
        g_tsmHost->log("SmartBayAccess  TsmPluginStart");
    }
    return 0;
}
