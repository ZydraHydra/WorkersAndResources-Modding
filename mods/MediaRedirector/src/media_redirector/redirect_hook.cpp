// ============================================================================
// redirect_hook.cpp — Zero-allocation, lock-free Win32 VFS detour hooks
// ============================================================================
#include "redirect_hook.hpp"
#include "config.hpp"
#include "vfs_index.hpp"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "wrsr/logger.hpp"

namespace mr::hook {

namespace {

constexpr size_t kNormBufCap = 512;

using PFN_CreateFileW          = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PFN_CreateFileA          = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PFN_CreateFile2          = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
using PFN_GetFileAttributesW   = DWORD(WINAPI*)(LPCWSTR);
using PFN_GetFileAttributesA   = DWORD(WINAPI*)(LPCSTR);
using PFN_GetFileAttributesExW = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);

PFN_CreateFileW          g_origCreateFileW          = nullptr;
PFN_CreateFileA          g_origCreateFileA          = nullptr;
PFN_CreateFile2          g_origCreateFile2          = nullptr;
PFN_GetFileAttributesW   g_origGetFileAttributesW   = nullptr;
PFN_GetFileAttributesA   g_origGetFileAttributesA   = nullptr;
PFN_GetFileAttributesExW g_origGetFileAttributesExW = nullptr;

std::atomic<HookState> g_state{HookState::Uninitialized};
std::atomic<uint64_t>  g_redirectHits{0};
std::atomic<uint32_t>  g_loggedSampleHits{0};
char                   g_statusText[160] = "Hook: Initializing...";

// Prevents re-entrant double-lookup/counting when CreateFileA -> CreateFileW
thread_local int g_bypassDepth = 0;

struct ScopedBypass {
    ScopedBypass() noexcept { ++g_bypassDepth; }
    ~ScopedBypass() noexcept { --g_bypassDepth; }
};

[[nodiscard]] inline wchar_t toLowerAscii(wchar_t c) noexcept
{
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

[[nodiscard]] inline bool isSep(wchar_t c) noexcept
{
    return c == L'\\' || c == L'/';
}

[[nodiscard]] const wchar_t* findMediaSovietSegment(const wchar_t* path) noexcept
{
    constexpr wchar_t kNeedle[]  = L"media_soviet";
    constexpr size_t  kNeedleLen = 12;

    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (toLowerAscii(*p) != L'm') {
            continue;
        }
        if (p != path && !isSep(p[-1])) {
            continue;
        }
        size_t i = 1;
        for (; i < kNeedleLen; ++i) {
            const wchar_t ch = p[i];
            if (ch == L'\0' || toLowerAscii(ch) != kNeedle[i]) {
                break;
            }
        }
        if (i == kNeedleLen && isSep(p[kNeedleLen])) {
            return p;
        }
    }
    return nullptr;
}

[[nodiscard]] const wchar_t* tryResolveOverrideW(
    const wchar_t* lpFileName,
    wchar_t (&normBuf)[kNormBufCap]) noexcept
{
    if (g_bypassDepth > 0 || !Config::get().isEnabled() || !lpFileName) {
        return nullptr;
    }

    const wchar_t* seg = findMediaSovietSegment(lpFileName);
    if (!seg) {
        return nullptr;
    }

    size_t len = 0;
    for (const wchar_t* p = seg; *p != L'\0'; ++p) {
        wchar_t ch = *p;
        if (ch == L'/') {
            ch = L'\\';
        } else {
            ch = toLowerAscii(ch);
        }

        if (ch == L'\\' && len > 0 && normBuf[len - 1] == L'\\') {
            continue;
        }

        if (len + 1 >= kNormBufCap) {
            return nullptr;
        }
        normBuf[len++] = ch;
    }

    if (len == 0) {
        return nullptr;
    }
    normBuf[len] = L'\0';

    const auto& vfsMap = VfsIndex::get().map();
    const auto  it     = vfsMap.find(std::wstring_view(normBuf, len));
    if (it == vfsMap.end()) {
        return nullptr;
    }

    g_redirectHits.fetch_add(1, std::memory_order_relaxed);
    if (g_loggedSampleHits.load(std::memory_order_relaxed) < 5) {
        if (g_loggedSampleHits.fetch_add(1, std::memory_order_relaxed) < 5) {
            wrsr::log::info("VFS redirect: %ls -> %ls", normBuf, it->second.c_str());
        }
    }
    return it->second.c_str();
}

[[nodiscard]] const wchar_t* tryResolveOverrideA(
    const char* lpFileName,
    wchar_t (&wideIn)[kNormBufCap],
    wchar_t (&normBuf)[kNormBufCap]) noexcept
{
    if (g_bypassDepth > 0 || !Config::get().isEnabled() || !lpFileName) {
        return nullptr;
    }

    const int wLen = MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wideIn, static_cast<int>(kNormBufCap));
    if (wLen <= 0) {
        return nullptr;
    }
    return tryResolveOverrideW(wideIn, normBuf);
}

