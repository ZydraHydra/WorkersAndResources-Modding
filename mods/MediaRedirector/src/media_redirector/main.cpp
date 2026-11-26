// ============================================================================
// main.cpp — MediaRedirector entry point, loader ABI & F9 Media VFS tab
// ============================================================================
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "mod_overlay.h"
#include "wrsr/loader_abi.hpp"
#include "wrsr/logger.hpp"

#include "config.hpp"
#include "redirect_hook.hpp"
#include "vfs_index.hpp"

#ifndef WRSR_MOD_VERSION
#define WRSR_MOD_VERSION "1.0.0"
#endif

#ifndef WRSR_GAME_VERSION
#define WRSR_GAME_VERSION "latest"
#endif

namespace {

constexpr size_t kMaxUiPacks = 64;

std::atomic<bool>            g_earlyInitDone{false};
std::atomic<bool>            g_overlayStarted{false};
const wrsr::abi::RmlHostApi* g_rmlHost = nullptr;
const wrsr::abi::TsmHost*    g_tsmHost = nullptr;

char g_telemetryText[256] = "Indexed Files: 0  |  Active Conflicts: 0  |  Redirected File Opens: 0";
char g_saveFeedback[128]  = "Tip: Toggle Enable Runtime Redirection live, or Save Config for pack order.";
char g_packLabels[kMaxUiPacks][256]{};
std::string g_conflictInspectorText;

std::wstring getSelfModuleDir()
{
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&getSelfModuleDir),
            &hMod) ||
        !hMod) {
        return {};
    }
    std::wstring path(MAX_PATH * 4, L'\0');
    const DWORD  len = GetModuleFileNameW(hMod, path.data(), static_cast<DWORD>(path.size()));
    if (len == 0 || len >= path.size()) {
        return {};
    }
    path.resize(len);
    const size_t slash = path.find_last_of(L"\\/");
    return (slash != std::wstring::npos) ? path.substr(0, slash) : std::wstring{};
}

void refreshTelemetryText() noexcept
{
    const auto& vfs = mr::VfsIndex::get();
    std::snprintf(g_telemetryText, sizeof(g_telemetryText),
                  "Indexed Files: %zu  |  Active Conflicts: %zu  |  Redirected File Opens: %llu",
                  vfs.indexedFileCount(),
                  vfs.conflictCount(),
                  static_cast<unsigned long long>(mr::hook::redirectHits()));
}

void refreshPackLabels()
{
    const auto& packs = mr::VfsIndex::get().packs();
    const size_t count = (std::min)(packs.size(), kMaxUiPacks);
    for (size_t i = 0; i < count; ++i) {
        const auto& p   = packs[i];
        const char* src = (p.source == mr::PackSource::Workshop) ? "Workshop" : "Local";
        if (p.conflictCount > 0) {
            std::snprintf(g_packLabels[i], sizeof(g_packLabels[i]),
                          "#%zu [%s] %s - %zu/%zu files (%zu conflicts)",
                          i + 1, src, p.displayName.c_str(), p.indexedFiles, p.totalFiles, p.conflictCount);
        } else {
            std::snprintf(g_packLabels[i], sizeof(g_packLabels[i]),
                          "#%zu [%s] %s - %zu/%zu files",
                          i + 1, src, p.displayName.c_str(), p.indexedFiles, p.totalFiles);
        }
    }
}

void buildConflictInspectorText()
{
    const auto& conflicts = mr::VfsIndex::get().conflicts();
    if (conflicts.empty()) {
        g_conflictInspectorText = "No file conflicts detected across enabled override packs.";
        return;
    }

    constexpr size_t kMaxShown = 40;
    std::string out;
    out.reserve(2048);

    const size_t limit = (std::min)(conflicts.size(), kMaxShown);
    for (size_t i = 0; i < limit; ++i) {
        const auto& c = conflicts[i];
        out += c.relativePath;
        out += "\n  Winner: ";
        out += c.winningPack;
        out += "  |  Overridden: ";
        for (size_t j = 0; j < c.overriddenPacks.size(); ++j) {
            if (j > 0) {
                out += ", ";
            }
            out += c.overriddenPacks[j];
        }
        out += "\n";
    }

    if (conflicts.size() > kMaxShown) {
        char tail[96]{};
        std::snprintf(tail, sizeof(tail), "\n... and %zu more conflicts (see MediaRedirector.log).",
                      conflicts.size() - kMaxShown);
        out += tail;
    }

    g_conflictInspectorText = std::move(out);
}

