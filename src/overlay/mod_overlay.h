#pragma once
// ============================================================================
// mod_overlay.h — Dual-mode F9 multi-mod ImGui overlay (GRML + Standalone)
// ============================================================================
// When loaded by Glorious Republic Mod Loader (GRML), GrmlPluginInit() docks
// all tabs, WRSR_PROFILE_HOOK telemetry, and save-lifecycle callbacks directly
// into GRML's unified F9 Mod Settings hub (zero duplicate DX11/CreateFileW hooks).
//
// When loaded under legacy RML, TesmioLoader, or raw injection, falls back
// transparently to the process-local Win32 mapping (Local\WRSR_ModOverlay_<PID>).
// ============================================================================

#include <stdint.h>

#include "grml_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WRSR_MOD_OVERLAY_ABI_VERSION 1u

#if defined(_WIN32)
#define WRSR_OVERLAY_CALL __cdecl
#else
#define WRSR_OVERLAY_CALL
#endif

typedef void (WRSR_OVERLAY_CALL* ModOverlaySaveFn)(void* user_data);
typedef void (WRSR_OVERLAY_CALL* ModOverlayButtonFn)(void* user_data);
typedef void (WRSR_OVERLAY_CALL* ModOverlayChangeFn)(void* user_data);

typedef struct ModTabDesc {
    uint32_t         struct_size;
    const char*      tab_id;    /* Unique stable tab identifier (copied by host) */
    const char*      title;     /* Display title in tab bar (copied by host)     */
    ModOverlaySaveFn on_save;   /* Optional save callback (e.g. Ctrl+S)          */
    void*            user_data; /* Opaque pointer passed to on_save              */
} ModTabDesc;

typedef struct ModFloatSliderDesc {
    uint32_t           struct_size;
    const char*        label;     /* Widget label (copied by host)               */
    float*             value;     /* Caller-owned float pointer                  */
    float              min_value;
    float              max_value;
    const char*        format;    /* Printf format, e.g. "%.2fx" (NULL -> "%.2f") */
    const char*        tooltip;   /* Optional hover tooltip (NULL for none)      */
    ModOverlayChangeFn on_change; /* Optional callback invoked when value changes */
    void*              user_data;
} ModFloatSliderDesc;

typedef struct ModCheckboxDesc {
    uint32_t           struct_size;
    const char*        label;     /* Widget label (copied by host)               */
    uint8_t*           value;     /* Caller-owned 1-byte boolean (0 or 1)        */
    const char*        tooltip;   /* Optional hover tooltip (NULL for none)      */
    ModOverlayChangeFn on_change; /* Optional callback invoked when toggled      */
    void*              user_data;
} ModCheckboxDesc;

typedef struct ModButtonDesc {
    uint32_t           struct_size;
    const char*        label;     /* Button text (copied by host)                */
    uint8_t            same_line; /* Non-zero places button on same row          */
    const char*        tooltip;   /* Optional hover tooltip (NULL for none)      */
    ModOverlayButtonFn on_click;  /* Callback invoked when clicked               */
    void*              user_data;
} ModButtonDesc;

typedef struct ModLabelDesc {
    uint32_t    struct_size;
    const char* text;             /* Persistent UTF-8 string/buffer (process lifetime) */
    uint8_t     separator_before; /* Non-zero draws ImGui::Separator() first           */
    uint32_t    rgba_color;       /* 0 = default theme color, else 0xRRGGBBAA          */
} ModLabelDesc;

typedef struct ModCollapsibleLabelDesc {
    uint32_t    struct_size;
    const char* header;           /* CollapsingHeader title (copied by host)           */
    const char* text;             /* Persistent UTF-8 string/buffer (process lifetime) */
    uint8_t     default_open;     /* Non-zero opens header by default                  */
    uint8_t     separator_before; /* Non-zero draws ImGui::Separator() first           */
    uint32_t    rgba_color;       /* 0 = default theme color, else 0xRRGGBBAA          */
} ModCollapsibleLabelDesc;

