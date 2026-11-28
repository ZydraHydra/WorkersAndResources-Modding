#ifndef GRML_API_H
#define GRML_API_H

/* ============================================================================
 * grml_api.h — Glorious Republic Mod Loader (GRML) Pure-C Plugin ABI (v2)
 * ============================================================================
 * Canonical ABI header owned by GloriousRepublicModLoader (include/grml_api.h).
 * Downstream mod repositories mirror this file byte-for-byte.
 *
 * STRICT ABI RULES (/MT Static CRT Boundary Safety):
 *   1. Pure C only (extern "C"). Zero C++ STL types, templates, or exceptions.
 *   2. Zero ImGuiContext* pointers or cross-DLL heap allocations/frees.
 *   3. Every ABI struct starts with `abi_version` and `struct_size`.
 *   4. Append-only evolution: never reorder or remove existing fields.
 * ============================================================================ */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRML_API_VERSION 2u

#if defined(_WIN32)
  #define GRML_CALL __cdecl
  #ifdef __cplusplus
    #define GRML_EXPORT extern "C" __declspec(dllexport)
  #else
    #define GRML_EXPORT __declspec(dllexport)
  #endif
#else
  #define GRML_CALL
  #define GRML_EXPORT
#endif

/* Log severity levels for GrmlHostApi::Log */
typedef enum GrmlLogLevel {
    GRML_LOG_TRACE = 0,
    GRML_LOG_INFO  = 1,
    GRML_LOG_WARN  = 2,
    GRML_LOG_ERROR = 3
} GrmlLogLevel;

/* Field value types for zero-code declarative .ini UI tabs */
typedef enum GrmlIniFieldType {
    GRML_INI_BOOL  = 0, /* uint8_t (0 or 1) */
    GRML_INI_INT   = 1, /* int32_t          */
    GRML_INI_FLOAT = 2  /* float            */
} GrmlIniFieldType;

typedef struct GrmlUiApi GrmlUiApi;
typedef struct GrmlHookApi GrmlHookApi;
typedef struct GrmlWrsrApi GrmlWrsrApi;
typedef struct GrmlHostApi GrmlHostApi;

/* Table flags for GrmlUiApi::BeginTable */
#define GRML_TABLE_BORDERS      (1u << 0)
#define GRML_TABLE_ROW_BG       (1u << 1)
#define GRML_TABLE_RESIZABLE    (1u << 2)
#define GRML_TABLE_SCROLL_Y     (1u << 3)
#define GRML_TABLE_SIZING_FIXED (1u << 4)

/* Callback invoked each frame while a plugin's F9 Mod Settings tab is active.
 * Always runs inside GRML's PushID + BeginChild + SEH fault-isolation sandbox. */
typedef void (GRML_CALL* GrmlRenderTabFn)(const GrmlUiApi* ui, void* user_data);

/* Callback invoked on centralized save lifecycle events (buildings.bin load /
 * header.bin write / return to main menu). `save_dir` is wide UTF-16 or NULL
 * on main-menu unload. */
typedef void (GRML_CALL* GrmlSaveEventFn)(const wchar_t* save_dir, void* user_data);

/* Optional callback invoked once right after GRML finishes its single-pass
 * .text AOB scan across SOVIET64.exe (all RegisterPatternScan targets ready). */
typedef void (GRML_CALL* GrmlPostScanFn)(void* user_data);

/* Optional callback invoked after declarative .ini settings are loaded/applied. */
typedef void (GRML_CALL* GrmlIniAppliedFn)(void* user_data);

/* Callback invoked when a centralized hotkey registered via RegisterHotkey fires. */
typedef void (GRML_CALL* GrmlHotkeyFn)(void* user_data);

/* Optional exported entrypoint queried by GRML after LoadLibraryW:
 *   extern "C" __declspec(dllexport) void __cdecl GrmlPluginInit(const GrmlHostApi* host);
 */
typedef void (GRML_CALL* GrmlPluginInitFn)(const GrmlHostApi* host);

typedef struct GrmlIniFieldDesc {
    uint32_t         abi_version;   /* Set to GRML_API_VERSION                    */
    uint32_t         struct_size;   /* Set to sizeof(GrmlIniFieldDesc)            */
    const char*      section;       /* INI [section] name                         */
    const char*      key;           /* INI key name                               */
    const char*      label;         /* Display label in F9 Mod Settings           */
    const char*      tooltip;       /* Optional hover tooltip (NULL for none)     */
    GrmlIniFieldType type;          /* GRML_INI_BOOL / INT / FLOAT                */
    double           min_value;     /* Minimum bound (INT / FLOAT)                */
    double           max_value;     /* Maximum bound (INT / FLOAT)                */
    double           default_value; /* Fallback value if key absent from .ini     */
    const char*      format;        /* Optional printf format (e.g. "%.2fx")      */
    void*            live_ptr;      /* Optional uint8_t* / int32_t* / float*      */
} GrmlIniFieldDesc;

