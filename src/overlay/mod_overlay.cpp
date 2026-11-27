// ============================================================================
// mod_overlay.cpp — Self-contained F9 multi-mod DX11 + ImGui overlay hub
// ============================================================================
#include "mod_overlay.h"
#include "wrsr/game_signatures.hpp"

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif

#include <windows.h>
#include <d3d11.h>
#include <dinput.h>
#include <dxgi.h>
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

constexpr uint32_t kHubMagic = 0x57524D4Fu; // 'WRMO'

struct SharedOverlayHub {
    uint32_t      magic;
    uint32_t      abiVersion;
    uint32_t      structSize;
    volatile LONG ownerInitialized;
    DWORD         ownerPid;
    HMODULE       ownerModule;

    int32_t(WRSR_OVERLAY_CALL* registerTab)(const ModTabDesc*);
    int32_t(WRSR_OVERLAY_CALL* addFloatSlider)(int32_t, const ModFloatSliderDesc*);
    int32_t(WRSR_OVERLAY_CALL* addCheckbox)(int32_t, const ModCheckboxDesc*);
    int32_t(WRSR_OVERLAY_CALL* addButton)(int32_t, const ModButtonDesc*);
    int32_t(WRSR_OVERLAY_CALL* addLabel)(int32_t, const ModLabelDesc*);
    int32_t(WRSR_OVERLAY_CALL* isVisible)(void);
    void(WRSR_OVERLAY_CALL* setVisible)(int32_t);
    int32_t(WRSR_OVERLAY_CALL* addCollapsibleLabel)(int32_t, const ModCollapsibleLabelDesc*);
    int32_t(WRSR_OVERLAY_CALL* setTabFrameCallback)(int32_t, ModOverlayButtonFn, void*);
    int32_t(WRSR_OVERLAY_CALL* setTabMainMenuAvailable)(int32_t, uint8_t);
};

// ── Cross-DLL IPC mapping handles (intentionally kept for process lifetime) ──
std::mutex        g_ipcMutex;
HANDLE            g_mapHandle = nullptr;
SharedOverlayHub* g_hub       = nullptr;
bool              g_isHost    = false;

// ── Host-side widget & tab storage ───────────────────────────────────────────
enum class WidgetKind {
    FloatSlider,
    Checkbox,
    Button,
    Label,
    CollapsibleLabel,
};

struct StoredWidget {
    WidgetKind kind{WidgetKind::Label};
    std::string label;
    std::string format;
    std::string tooltip;

    float*             floatPtr{nullptr};
    float              minVal{0.f};
    float              maxVal{1.f};
    uint8_t*           boolPtr{nullptr};
    const char*        persistentText{nullptr};
    uint8_t            sameLine{0};
    uint8_t            separatorBefore{0};
    uint8_t            defaultOpen{0};
    uint32_t           rgbaColor{0};
    ModOverlayChangeFn onChange{nullptr};
    ModOverlayButtonFn onClick{nullptr};
    void*              userData{nullptr};
};

struct StoredTab {
    std::string               id;
    std::string               title;
    ModOverlaySaveFn          onSave{nullptr};
    void*                     userData{nullptr};
    ModOverlayButtonFn        onFrame{nullptr};
    void*                     frameUserData{nullptr};
    uint8_t                   mainMenuAvailable{0};
    std::vector<StoredWidget> widgets;
};

std::mutex             g_tabsMutex;
std::vector<StoredTab> g_tabs;

// ── GRML Host Adapter State (when loaded by GloriousRepublicModLoader) ───────
std::atomic<const GrmlHostApi*> g_grmlHost{nullptr};
std::string                     g_grmlPrimaryPluginId{"wrsr.mod"};

struct ProfileBucket {
    std::atomic<const char*> hookName{nullptr};
    std::atomic<uint64_t>    calls{0};
    std::atomic<int64_t>     qpcTicks{0};
};
constexpr size_t kMaxProfileBuckets = 32;
ProfileBucket    g_profileBuckets[kMaxProfileBuckets];
double           g_qpcFreqMs = 0.0;

void initQpcFreq() noexcept
{
    if (g_qpcFreqMs == 0.0) {
        LARGE_INTEGER freq{};
        if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0) {
            g_qpcFreqMs = 1000.0 / static_cast<double>(freq.QuadPart);
        }
    }
}

void drawWidgetToGrmlUi(const GrmlUiApi* ui, const StoredWidget& w)
{
    switch (w.kind) {
    case WidgetKind::FloatSlider:
        if (w.floatPtr) {
            const char* fmt = w.format.empty() ? "%.2f" : w.format.c_str();
            if (ui->SliderFloat(w.label.c_str(), w.floatPtr, w.minVal, w.maxVal, fmt)) {
                if (w.onChange) w.onChange(w.userData);
            }
            if (!w.tooltip.empty() && ui->SetItemTooltip) {
                ui->SetItemTooltip("%s", w.tooltip.c_str());
            }
        }
        break;
    case WidgetKind::Checkbox:
        if (w.boolPtr) {
            if (ui->Checkbox(w.label.c_str(), w.boolPtr)) {
                if (w.onChange) w.onChange(w.userData);
            }
            if (!w.tooltip.empty() && ui->SetItemTooltip) {
                ui->SetItemTooltip("%s", w.tooltip.c_str());
            }
        }
        break;
    case WidgetKind::Button:
        if (w.sameLine) ui->SameLine();
        if (ui->Button(w.label.c_str())) {
            if (w.onClick) w.onClick(w.userData);
        }
        if (!w.tooltip.empty() && ui->SetItemTooltip) {
            ui->SetItemTooltip("%s", w.tooltip.c_str());
        }
        break;
    case WidgetKind::Label:
        if (w.separatorBefore) ui->Separator();
        {
            const char* txt = w.persistentText ? w.persistentText : "";
            if (w.rgbaColor != 0) ui->TextColored(w.rgbaColor, "%s", txt);
            else if (ui->TextWrapped) ui->TextWrapped("%s", txt);
            else ui->Text("%s", txt);
        }
        break;
    case WidgetKind::CollapsibleLabel:
        if (w.separatorBefore) ui->Separator();
        if (ui->CollapsingHeader && ui->CollapsingHeader(w.label.c_str(), w.defaultOpen)) {
            const char* txt = w.persistentText ? w.persistentText : "";
            if (w.rgbaColor != 0) ui->TextColored(w.rgbaColor, "%s", txt);
            else if (ui->TextWrapped) ui->TextWrapped("%s", txt);
            else ui->Text("%s", txt);
        }
        break;
    }
}

