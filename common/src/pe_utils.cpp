// ============================================================================
// pe_utils.cpp — Shared PE helpers, AOB pattern scanner, and IAT patcher
// ============================================================================
#include "wrsr/pe_utils.hpp"

#include <windows.h>
#include <charconv>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace wrsr::pe {

namespace {
bool parsePattern(const char* patStr,
                  std::vector<uint8_t>& bytes,
                  std::vector<uint8_t>& masks)
{
    bytes.clear();
    masks.clear();
    if (!patStr) {
        return false;
    }

    std::istringstream ss(patStr);
    std::string token;
    while (ss >> token) {
        if (token == "?" || token == "??") {
            bytes.push_back(0x00);
            masks.push_back(0x00);
        } else {
            uint8_t b = 0;
            auto res = std::from_chars(token.data(), token.data() + token.size(), b, 16);
            if (res.ec != std::errc{} || res.ptr != token.data() + token.size()) {
                return false;
            }
            bytes.push_back(b);
            masks.push_back(0xFF);
        }
    }
    return !bytes.empty();
}
} // namespace

ModuleRange getMainModuleRange() noexcept
{
    HMODULE hExe = GetModuleHandleW(nullptr);
    if (!hExe) {
        return {};
    }
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hExe);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return {};
    }
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8_t*>(hExe) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return {};
    }
    return { reinterpret_cast<uint8_t*>(hExe), nt->OptionalHeader.SizeOfImage };
}

ModuleRange getSectionRange(uint8_t* moduleBase, const char* sectionName) noexcept
{
    if (!moduleBase || !sectionName) {
        return {};
    }
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return {};
    }
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return {};
    }

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const size_t nameLen = std::strlen(sectionName);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char secBuf[9]{};
        std::memcpy(secBuf, sec[i].Name, 8);
        if (nameLen <= 8 && std::strcmp(secBuf, sectionName) == 0) {
            return { moduleBase + sec[i].VirtualAddress, sec[i].Misc.VirtualSize };
        }
    }
    return {};
}

uint8_t* aobScan(uint8_t* base, size_t size,
                 const uint8_t* pattern, const uint8_t* mask, size_t len) noexcept
{
    if (!base || !pattern || !mask || len == 0 || size < len) {
        return nullptr;
    }

    uint8_t* end = base + size - len;
    for (uint8_t* p = base; p <= end; ++p) {
        bool match = true;
        for (size_t i = 0; i < len; ++i) {
            if (mask[i] && p[i] != pattern[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            return p;
        }
    }
    return nullptr;
}

uint8_t* aobScanStr(uint8_t* base, size_t size, const char* patternStr) noexcept
{
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> masks;
    if (!parsePattern(patternStr, bytes, masks)) {
        return nullptr;
    }
    // Reject all-wildcard patterns so placeholders never falsely match offset 0
    bool hasConcreteByte = false;
    for (uint8_t m : masks) {
        if (m != 0) {
            hasConcreteByte = true;
            break;
        }
    }
    if (!hasConcreteByte) {
        return nullptr;
    }
    return aobScan(base, size, bytes.data(), masks.data(), bytes.size());
}

bool patchIat(void* moduleHandle, const char* dllName, const char* funcName,
              void* detour, void** outOriginal) noexcept
{
    if (!moduleHandle || !dllName || !funcName || !detour) {
        return false;
    }

    auto* base = static_cast<uint8_t*>(moduleHandle);
    auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress) {
        return false;
    }

    auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* curDll = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(curDll, dllName) != 0) {
            continue;
        }

        auto* thunk     = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        auto* origThunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));

        for (; thunk->u1.Function; ++thunk, ++origThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(origThunk->u1.Ordinal)) {
                continue;
            }
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + origThunk->u1.AddressOfData);
            if (std::strcmp(byName->Name, funcName) != 0) {
                continue;
            }

            void** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            if (outOriginal) {
                *outOriginal = *slot;
            }

            DWORD oldProt = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                return false;
            }
            *slot = detour;
            VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
            return true;
        }
    }
    return false;
}

} // namespace wrsr::pe
