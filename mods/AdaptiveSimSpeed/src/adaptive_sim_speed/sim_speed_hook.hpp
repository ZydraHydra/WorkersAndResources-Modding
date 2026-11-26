#pragma once
// ============================================================================
// sim_speed_hook.hpp — Hardware-adaptive fixed-step simulation sub-stepping
// ============================================================================

#include <cstddef>
#include <cstdint>

namespace ass::hook {

enum class HookState : uint8_t {
    Uninitialized = 0,
    Active,
    FailedSignature,
    FailedHook,
};

bool install(uint8_t* exeBase, size_t exeSize);
void uninstall() noexcept;

[[nodiscard]] HookState   state() noexcept;
[[nodiscard]] const char* statusText() noexcept;
[[nodiscard]] const char* telemetryText() noexcept;

[[nodiscard]] float    lastSingleTickMs() noexcept;
[[nodiscard]] float    lastActiveMultiplier() noexcept;
[[nodiscard]] uint32_t lastExtraSubSteps() noexcept;

} // namespace ass::hook