void GRML_CALL grmlRenderTabTrampoline(const GrmlUiApi* ui, void* userData)
{
    if (!ui) return;
    ModOverlay_FlushHookTelemetry();

    const size_t tabIdx = reinterpret_cast<uintptr_t>(userData);
    StoredTab    snapshot;
    {
        std::lock_guard lock(g_tabsMutex);
        if (tabIdx >= g_tabs.size()) return;
        snapshot = g_tabs[tabIdx];
    }

    if (snapshot.onFrame) {
        snapshot.onFrame(snapshot.frameUserData);
    }
    for (size_t i = 0; i < snapshot.widgets.size(); ++i) {
        drawWidgetToGrmlUi(ui, snapshot.widgets[i]);
    }
}

// ── Host-side DX11 / WndProc / Input guard state ─────────────────────────────
std::atomic<bool>    g_dxInitialized{false};
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
const volatile uint8_t* g_inGameFlagPtr = nullptr;

void initInGameFlagProbe() noexcept
{
    HMODULE hExe = GetModuleHandleW(nullptr);
    if (!hExe) {
        return;
    }
    auto* base = reinterpret_cast<const uint8_t*>(hExe);
    auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const uint8_t* textBase = nullptr;
    size_t         textSize = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
            textBase = base + sec[i].VirtualAddress;
            textSize = sec[i].Misc.VirtualSize;
            break;
        }
    }
    if (!textBase || textSize < 32) {
        return;
    }

    // pre_sim_call_aob: 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 44 38 35 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8
    static constexpr uint8_t kPat[] = {
        0x48, 0x8D, 0x0D, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0,
        0x44, 0x38, 0x35, 0, 0, 0, 0, 0x0F, 0x84, 0, 0, 0, 0,
        0x48, 0x8D, 0x0D, 0, 0, 0, 0, 0xE8
    };
    static constexpr char kMask[] = "xxx????x????xxx????xx????xxx????x";
    constexpr size_t kLen = sizeof(kPat);

    for (size_t i = 0; i + kLen <= textSize; ++i) {
        bool match = true;
        for (size_t j = 0; j < kLen; ++j) {
            if (kMask[j] == 'x' && textBase[i + j] != kPat[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            int32_t disp = 0;
            std::memcpy(&disp, textBase + i + 15, sizeof(disp));
            g_inGameFlagPtr = textBase + i + 19 + disp;
            return;
        }
    }
}

bool isGameRunning() noexcept
{
    if (!g_inGameFlagPtr) {
        return true; // Fallback if AOB not matched on an unknown build
    }
    return (*g_inGameFlagPtr) != 0;
}

ID3D11Device*           g_device      = nullptr;
ID3D11DeviceContext*    g_ctx         = nullptr;
ID3D11RenderTargetView* g_rtv         = nullptr;
HWND                    g_hwnd        = nullptr;
WNDPROC                 g_origWndProc = nullptr;

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

PFN_Present          g_origPresent               = nullptr;
PFN_ResizeBuffers    g_origResizeBuffers         = nullptr;
PFN_DiGetDeviceState g_origDiGetDeviceState      = nullptr;
PFN_C3dMouseBool     g_origGetMouseLeftPress     = nullptr;
PFN_C3dMouseBool     g_origGetMouseLeftPressOff  = nullptr;
PFN_C3dMouseBool     g_origGetMouseLeftDown      = nullptr;
PFN_C3dMouseBool     g_origGetMouseRightPress    = nullptr;
PFN_C3dMouseBool     g_origGetMouseRightPressOff = nullptr;
PFN_C3dMouseBool     g_origGetMouseRightDown     = nullptr;
PFN_C3dMouseBool     g_origGetMouseMiddlePress   = nullptr;
PFN_C3dMouseBool     g_origGetMouseMiddlePressOff= nullptr;
PFN_C3dMouseBool     g_origGetMouseMiddleDown    = nullptr;
PFN_C3dMouseRel      g_origGetMouseRelative      = nullptr;
PFN_C3dKeyBool       g_origGetKeyDown            = nullptr;
PFN_C3dKeyBool       g_origGetKeyPress           = nullptr;

void logDebug(const char* msg) noexcept
{
    OutputDebugStringA("[WRSR_ModOverlay] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

bool patchModuleIat(HMODULE hMod, const char* dllName, const char* funcName,
                    void* detour, void** outOriginal) noexcept
{
    if (!hMod || !dllName || !funcName || !detour) {
        return false;
    }

    auto* base = reinterpret_cast<uint8_t*>(hMod);
    auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        return false;
    }

    auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; imp->Name != 0; ++imp) {
        const char* modName = reinterpret_cast<const char*>(base + imp->Name);
        if (_stricmp(modName, dllName) != 0) {
            continue;
        }

        const DWORD oft = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        auto* thunkOrig = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + oft);
        auto* thunkIat  = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);

        for (; thunkOrig->u1.AddressOfData != 0; ++thunkOrig, ++thunkIat) {
            if (IMAGE_SNAP_BY_ORDINAL64(thunkOrig->u1.Ordinal)) {
                continue;
            }
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + thunkOrig->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), funcName) != 0) {
                continue;
            }

            DWORD oldProt = 0;
            if (!VirtualProtect(&thunkIat->u1.Function, sizeof(ULONG_PTR),
                                PAGE_READWRITE, &oldProt)) {
                return false;
            }
            if (outOriginal) {
                *outOriginal = reinterpret_cast<void*>(thunkIat->u1.Function);
            }
            thunkIat->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
            VirtualProtect(&thunkIat->u1.Function, sizeof(ULONG_PTR), oldProt, &oldProt);
            return true;
        }
    }
    return false;
}

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