HANDLE WINAPI DetourCreateFileW(
    LPCWSTR               lpFileName,
    DWORD                 dwDesiredAccess,
    DWORD                 dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD                 dwCreationDisposition,
    DWORD                 dwFlagsAndAttributes,
    HANDLE                hTemplateFile)
{
    constexpr DWORD kWriteMask = GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
    if ((dwDesiredAccess & kWriteMask) == 0 && dwCreationDisposition == OPEN_EXISTING) {
        wchar_t normBuf[kNormBufCap];
        if (const wchar_t* target = tryResolveOverrideW(lpFileName, normBuf)) {
            ScopedBypass bypass;
            return g_origCreateFileW(target, dwDesiredAccess, dwShareMode,
                                     lpSecurityAttributes, dwCreationDisposition,
                                     dwFlagsAndAttributes, hTemplateFile);
        }
    }

    return g_origCreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
                             lpSecurityAttributes, dwCreationDisposition,
                             dwFlagsAndAttributes, hTemplateFile);
}

HANDLE WINAPI DetourCreateFileA(
    LPCSTR                lpFileName,
    DWORD                 dwDesiredAccess,
    DWORD                 dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD                 dwCreationDisposition,
    DWORD                 dwFlagsAndAttributes,
    HANDLE                hTemplateFile)
{
    constexpr DWORD kWriteMask = GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
    if ((dwDesiredAccess & kWriteMask) == 0 && dwCreationDisposition == OPEN_EXISTING) {
        wchar_t wideIn[kNormBufCap];
        wchar_t normBuf[kNormBufCap];
        if (const wchar_t* target = tryResolveOverrideA(lpFileName, wideIn, normBuf)) {
            ScopedBypass bypass;
            return g_origCreateFileW(target, dwDesiredAccess, dwShareMode,
                                     lpSecurityAttributes, dwCreationDisposition,
                                     dwFlagsAndAttributes, hTemplateFile);
        }
    }

    return g_origCreateFileA(lpFileName, dwDesiredAccess, dwShareMode,
                             lpSecurityAttributes, dwCreationDisposition,
                             dwFlagsAndAttributes, hTemplateFile);
}

HANDLE WINAPI DetourCreateFile2(
    LPCWSTR                           lpFileName,
    DWORD                             dwDesiredAccess,
    DWORD                             dwShareMode,
    DWORD                             dwCreationDisposition,
    LPCREATEFILE2_EXTENDED_PARAMETERS pCreateExParams)
{
    constexpr DWORD kWriteMask = GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
    if ((dwDesiredAccess & kWriteMask) == 0 && dwCreationDisposition == OPEN_EXISTING) {
        wchar_t normBuf[kNormBufCap];
        if (const wchar_t* target = tryResolveOverrideW(lpFileName, normBuf)) {
            ScopedBypass bypass;
            return g_origCreateFile2(target, dwDesiredAccess, dwShareMode,
                                     dwCreationDisposition, pCreateExParams);
        }
    }

    return g_origCreateFile2(lpFileName, dwDesiredAccess, dwShareMode,
                             dwCreationDisposition, pCreateExParams);
}

DWORD WINAPI DetourGetFileAttributesW(LPCWSTR lpFileName)
{
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* target = tryResolveOverrideW(lpFileName, normBuf)) {
        ScopedBypass bypass;
        return g_origGetFileAttributesW(target);
    }
    return g_origGetFileAttributesW(lpFileName);
}

DWORD WINAPI DetourGetFileAttributesA(LPCSTR lpFileName)
{
    wchar_t wideIn[kNormBufCap];
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* target = tryResolveOverrideA(lpFileName, wideIn, normBuf)) {
        ScopedBypass bypass;
        return g_origGetFileAttributesW(target);
    }
    return g_origGetFileAttributesA(lpFileName);
}

