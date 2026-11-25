// ============================================================================
// save_tracker.cpp — CreateFileW hook tracking media_soviet\save\<Name>\
// ============================================================================
// Hooks KERNELBASE!CreateFileW (with KERNEL32 fallback) via MinHook so both
// direct Win32 calls AND CRT fopen/_wfopen/std::_Fiopen calls are captured.
//   - Load Game:             read  on *\media_soviet\save\<Name>\buildings.bin
//                            (ignores header.bin reads from Load menu scanning)
//   - Save / Autosave / As:  write on *\media_soviet\save\<Name>\header.bin
// ============================================================================
#include "save_tracker.hpp"

#include <windows.h>
#include <MinHook.h>

#include <cwctype>
#include <mutex>
#include <string>
#include <string_view>

#include "wrsr/logger.hpp"
#include "../core/rules.hpp"
#include "dx11_overlay.hpp"
#include "game_hooks.hpp"

namespace tpl::save_tracker {

namespace {

std::wstring g_lastSaveDir;
std::mutex   g_mutex;

using PFN_CreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD,
                                        LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
PFN_CreateFileW g_origCreateFileW = nullptr;

bool endsWithFileName(std::wstring_view path, std::wstring_view target) noexcept
{
    if (path.size() < target.size() + 1) {
        return false;
    }
    const size_t  offset = path.size() - target.size();
    const wchar_t prev   = path[offset - 1];
    if (prev != L'\\' && prev != L'/') {
        return false;
    }
    for (size_t i = 0; i < target.size(); ++i) {
        if (static_cast<wchar_t>(std::towlower(path[offset + i])) != target[i]) {
            return false;
        }
    }
    return true;
}

std::wstring extractSaveDir(std::wstring_view rawPath) noexcept
{
    std::wstring normalized(rawPath);
    for (auto& c : normalized) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (normalized.size() >= 2 && normalized[0] == L'.' && normalized[1] == L'\\') {
        normalized.erase(0, 2);
    }

    std::wstring lower = normalized;
    for (auto& c : lower) {
        c = static_cast<wchar_t>(std::towlower(c));
    }

    constexpr std::wstring_view kNeedles[] = {
        L"\\media_soviet\\save\\",
        L"\\media_soviet\\save_cloud\\",
        L"media_soviet\\save\\",
        L"media_soviet\\save_cloud\\",
    };

    size_t nameStart = std::wstring::npos;
    for (const auto needle : kNeedles) {
        const size_t pos = lower.find(needle);
        if (pos == std::wstring::npos) {
            continue;
        }
        // Relative needles (no leading '\') must match at index 0 to avoid false positives.
        if (needle.front() != L'\\' && pos != 0) {
            continue;
        }
        nameStart = pos + needle.size();
        break;
    }
    if (nameStart == std::wstring::npos) {
        return {};
    }

    const size_t nextSlash = lower.find(L'\\', nameStart);
    if (nextSlash == std::wstring::npos || nextSlash == nameStart) {
        return {};
    }

    std::wstring relOrFull = normalized.substr(0, nextSlash);
    wchar_t      fullBuf[MAX_PATH * 4]{};
    const DWORD  fullLen = GetFullPathNameW(relOrFull.c_str(), _countof(fullBuf), fullBuf, nullptr);
    if (fullLen > 0 && fullLen < _countof(fullBuf)) {
        return std::wstring(fullBuf, fullLen);
    }
    return relOrFull;
}

HANDLE WINAPI detourCreateFileW(LPCWSTR               lpFileName,
                                DWORD                 dwDesiredAccess,
                                DWORD                 dwShareMode,
                                LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                DWORD                 dwCreationDisposition,
                                DWORD                 dwFlagsAndAttributes,
                                HANDLE                hTemplateFile)
{
    HANDLE h = g_origCreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
                                 lpSecurityAttributes, dwCreationDisposition,
                                 dwFlagsAndAttributes, hTemplateFile);

    static thread_local bool t_inHook = false;
    if (t_inHook || !lpFileName || h == INVALID_HANDLE_VALUE) {
        return h;
    }

    const std::wstring_view pathView(lpFileName);
    const bool              isWrite = (dwDesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;

    // Trigger saves on header.bin writes; trigger loads only on buildings.bin reads
    // so browsing the Load Game menu (which reads header.bin across all saves)
    // does not hijack the active save directory.
    if (isWrite) {
        if (!endsWithFileName(pathView, L"header.bin")) {
            return h;
        }
    } else {
        if (!endsWithFileName(pathView, L"buildings.bin")) {
            return h;
        }
    }

    const std::wstring saveDir = extractSaveDir(pathView);
    if (saveDir.empty()) {
        return h;
    }

    t_inHook = true;

    bool dirChanged = false;
    {
        std::lock_guard lock(g_mutex);
        if (saveDir != g_lastSaveDir) {
            g_lastSaveDir = saveDir;
            dirChanged    = true;
        }
    }

    auto& engine = tpl::RuleEngine::get();
    if (isWrite) {
        engine.setActiveSaveDir(saveDir);
        engine.saveToSave(saveDir);
        tpl::game::resetBlockedStats();
        tpl::overlay::setActiveSavePath(saveDir.c_str());
        wrsr::log::info("save_tracker: saved sidecar INI in active save dir");
    } else if (dirChanged) {
        engine.setActiveSaveDir(saveDir);
        engine.loadFromSave(saveDir);
        tpl::game::resetBlockedStats();
        tpl::game::syncLivePricesToRules();
        tpl::overlay::setActiveSavePath(saveDir.c_str());
        wrsr::log::info("save_tracker: loaded sidecar INI for active save dir");
    }

    t_inHook = false;
    return h;
}

} // namespace

bool install()
{
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        return false;
    }

    void* target = nullptr;
    MH_STATUS st = MH_CreateHookApiEx(L"kernelbase.dll", "CreateFileW",
                                       reinterpret_cast<void*>(detourCreateFileW),
                                       reinterpret_cast<void**>(&g_origCreateFileW),
                                       &target);
    if (st != MH_OK) {
        st = MH_CreateHookApiEx(L"kernel32.dll", "CreateFileW",
                                reinterpret_cast<void*>(detourCreateFileW),
                                reinterpret_cast<void**>(&g_origCreateFileW),
                                &target);
    }
    if (st != MH_OK || !target) {
        return false;
    }

    return MH_EnableHook(target) == MH_OK;
}

} // namespace tpl::save_tracker