bool __fastcall detourMouseLeftPress(void* self)     { return isMouseLockedForGame() ? false : g_origGetMouseLeftPress(self); }
bool __fastcall detourMouseLeftPressOff(void* self)  { return isMouseLockedForGame() ? false : g_origGetMouseLeftPressOff(self); }
bool __fastcall detourMouseLeftDown(void* self)      { return isMouseLockedForGame() ? false : g_origGetMouseLeftDown(self); }
bool __fastcall detourMouseRightPress(void* self)    { return isMouseLockedForGame() ? false : g_origGetMouseRightPress(self); }
bool __fastcall detourMouseRightPressOff(void* self) { return isMouseLockedForGame() ? false : g_origGetMouseRightPressOff(self); }
bool __fastcall detourMouseRightDown(void* self)     { return isMouseLockedForGame() ? false : g_origGetMouseRightDown(self); }
bool __fastcall detourMouseMiddlePress(void* self)   { return isMouseLockedForGame() ? false : g_origGetMouseMiddlePress(self); }
bool __fastcall detourMouseMiddlePressOff(void* self){ return isMouseLockedForGame() ? false : g_origGetMouseMiddlePressOff(self); }
bool __fastcall detourMouseMiddleDown(void* self)    { return isMouseLockedForGame() ? false : g_origGetMouseMiddleDown(self); }

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
        logDebug("dinput8.dll not loaded; skipping GetDeviceState guard");
        return;
    }

    auto pfnCreate = reinterpret_cast<PFN_DirectInput8Create>(
        GetProcAddress(hDi, "DirectInput8Create"));
    if (!pfnCreate) {
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
        return;
    }
    if (FAILED(di->CreateDevice(kGuidSysMouse, &dev, nullptr)) || !dev) {
        di->Release();
        return;
    }

    void** vtable = *reinterpret_cast<void***>(dev);
    void*  target = vtable[9];
    dev->Release();
    di->Release();

    if (MH_CreateHook(target, reinterpret_cast<void*>(detourDiGetDeviceState),
                      reinterpret_cast<void**>(&g_origDiGetDeviceState)) == MH_OK) {
        MH_EnableHook(target);
    }
}

void installC3dInputGuards()
{
    HMODULE hExe = GetModuleHandleW(nullptr);
    if (!hExe) {
        return;
    }

    struct IatEntry {
        const char* sym;
        void*       detour;
        void**      orig;
    };
    const IatEntry entries[] = {
        { "?GetMouseLeftPress@C3D_INPUT@@QEAA_NXZ",             reinterpret_cast<void*>(detourMouseLeftPress),      reinterpret_cast<void**>(&g_origGetMouseLeftPress) },
        { "?GetMouseLeftPressAOff@C3D_INPUT@@QEAA_NXZ",         reinterpret_cast<void*>(detourMouseLeftPressOff),   reinterpret_cast<void**>(&g_origGetMouseLeftPressOff) },
        { "?GetMouseLeftDown@C3D_INPUT@@QEAA_NXZ",              reinterpret_cast<void*>(detourMouseLeftDown),       reinterpret_cast<void**>(&g_origGetMouseLeftDown) },
        { "?GetMouseRightPress@C3D_INPUT@@QEAA_NXZ",            reinterpret_cast<void*>(detourMouseRightPress),     reinterpret_cast<void**>(&g_origGetMouseRightPress) },
        { "?GetMouseRightPressAOff@C3D_INPUT@@QEAA_NXZ",        reinterpret_cast<void*>(detourMouseRightPressOff),  reinterpret_cast<void**>(&g_origGetMouseRightPressOff) },
        { "?GetMouseRightDown@C3D_INPUT@@QEAA_NXZ",             reinterpret_cast<void*>(detourMouseRightDown),      reinterpret_cast<void**>(&g_origGetMouseRightDown) },
        { "?GetMouseMiddlePress@C3D_INPUT@@QEAA_NXZ",           reinterpret_cast<void*>(detourMouseMiddlePress),    reinterpret_cast<void**>(&g_origGetMouseMiddlePress) },
        { "?GetMouseMiddlePressAOff@C3D_INPUT@@QEAA_NXZ",       reinterpret_cast<void*>(detourMouseMiddlePressOff), reinterpret_cast<void**>(&g_origGetMouseMiddlePressOff) },
        { "?GetMouseMiddleDown@C3D_INPUT@@QEAA_NXZ",            reinterpret_cast<void*>(detourMouseMiddleDown),     reinterpret_cast<void**>(&g_origGetMouseMiddleDown) },
        { "?GetMouseRelative@C3D_INPUT@@QEAA?AVC3DVECTOR3@@XZ", reinterpret_cast<void*>(detourMouseRelative),       reinterpret_cast<void**>(&g_origGetMouseRelative) },
        { "?GetKeyDown@C3D_INPUT@@QEAA_NH@Z",                   reinterpret_cast<void*>(detourKeyDown),             reinterpret_cast<void**>(&g_origGetKeyDown) },
        { "?GetKeyPress@C3D_INPUT@@QEAA_NH@Z",                  reinterpret_cast<void*>(detourKeyPress),            reinterpret_cast<void**>(&g_origGetKeyPress) },
    };

    for (const auto& e : entries) {
        patchModuleIat(hExe, "C3DDLL64.dll", e.sym, e.detour, e.orig);
    }
}

