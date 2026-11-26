#pragma once
// ============================================================================
// bay_access_hook.hpp — Multi-bay road building apron batching hook
// ============================================================================

#include <cstddef>
#include <cstdint>

namespace sba::hook {

enum class HookState : int32_t {
    Uninitialized = 0,
    Active        = 1,
    AobNotMatched = 2,
};

bool install(uint8_t* exeBase, size_t exeSize);

HookState   state() noexcept;
const char* statusText() noexcept;
const char* trackedBuildingsText() noexcept;
const char* batchedEntriesText() noexcept;

uint32_t activeTrackedBuildings() noexcept;
uint64_t batchedEntriesAllowed() noexcept;

void resetTracking() noexcept;

} // namespace sba::hook