void WRSR_OVERLAY_CALL onSettingChanged(void* /*userData*/)
{
    mr::Config::get().syncFromUi();
}

void WRSR_OVERLAY_CALL onPackToggleChanged(void* /*userData*/)
{
    mr::VfsIndex::get().syncPacksToConfig(mr::Config::get());
    std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                  "Pack state updated (Save Config & relaunch game to apply).");
}

void WRSR_OVERLAY_CALL onMovePackUp(void* userData)
{
    const size_t idx = reinterpret_cast<uintptr_t>(userData);
    if (mr::VfsIndex::get().movePackUp(idx, mr::Config::get())) {
        refreshPackLabels();
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "Moved pack up (Save Config & relaunch game to apply).");
    }
}

void WRSR_OVERLAY_CALL onMovePackDown(void* userData)
{
    const size_t idx = reinterpret_cast<uintptr_t>(userData);
    if (mr::VfsIndex::get().movePackDown(idx, mr::Config::get())) {
        refreshPackLabels();
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "Moved pack down (Save Config & relaunch game to apply).");
    }
}

void WRSR_OVERLAY_CALL onSaveConfig(void* /*userData*/)
{
    mr::VfsIndex::get().syncPacksToConfig(mr::Config::get());
    if (mr::Config::get().save()) {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "Saved MediaRedirector.ini successfully.");
    } else {
        std::snprintf(g_saveFeedback, sizeof(g_saveFeedback),
                      "ERROR: Could not write MediaRedirector.ini.");
    }
}

void WRSR_OVERLAY_CALL onOverlayFrame(void* /*userData*/)
{
    WRSR_PROFILE_HOOK("MediaRedirector::onOverlayFrame");
    refreshTelemetryText();
}