void applyWrsrTheme()
{
    ImGuiStyle& style       = ImGui::GetStyle();
    style.WindowPadding     = ImVec2(12.f, 10.f);
    style.FramePadding      = ImVec2(8.f, 4.f);
    style.ItemSpacing       = ImVec2(8.f, 6.f);
    style.WindowRounding    = 2.f;
    style.ChildRounding     = 2.f;
    style.FrameRounding     = 2.f;
    style.TabRounding       = 2.f;
    style.ScrollbarRounding = 2.f;
    style.WindowBorderSize  = 1.f;
    style.FrameBorderSize   = 1.f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.91f, 0.88f, 0.80f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.55f, 0.52f, 0.45f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.12f, 0.12f, 0.10f, 0.96f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.14f, 0.13f, 0.11f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.14f, 0.13f, 0.11f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.32f, 0.29f, 0.24f, 1.00f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.40f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.16f, 0.15f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.27f, 0.22f, 0.14f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.14f, 0.13f, 0.11f, 0.90f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.09f, 0.09f, 0.07f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.20f, 0.18f, 0.13f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.28f, 0.23f, 0.15f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.24f, 0.21f, 0.16f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.42f, 0.34f, 0.20f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.58f, 0.45f, 0.22f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.28f, 0.24f, 0.16f, 0.90f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.42f, 0.34f, 0.20f, 0.95f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.55f, 0.43f, 0.22f, 1.00f);
    c[ImGuiCol_Tab]                  = ImVec4(0.18f, 0.16f, 0.12f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.42f, 0.34f, 0.20f, 1.00f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.32f, 0.26f, 0.16f, 1.00f);
    c[ImGuiCol_TabDimmed]            = ImVec4(0.15f, 0.14f, 0.11f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.25f, 0.21f, 0.14f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.68f, 0.52f, 0.22f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.86f, 0.67f, 0.29f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.84f, 0.65f, 0.27f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.35f, 0.31f, 0.24f, 1.00f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.45f, 0.37f, 0.22f, 0.60f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.75f, 0.58f, 0.26f, 0.85f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.85f, 0.66f, 0.28f, 1.00f);
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
    if (g_device) {
        g_device->CreateRenderTargetView(backBuf, nullptr, &g_rtv);
    }
    backBuf->Release();
}

