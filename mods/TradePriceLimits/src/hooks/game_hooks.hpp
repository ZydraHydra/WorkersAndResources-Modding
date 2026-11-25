#pragma once
// ============================================================================
// game_hooks.hpp — TradePriceLimits dispatch gating via shared wrsr::game
// ============================================================================

#include <cstdint>
#include <optional>

#include "wrsr/game_signatures.hpp"

namespace tpl::game {

using ResourceVector = wrsr::game::ResourceVector;
using LivePrice      = wrsr::game::LivePrice;

struct GameAddresses {
    wrsr::game::GameContext ctx{};
    ResourceVector*         resourceVector{nullptr};
    uint8_t*                dispatchCheckSite{nullptr};
};

extern GameAddresses g_addrs;

bool     install(uint8_t* exeBase, size_t exeSize);
void     syncLivePricesToRules() noexcept;
void     resetBlockedStats() noexcept;
uint64_t getBlockedCount() noexcept;

std::optional<LivePrice> readLivePrice(uint32_t resourceIndex) noexcept;

} // namespace tpl::game