void registerOverlayTab()
{
    WRSR_PROFILE_HOOK("MediaRedirector::registerOverlayTab");

    if (!ModOverlay_Initialize()) {
        wrsr::log::error("ModOverlay_Initialize failed — F9 overlay unavailable");
        return;
    }

    ModTabDesc tab{};
    tab.struct_size = sizeof(ModTabDesc);
    tab.tab_id      = "zydrahydra.media_redirector";
    tab.title       = "Media VFS";
    tab.on_save     = onSaveConfig;
    tab.user_data   = nullptr;

    const int32_t tabId = RegisterModTab(&tab);
    if (tabId < 0) {
        wrsr::log::error("RegisterModTab('Media VFS') failed");
        return;
    }

    refreshTelemetryText();
    refreshPackLabels();
    buildConflictInspectorText();
    SetTabFrameCallback(tabId, onOverlayFrame, nullptr);
    SetTabMainMenuAvailable(tabId, 1);

    auto& cfg = mr::Config::get();

    const uint32_t statusColor = (mr::hook::state() == mr::hook::HookState::Active)
                                     ? 0xA6D973FFu
                                     : 0xE6AD47FFu;

    ModLabelDesc statusLbl{};
    statusLbl.struct_size      = sizeof(ModLabelDesc);
    statusLbl.text             = mr::hook::statusText();
    statusLbl.separator_before = 0;
    statusLbl.rgba_color       = statusColor;
    AddLabel(tabId, &statusLbl);

    ModLabelDesc telemetryLbl{};
    telemetryLbl.struct_size      = sizeof(ModLabelDesc);
    telemetryLbl.text             = g_telemetryText;
    telemetryLbl.separator_before = 0;
    telemetryLbl.rgba_color       = 0xE8D8A0FFu;
    AddLabel(tabId, &telemetryLbl);

    ModCheckboxDesc enableCheck{};
    enableCheck.struct_size = sizeof(ModCheckboxDesc);
    enableCheck.label       = "Enable Runtime Redirection";
    enableCheck.value       = cfg.uiEnabledPtr();
    enableCheck.tooltip     = "Live atomic toggle for VFS redirection of read-only media_soviet\\ opens.";
    enableCheck.on_change   = onSettingChanged;
    enableCheck.user_data   = nullptr;
    AddCheckbox(tabId, &enableCheck);

    ModCheckboxDesc wsCheck{};
    wsCheck.struct_size = sizeof(ModCheckboxDesc);
    wsCheck.label       = "Scan Steam Workshop Overrides";
    wsCheck.value       = cfg.uiScanWorkshopPtr();
    wsCheck.tooltip     = "Scan subscribed Workshop items containing media_soviet\\ or override\\ (applies on next launch).";
    wsCheck.on_change   = onSettingChanged;
    wsCheck.user_data   = nullptr;
    AddCheckbox(tabId, &wsCheck);

    ModLabelDesc packHeaderLbl{};
    packHeaderLbl.struct_size      = sizeof(ModLabelDesc);
    packHeaderLbl.text             = "Pack Priority List (top = highest priority; changes apply on next game launch):";
    packHeaderLbl.separator_before = 1;
    packHeaderLbl.rgba_color       = 0xAAA28AFFu;
    AddLabel(tabId, &packHeaderLbl);

    auto& packs = mr::VfsIndex::get().uiPacks();
    if (packs.empty()) {
        ModLabelDesc emptyLbl{};
        emptyLbl.struct_size      = sizeof(ModLabelDesc);
        emptyLbl.text             = "No override packs found in media_overrides\\ or Workshop.";
        emptyLbl.separator_before = 0;
        emptyLbl.rgba_color       = 0xAAA28AFFu;
        AddLabel(tabId, &emptyLbl);
    } else {
        const size_t count = (std::min)(packs.size(), kMaxUiPacks);
        for (size_t i = 0; i < count; ++i) {
            ModLabelDesc pLbl{};
            pLbl.struct_size      = sizeof(ModLabelDesc);
            pLbl.text             = g_packLabels[i];
            pLbl.separator_before = 0;
            pLbl.rgba_color       = (packs[i].conflictCount > 0) ? 0xE6AD47FFu : 0u;
            AddLabel(tabId, &pLbl);

            char chkLabel[48]{};
            std::snprintf(chkLabel, sizeof(chkLabel), "Enabled##mr_pack_%zu", i);
            ModCheckboxDesc pChk{};
            pChk.struct_size = sizeof(ModCheckboxDesc);
            pChk.label       = chkLabel;
            pChk.value       = &packs[i].enabled;
            pChk.tooltip     = "Enable or disable this pack on next launch.";
            pChk.on_change   = onPackToggleChanged;
            pChk.user_data   = nullptr;
            AddCheckbox(tabId, &pChk);

            char upLabel[48]{};
            std::snprintf(upLabel, sizeof(upLabel), "Move Up##mr_up_%zu", i);
            ModButtonDesc upBtn{};
            upBtn.struct_size = sizeof(ModButtonDesc);
            upBtn.label       = upLabel;
            upBtn.same_line   = 1;
            upBtn.tooltip     = "Increase priority (higher packs win file conflicts).";
            upBtn.on_click    = onMovePackUp;
            upBtn.user_data   = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
            AddButton(tabId, &upBtn);

            char dnLabel[48]{};
            std::snprintf(dnLabel, sizeof(dnLabel), "Move Down##mr_dn_%zu", i);
            ModButtonDesc dnBtn{};
            dnBtn.struct_size = sizeof(ModButtonDesc);
            dnBtn.label       = dnLabel;
            dnBtn.same_line   = 1;
            dnBtn.tooltip     = "Decrease priority.";
            dnBtn.on_click    = onMovePackDown;
            dnBtn.user_data   = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
            AddButton(tabId, &dnBtn);
        }
    }

    char conflictHeader[64]{};
    std::snprintf(conflictHeader, sizeof(conflictHeader), "Conflict Inspector (%zu)",
                  mr::VfsIndex::get().conflictCount());

    ModCollapsibleLabelDesc conflictSec{};
    conflictSec.struct_size      = sizeof(ModCollapsibleLabelDesc);
    conflictSec.header           = conflictHeader;
    conflictSec.text             = g_conflictInspectorText.c_str();
    conflictSec.default_open     = 0;
    conflictSec.separator_before = 1;
    conflictSec.rgba_color       = mr::VfsIndex::get().conflictCount() > 0 ? 0xE6AD47FFu : 0xAAA28AFFu;
    AddCollapsibleLabel(tabId, &conflictSec);

    ModButtonDesc saveBtn{};
    saveBtn.struct_size = sizeof(ModButtonDesc);
    saveBtn.label       = "Save Config";
    saveBtn.same_line   = 0;
    saveBtn.tooltip     = "Write settings and pack priority order to MediaRedirector.ini (Ctrl+S).";
    saveBtn.on_click    = onSaveConfig;
    saveBtn.user_data   = nullptr;
    AddButton(tabId, &saveBtn);

    ModLabelDesc feedbackLbl{};
    feedbackLbl.struct_size      = sizeof(ModLabelDesc);
    feedbackLbl.text             = g_saveFeedback;
    feedbackLbl.separator_before = 0;
    feedbackLbl.rgba_color       = 0xAAA28AFFu;
    AddLabel(tabId, &feedbackLbl);

    wrsr::log::info("Registered 'Media VFS' tab in F9 WRSR Tweaks overlay");
}

