// ============================================================================
// dx11_overlay.cpp — DX11 Present hook + Dear ImGui overlay (toggle: F8)
// ============================================================================
#include "dx11_overlay.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include "wrsr/logger.hpp"
#include "wrsr/pe_utils.hpp"
#include "../core/rules.hpp"
#include "game_hooks.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace tpl::overlay {

namespace {

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_visible{false};
std::mutex        g_d3dMutex;

ID3D11Device*           g_device      = nullptr;
ID3D11DeviceContext*    g_ctx         = nullptr;
ID3D11RenderTargetView* g_rtv         = nullptr;
IDXGISwapChain*         g_swapChain   = nullptr;
HWND                    g_hwnd        = nullptr;
WNDPROC                 g_origWndProc = nullptr;

std::wstring g_activeSavePath;
std::mutex   g_savePathMutex;

std::chrono::steady_clock::time_point g_lastPriceSync{};

using PFN_Present       = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using PFN_ResizeBuffers = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_C3dMouseBool  = bool(__fastcall*)(void*);

PFN_Present       g_origPresent            = nullptr;
PFN_ResizeBuffers g_origResizeBuffers      = nullptr;
PFN_C3dMouseBool  g_origGetMouseLeftPress  = nullptr;
PFN_C3dMouseBool  g_origGetMouseLeftDown   = nullptr;
PFN_C3dMouseBool  g_origGetMouseRightPress = nullptr;
PFN_C3dMouseBool  g_origGetMouseRightDown  = nullptr;

bool __fastcall detourMouseLeftPress(void* self)
{
    return g_visible.load() ? false : g_origGetMouseLeftPress(self);
}
bool __fastcall detourMouseLeftDown(void* self)
{
    return g_visible.load() ? false : g_origGetMouseLeftDown(self);
}
bool __fastcall detourMouseRightPress(void* self)
{
    return g_visible.load() ? false : g_origGetMouseRightPress(self);
}
bool __fastcall detourMouseRightDown(void* self)
{
    return g_visible.load() ? false : g_origGetMouseRightDown(self);
}

void installC3dInputGuards()
{
    HMODULE hExe = GetModuleHandleW(nullptr);
    if (!hExe) {
        return;
    }
    int hooked = 0;
    if (wrsr::pe::patchIat(hExe, "C3DDLL64.dll", "?GetMouseLeftPress@C3D_INPUT@@QEAA_NXZ",
                           reinterpret_cast<void*>(detourMouseLeftPress),
                           reinterpret_cast<void**>(&g_origGetMouseLeftPress))) {
        ++hooked;
    }
    if (wrsr::pe::patchIat(hExe, "C3DDLL64.dll", "?GetMouseLeftDown@C3D_INPUT@@QEAA_NXZ",
                           reinterpret_cast<void*>(detourMouseLeftDown),
                           reinterpret_cast<void**>(&g_origGetMouseLeftDown))) {
        ++hooked;
    }
    if (wrsr::pe::patchIat(hExe, "C3DDLL64.dll", "?GetMouseRightPress@C3D_INPUT@@QEAA_NXZ",
                           reinterpret_cast<void*>(detourMouseRightPress),
                           reinterpret_cast<void**>(&g_origGetMouseRightPress))) {
        ++hooked;
    }
    if (wrsr::pe::patchIat(hExe, "C3DDLL64.dll", "?GetMouseRightDown@C3D_INPUT@@QEAA_NXZ",
                           reinterpret_cast<void*>(detourMouseRightDown),
                           reinterpret_cast<void**>(&g_origGetMouseRightDown))) {
        ++hooked;
    }
    wrsr::log::info("overlay: installed %d/4 C3D_INPUT mouse guard hooks", hooked);
}

// ---------------------------------------------------------------------------
// WRSR Soviet-panel color theme (warm olive-charcoal, ochre-gold, cream text)
// ---------------------------------------------------------------------------
void applyWrsrTheme()
{
    ImGuiStyle& style   = ImGui::GetStyle();
    style.WindowPadding = ImVec2(10.f, 8.f);
    style.FramePadding  = ImVec2(6.f, 3.f);
    style.CellPadding   = ImVec2(5.f, 3.f);
    style.ItemSpacing   = ImVec2(8.f, 5.f);
    style.WindowRounding    = 2.f;
    style.ChildRounding     = 2.f;
    style.FrameRounding     = 2.f;
    style.ScrollbarRounding = 2.f;
    style.WindowBorderSize  = 1.f;
    style.FrameBorderSize   = 1.f;

    ImVec4* c = style.Colors;
    // Warm cream text
    c[ImGuiCol_Text]                 = ImVec4(0.91f, 0.88f, 0.80f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.55f, 0.52f, 0.45f, 1.00f);
    // Dark olive-charcoal panels
    c[ImGuiCol_WindowBg]             = ImVec4(0.12f, 0.12f, 0.10f, 0.96f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.14f, 0.13f, 0.11f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.14f, 0.13f, 0.11f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.32f, 0.29f, 0.24f, 1.00f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.40f);
    // Title bar & headers
    c[ImGuiCol_TitleBg]              = ImVec4(0.16f, 0.15f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.27f, 0.22f, 0.14f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.14f, 0.13f, 0.11f, 0.90f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.17f, 0.16f, 0.13f, 1.00f);
    // Input fields
    c[ImGuiCol_FrameBg]              = ImVec4(0.09f, 0.09f, 0.07f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.20f, 0.18f, 0.13f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.28f, 0.23f, 0.15f, 1.00f);
    // Buttons (muted khaki / ochre highlight)
    c[ImGuiCol_Button]               = ImVec4(0.24f, 0.21f, 0.16f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.42f, 0.34f, 0.20f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.58f, 0.45f, 0.22f, 1.00f);
    // Headers & table rows
    c[ImGuiCol_Header]               = ImVec4(0.28f, 0.24f, 0.16f, 0.90f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.42f, 0.34f, 0.20f, 0.95f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.55f, 0.43f, 0.22f, 1.00f);
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.20f, 0.18f, 0.14f, 1.00f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.35f, 0.31f, 0.25f, 1.00f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.24f, 0.22f, 0.18f, 1.00f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0.12f, 0.12f, 0.10f, 1.00f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(0.16f, 0.15f, 0.13f, 1.00f);
    // Scrollbars, separators, resize grip, gold accents
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.09f, 0.09f, 0.07f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.30f, 0.26f, 0.20f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.46f, 0.38f, 0.24f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.65f, 0.50f, 0.25f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.35f, 0.31f, 0.24f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.84f, 0.65f, 0.27f, 1.00f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.45f, 0.37f, 0.22f, 0.60f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.75f, 0.58f, 0.26f, 0.85f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.85f, 0.66f, 0.28f, 1.00f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.85f, 0.66f, 0.28f, 1.00f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.58f, 0.45f, 0.20f, 0.60f);
}