void toggleOverlay(bool show) noexcept
{
    g_visible.store(show, std::memory_order_relaxed);
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
    if (msg == WM_KEYDOWN && wp == VK_F9 && ((lp & (1LL << 30)) == 0)) {
        toggleOverlay(!g_visible.load(std::memory_order_relaxed));
        return 0;
    }

    if (g_visible.load(std::memory_order_relaxed)) {
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

ImVec4 unpackRgba(uint32_t rgba) noexcept
{
    const float r = static_cast<float>((rgba >> 24) & 0xFF) / 255.f;
    const float g = static_cast<float>((rgba >> 16) & 0xFF) / 255.f;
    const float b = static_cast<float>((rgba >>  8) & 0xFF) / 255.f;
    const float a = static_cast<float>((rgba >>  0) & 0xFF) / 255.f;
    return ImVec4(r, g, b, a);
}

void drawWidget(const StoredWidget& w, size_t idx)
{
    ImGui::PushID(static_cast<int>(idx));

    switch (w.kind) {
    case WidgetKind::FloatSlider: {
        if (w.floatPtr) {
            const char* fmt = w.format.empty() ? "%.2f" : w.format.c_str();
            if (ImGui::SliderFloat(w.label.c_str(), w.floatPtr, w.minVal, w.maxVal, fmt,
                                   ImGuiSliderFlags_AlwaysClamp)) {
                if (w.onChange) {
                    w.onChange(w.userData);
                }
            }
            if (!w.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", w.tooltip.c_str());
            }
        }
        break;
    }
    case WidgetKind::Checkbox: {
        if (w.boolPtr) {
            bool checked = (*w.boolPtr != 0);
            if (ImGui::Checkbox(w.label.c_str(), &checked)) {
                *w.boolPtr = checked ? 1u : 0u;
                if (w.onChange) {
                    w.onChange(w.userData);
                }
            }
            if (!w.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", w.tooltip.c_str());
            }
        }
        break;
    }
    case WidgetKind::Button: {
        if (w.sameLine) {
            ImGui::SameLine();
        }
        if (ImGui::Button(w.label.c_str())) {
            if (w.onClick) {
                w.onClick(w.userData);
            }
        }
        if (!w.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", w.tooltip.c_str());
        }
        break;
    }
    case WidgetKind::Label: {
        if (w.separatorBefore) {
            ImGui::Separator();
        }
        const char* txt = w.persistentText ? w.persistentText : "";
        if (w.rgbaColor != 0) {
            ImGui::TextColored(unpackRgba(w.rgbaColor), "%s", txt);
        } else {
            ImGui::TextWrapped("%s", txt);
        }
        break;
    }
    case WidgetKind::CollapsibleLabel: {
        if (w.separatorBefore) {
            ImGui::Separator();
        }
        const ImGuiTreeNodeFlags flags = w.defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
        if (ImGui::CollapsingHeader(w.label.c_str(), flags)) {
            const char* txt = w.persistentText ? w.persistentText : "";
            if (w.rgbaColor != 0) {
                ImGui::TextColored(unpackRgba(w.rgbaColor), "%s", txt);
            } else {
                ImGui::TextWrapped("%s", txt);
            }
        }
        break;
    }
    }

    ImGui::PopID();
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

    ImGuiIO& io        = ImGui::GetIO();
    io.MouseDrawCursor = isMouseLockedForGame();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowSize(ImVec2(540.f, 360.f), ImGuiCond_FirstUseEver);
    bool open = true;
    const bool windowOpen = ImGui::Begin("WRSR Tweaks [F9]###WRSR_Tweaks_Overlay",
                                         &open, ImGuiWindowFlags_NoCollapse);
    {
        const ImVec2 pos  = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        g_rectL.store(static_cast<int32_t>(pos.x), std::memory_order_relaxed);
        g_rectT.store(static_cast<int32_t>(pos.y), std::memory_order_relaxed);
        g_rectR.store(static_cast<int32_t>(pos.x + size.x), std::memory_order_relaxed);
        g_rectB.store(static_cast<int32_t>(pos.y + size.y), std::memory_order_relaxed);
        g_rectValid.store(true, std::memory_order_release);
    }

    if (windowOpen) {
        std::lock_guard lock(g_tabsMutex);
        const bool gameRunning = isGameRunning();
        size_t     visibleTabs = 0;
        size_t     hiddenTabs  = 0;
        for (const auto& tab : g_tabs) {
            if (gameRunning || tab.mainMenuAvailable != 0) {
                ++visibleTabs;
            } else {
                ++hiddenTabs;
            }
        }

        if (g_tabs.empty()) {
            ImGui::TextDisabled("No mod tabs registered yet.");
        } else if (visibleTabs == 0) {
            ImGui::TextDisabled("Waiting for a republic to load (no main-menu tabs registered).");
        } else if (ImGui::BeginTabBar("##WRSR_ModTabs", ImGuiTabBarFlags_None)) {
            for (const auto& tab : g_tabs) {
                if (!gameRunning && tab.mainMenuAvailable == 0) {
                    continue;
                }
                std::string tabLabel = tab.title + "###" + tab.id;
                if (ImGui::BeginTabItem(tabLabel.c_str())) {
                    ImGui::Spacing();
                    if (tab.onFrame) {
                        tab.onFrame(tab.frameUserData);
                    }
                    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && tab.onSave) {
                        tab.onSave(tab.userData);
                    }
                    for (size_t i = 0; i < tab.widgets.size(); ++i) {
                        drawWidget(tab.widgets[i], i);
                    }
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        if (hiddenTabs > 0) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled("%zu in-game tweak tab(s) hidden on Main Menu — load a republic to access them.",
                                hiddenTabs);
        }
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
    if (g_rtv && g_ctx) {
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

HRESULT WINAPI hookedPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags)
{
    if (!g_dxInitialized.exchange(true)) {
        sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device));
        g_device->GetImmediateContext(&g_ctx);

        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;

        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hookedWndProc)));

        installDirectInputGuard();
        installC3dInputGuards();
        initInGameFlagProbe();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io    = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        applyWrsrTheme();
        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX11_Init(g_device, g_ctx);

        createRenderTarget(sc);
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

bool installDx11Hook()
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.lpszClassName = L"WRSR_ModOverlay_Probe";
    wc.hInstance     = GetModuleHandleW(nullptr);
    RegisterClassExW(&wc);

    HWND hwndProbe = CreateWindowExW(0, L"WRSR_ModOverlay_Probe", L"",
                                     WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                     nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwndProbe) {
        UnregisterClassW(L"WRSR_ModOverlay_Probe", wc.hInstance);
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

    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &probeSwapChain, &probeDevice, &fl, &probeCtx);

    if (FAILED(hr) || !probeSwapChain) {
        DestroyWindow(hwndProbe);
        UnregisterClassW(L"WRSR_ModOverlay_Probe", wc.hInstance);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(probeSwapChain);

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        probeSwapChain->Release();
        probeDevice->Release();
        probeCtx->Release();
        DestroyWindow(hwndProbe);
        UnregisterClassW(L"WRSR_ModOverlay_Probe", wc.hInstance);
        return false;
    }

    const bool ok =
        MH_CreateHook(vtable[8], reinterpret_cast<void*>(hookedPresent),
                      reinterpret_cast<void**>(&g_origPresent)) == MH_OK &&
        MH_CreateHook(vtable[13], reinterpret_cast<void*>(hookedResizeBuffers),
                      reinterpret_cast<void**>(&g_origResizeBuffers)) == MH_OK &&
        MH_EnableHook(vtable[8]) == MH_OK &&
        MH_EnableHook(vtable[13]) == MH_OK;

    probeSwapChain->Release();
    probeDevice->Release();
    probeCtx->Release();
    DestroyWindow(hwndProbe);
    UnregisterClassW(L"WRSR_ModOverlay_Probe", wc.hInstance);
    return ok;
}

// ── Host implementation of pure-C ABI callbacks ──────────────────────────────
int32_t WRSR_OVERLAY_CALL hostRegisterTab(const ModTabDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(ModTabDesc) || !desc->tab_id || !desc->title) {
        return -1;
    }
    std::lock_guard lock(g_tabsMutex);
    for (size_t i = 0; i < g_tabs.size(); ++i) {
        if (g_tabs[i].id == desc->tab_id) {
            return static_cast<int32_t>(i);
        }
    }
    StoredTab tab;
    tab.id       = desc->tab_id;
    tab.title    = desc->title;
    tab.onSave   = desc->on_save;
    tab.userData = desc->user_data;
    g_tabs.push_back(std::move(tab));
    return static_cast<int32_t>(g_tabs.size() - 1);
}

