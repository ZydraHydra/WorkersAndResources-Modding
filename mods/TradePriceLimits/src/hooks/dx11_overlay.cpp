// ============================================================================
// dx11_overlay.cpp — DX11 Present hook + Dear ImGui overlay (toggle: F8)
// ============================================================================
#include "dx11_overlay.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <d3d11.h>
#include <dinput.h>
#include <dxgi.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

std::atomic<bool>    g_initialized{false};
std::atomic<bool>    g_visible{false};
std::atomic<bool>    g_wantCaptureMouse{false};
std::atomic<bool>    g_wantCaptureKeyboard{false};
std::atomic<bool>    g_rectValid{false};
std::atomic<int32_t> g_rectL{0};
std::atomic<int32_t> g_rectT{0};
std::atomic<int32_t> g_rectR{0};
std::atomic<int32_t> g_rectB{0};
std::atomic<uint8_t> g_externalMouseButtons{0};
std::atomic<uint8_t> g_imguiMouseButtons{0};
std::mutex           g_d3dMutex;

ID3D11Device*           g_device      = nullptr;
ID3D11DeviceContext*    g_ctx         = nullptr;
ID3D11RenderTargetView* g_rtv         = nullptr;
IDXGISwapChain*         g_swapChain   = nullptr;
HWND                    g_hwnd        = nullptr;
WNDPROC                 g_origWndProc = nullptr;

std::wstring g_activeSavePath;
std::mutex   g_savePathMutex;

std::chrono::steady_clock::time_point g_lastPriceSync{};

struct C3dVec3 {
    float x{0.f};
    float y{0.f};
    float z{0.f};
};

using PFN_Present          = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using PFN_ResizeBuffers    = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_DiGetDeviceState = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using PFN_C3dMouseBool     = bool(__fastcall*)(void*);
using PFN_C3dKeyBool       = bool(__fastcall*)(void*, int);
using PFN_C3dMouseRel      = C3dVec3*(__fastcall*)(void*, C3dVec3*);

PFN_Present          g_origPresent              = nullptr;
PFN_ResizeBuffers    g_origResizeBuffers        = nullptr;
PFN_DiGetDeviceState g_origDiGetDeviceState     = nullptr;
PFN_C3dMouseBool     g_origGetMouseLeftPress    = nullptr;
PFN_C3dMouseBool     g_origGetMouseLeftPressOff = nullptr;
PFN_C3dMouseBool     g_origGetMouseLeftDown     = nullptr;
PFN_C3dMouseBool     g_origGetMouseRightPress   = nullptr;
PFN_C3dMouseBool     g_origGetMouseRightPressOff= nullptr;
PFN_C3dMouseBool     g_origGetMouseRightDown    = nullptr;
PFN_C3dMouseBool     g_origGetMouseMiddlePress  = nullptr;
PFN_C3dMouseBool     g_origGetMouseMiddlePressOff=nullptr;
PFN_C3dMouseBool     g_origGetMouseMiddleDown   = nullptr;
PFN_C3dMouseRel      g_origGetMouseRelative     = nullptr;
PFN_C3dKeyBool       g_origGetKeyDown           = nullptr;
PFN_C3dKeyBool       g_origGetKeyPress          = nullptr;

bool isCursorOverOverlayRect() noexcept
{
    if (!g_visible.load(std::memory_order_relaxed) ||
        !g_rectValid.load(std::memory_order_acquire) ||
        !g_hwnd) {
        return false;
    }
    POINT pt{};
    if (!GetCursorPos(&pt) || !ScreenToClient(g_hwnd, &pt)) {
        return false;
    }
    const int32_t l = g_rectL.load(std::memory_order_relaxed);
    const int32_t t = g_rectT.load(std::memory_order_relaxed);
    const int32_t r = g_rectR.load(std::memory_order_relaxed);
    const int32_t b = g_rectB.load(std::memory_order_relaxed);
    return pt.x >= l && pt.x < r && pt.y >= t && pt.y < b;
}

bool isMouseLockedForGame() noexcept
{
    if (!g_visible.load(std::memory_order_relaxed)) {
        return false;
    }
    // Keep camera pans/drags started outside the overlay intact until released.
    if (g_externalMouseButtons.load(std::memory_order_relaxed) != 0) {
        return false;
    }
    if (g_imguiMouseButtons.load(std::memory_order_relaxed) != 0) {
        return true;
    }
    return g_wantCaptureMouse.load(std::memory_order_relaxed) || isCursorOverOverlayRect();
}