BOOL WINAPI DetourGetFileAttributesExW(
    LPCWSTR                lpFileName,
    GET_FILEEX_INFO_LEVELS fInfoLevelId,
    LPVOID                 lpFileInformation)
{
    wchar_t normBuf[kNormBufCap];
    if (const wchar_t* target = tryResolveOverrideW(lpFileName, normBuf)) {
        ScopedBypass bypass;
        return g_origGetFileAttributesExW(target, fInfoLevelId, lpFileInformation);
    }
    return g_origGetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
}

void* resolveFileApi(HMODULE hKernelBase, HMODULE hKernel32, const char* name) noexcept
{
    if (hKernelBase) {
        if (void* p = reinterpret_cast<void*>(GetProcAddress(hKernelBase, name))) {
            return p;
        }
    }
    if (hKernel32) {
        return reinterpret_cast<void*>(GetProcAddress(hKernel32, name));
    }
    return nullptr;
}

template <typename FnPtr>
bool hookApi(HMODULE hKernelBase, HMODULE hKernel32, const char* name, void* detour, FnPtr& outOrig) noexcept
{
    void* target = resolveFileApi(hKernelBase, hKernel32, name);
    if (!target) {
        wrsr::log::warn("File API %s not found", name);
        return false;
    }
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(&outOrig)) != MH_OK) {
        wrsr::log::warn("MH_CreateHook failed for %s", name);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        wrsr::log::warn("MH_EnableHook failed for %s", name);
        return false;
    }
    return true;
}

} // namespace

bool install() noexcept
{
    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        g_state.store(HookState::Failed, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: FAILED (MH_Initialize=%d)", static_cast<int>(mhInit));
        wrsr::log::error("MH_Initialize failed: %d", static_cast<int>(mhInit));
        return false;
    }

    HMODULE hKernelBase = GetModuleHandleW(L"kernelbase.dll");
    HMODULE hKernel32   = GetModuleHandleW(L"kernel32.dll");

    const bool okW   = hookApi(hKernelBase, hKernel32, "CreateFileW", reinterpret_cast<void*>(&DetourCreateFileW), g_origCreateFileW);
    const bool okA   = hookApi(hKernelBase, hKernel32, "CreateFileA", reinterpret_cast<void*>(&DetourCreateFileA), g_origCreateFileA);
    const bool ok2   = hookApi(hKernelBase, hKernel32, "CreateFile2", reinterpret_cast<void*>(&DetourCreateFile2), g_origCreateFile2);
    const bool okGAW = hookApi(hKernelBase, hKernel32, "GetFileAttributesW", reinterpret_cast<void*>(&DetourGetFileAttributesW), g_origGetFileAttributesW);
    const bool okGAA = hookApi(hKernelBase, hKernel32, "GetFileAttributesA", reinterpret_cast<void*>(&DetourGetFileAttributesA), g_origGetFileAttributesA);
    const bool okGAE = hookApi(hKernelBase, hKernel32, "GetFileAttributesExW", reinterpret_cast<void*>(&DetourGetFileAttributesExW), g_origGetFileAttributesExW);

    if (!okW) {
        g_state.store(HookState::Failed, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText), "Hook: FAILED (CreateFileW)");
        return false;
    }

    g_state.store(HookState::Active, std::memory_order_relaxed);
    std::snprintf(g_statusText, sizeof(g_statusText),
                  "Hook: ACTIVE (CreateFileW/A/2 + GetFileAttributesW/A/ExW | Zero-Alloc)");
    wrsr::log::info("Installed VFS detours (W=%d A=%d 2=%d GAW=%d GAA=%d GAE=%d)",
                    okW ? 1 : 0, okA ? 1 : 0, ok2 ? 1 : 0,
                    okGAW ? 1 : 0, okGAA ? 1 : 0, okGAE ? 1 : 0);
    return true;
}

HookState state() noexcept
{
    return g_state.load(std::memory_order_relaxed);
}

const char* statusText() noexcept
{
    return g_statusText;
}

uint64_t redirectHits() noexcept
{
    return g_redirectHits.load(std::memory_order_relaxed);
}

} // namespace mr::hook