void releaseRenderTarget()
{
    std::lock_guard lock(g_d3dMutex);
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

void createRenderTarget(IDXGISwapChain* sc)
{
    std::lock_guard lock(g_d3dMutex);
    ID3D11Texture2D* backBuf = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuf))) || !backBuf) {
        return;
    }
    g_device->CreateRenderTargetView(backBuf, nullptr, &g_rtv);
    backBuf->Release();
}

void toggleOverlay(bool show)
{
    g_visible = show;
    if (show) {
        ClipCursor(nullptr);
    }
}

LRESULT CALLBACK hookedWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const uint32_t toggleVk = tpl::RuleEngine::get().toggleKey();
    if (msg == WM_KEYDOWN && static_cast<uint32_t>(wp) == toggleVk && ((lp & (1LL << 30)) == 0)) {
        toggleOverlay(!g_visible.load());
        return 0;
    }

    if (g_visible.load()) {
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE && ((lp & (1LL << 30)) == 0)) {
            toggleOverlay(false);
            return 0;
        }

        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp);

        if (msg == WM_SETCURSOR) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return 1;
        }

        if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
            (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
            msg == WM_INPUT) {
            return 0;
        }
    }

    return CallWindowProcW(g_origWndProc, hWnd, msg, wp, lp);
}

