#pragma once
// ============================================================================
// pe_utils.hpp — Shared PE helpers, AOB pattern scanner, and IAT patcher
// ============================================================================

#include <cstddef>
#include <cstdint>

namespace wrsr::pe {

struct ModuleRange {
    uint8_t* base{nullptr};
    size_t   size{0};
};

struct ExecutableFingerprint {
    uint32_t timeDateStamp{0};
    uint32_t sizeOfImage{0};
};

// Returns image base and SizeOfImage for the main process executable (SOVIET64.exe)
ModuleRange getMainModuleRange() noexcept;

// Extracts PE TimeDateStamp and SizeOfImage from a loaded PE module
ExecutableFingerprint getExecutableFingerprint(uint8_t* moduleBase) noexcept;

// Returns virtual address and virtual size of a named PE section (e.g. ".text")
ModuleRange getSectionRange(uint8_t* moduleBase, const char* sectionName) noexcept;

// Raw byte+mask AOB pattern scan over [base, base + size)
uint8_t* aobScan(uint8_t* base, size_t size,
                 const uint8_t* pattern, const uint8_t* mask, size_t len) noexcept;

// String pattern scan: "48 8B 05 ?? ?? ?? ??" ("?" or "??" are wildcards)
uint8_t* aobScanStr(uint8_t* base, size_t size, const char* patternStr) noexcept;

// Patch an imported function in a module's IAT (case-insensitive DLL match)
bool patchIat(void* moduleHandle, const char* dllName, const char* funcName,
              void* detour, void** outOriginal) noexcept;

} // namespace wrsr::pe
