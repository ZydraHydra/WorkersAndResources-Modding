// ============================================================================
// main.cpp — DLL entry point + RML & TesmioLoader lifecycle exports
// ============================================================================
// Dual-loader support:
//   - RML native exports:    RmlPluginQuery, RmlPluginLoad (API v1)
//   - TesmioLoader exports:  TsmPluginApiVersion, TsmPluginInit, TsmPluginStart (API v4)
//
// Hook initialization runs idempotently once from a dedicated worker thread
// spawned on DLL_PROCESS_ATTACH so both loaders and raw injection work cleanly.
// ============================================================================

#include <windows.h>

#include <atomic>
#include <string>

#include "grml_api.h"
#include "wrsr/game_signatures.hpp"
#include "wrsr/loader_abi.hpp"
#include "wrsr/logger.hpp"
#include "wrsr/pe_utils.hpp"

#include "core/rules.hpp"
#include "hooks/dx11_overlay.hpp"
#include "hooks/game_hooks.hpp"
#include "hooks/save_tracker.hpp"

#ifndef WRSR_MOD_VERSION
#define WRSR_MOD_VERSION "1.0.0"
#endif

#ifndef WRSR_GAME_VERSION
#define WRSR_GAME_VERSION "latest"
#endif

namespace {

std::atomic<bool>            g_workerStarted{false};
std::atomic<const GrmlHostApi*> g_grmlHost{nullptr};
std::atomic<HANDLE>             g_grmlPostScanEvent{nullptr};
std::atomic<bool>               g_grmlPostScanDone{false};
const wrsr::abi::RmlHostApi* g_rmlHost = nullptr;
const wrsr::abi::TsmHost*    g_tsmHost = nullptr;

void GRML_CALL onGrmlPostScan(void*)
{
    g_grmlPostScanDone.store(true, std::memory_order_release);
    if (HANDLE ev = g_grmlPostScanEvent.load(std::memory_order_acquire)) {
        SetEvent(ev);
    }
}

void GRML_CALL onGrmlSaveLoad(const wchar_t* saveDir, void*)
{
    tpl::save_tracker::handleSaveLoad(saveDir);
}

void GRML_CALL onGrmlSaveWrite(const wchar_t* saveDir, void*)
{
    tpl::save_tracker::handleSaveWrite(saveDir);
}

void GRML_CALL renderGrmlSummaryTab(const GrmlUiApi* ui, void*)
{
    if (!ui) return;
    auto& engine = tpl::RuleEngine::get();
    ui->TextColored(0xE6C873FFu, "TradePriceLimits v%s", WRSR_MOD_VERSION);
    ui->TextWrapped("Full per-resource price table opens with the F8 hotkey (configurable in TradePriceLimits.ini).");
    ui->Separator();
    uint8_t enforce = engine.isEnforcementEnabled() ? 1u : 0u;
    if (ui->Checkbox("Enforce Trade Price Limits", &enforce)) {
        engine.setEnforcementEnabled(enforce != 0);
    }
    ui->Text("Toggle Key: %s", engine.toggleKeyName().c_str());
    ui->Text("Blocked Dispatches: %llu",
             static_cast<unsigned long long>(tpl::game::getBlockedCount()));
}

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

DWORD WINAPI workerThread(LPVOID)
{
    constexpr DWORD kAttachPollMs = 20;
    DWORD waited = 0;
    while (!g_grmlHost.load(std::memory_order_acquire) && waited < 500) {
        Sleep(kAttachPollMs);
        waited += kAttachPollMs;
    }
    if (g_grmlHost.load(std::memory_order_acquire) && !g_grmlPostScanDone.load(std::memory_order_acquire)) {
        if (HANDLE ev = g_grmlPostScanEvent.load(std::memory_order_acquire)) {
            WaitForSingleObject(ev, 15000);
        }
    }

    wrsr::log::info("TradePriceLimits v%s (WRSR target: %s) — worker thread started",
                    WRSR_MOD_VERSION, WRSR_GAME_VERSION);

    const std::wstring dllDir = getSelfModuleDir();
    if (!dllDir.empty()) {
        tpl::RuleEngine::get().loadGlobalConfig(dllDir);
        wrsr::log::info("Global config loaded (toggle_key=%s)",
                        tpl::RuleEngine::get().toggleKeyName().c_str());
    }

    const auto exe = wrsr::pe::getMainModuleRange();
    if (!exe.base || exe.size == 0) {
        wrsr::log::error("Failed to locate SOVIET64.exe image range — aborting");
        return 1;
    }

    wrsr::log::info("SOVIET64.exe base=0x%p size=0x%zX", exe.base, exe.size);

    if (!tpl::game::install(exe.base, exe.size)) {
        wrsr::log::warn("game_hooks::install: signatures not matched — overlay running in mock/standalone mode");
    }

    if (g_grmlHost.load(std::memory_order_acquire) != nullptr) {
        wrsr::log::info("Using GRML centralized save-lifecycle events (skipping duplicate CreateFileW hook)");
    } else if (!tpl::save_tracker::install()) {
        wrsr::log::error("save_tracker::install failed");
    } else {
        wrsr::log::info("save_tracker::install succeeded (CreateFileW IAT hooked)");
    }

    if (!tpl::overlay::install()) {
        wrsr::log::error("overlay::install failed");
    } else {
        wrsr::log::info("overlay::install succeeded (DX11 Present/ResizeBuffers hooked)");
    }

    wrsr::log::info("Initialization sequence complete");
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
        wrsr::log::init(L"TradePriceLimits.log");
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
    out->id                 = "zydrahydra.trade_price_limits";
    out->name               = "TradePriceLimits";
    out->version            = WRSR_MOD_VERSION;
    out->author             = "ZydraHydra";
    return (hostApiVersion >= 1) ? 1 : 0;
}

WRSR_ABI_EXPORT int WRSR_ABI_CALL RmlPluginLoad(const wrsr::abi::RmlHostApi* host)
{
    g_rmlHost = host;
    if (host && host->log) {
        host->log(host->plugin_context, wrsr::abi::RML_LOG_INFO,
                  "TradePriceLimits: RmlPluginLoad initialized");

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
        info->name    = "TradePriceLimits";
        info->version = WRSR_MOD_VERSION;
    }
    if (host && host->log) {
        host->log("TradePriceLimits  TsmPluginInit (host API %u)", host->apiVersion);
    }
    ensureWorkerStarted();
    return 0;
}

WRSR_ABI_EXPORT int TsmPluginStart(void)
{
    if (g_tsmHost && g_tsmHost->log) {
        g_tsmHost->log("TradePriceLimits  TsmPluginStart");
    }
    return 0;
}

// ============================================================================
// Glorious Republic Mod Loader (GRML) — Pure-C ABI v1 Entrypoint
// ============================================================================
GRML_EXPORT void GRML_CALL GrmlPluginInit(const GrmlHostApi* host)
{
    if (!host || host->abi_version < 1 || host->struct_size < sizeof(GrmlHostApi)) {
        return;
    }
    if (!g_grmlPostScanEvent.load(std::memory_order_acquire)) {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE expected = nullptr;
        if (ev && !g_grmlPostScanEvent.compare_exchange_strong(expected, ev, std::memory_order_acq_rel)) {
            CloseHandle(ev);
        }
    }
    g_grmlHost.store(host, std::memory_order_release);
    wrsr::game::setGrmlHost(host);
    if (host->abi_version >= 2 && host->RegisterPostScanCallback) {
        host->RegisterPostScanCallback(&onGrmlPostScan, nullptr);
    } else {
        onGrmlPostScan(nullptr);
    }
    if (host->RegisterSaveEvents) {
        host->RegisterSaveEvents(&onGrmlSaveLoad, nullptr, nullptr);
    }
    if (host->RegisterSaveWriteEvent) {
        host->RegisterSaveWriteEvent(&onGrmlSaveWrite, nullptr);
    }
    if (host->ui && host->ui->RegisterTab) {
        const int32_t tabId = host->ui->RegisterTab(
            "zydrahydra.trade_price_limits", "Trade Limits", &renderGrmlSummaryTab, nullptr);
        if (tabId >= 0 && host->ui->SetTabMainMenuAvailable) {
            host->ui->SetTabMainMenuAvailable(tabId, 1);
        }
    }
    ensureWorkerStarted();
}