void seedMockResourcesIfEmpty()
{
    if (!tpl::RuleEngine::get().rules().empty()) {
        return;
    }
    struct MockSeed {
        const char* name;
        float       rub;
        float       usd;
    };
    static constexpr MockSeed kSeeds[] = {
        { "coal",         16.13f,   14.20f },
        { "iron",         24.50f,   21.80f },
        { "steel",       195.00f,  175.00f },
        { "gravel",        4.20f,    3.80f },
        { "wood",         11.50f,   10.20f },
        { "boards",       29.00f,   26.00f },
        { "bricks",       38.00f,   34.50f },
        { "prefabpanels", 54.00f,   49.00f },
        { "cement",       42.00f,   38.00f },
        { "bitumen",      64.00f,   58.00f },
        { "oil",          45.00f,   41.00f },
        { "fuel",        118.00f,  108.00f },
        { "chemicals",   245.00f,  220.00f },
        { "plastics",    280.00f,  255.00f },
        { "food",         68.00f,   61.00f },
        { "clothes",     152.00f,  138.00f },
        { "meat",        134.00f,  120.00f },
        { "eletric",       0.85f,    0.75f },
    };

    auto& engine = tpl::RuleEngine::get();
    for (const auto& s : kSeeds) {
        engine.ensureResource(s.name, s.rub, s.usd);
    }
}

bool containsCaseInsensitive(std::string_view haystack, std::string_view needle) noexcept
{
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    const auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

void drawHeaderCellWithTip(int colIdx, const char* label, const char* tooltip)
{
    ImGui::TableSetColumnIndex(colIdx);
    ImGui::TableHeader(label);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", tooltip);
    }
}

void drawLivePriceCell(int colIdx, float price, bool blocked)
{
    ImGui::TableSetColumnIndex(colIdx);
    if (blocked) {
        ImGui::TextColored(ImVec4(0.92f, 0.42f, 0.35f, 1.f), "%.2f", price);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Currently blocked by your active limit (price %.2f).", price);
        }
    } else {
        ImGui::Text("%.2f", price);
    }
}

void drawLimitCell(int colIdx, const char* id, bool isBaseline,
                   float* fixedValue, float computedLimit, float pctValue, bool& outDirty)
{
    ImGui::TableSetColumnIndex(colIdx);
    if (isBaseline) {
        if (pctValue <= 0.f || computedLimit == 0.f) {
            ImGui::TextDisabled("Off");
        } else {
            ImGui::TextColored(ImVec4(0.84f, 0.72f, 0.34f, 1.f), "= %.2f", computedLimit);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Auto-computed in Base %% mode: Baseline * %.0f%% = %.2f", pctValue, computedLimit);
            }
        }
    } else {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputFloat(id, fixedValue, 0.f, 0.f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            outDirty = true;
        }
    }
}