typedef struct GrmlDeclarativeIniTabDesc {
    uint32_t                abi_version;         /* Set to GRML_API_VERSION                */
    uint32_t                struct_size;         /* Set to sizeof(GrmlDeclarativeIniTabDesc) */
    const char*             plugin_id;           /* Stable reverse-DNS plugin ID           */
    const char*             tab_title;           /* Tab label in F9 Mod Settings           */
    const char*             ini_filename;        /* UTF-8 filename/path (NULL -> auto)     */
    uint8_t                 main_menu_available; /* Non-zero shows tab on main menu        */
    const GrmlIniFieldDesc* fields;              /* Array of field descriptors             */
    uint32_t                field_count;         /* Number of entries in `fields`          */
    GrmlIniAppliedFn        on_applied;          /* Optional callback after load/apply     */
    void*                   user_data;           /* Opaque context passed to `on_applied`  */
} GrmlDeclarativeIniTabDesc;

typedef struct GrmlGameInfo {
    uint32_t    abi_version;     /* Set to GRML_API_VERSION */
    uint32_t    struct_size;     /* Set to sizeof(GrmlGameInfo) */
    uintptr_t   image_base;      /* SOVIET64.exe base address */
    uint32_t    image_size;      /* SizeOfImage */
    uint32_t    pe_timestamp;    /* PE FileHeader.TimeDateStamp */
    const char* game_version;    /* Detected WRSR version string (e.g. "1.2.0.9") */
    const char* sha256;          /* Lowercase hex SHA-256 of SOVIET64.exe */
    const char* loader_version;  /* GRML runtime version string */
} GrmlGameInfo;

/* ============================================================================
 * GrmlUiApi (v1) — Unified F9 Mod Settings & Telemetry Interface
 * ============================================================================ */
struct GrmlUiApi {
    uint32_t abi_version;
    uint32_t struct_size;

    /* Registers a custom immediate-mode Mod Settings tab. Returns tab_id >= 0, or -1. */
    int32_t (GRML_CALL* RegisterTab)(
        const char*     plugin_id,
        const char*     tab_title,
        GrmlRenderTabFn render_cb,
        void*           user_data);

    /* Zero-code declarative .ini tab auto-rendered & persisted by GRML. */
    int32_t (GRML_CALL* RegisterDeclarativeIniTab)(
        const GrmlDeclarativeIniTabDesc* desc);

    /* Controls whether a registered tab appears on the main menu (1) or in-game only (0). */
    void (GRML_CALL* SetTabMainMenuAvailable)(int32_t tab_id, uint8_t available);

    /* Safe primitive widgets (valid ONLY inside GrmlRenderTabFn). */
    int32_t (GRML_CALL* Checkbox)(const char* label, uint8_t* value);
    int32_t (GRML_CALL* SliderFloat)(const char* label, float* value, float min_val, float max_val, const char* fmt);
    int32_t (GRML_CALL* SliderInt)(const char* label, int32_t* value, int32_t min_val, int32_t max_val, const char* fmt);
    int32_t (GRML_CALL* Button)(const char* label);
    void    (GRML_CALL* Text)(const char* fmt, ...);
    void    (GRML_CALL* TextColored)(uint32_t rgba, const char* fmt, ...); /* 0xRRGGBBAA */
    void    (GRML_CALL* TextWrapped)(const char* fmt, ...);
    void    (GRML_CALL* Separator)(void);
    void    (GRML_CALL* SameLine)(void);
    void    (GRML_CALL* BeginDisabled)(uint8_t disabled);
    void    (GRML_CALL* EndDisabled)(void);
    int32_t (GRML_CALL* CollapsingHeader)(const char* label, uint8_t default_open);
    void    (GRML_CALL* SetItemTooltip)(const char* fmt, ...);

    /* Reports per-frame hook timing/call counts (compatible with WRSR_PROFILE_HOOK). */
    void (GRML_CALL* ReportHookTiming)(
        const char* plugin_id,
        const char* hook_name,
        uint64_t    calls_delta,
        double      elapsed_ms);