bool isKeyboardLockedForGame() noexcept
{
    return g_visible.load(std::memory_order_relaxed) &&
           g_wantCaptureKeyboard.load(std::memory_order_relaxed);
}

HRESULT STDMETHODCALLTYPE detourDiGetDeviceState(IDirectInputDevice8W* self, DWORD cbData, LPVOID lpvData)
{
    const HRESULT hr = g_origDiGetDeviceState(self, cbData, lpvData);
    if (FAILED(hr) || !lpvData) {
        return hr;
    }

    if (cbData == sizeof(DIMOUSESTATE) || cbData == sizeof(DIMOUSESTATE2)) {
        const size_t btnCount = (cbData == sizeof(DIMOUSESTATE2)) ? 8 : 4;
        const auto*  btns     = static_cast<const BYTE*>(lpvData) + offsetof(DIMOUSESTATE, rgbButtons);
        uint8_t curMask = 0;
        for (size_t i = 0; i < btnCount; ++i) {
            if (btns[i] & 0x80) {
                curMask |= static_cast<uint8_t>(1u << i);
            }
        }

        if (!g_visible.load(std::memory_order_relaxed)) {
            g_externalMouseButtons.store(curMask, std::memory_order_relaxed);
            g_imguiMouseButtons.store(0, std::memory_order_relaxed);
            return hr;
        }

        uint8_t extMask = g_externalMouseButtons.load(std::memory_order_relaxed) & curMask;
        uint8_t imgMask = g_imguiMouseButtons.load(std::memory_order_relaxed) & curMask;
        const uint8_t newlyPressed = curMask & static_cast<uint8_t>(~(extMask | imgMask));
        if (newlyPressed != 0) {
            const bool overUi = (extMask == 0) &&
                (imgMask != 0 ||
                 g_wantCaptureMouse.load(std::memory_order_relaxed) ||
                 isCursorOverOverlayRect());
            if (overUi) {
                imgMask |= newlyPressed;
            } else {
                extMask |= newlyPressed;
            }
        }
        g_externalMouseButtons.store(extMask, std::memory_order_relaxed);
        g_imguiMouseButtons.store(imgMask, std::memory_order_relaxed);

        if (isMouseLockedForGame()) {
            std::memset(lpvData, 0, cbData);
        }
    } else if (cbData == 256) {
        if (isKeyboardLockedForGame()) {
            std::memset(lpvData, 0, cbData);
        }
    }

    return hr;
}

bool __fastcall detourMouseLeftPress(void* self)
{
    return isMouseLockedForGame() ? false : g_origGetMouseLeftPress(self);
}
bool __fastcall detourMouseLeftPressOff(void* self)
{
    return isMouseLockedForGame() ? false : g_origGetMouseLeftPressOff(self);
}
bool __fastcall detourMouseLeftDown(void* self)
{
    return isMouseLockedForGame() ? false : g_origGetMouseLeftDown(self);
}
bool __fastcall detourMouseRightPress(void* self)
{
    return isMouseLockedForGame() ? false : g_origGetMouseRightPress(self);
}
bool __fastcall detourMouseRightPressOff(void* self)
{
    return isMouseLockedForGame() ? false : g_origGetMouseRightPressOff(self);
}
bool __fastcall detourMouseRightDown(void* self)
{
    return isMouseLockedForGame() ? false : g_origGetMouseRightDown(self);
}
bool __fastcall detourMouseMiddlePress(void* self)
{
    return isMouseLockedForGame() ? false : g_origGetMouseMiddlePress(self);
}
bool __fastcall detourMouseMiddlePressOff(void* self)
{
    return isMouseLockedForGame() ? false : g_origGetMouseMiddlePressOff(self);
}
bool __fastcall detourMouseMiddleDown(void* self)
{
    return isMouseLockedForGame() ? false : g_origGetMouseMiddleDown(self);
}
C3dVec3* __fastcall detourMouseRelative(void* self, C3dVec3* out)
{
    C3dVec3* res = g_origGetMouseRelative(self, out);
    if (res && isMouseLockedForGame()) {
        res->x = 0.f;
        res->y = 0.f;
        res->z = 0.f;
    }
    return res;
}
bool __fastcall detourKeyDown(void* self, int key)
{
    return isKeyboardLockedForGame() ? false : g_origGetKeyDown(self, key);
}
bool __fastcall detourKeyPress(void* self, int key)
{
    return isKeyboardLockedForGame() ? false : g_origGetKeyPress(self, key);
}