void renderUI()
{
    const bool visible = g_visible.load(std::memory_order_relaxed);
    if (!visible) {
        ImGui::GetIO().MouseDrawCursor = false;
        return;
    }

    static bool g_showHelp       = false;
    static bool g_onlyActive     = false;
    static char g_filterBuf[64]{};

    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastPriceSync > std::chrono::milliseconds(500)) {
        g_lastPriceSync = now;
        tpl::game::syncLivePricesToRules();
    }

    ImGuiIO& io        = ImGui::GetIO();
    io.MouseDrawCursor = true;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        const auto saveDir = tpl::RuleEngine::get().activeSaveDir();
        if (!saveDir.empty()) {
            tpl::RuleEngine::get().saveToSave(saveDir);
        }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        auto& engine = tpl::RuleEngine::get();
        for (auto r : engine.rules()) {
            r.rebase();
            engine.updateRule(r);
        }
    }

    ImGui::SetNextWindowSize(ImVec2(1180, 620), ImGuiCond_FirstUseEver);
    const std::string keyName = tpl::RuleEngine::get().toggleKeyName();
    char titleBuf[96]{};
    std::snprintf(titleBuf, sizeof(titleBuf), "TradePriceLimits  [%s / Esc]###TradePriceLimits", keyName.c_str());
    bool open = true;
    if (ImGui::Begin(titleBuf, &open, ImGuiWindowFlags_NoCollapse)) {
        // ── Header status bar ─────────────────────────────────────────────
        {
            std::lock_guard lock(g_savePathMutex);
            if (!g_activeSavePath.empty()) {
                int sz = WideCharToMultiByte(CP_UTF8, 0, g_activeSavePath.c_str(), -1,
                                             nullptr, 0, nullptr, nullptr);
                std::string narrow(sz > 1 ? static_cast<size_t>(sz - 1) : 0, '\0');
                if (sz > 1) {
                    WideCharToMultiByte(CP_UTF8, 0, g_activeSavePath.c_str(), -1,
                                        narrow.data(), sz, nullptr, nullptr);
                }
                ImGui::TextColored(ImVec4(0.65f, 0.85f, 0.45f, 1.f), "Save: %s", narrow.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.90f, 0.68f, 0.28f, 1.f),
                                   "Save: (load or save a game to bind sidecar INI)");
            }
        }

        ImGui::SameLine();
        if (tpl::game::g_addrs.resourceVector) {
            ImGui::TextColored(ImVec4(0.84f, 0.72f, 0.34f, 1.f), " | Prices: LIVE");
        } else {
            ImGui::TextColored(ImVec4(0.82f, 0.55f, 0.30f, 1.f), " | Prices: MOCK");
        }

        ImGui::SameLine();
        bool enforceEnabled = tpl::RuleEngine::get().isEnforcementEnabled();
        if (ImGui::Checkbox("Enforce Limits", &enforceEnabled)) {
            tpl::RuleEngine::get().setEnforcementEnabled(enforceEnabled);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Master switch. Uncheck to pause all price blocking without losing your rules.");
        }

        ImGui::SameLine();
        if (!tpl::game::g_addrs.dispatchCheckSite) {
            ImGui::TextColored(ImVec4(0.85f, 0.40f, 0.35f, 1.f), " | Hook: OFF");
        } else if (!enforceEnabled) {
            ImGui::TextColored(ImVec4(0.90f, 0.68f, 0.28f, 1.f), " | Enforcement: PAUSED");
        } else {
            const uint64_t blocked = tpl::game::getBlockedCount();
            ImGui::TextColored(ImVec4(0.65f, 0.85f, 0.45f, 1.f),
                               " | Enforcement: ACTIVE (Blocked Checks: %llu)",
                               static_cast<unsigned long long>(blocked));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Distribution Office customs checks blocked since last Save/Autosave/Load (counts planner re-checks, not individual trucks).");
            }
        }

        // ── Filter & Help toolbar ─────────────────────────────────────────
        ImGui::SetNextItemWidth(220.f);
        ImGui::InputTextWithHint("##tpl_filter", "Filter resources...", g_filterBuf, sizeof(g_filterBuf));
        if (g_filterBuf[0] != '\0') {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                g_filterBuf[0] = '\0';
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox("Active Limits Only", &g_onlyActive);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Show only resources with at least one non-zero limit.");
        }

        ImGui::SameLine();
        if (ImGui::Button(g_showHelp ? "Hide Help" : "? Help")) {
            g_showHelp = !g_showHelp;
        }

        if (g_showHelp) {
            ImGui::BeginChild("##tpl_help", ImVec2(0.f, 152.f), true);
            ImGui::TextColored(ImVec4(0.84f, 0.72f, 0.34f, 1.f), "How TradePriceLimits Works (0 = Disabled):");
            ImGui::BulletText("Dispatch gating: Stops NEW Distribution Office dispatches (Road & Rail) to/from Customs. Trucks/trains already en route are NOT cancelled and will finish their current trip.");
            ImGui::BulletText("Fixed mode: Set exact currency limits. Max Buy stops imports above your cap; Min Sell stops exports below your floor (RUB = Soviet Customs, USD = Western Customs).");
            ImGui::BulletText("Negative prices (waste/trash): Signed limits work directly (e.g. Max Buy -300 stops importing trash if Customs pays less than 300/t; Min Sell -400 stops exporting if disposal costs > 400/t).");
            ImGui::BulletText("Base %% mode: Scales with inflation! Max Buy %% is a %% of Baseline Buy (e.g. 115%% = +15%% cap; for negative waste use e.g. 90%% so payout cannot drop >10%%). Min Sell %% is a %% of Baseline Sell.");
            ImGui::BulletText("Baseline, Save & Hotkey: Click Rebase (or Rebase All) to snapshot live prices as 100%%. Auto-saves on Save/Autosave (or Ctrl+S); rebind open key via toggle_key in trade_price_limits.ini.");
            ImGui::EndChild();
        }

        ImGui::Separator();

        // ── Resource Thresholds Table ─────────────────────────────────────
        constexpr ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
            ImGuiTableFlags_Resizable;

        const ImVec2 tableSize(0.f, ImGui::GetContentRegionAvail().y - 38.f);
        if (ImGui::BeginTable("##tpl_rules", 13, tableFlags, tableSize)) {
            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableSetupColumn("Resource",        ImGuiTableColumnFlags_WidthFixed, 122.f);
            ImGui::TableSetupColumn("Mode",            ImGuiTableColumnFlags_WidthFixed,  68.f);
            ImGui::TableSetupColumn("Buy RUB",         ImGuiTableColumnFlags_WidthFixed,  66.f);
            ImGui::TableSetupColumn("Max Buy RUB",     ImGuiTableColumnFlags_WidthFixed,  86.f);
            ImGui::TableSetupColumn("Sell RUB",        ImGuiTableColumnFlags_WidthFixed,  66.f);
            ImGui::TableSetupColumn("Min Sell RUB",    ImGuiTableColumnFlags_WidthFixed,  86.f);
            ImGui::TableSetupColumn("Buy USD",         ImGuiTableColumnFlags_WidthFixed,  66.f);
            ImGui::TableSetupColumn("Max Buy USD",     ImGuiTableColumnFlags_WidthFixed,  86.f);
            ImGui::TableSetupColumn("Sell USD",        ImGuiTableColumnFlags_WidthFixed,  66.f);
            ImGui::TableSetupColumn("Min Sell USD",    ImGuiTableColumnFlags_WidthFixed,  86.f);
            ImGui::TableSetupColumn("Max Buy %",       ImGuiTableColumnFlags_WidthFixed,  74.f);
            ImGui::TableSetupColumn("Min Sell %",      ImGuiTableColumnFlags_WidthFixed,  74.f);
            ImGui::TableSetupColumn("Baseline (100%)", ImGuiTableColumnFlags_WidthFixed, 152.f);

            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            drawHeaderCellWithTip(0,  "Resource",        "Internal game resource name.");
            drawHeaderCellWithTip(1,  "Mode",            "Click to switch between Fixed currency caps and Base % inflation-scaled limits.");
            drawHeaderCellWithTip(2,  "Buy RUB",         "Live Soviet Customs import price (RUB). Red = blocked by Max Buy RUB.");
            drawHeaderCellWithTip(3,  "Max Buy RUB",     "Maximum import price in RUB (0 = disabled). Computed '= value' in Base % mode.");
            drawHeaderCellWithTip(4,  "Sell RUB",        "Live Soviet Customs export price (RUB). Red = blocked by Min Sell RUB.");
            drawHeaderCellWithTip(5,  "Min Sell RUB",    "Minimum export price in RUB (0 = disabled). Computed '= value' in Base % mode.");
            drawHeaderCellWithTip(6,  "Buy USD",         "Live Western Customs import price (USD). Red = blocked by Max Buy USD.");
            drawHeaderCellWithTip(7,  "Max Buy USD",     "Maximum import price in USD (0 = disabled). Computed '= value' in Base % mode.");
            drawHeaderCellWithTip(8,  "Sell USD",        "Live Western Customs export price (USD). Red = blocked by Min Sell USD.");
            drawHeaderCellWithTip(9,  "Min Sell USD",    "Minimum export price in USD (0 = disabled). Computed '= value' in Base % mode.");
            drawHeaderCellWithTip(10, "Max Buy %",       "Base % mode: Max Buy limit as % of Baseline Buy price (e.g. 115 = allow +15%).");
            drawHeaderCellWithTip(11, "Min Sell %",      "Base % mode: Min Sell floor as % of Baseline Sell price (e.g. 85 = allow -15%).");
            drawHeaderCellWithTip(12, "Baseline (100%)", "Snapshot used as 100% in Base % mode. Click Rebase to capture today's prices.");

            auto& engine = tpl::RuleEngine::get();
            auto  rules  = engine.rules();
            const std::string_view filterView(g_filterBuf);

            for (auto& r : rules) {
                if (!containsCaseInsensitive(r.name, filterView)) {
                    continue;
                }
                if (g_onlyActive &&
                    r.effectiveMaxBuyRub() == 0.f && r.effectiveMinSellRub() == 0.f &&
                    r.effectiveMaxBuyUsd() == 0.f && r.effectiveMinSellUsd() == 0.f &&
                    r.max_buy_pct <= 0.f && r.min_sell_pct <= 0.f) {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::PushID(r.name.c_str());

                bool dirty = false;

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(r.name.c_str());

                ImGui::TableSetColumnIndex(1);
                const bool isBaseline = (r.mode == tpl::RuleMode::BaselinePct);
                if (ImGui::SmallButton(isBaseline ? "Base %" : "Fixed")) {
                    r.mode = isBaseline ? tpl::RuleMode::FixedCap : tpl::RuleMode::BaselinePct;
                    if (!isBaseline) {
                        r.rebase();
                    }
                    dirty = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip(isBaseline
                                          ? "Using Base %% mode (scales with Baseline). Click to switch to Fixed caps."
                                          : "Using Fixed caps. Click to switch to Base %% (rebases to live prices).");
                }

                // RUB: Buy -> Max Buy -> Sell -> Min Sell
                drawLivePriceCell(2, r.live_buy_rub, r.shouldBlockBuyRub(r.live_buy_rub));
                drawLimitCell(3, "##mbr", isBaseline, &r.max_buy_rub, r.effectiveMaxBuyRub(), r.max_buy_pct, dirty);
                drawLivePriceCell(4, r.live_sell_rub, r.shouldBlockSellRub(r.live_sell_rub));
                drawLimitCell(5, "##msr", isBaseline, &r.min_sell_rub, r.effectiveMinSellRub(), r.min_sell_pct, dirty);

                // USD: Buy -> Max Buy -> Sell -> Min Sell
                drawLivePriceCell(6, r.live_buy_usd, r.shouldBlockBuyUsd(r.live_buy_usd));
                drawLimitCell(7, "##mbu", isBaseline, &r.max_buy_usd, r.effectiveMaxBuyUsd(), r.max_buy_pct, dirty);
                drawLivePriceCell(8, r.live_sell_usd, r.shouldBlockSellUsd(r.live_sell_usd));
                drawLimitCell(9, "##msu", isBaseline, &r.min_sell_usd, r.effectiveMinSellUsd(), r.min_sell_pct, dirty);

                if (isBaseline) {
                    ImGui::TableSetColumnIndex(10);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputFloat("##bpct", &r.max_buy_pct, 0.f, 0.f, "%.0f%%");
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        dirty = true;
                    }

                    ImGui::TableSetColumnIndex(11);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputFloat("##spct", &r.min_sell_pct, 0.f, 0.f, "%.0f%%");
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        dirty = true;
                    }

                    ImGui::TableSetColumnIndex(12);
                    if (ImGui::SmallButton("Rebase")) {
                        r.rebase();
                        dirty = true;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Set 100%% baseline to today's live market Buy & Sell prices.");
                    }
                    ImGui::SameLine();
                    ImGui::Text("%.1f/%.1f", r.baseline_rub, r.baseline_usd);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("100%% Baseline Snapshot:\n"
                                          "  RUB: Buy %.2f | Sell %.2f\n"
                                          "  USD: Buy %.2f | Sell %.2f",
                                          r.baseline_rub,
                                          (r.baseline_sell_rub != 0.f ? r.baseline_sell_rub : r.baseline_rub),
                                          r.baseline_usd,
                                          (r.baseline_sell_usd != 0.f ? r.baseline_sell_usd : r.baseline_usd));
                    }
                } else {
                    ImGui::TableSetColumnIndex(10);
                    ImGui::TextDisabled("--");
                    ImGui::TableSetColumnIndex(11);
                    ImGui::TextDisabled("--");
                    ImGui::TableSetColumnIndex(12);
                    ImGui::TextDisabled("Fixed mode");
                }

                if (dirty) {
                    engine.updateRule(r);
                }

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        // ── Bottom toolbar ────────────────────────────────────────────────
        ImGui::Separator();
        const bool hasSave = tpl::RuleEngine::get().hasSave();
        if (!hasSave) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Save to Sidecar INI (Ctrl+S)")) {
            const auto saveDir = tpl::RuleEngine::get().activeSaveDir();
            if (!saveDir.empty()) {
                tpl::RuleEngine::get().saveToSave(saveDir);
            }
        }
        if (!hasSave) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Rebase All (Ctrl+R)")) {
            auto& engine = tpl::RuleEngine::get();
            for (auto r : engine.rules()) {
                r.rebase();
                engine.updateRule(r);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Snapshot current live Buy & Sell prices as the 100%% Baseline for every resource.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Auto-saves on Save/Autosave | Red = Blocked | 0 = Disabled | Hover headers for tips");
    }
    ImGui::End();

    if (!open) {
        toggleOverlay(false);
    }

    ImGui::Render();

    std::lock_guard lock(g_d3dMutex);
    if (g_rtv) {
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

HRESULT WINAPI hookedPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags)
{
    if (!g_initialized.exchange(true)) {
        sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device));
        g_device->GetImmediateContext(&g_ctx);
        g_swapChain = sc;

        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;

        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hookedWndProc)));

        installC3dInputGuards();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io    = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        applyWrsrTheme();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX11_Init(g_device, g_ctx);

        createRenderTarget(sc);
        tpl::game::syncLivePricesToRules();
        seedMockResourcesIfEmpty();
    }

    renderUI();
    return g_origPresent(sc, syncInterval, flags);
}