int32_t WRSR_OVERLAY_CALL hostAddFloatSlider(int32_t tabId, const ModFloatSliderDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(ModFloatSliderDesc) || !desc->label || !desc->value) {
        return 0;
    }
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) {
        return 0;
    }
    StoredWidget w;
    w.kind     = WidgetKind::FloatSlider;
    w.label    = desc->label;
    w.format   = desc->format ? desc->format : "%.2f";
    w.tooltip  = desc->tooltip ? desc->tooltip : "";
    w.floatPtr = desc->value;
    w.minVal   = desc->min_value;
    w.maxVal   = desc->max_value;
    w.onChange = desc->on_change;
    w.userData = desc->user_data;
    g_tabs[static_cast<size_t>(tabId)].widgets.push_back(std::move(w));
    return 1;
}

int32_t WRSR_OVERLAY_CALL hostAddCheckbox(int32_t tabId, const ModCheckboxDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(ModCheckboxDesc) || !desc->label || !desc->value) {
        return 0;
    }
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) {
        return 0;
    }
    StoredWidget w;
    w.kind     = WidgetKind::Checkbox;
    w.label    = desc->label;
    w.tooltip  = desc->tooltip ? desc->tooltip : "";
    w.boolPtr  = desc->value;
    w.onChange = desc->on_change;
    w.userData = desc->user_data;
    g_tabs[static_cast<size_t>(tabId)].widgets.push_back(std::move(w));
    return 1;
}

int32_t WRSR_OVERLAY_CALL hostAddButton(int32_t tabId, const ModButtonDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(ModButtonDesc) || !desc->label) {
        return 0;
    }
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) {
        return 0;
    }
    StoredWidget w;
    w.kind     = WidgetKind::Button;
    w.label    = desc->label;
    w.sameLine = desc->same_line;
    w.tooltip  = desc->tooltip ? desc->tooltip : "";
    w.onClick  = desc->on_click;
    w.userData = desc->user_data;
    g_tabs[static_cast<size_t>(tabId)].widgets.push_back(std::move(w));
    return 1;
}

int32_t WRSR_OVERLAY_CALL hostAddLabel(int32_t tabId, const ModLabelDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(ModLabelDesc) || !desc->text) {
        return 0;
    }
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) {
        return 0;
    }
    StoredWidget w;
    w.kind            = WidgetKind::Label;
    w.persistentText  = desc->text;
    w.separatorBefore = desc->separator_before;
    w.rgbaColor       = desc->rgba_color;
    g_tabs[static_cast<size_t>(tabId)].widgets.push_back(std::move(w));
    return 1;
}

int32_t WRSR_OVERLAY_CALL hostAddCollapsibleLabel(int32_t tabId, const ModCollapsibleLabelDesc* desc)
{
    if (!desc || desc->struct_size < sizeof(ModCollapsibleLabelDesc) || !desc->header || !desc->text) {
        return 0;
    }
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) {
        return 0;
    }
    StoredWidget w;
    w.kind            = WidgetKind::CollapsibleLabel;
    w.label           = desc->header;
    w.persistentText  = desc->text;
    w.defaultOpen     = desc->default_open;
    w.separatorBefore = desc->separator_before;
    w.rgbaColor       = desc->rgba_color;
    g_tabs[static_cast<size_t>(tabId)].widgets.push_back(std::move(w));
    return 1;
}

int32_t WRSR_OVERLAY_CALL hostSetTabFrameCallback(int32_t tabId, ModOverlayButtonFn onFrame, void* userData)
{
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) {
        return 0;
    }
    g_tabs[static_cast<size_t>(tabId)].onFrame       = onFrame;
    g_tabs[static_cast<size_t>(tabId)].frameUserData = userData;
    return 1;
}

int32_t WRSR_OVERLAY_CALL hostSetTabMainMenuAvailable(int32_t tabId, uint8_t available)
{
    std::lock_guard lock(g_tabsMutex);
    if (tabId < 0 || static_cast<size_t>(tabId) >= g_tabs.size()) {
        return 0;
    }
    g_tabs[static_cast<size_t>(tabId)].mainMenuAvailable = available ? 1u : 0u;
    return 1;
}

int32_t WRSR_OVERLAY_CALL hostIsVisible(void)
{
    return g_visible.load(std::memory_order_relaxed) ? 1 : 0;
}

void WRSR_OVERLAY_CALL hostSetVisible(int32_t visible)
{
    toggleOverlay(visible != 0);
}