/* Connects to GRML (if active) or the process-local standalone overlay hub. */
int32_t WRSR_OVERLAY_CALL ModOverlay_Initialize(void);

/* Visibility helpers (returns 1 if visible, 0 if hidden). */
int32_t WRSR_OVERLAY_CALL ModOverlay_IsVisible(void);
void    WRSR_OVERLAY_CALL ModOverlay_SetVisible(int32_t visible);

/* Registers a new mod tab and returns a non-negative tab_id, or -1 on error. */
int32_t WRSR_OVERLAY_CALL RegisterModTab(const ModTabDesc* desc);

/* Adds POD-bound widgets to an existing tab (returns 1 on success, 0 on error). */
int32_t WRSR_OVERLAY_CALL AddFloatSlider(int32_t tab_id, const ModFloatSliderDesc* desc);
int32_t WRSR_OVERLAY_CALL AddCheckbox(int32_t tab_id, const ModCheckboxDesc* desc);
int32_t WRSR_OVERLAY_CALL AddButton(int32_t tab_id, const ModButtonDesc* desc);
int32_t WRSR_OVERLAY_CALL AddLabel(int32_t tab_id, const ModLabelDesc* desc);
int32_t WRSR_OVERLAY_CALL AddCollapsibleLabel(int32_t tab_id, const ModCollapsibleLabelDesc* desc);
int32_t WRSR_OVERLAY_CALL SetTabFrameCallback(int32_t tab_id, ModOverlayButtonFn on_frame, void* user_data);
int32_t WRSR_OVERLAY_CALL SetTabMainMenuAvailable(int32_t tab_id, uint8_t available);

/* GRML host access & lock-free telemetry helpers */
int32_t            WRSR_OVERLAY_CALL ModOverlay_IsHostedByGrml(void);
const GrmlHostApi* WRSR_OVERLAY_CALL ModOverlay_GetGrmlHost(void);
void               WRSR_OVERLAY_CALL ModOverlay_WaitForHostReady(uint32_t fallback_sleep_ms);
void               WRSR_OVERLAY_CALL ModOverlay_RecordHookTicks(const char* hook_name, int64_t qpc_ticks);
void               WRSR_OVERLAY_CALL ModOverlay_FlushHookTelemetry(void);

#ifdef __cplusplus
}

#ifndef WRSR_PROFILE_HOOK
#include <windows.h>
namespace wrsr::detail {
struct GrmlHookProfileScope {
    const char*   name;
    LARGE_INTEGER start{};
    explicit GrmlHookProfileScope(const char* hookName) noexcept : name(hookName)
    {
        if (ModOverlay_IsHostedByGrml()) {
            QueryPerformanceCounter(&start);
        } else {
            start.QuadPart = 0;
        }
    }
    ~GrmlHookProfileScope() noexcept
    {
        if (start.QuadPart != 0) {
            LARGE_INTEGER end{};
            QueryPerformanceCounter(&end);
            ModOverlay_RecordHookTicks(name, end.QuadPart - start.QuadPart);
        }
    }
    GrmlHookProfileScope(const GrmlHookProfileScope&)            = delete;
    GrmlHookProfileScope& operator=(const GrmlHookProfileScope&) = delete;
};
} // namespace wrsr::detail
#define WRSR_PROFILE_CONCAT_INNER(a, b) a##b
#define WRSR_PROFILE_CONCAT(a, b) WRSR_PROFILE_CONCAT_INNER(a, b)
#define WRSR_PROFILE_HOOK(name) ::wrsr::detail::GrmlHookProfileScope WRSR_PROFILE_CONCAT(_wrsrProf_, __LINE__)(name)
#endif
#else
#ifndef WRSR_PROFILE_HOOK
#define WRSR_PROFILE_HOOK(name) ((void)0)
#endif
#endif