void installDirectInputGuard()
{
    using PFN_DirectInput8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    HMODULE hDi = GetModuleHandleW(L"dinput8.dll");
    if (!hDi) {
        hDi = LoadLibraryW(L"dinput8.dll");
    }
    if (!hDi) {
        wrsr::log::warn("overlay: dinput8.dll not loaded; skipping GetDeviceState guard");
        return;
    }

    auto pfnCreate = reinterpret_cast<PFN_DirectInput8Create>(
        GetProcAddress(hDi, "DirectInput8Create"));
    if (!pfnCreate) {
        wrsr::log::warn("overlay: DirectInput8Create export not found");
        return;
    }

    static constexpr GUID kIidDirectInput8W = {
        0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 }
    };
    static constexpr GUID kGuidSysMouse = {
        0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 }
    };

    IDirectInput8W*       di  = nullptr;
    IDirectInputDevice8W* dev = nullptr;
    if (FAILED(pfnCreate(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                         kIidDirectInput8W, reinterpret_cast<void**>(&di), nullptr)) || !di) {
        wrsr::log::warn("overlay: DirectInput8Create probe failed");
        return;
    }
    if (FAILED(di->CreateDevice(kGuidSysMouse, &dev, nullptr)) || !dev) {
        di->Release();
        wrsr::log::warn("overlay: IDirectInput8::CreateDevice(SysMouse) probe failed");
        return;
    }

    void** vtable = *reinterpret_cast<void***>(dev);
    void*  target = vtable[9]; // IDirectInputDevice8W::GetDeviceState
    dev->Release();
    di->Release();

    if (MH_CreateHook(target, reinterpret_cast<void*>(detourDiGetDeviceState),
                      reinterpret_cast<void**>(&g_origDiGetDeviceState)) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        wrsr::log::info("overlay: installed DirectInput8 GetDeviceState guard (slot 9)");
    } else {
        wrsr::log::warn("overlay: failed to hook DirectInput8 GetDeviceState");
    }
}

void installC3dInputGuards()
{
    HMODULE hExe = GetModuleHandleW(nullptr);
    if (!hExe) {
        return;
    }

    struct IatHookEntry {
        const char* sym;
        void*       detour;
        void**      orig;
    };
    const IatHookEntry entries[] = {
        { "?GetMouseLeftPress@C3D_INPUT@@QEAA_NXZ",      reinterpret_cast<void*>(detourMouseLeftPress),      reinterpret_cast<void**>(&g_origGetMouseLeftPress) },
        { "?GetMouseLeftPressAOff@C3D_INPUT@@QEAA_NXZ",  reinterpret_cast<void*>(detourMouseLeftPressOff),   reinterpret_cast<void**>(&g_origGetMouseLeftPressOff) },
        { "?GetMouseLeftDown@C3D_INPUT@@QEAA_NXZ",       reinterpret_cast<void*>(detourMouseLeftDown),       reinterpret_cast<void**>(&g_origGetMouseLeftDown) },
        { "?GetMouseRightPress@C3D_INPUT@@QEAA_NXZ",     reinterpret_cast<void*>(detourMouseRightPress),     reinterpret_cast<void**>(&g_origGetMouseRightPress) },
        { "?GetMouseRightPressAOff@C3D_INPUT@@QEAA_NXZ", reinterpret_cast<void*>(detourMouseRightPressOff),  reinterpret_cast<void**>(&g_origGetMouseRightPressOff) },
        { "?GetMouseRightDown@C3D_INPUT@@QEAA_NXZ",      reinterpret_cast<void*>(detourMouseRightDown),      reinterpret_cast<void**>(&g_origGetMouseRightDown) },
        { "?GetMouseMiddlePress@C3D_INPUT@@QEAA_NXZ",    reinterpret_cast<void*>(detourMouseMiddlePress),    reinterpret_cast<void**>(&g_origGetMouseMiddlePress) },
        { "?GetMouseMiddlePressAOff@C3D_INPUT@@QEAA_NXZ",reinterpret_cast<void*>(detourMouseMiddlePressOff), reinterpret_cast<void**>(&g_origGetMouseMiddlePressOff) },
        { "?GetMouseMiddleDown@C3D_INPUT@@QEAA_NXZ",     reinterpret_cast<void*>(detourMouseMiddleDown),     reinterpret_cast<void**>(&g_origGetMouseMiddleDown) },
        { "?GetMouseRelative@C3D_INPUT@@QEAA?AVC3DVECTOR3@@XZ", reinterpret_cast<void*>(detourMouseRelative), reinterpret_cast<void**>(&g_origGetMouseRelative) },
        { "?GetKeyDown@C3D_INPUT@@QEAA_NH@Z",            reinterpret_cast<void*>(detourKeyDown),             reinterpret_cast<void**>(&g_origGetKeyDown) },
        { "?GetKeyPress@C3D_INPUT@@QEAA_NH@Z",           reinterpret_cast<void*>(detourKeyPress),            reinterpret_cast<void**>(&g_origGetKeyPress) },
    };

    int hooked = 0;
    for (const auto& e : entries) {
        if (wrsr::pe::patchIat(hExe, "C3DDLL64.dll", e.sym, e.detour, e.orig)) {
            ++hooked;
        }
    }
    wrsr::log::info("overlay: installed %d/%zu C3D_INPUT IAT guard hooks",
                    hooked, sizeof(entries) / sizeof(entries[0]));
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
    } else {
        g_wantCaptureMouse.store(false, std::memory_order_relaxed);
        g_wantCaptureKeyboard.store(false, std::memory_order_relaxed);
        g_imguiMouseButtons.store(0, std::memory_order_relaxed);
        g_rectValid.store(false, std::memory_order_relaxed);
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

        const bool isMouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
        const bool isKeyMsg   = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);
        const bool extDrag    = (g_externalMouseButtons.load(std::memory_order_relaxed) != 0);

        if (!isMouseMsg || !extDrag) {
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp);
        }

        const bool lockMouse = isMouseLockedForGame();
        if (msg == WM_SETCURSOR && lockMouse) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return 1;
        }

        if (isMouseMsg && lockMouse) {
            return 0;
        }
        if (isKeyMsg && isKeyboardLockedForGame()) {
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

int filterNumericChar(ImGuiInputTextCallbackData* data)
{
    const ImWchar ch = data->EventChar;
    if ((ch >= '0' && ch <= '9') || ch == '.') {
        return 0;
    }
    return 1;
}

void drawNumericFloatInput(const char* id, float* value, const char* fmt, bool& outDirty)
{
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), fmt, *value);

    ImGui::SetNextItemWidth(-FLT_MIN);
    const ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CallbackCharFilter;
    if (ImGui::InputText(id, buf, sizeof(buf), flags, filterNumericChar)) {
        if (buf[0] == '\0') {
            *value = 0.f;
        } else {
            char* end = nullptr;
            const float parsed = std::strtof(buf, &end);
            if (end != buf && std::isfinite(parsed)) {
                *value = parsed;
            }
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        outDirty = true;
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
        drawNumericFloatInput(id, fixedValue, "%.2f", outDirty);
    }
}

