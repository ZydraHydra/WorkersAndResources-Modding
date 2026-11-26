#pragma once
// ============================================================================
// tech_service_hook.hpp — TechServiceControl MinHook installation & status
// ============================================================================

#include <cstddef>
#include <cstdint>

namespace tsc::hook {

enum class HookState : int {
    Uninitialized = 0,
    Active        = 1,
    Standby       = 2,
    Failed        = 3,
};

bool install(uint8_t* exeBase, size_t exeSize);
void shutdown();

// Updates live threshold floats (used by the FindWasteTarget RIP-relative patch).
void syncThresholds() noexcept;

HookState   state() noexcept;
const char* statusText() noexcept;
uint64_t    gatedCount() noexcept;

} // namespace tsc::hook
