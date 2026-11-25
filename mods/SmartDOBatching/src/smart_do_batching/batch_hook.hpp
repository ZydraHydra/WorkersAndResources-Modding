#pragma once
// ============================================================================
// batch_hook.hpp — SmartDOBatching EvaluateDistTask detour declarations
// ============================================================================

#include <cstdint>

namespace sdb::hook {

enum class HookState {
    Uninitialized,
    Active,
    AobNotMatched,
};

bool      install(uint8_t* exeBase, size_t exeSize);
HookState state() noexcept;
const char* statusText() noexcept;
uint64_t  rejectedCount() noexcept;
void      resetRejectedCount() noexcept;

} // namespace sdb::hook