void renderUI()
{
    const bool visible = g_visible.load(std::memory_order_relaxed);
    if (!visible) {
        g_wantCaptureMouse.store(false, std::memory_order_relaxed);
        g_wantCaptureKeyboard.store(false, std::memory_order_relaxed);
        g_rectValid.store(false, std::memory_order_relaxed);
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
    io.MouseDrawCursor = isMouseLockedForGame();

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
    const bool windowOpen = ImGui::Begin(titleBuf, &open, ImGuiWindowFlags_NoCollapse);
    {
        const ImVec2 pos  = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        g_rectL.store(static_cast<LONG>(pos.x), std::memory_order_relaxed);
        g_rectT.store(static_cast<LONG>(pos.y), std::memory_order_relaxed);
        g_rectR.store(static_cast<LONG>(pos.x + size.x), std::memory_order_relaxed);
        g_rectB.store(static_cast<LONG>(pos.y + size.y), std::memory_order_relaxed);
        g_rectValid.store(true, std::memory_order_relaxed);
    }
    if (windowOpen) {
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
            ImGui::BulletText("Baseline, Save & Hotkey: Click Rebase (or Rebase All) to snapshot live prices as 100%%. Auto-saves on Save/Autosave (or Ctrl+S); rebind open key via toggle_key in TradePriceLimits.ini (next to the DLL).");
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
                    drawNumericFloatInput("##bpct", &r.max_buy_pct, "%.0f", dirty);

                    ImGui::TableSetColumnIndex(11);
                    drawNumericFloatInput("##spct", &r.min_sell_pct, "%.0f", dirty);

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

    const bool mouseCaptured =
        io.WantCaptureMouse ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow |
                               ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
        ImGui::IsAnyItemActive();
    const bool keyboardCaptured = io.WantTextInput || io.WantCaptureKeyboard;
    g_wantCaptureMouse.store(mouseCaptured, std::memory_order_relaxed);
    g_wantCaptureKeyboard.store(keyboardCaptured, std::memory_order_relaxed);
    io.MouseDrawCursor = isMouseLockedForGame();

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

        installDirectInputGuard();
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
