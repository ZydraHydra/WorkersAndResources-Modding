#pragma once
// ============================================================================
// loader_abi.hpp — Clean-room C ABI declarations for RML v1 & Tesmio v4
// ============================================================================
// Defines only the binary-level C ABI structs and function signatures queried
// via GetProcAddress by Republic Mod Loader (RML API v1) and TesmioLoader
// (Tesmio API v4). Written from scratch for ABI interoperability; contains
// no third-party licensed source code.
// ============================================================================

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define WRSR_ABI_CALL __cdecl
#define WRSR_ABI_EXPORT extern "C" __declspec(dllexport)
#else
#define WRSR_ABI_CALL
#define WRSR_ABI_EXPORT extern "C"
#endif

namespace wrsr::abi {

// ---------------------------------------------------------------------------
// Republic Mod Loader (RML) — Host API v1
// ---------------------------------------------------------------------------
inline constexpr uint32_t kRmlApiVersion              = 1u;
inline constexpr uint32_t kRmlPluginDescriptorVersion = 1u;

enum RmlLogLevel : int32_t {
    RML_LOG_TRACE = 0,
    RML_LOG_INFO  = 1,
    RML_LOG_WARN  = 2,
    RML_LOG_ERROR = 3,
};

enum RmlSymbolConfidence : int32_t {
    RML_SYMBOL_UNRESOLVED   = 0,
    RML_SYMBOL_PROBABLE     = 1,
    RML_SYMBOL_CONFIRMED    = 2,
    RML_SYMBOL_AMBIGUOUS    = 3,
    RML_SYMBOL_INCOMPATIBLE = 4,
    RML_SYMBOL_OBSERVED     = 5,
};

struct RmlPluginDescriptor {
    uint32_t    struct_size;
    uint32_t    descriptor_version;
    uint32_t    api_min;
    uint32_t    api_max;
    const char* id;
    const char* name;
    const char* version;
    const char* author;
};

struct RmlSymbolInfo {
    uint32_t            struct_size;
    const char*         id;
    uintptr_t           address;
    RmlSymbolConfidence confidence;
    const char*         resolver;
};

struct RmlGameInfo {
    uint32_t    struct_size;
    const char* version;
    const char* sha256;
    uintptr_t   image_base;
    uint32_t    loader_api;
    const char* loader_version;
};

struct RmlServiceInfo {
    uint32_t    struct_size;
    const char* name;
    uint32_t    version;
    const char* provider_id;
};

struct RmlHostApi {
    uint32_t struct_size;
    uint32_t api_version;
    void*    plugin_context;

    void(WRSR_ABI_CALL* log)(void* plugin_context, RmlLogLevel level, const char* message);
    int(WRSR_ABI_CALL* get_game_info)(void* plugin_context, RmlGameInfo* out_info);
    int(WRSR_ABI_CALL* get_symbol)(void* plugin_context, const char* symbol_id, RmlSymbolInfo* out_info);
    uint32_t(WRSR_ABI_CALL* get_symbol_count)(void* plugin_context);
    int(WRSR_ABI_CALL* get_symbol_by_index)(void* plugin_context, uint32_t index, RmlSymbolInfo* out_info);
    int(WRSR_ABI_CALL* provide_service)(void* plugin_context, const char* name, uint32_t version, void* service);
    void*(WRSR_ABI_CALL* consume_service)(void* plugin_context, const char* name, uint32_t version,
                                         RmlServiceInfo* out_info);
};

// ---------------------------------------------------------------------------
// TesmioLoader — Host API v4
// ---------------------------------------------------------------------------
inline constexpr unsigned kTsmApiVersion = 4u;

struct TsmHost {
    unsigned       apiVersion;
    unsigned       structSize;
    void*          exeModule;
    unsigned char* exeBase;
    size_t         exeSize;
    void*          engineModule;
    const char*    baseDir;
    const char*    pluginDir;

    void (*log)(const char* fmt, ...);
    void** (*findIatSlot)(void* module, const char* dll, const char* fn);
    int (*patchIat)(void* module, const char* dll, const char* fn, void* detour, void** original, const char* label);
    int (*installInlineHook)(void* target, void* detour, void** trampoline, const unsigned char* expect, size_t stolen,
                             const char* label);
    unsigned char* (*allocNear)(unsigned char* anchor, size_t size);
    int (*readablePtr)(const void* p, size_t n);
    long (*faultFilter)(const char* what, void* exceptionPointers);
    int (*configInt)(const char* iniName, const char* section, const char* key, int fallback);
    int (*configString)(const char* iniName, const char* section, const char* key, char* out, int outSize,
                        const char* fallback);
    int (*provide)(const char* service, unsigned version, const void* iface);
    const void* (*consume)(const char* service, unsigned version);
    const char* vfsRoot; // v4+
};

struct TsmPluginInfo {
    const char* name;
    const char* version;
};

} // namespace wrsr::abi