HRESULT WINAPI hookedResizeBuffers(IDXGISwapChain* sc,
                                   UINT bufCount, UINT w, UINT h,
                                   DXGI_FORMAT fmt, UINT flags)
{
    releaseRenderTarget();
    ImGui_ImplDX11_InvalidateDeviceObjects();
    HRESULT hr = g_origResizeBuffers(sc, bufCount, w, h, fmt, flags);
    ImGui_ImplDX11_CreateDeviceObjects();
    createRenderTarget(sc);
    return hr;
}

} // namespace

bool install()
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.lpszClassName = L"TPL_D3D_Probe";
    wc.hInstance     = GetModuleHandleW(nullptr);
    RegisterClassExW(&wc);

    HWND hwndProbe = CreateWindowExW(0, L"TPL_D3D_Probe", L"",
                                     WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                     nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwndProbe) {
        UnregisterClassW(L"TPL_D3D_Probe", wc.hInstance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount       = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = hwndProbe;
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device*        probeDevice    = nullptr;
    ID3D11DeviceContext* probeCtx       = nullptr;
    IDXGISwapChain*      probeSwapChain = nullptr;
    D3D_FEATURE_LEVEL    fl{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &probeSwapChain, &probeDevice, &fl, &probeCtx);

    if (FAILED(hr) || !probeSwapChain) {
        DestroyWindow(hwndProbe);
        UnregisterClassW(L"TPL_D3D_Probe", wc.hInstance);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(probeSwapChain);

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        probeSwapChain->Release();
        probeDevice->Release();
        probeCtx->Release();
        DestroyWindow(hwndProbe);
        UnregisterClassW(L"TPL_D3D_Probe", wc.hInstance);
        return false;
    }

    MH_CreateHook(vtable[8], reinterpret_cast<void*>(hookedPresent),
                  reinterpret_cast<void**>(&g_origPresent));
    MH_CreateHook(vtable[13], reinterpret_cast<void*>(hookedResizeBuffers),
                  reinterpret_cast<void**>(&g_origResizeBuffers));
    MH_EnableHook(vtable[8]);
    MH_EnableHook(vtable[13]);

    probeSwapChain->Release();
    probeDevice->Release();
    probeCtx->Release();
    DestroyWindow(hwndProbe);
    UnregisterClassW(L"TPL_D3D_Probe", wc.hInstance);
    return true;
}

void setActiveSavePath(const wchar_t* path)
{
    std::lock_guard lock(g_savePathMutex);
    g_activeSavePath = path ? path : L"";
}

} // namespace tpl::overlay