bool ensureHubConnected()
{
    std::lock_guard lock(g_ipcMutex);
    if (g_hub && g_hub->magic == kHubMagic && g_hub->ownerInitialized == 1) {
        return true;
    }

    const DWORD pid = GetCurrentProcessId();
    wchar_t mutexName[96]{};
    wchar_t mapName[96]{};
    std::swprintf(mutexName, sizeof(mutexName) / sizeof(mutexName[0]),
                  L"Local\\WRSR_ModOverlay_Mutex_%lu", pid);
    std::swprintf(mapName, sizeof(mapName) / sizeof(mapName[0]),
                  L"Local\\WRSR_ModOverlay_%lu", pid);

    HANDLE hSyncMutex = CreateMutexW(nullptr, FALSE, mutexName);
    if (hSyncMutex) {
        WaitForSingleObject(hSyncMutex, INFINITE);
    }

    bool ready = false;
    if (!g_mapHandle) {
        g_mapHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         0, sizeof(SharedOverlayHub), mapName);
    }
    if (g_mapHandle && !g_hub) {
        g_hub = static_cast<SharedOverlayHub*>(
            MapViewOfFile(g_mapHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedOverlayHub)));
    }

    if (g_hub) {
        if (g_hub->ownerInitialized == 1 &&
            g_hub->magic == kHubMagic &&
            g_hub->abiVersion == WRSR_MOD_OVERLAY_ABI_VERSION) {
            g_isHost = false;
            ready    = true;
        } else if (g_hub->ownerInitialized == 0) {
            if (installDx11Hook()) {
                HMODULE selfMod = nullptr;
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&ensureHubConnected),
                    &selfMod);

                g_hub->magic          = kHubMagic;
                g_hub->abiVersion     = WRSR_MOD_OVERLAY_ABI_VERSION;
                g_hub->structSize     = sizeof(SharedOverlayHub);
                g_hub->ownerPid       = pid;
                g_hub->ownerModule    = selfMod;
                g_hub->registerTab    = &hostRegisterTab;
                g_hub->addFloatSlider = &hostAddFloatSlider;
                g_hub->addCheckbox    = &hostAddCheckbox;
                g_hub->addButton      = &hostAddButton;
                g_hub->addLabel       = &hostAddLabel;
                g_hub->isVisible           = &hostIsVisible;
                g_hub->setVisible          = &hostSetVisible;
                g_hub->addCollapsibleLabel     = &hostAddCollapsibleLabel;
                g_hub->setTabFrameCallback     = &hostSetTabFrameCallback;
                g_hub->setTabMainMenuAvailable = &hostSetTabMainMenuAvailable;
                InterlockedExchange(&g_hub->ownerInitialized, 1);

                g_isHost = true;
                ready    = true;
                logDebug("Initialized process-local WRSR_ModOverlay host (F9)");
            } else {
                logDebug("installDx11Hook failed");
            }
        }
    }

    if (hSyncMutex) {
        ReleaseMutex(hSyncMutex);
        CloseHandle(hSyncMutex);
    }
    return ready;
}

} // namespace

GRML_EXPORT void GRML_CALL GrmlPluginInit(const GrmlHostApi* host)
{
    if (!host || host->abi_version < 1 || host->struct_size < sizeof(GrmlHostApi) || !host->ui) {
        return;
    }
    initQpcFreq();
    g_grmlHost.store(host, std::memory_order_release);
    wrsr::game::setGrmlHost(host);
    if (host->Log) {
        host->Log(GRML_LOG_INFO, "OverlayAdapter", "GrmlPluginInit attached — docking F9 UI & telemetry into GRML");
    }
}

