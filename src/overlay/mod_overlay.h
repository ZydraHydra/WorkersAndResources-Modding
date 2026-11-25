#pragma once
// ============================================================================
// mod_overlay.h — Self-contained F9 multi-mod ImGui overlay (Pure-C ABI)
// ============================================================================
// Zero mod-specific dependencies. Any plugin DLL that compiles mod_overlay.cpp
// shares a single DX11 Present/ResizeBuffers + WndProc hook in SOVIET64.exe via
// a process-local Win32 named file mapping (Local\WRSR_ModOverlay_<PID>).
//
// Whichever mod initializes first owns the DX11/ImGui context and draws the
// tabbed "WRSR Tweaks [F9]" window. Subsequent mods discover the hub and
// register their own tabs + POD widget bindings across the C ABI boundary
// without ever passing ImGuiContext* or C++ STL types across DLLs.
// ============================================================================

#include <stdint.h>

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

/* Connects to (or creates & becomes host for) the process-local overlay hub. */
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

#ifdef __cplusplus
}
#endif