    /* Extended immediate-mode widgets (v2+, valid ONLY inside GrmlRenderTabFn). */
    int32_t (GRML_CALL* InputFloat)(const char* label, float* value, float step, float step_fast, const char* fmt);
    int32_t (GRML_CALL* InputInt)(const char* label, int32_t* value, int32_t step, int32_t step_fast);
    int32_t (GRML_CALL* InputText)(const char* label, char* buf, uint32_t buf_size);
    int32_t (GRML_CALL* Combo)(const char* label, int32_t* current_item, const char* const* items, int32_t item_count);
    int32_t (GRML_CALL* BeginTable)(const char* id, int32_t columns, uint32_t flags);
    void    (GRML_CALL* EndTable)(void);
    void    (GRML_CALL* TableNextRow)(void);
    int32_t (GRML_CALL* TableNextColumn)(void);
    void    (GRML_CALL* TableSetupColumn)(const char* label, float width);
    void    (GRML_CALL* TableHeadersRow)(void);
    void    (GRML_CALL* PushItemWidth)(float width);
    void    (GRML_CALL* PopItemWidth)(void);
    void    (GRML_CALL* Indent)(float width);
    void    (GRML_CALL* Unindent)(float width);

    /* Standalone hotkeyed ImGui window hosted inside GRML's single DX11 Present loop.
     * Returns win_id >= 0 on success, or -1 on failure. */
    int32_t (GRML_CALL* RegisterWindow)(
        const char*     plugin_id,
        const char*     window_id,
        const char*     title,
        uint32_t        default_vk,
        GrmlRenderTabFn render_cb,
        void*           user_data);
    void    (GRML_CALL* SetWindowVisible)(int32_t win_id, uint8_t visible);
    int32_t (GRML_CALL* IsWindowVisible)(int32_t win_id);

    /* Centralized hotkey registration with conflict detection. Returns hotkey_id >= 0, or -1. */
    int32_t (GRML_CALL* RegisterHotkey)(
        const char*  plugin_id,
        const char*  action_id,
        const char*  label,
        uint32_t     default_vk,
        GrmlHotkeyFn on_trigger,
        void*        user_data);
};

/* ============================================================================
 * GrmlHookApi (v2) — Centralized MinHook Engine & Lifecycle Management
 * ============================================================================ */
struct GrmlHookApi {
    uint32_t abi_version;
    uint32_t struct_size;

    int32_t (GRML_CALL* CreateHook)(
        const char* plugin_id,
        const char* hook_name,
        void*       target,
        void*       detour,
        void**      out_original);

    int32_t (GRML_CALL* CreateApiHook)(
        const char*    plugin_id,
        const char*    hook_name,
        const wchar_t* module_name,
        const char*    proc_name,
        void*          detour,
        void**         out_original);

    int32_t (GRML_CALL* EnableHook)(void* target);
    int32_t (GRML_CALL* DisableHook)(void* target);
    int32_t (GRML_CALL* RemoveHook)(void* target);
};

/* ============================================================================
 * GrmlWrsrOffsets & GrmlWrsrApi (v2) — Shared WRSR Game State & Offsets
 * ============================================================================ */
typedef struct GrmlWrsrOffsets {
    uint32_t abi_version;
    uint32_t struct_size;
    uint8_t  exact_profile_match;
    /* ResourceRecord & ResourceVector */
    uint32_t res_stride;
    uint32_t res_min_header_size;
    uint32_t res_name_off;
    uint32_t res_kind_off;
    uint32_t res_buy_usd_off;
    uint32_t res_buy_rub_off;
    uint32_t res_sell_usd_off;
    uint32_t res_sell_rub_off;
    /* DistTask (0x80) & Building / BuildingDescriptor */
    uint32_t task_res_off;
    uint32_t task_src_bld_off;
    uint32_t task_dst_bld_off;
    uint32_t task_score_src_double_off;
    uint32_t task_score_src_flag1_off;
    uint32_t task_score_src_flag2_off;
    uint32_t task_score_dst_double_off;
    uint32_t task_score_dst_flag1_off;
    uint32_t task_score_dst_flag2_off;
    uint32_t bld_desc_off;
    uint32_t bld_faction_off;
    uint32_t desc_type_off;
    int32_t  customs_building_type;
    int32_t  faction_usd;
    int32_t  faction_rub;
} GrmlWrsrOffsets;

struct GrmlWrsrApi {
    uint32_t abi_version;
    uint32_t struct_size;

    /* Returns 1 if player is currently in-game (not on main menu), 0 otherwise. */
    int32_t (GRML_CALL* IsInGame)(void);