DWORD WINAPI overlayInitThread(LPVOID)
{
    Sleep(400);
    registerOverlayTab();
    return 0;
}

void ensureOverlayStarted()
{
    if (!g_overlayStarted.exchange(true)) {
        HANDLE hThread = CreateThread(nullptr, 0, overlayInitThread, nullptr, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            wrsr::log::error("CreateThread(overlayInitThread) failed: %lu", GetLastError());
        }
    }
}

void runEarlyBootInit()
{
    if (g_earlyInitDone.exchange(true)) {
        return;
    }
    wrsr::log::init(L"MediaRedirector.log");
    wrsr::log::info("MediaRedirector v%s (WRSR target: %s) — early boot init",
                    WRSR_MOD_VERSION, WRSR_GAME_VERSION);

    const std::wstring dllDir = getSelfModuleDir();
    mr::Config::get().load(dllDir);
    mr::VfsIndex::get().build(mr::Config::get());
    mr::hook::install();
}

} // namespace

// ============================================================================
// DLL Entry Point — Synchronous early-boot VFS indexing & MinHook install
// ============================================================================
BOOL WINAPI DllMain(HINSTANCE hDll, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hDll);
        runEarlyBootInit();
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
    out->id                 = "zydrahydra.media_redirector";
    out->name               = "MediaRedirector";
    out->version            = WRSR_MOD_VERSION;
    out->author             = "ZydraHydra";
    return (hostApiVersion >= 1) ? 1 : 0;
}

WRSR_ABI_EXPORT int WRSR_ABI_CALL RmlPluginLoad(const wrsr::abi::RmlHostApi* host)
{
    g_rmlHost = host;
    if (host && host->log) {
        host->log(host->plugin_context, wrsr::abi::RML_LOG_INFO,
                  "MediaRedirector: RmlPluginLoad initialized");
    }
    ensureOverlayStarted();
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
        info->name    = "MediaRedirector";
        info->version = WRSR_MOD_VERSION;
    }
    if (host && host->log) {
        host->log("MediaRedirector  TsmPluginInit (host API %u)", host->apiVersion);
    }
    return 0;
}

WRSR_ABI_EXPORT int TsmPluginStart(void)
{
    if (g_tsmHost && g_tsmHost->log) {
        g_tsmHost->log("MediaRedirector  TsmPluginStart");
    }
    ensureOverlayStarted();
    return 0;
}
