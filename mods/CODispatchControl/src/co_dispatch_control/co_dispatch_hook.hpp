#pragma once
// ============================================================================
// co_dispatch_hook.hpp — CODispatchControl DispatchCOTask hook API
// ============================================================================

#include <cstddef>
#include <cstdint>

namespace cdc::hook {

enum class HookState {
    Uninitialized,
    Active,
    AobNotMatched,
};

bool install(uint8_t* exeBase, size_t exeSize);

HookState   state() noexcept;
const char* statusText() noexcept;
uint64_t    rejectedCount() noexcept;
void        resetRejectedCount() noexcept;

} // namespace cdc::hook