    /* Resolves RIP-relative target: instr + next_ip_off + *(int32_t*)(instr + disp_off). */
    void*   (GRML_CALL* ResolveRipTarget)(const void* instr, uint32_t disp_off, uint32_t next_ip_off);

    /* Queries a resolved semantic symbol (e.g. "wrsr.resource_vector", "wrsr.evaluate_dist_task"). */
    int32_t (GRML_CALL* GetSymbol)(const char* symbol_id, void** out_addr);

    /* Populates profile-resolved WRSR struct offsets (from grml_offsets.ini). */
    int32_t (GRML_CALL* GetOffsets)(GrmlWrsrOffsets* out_offsets);

    /* Returns SOVIET64.exe .text section base and size. */
    int32_t (GRML_CALL* GetTextSection)(const void** out_base, uint32_t* out_size);
};

/* ============================================================================
 * GrmlHostApi (v2) — Centralized Loader Services Passed to GrmlPluginInit
 * ============================================================================ */
struct GrmlHostApi {
    uint32_t abi_version;
    uint32_t struct_size;

    const GrmlUiApi* ui;

    /* Queues an AOB pattern ("48 8B ?? ??") for GRML's single-pass .text scan
     * over SOVIET64.exe before gameplay hooks activate. Returns 1 if queued. */
    int32_t (GRML_CALL* RegisterPatternScan)(const char* pattern, void** out_addr);

    /* Optional callback invoked once immediately after the single-pass AOB scan. */
    int32_t (GRML_CALL* RegisterPostScanCallback)(GrmlPostScanFn cb, void* user_data);

    /* Registers callbacks on GRML's centralized header.bin/buildings.bin save hook. */
    int32_t (GRML_CALL* RegisterSaveEvents)(
        GrmlSaveEventFn on_save_load,
        GrmlSaveEventFn on_save_unload,
        void*           user_data);

    /* Optional callback specifically when WRSR writes header.bin (save/autosave). */
    int32_t (GRML_CALL* RegisterSaveWriteEvent)(
        GrmlSaveEventFn on_save_write,
        void*           user_data);

    /* Writes a formatted message into GRML's unified ring-buffer + grml.log. */
    void (GRML_CALL* Log)(int32_t level, const char* plugin_id, const char* fmt, ...);

    /* Populates GrmlGameInfo with SOVIET64.exe metadata. Returns 1 on success. */
    int32_t (GRML_CALL* GetGameInfo)(GrmlGameInfo* out_info);

    /* v2 services */
    const GrmlHookApi* hooks;
    const GrmlWrsrApi* wrsr;

    /* Resolves %LOCALAPPDATA%\GRML\configs\<plugin_id>.ini (creating directories and
     * seeding from `default_ini_path` if non-NULL and target doesn't exist yet). */
    int32_t (GRML_CALL* GetPluginConfigPath)(
        const char*    plugin_id,
        const wchar_t* default_ini_path,
        wchar_t*       out_buf,
        uint32_t       cap_wchars);

    /* Inter-mod service registry. */
    int32_t (GRML_CALL* RegisterService)(const char* name, uint32_t version, void* service_ptr);
    void*   (GRML_CALL* GetService)(const char* name, uint32_t version);

    /* Resolves a named offset section from grml_offsets.ini (e.g. "in_game_flag").
     * Populates *out_value with the resolved runtime address or disp32 field offset
     * and returns 1 on success, 0 if missing/ambiguous or not yet scanned.
     * Guaranteed ready inside GrmlPostScanFn (and immediately on cache-hit boots). */
    int32_t (GRML_CALL* ResolveNamedOffset)(const char* section_name, uintptr_t* out_value);

    /* Returns the active GRML launcher/UI language code from grml.ini (e.g. "en",
     * "de", "ru"). Never returns NULL; defaults to "en". */
    const char* (GRML_CALL* GetLanguage)(void);

    /* Loads JSON translation files (`en.json` fallback + `<lang>.json`) for `plugin_id`
     * from `locales_dir` (or `<plugin_dll_dir>/locales/` when `locales_dir` is NULL).
     * Returns 1 if at least one locale file was loaded, 0 otherwise. */
    int32_t (GRML_CALL* LoadTranslations)(const char* plugin_id, const wchar_t* locales_dir);

    /* Looks up a translated UTF-8 string for `plugin_id` and `key` (supports dot
     * notation for nested JSON objects). Falls back to `en.json`, then `key` itself.
     * Returned pointer is owned by GRML and remains valid for the process lifetime. */
    const char* (GRML_CALL* Translate)(const char* plugin_id, const char* key);
};

#ifdef __cplusplus
}
#endif

#endif /* GRML_API_H */
