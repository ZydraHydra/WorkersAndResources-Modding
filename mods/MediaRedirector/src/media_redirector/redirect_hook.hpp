#pragma once
// ============================================================================
// redirect_hook.hpp — Zero-allocation, lock-free Win32 VFS detour hooks
// ============================================================================

#include <cstdint>

namespace mr::hook {

enum class HookState {
    Uninitialized,
    Active,
    Failed,
};

// Installs CreateFileW, GetFileAttributesW, and GetFileAttributesExW detours.
// Safe to call synchronously inside DllMain (DLL_PROCESS_ATTACH).
bool install() noexcept;

[[nodiscard]] HookState   state() noexcept;
[[nodiscard]] const char* statusText() noexcept;
[[nodiscard]] uint64_t    redirectHits() noexcept;

} // namespace mr::hook