extern "C" {

int32_t WRSR_OVERLAY_CALL ModOverlay_IsHostedByGrml(void)
{
    return g_grmlHost.load(std::memory_order_acquire) != nullptr ? 1 : 0;
}

const GrmlHostApi* WRSR_OVERLAY_CALL ModOverlay_GetGrmlHost(void)
{
    return g_grmlHost.load(std::memory_order_acquire);
}

void WRSR_OVERLAY_CALL ModOverlay_RecordHookTicks(const char* hook_name, int64_t qpc_ticks)
{
    if (!hook_name || qpc_ticks < 0) return;
    for (size_t i = 0; i < kMaxProfileBuckets; ++i) {
        const char* cur = g_profileBuckets[i].hookName.load(std::memory_order_acquire);
        if (cur == hook_name) {
            g_profileBuckets[i].calls.fetch_add(1, std::memory_order_relaxed);
            g_profileBuckets[i].qpcTicks.fetch_add(qpc_ticks, std::memory_order_relaxed);
            return;
        }
        if (cur == nullptr) {
            const char* expected = nullptr;
            if (g_profileBuckets[i].hookName.compare_exchange_strong(expected, hook_name, std::memory_order_acq_rel) ||
                expected == hook_name) {
                g_profileBuckets[i].calls.fetch_add(1, std::memory_order_relaxed);
                g_profileBuckets[i].qpcTicks.fetch_add(qpc_ticks, std::memory_order_relaxed);
                return;
            }
        }
    }
}

void WRSR_OVERLAY_CALL ModOverlay_FlushHookTelemetry(void)
{
    const GrmlHostApi* host = g_grmlHost.load(std::memory_order_acquire);
    if (!host || !host->ui || !host->ui->ReportHookTiming) return;
    initQpcFreq();

    for (size_t i = 0; i < kMaxProfileBuckets; ++i) {
        const char* hook = g_profileBuckets[i].hookName.load(std::memory_order_acquire);
        if (!hook) break;
        const uint64_t calls = g_profileBuckets[i].calls.exchange(0, std::memory_order_relaxed);
        const int64_t  ticks = g_profileBuckets[i].qpcTicks.exchange(0, std::memory_order_relaxed);
        if (calls > 0) {
            const double ms = static_cast<double>(ticks) * g_qpcFreqMs;
            host->ui->ReportHookTiming(g_grmlPrimaryPluginId.c_str(), hook, calls, ms);
        }
    }
}

int32_t WRSR_OVERLAY_CALL ModOverlay_Initialize(void)
{
    if (g_grmlHost.load(std::memory_order_acquire) != nullptr) {
        return 1;
    }
    return ensureHubConnected() ? 1 : 0;
}

int32_t WRSR_OVERLAY_CALL ModOverlay_IsVisible(void)
{
    if (g_grmlHost.load(std::memory_order_acquire) != nullptr) {
        return 1;
    }
    if (!ensureHubConnected() || !g_hub || !g_hub->isVisible) {
        return 0;
    }
    return g_hub->isVisible();
}

void WRSR_OVERLAY_CALL ModOverlay_SetVisible(int32_t visible)
{
    if (g_grmlHost.load(std::memory_order_acquire) != nullptr) {
        return;
    }
    if (!ensureHubConnected() || !g_hub || !g_hub->setVisible) {
        return;
    }
    g_hub->setVisible(visible);
}

int32_t WRSR_OVERLAY_CALL RegisterModTab(const ModTabDesc* desc)
{
    if (const GrmlHostApi* host = g_grmlHost.load(std::memory_order_acquire)) {
        if (!desc || !desc->tab_id || !desc->title || !host->ui || !host->ui->RegisterTab) {
            return -1;
        }
        const int32_t localIdx = hostRegisterTab(desc);
        if (localIdx < 0) return -1;
        g_grmlPrimaryPluginId = desc->tab_id;
        host->ui->RegisterTab(desc->tab_id, desc->title, &grmlRenderTabTrampoline,
                              reinterpret_cast<void*>(static_cast<uintptr_t>(localIdx)));
        return localIdx;
    }
    if (!ensureHubConnected() || !g_hub || !g_hub->registerTab) {
        return -1;
    }
    return g_hub->registerTab(desc);
}

int32_t WRSR_OVERLAY_CALL AddFloatSlider(int32_t tab_id, const ModFloatSliderDesc* desc)
{
    if (g_grmlHost.load(std::memory_order_acquire) != nullptr) {
        return hostAddFloatSlider(tab_id, desc);
    }
    if (!ensureHubConnected() || !g_hub || !g_hub->addFloatSlider) {
        return 0;
    }
    return g_hub->addFloatSlider(tab_id, desc);
}

int32_t WRSR_OVERLAY_CALL AddCheckbox(int32_t tab_id, const ModCheckboxDesc* desc)
{
    if (g_grmlHost.load(std::memory_order_acquire) != nullptr) {
        return hostAddCheckbox(tab_id, desc);
    }
    if (!ensureHubConnected() || !g_hub || !g_hub->addCheckbox) {
        return 0;
    }
    return g_hub->addCheckbox(tab_id, desc);
}

int32_t WRSR_OVERLAY_CALL AddButton(int32_t tab_id, const ModButtonDesc* desc)
{
    if (g_grmlHost.load(std::memory_order_acquire) != nullptr) {
        return hostAddButton(tab_id, desc);
    }
    if (!ensureHubConnected() || !g_hub || !g_hub->addButton) {
        return 0;
    }
    return g_hub->addButton(tab_id, desc);
}

int32_t WRSR_OVERLAY_CALL AddLabel(int32_t tab_id, const ModLabelDesc* desc)
{
    if (g_grmlHost.load(std::memory_order_acquire) != nullptr) {
        return hostAddLabel(tab_id, desc);
    }
    if (!ensureHubConnected() || !g_hub || !g_hub->addLabel) {
        return 0;
    }
    return g_hub->addLabel(tab_id, desc);
}

int32_t WRSR_OVERLAY_CALL AddCollapsibleLabel(int32_t tab_id, const ModCollapsibleLabelDesc* desc)
{
    if (g_grmlHost.load(std::memory_order_acquire) != nullptr) {
        return hostAddCollapsibleLabel(tab_id, desc);
    }
    if (!ensureHubConnected() || !g_hub) {
        return 0;
    }
    if (g_hub->structSize >= sizeof(SharedOverlayHub) && g_hub->addCollapsibleLabel) {
        return g_hub->addCollapsibleLabel(tab_id, desc);
    }
    if (desc && desc->text && g_hub->addLabel) {
        ModLabelDesc fallback{};
        fallback.struct_size      = sizeof(ModLabelDesc);
        fallback.text             = desc->text;
        fallback.separator_before = desc->separator_before;
        fallback.rgba_color       = desc->rgba_color;
        return g_hub->addLabel(tab_id, &fallback);
    }
    return 0;
}

int32_t WRSR_OVERLAY_CALL SetTabFrameCallback(int32_t tab_id, ModOverlayButtonFn on_frame, void* user_data)
{
    if (g_grmlHost.load(std::memory_order_acquire) != nullptr) {
        return hostSetTabFrameCallback(tab_id, on_frame, user_data);
    }
    if (!ensureHubConnected() || !g_hub) {
        return 0;
    }
    if (g_hub->structSize >= sizeof(SharedOverlayHub) && g_hub->setTabFrameCallback) {
        return g_hub->setTabFrameCallback(tab_id, on_frame, user_data);
    }
    return 0;
}

int32_t WRSR_OVERLAY_CALL SetTabMainMenuAvailable(int32_t tab_id, uint8_t available)
{
    if (const GrmlHostApi* host = g_grmlHost.load(std::memory_order_acquire)) {
        hostSetTabMainMenuAvailable(tab_id, available);
        if (host->ui && host->ui->SetTabMainMenuAvailable) {
            host->ui->SetTabMainMenuAvailable(tab_id, available);
        }
        return 1;
    }
    if (!ensureHubConnected() || !g_hub) {
        return 0;
    }
    if (g_hub->structSize >= sizeof(SharedOverlayHub) && g_hub->setTabMainMenuAvailable) {
        return g_hub->setTabMainMenuAvailable(tab_id, available);
    }
    return 0;
}

} // extern "C"
