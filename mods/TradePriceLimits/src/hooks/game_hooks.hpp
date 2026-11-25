#pragma once
// ============================================================================
// game_hooks.hpp — Offset-profile driven AOB hooks & SOVIET64.exe structures
// ============================================================================

#include <cstdint>
#include <optional>
#include <string>

namespace tpl::game {

struct ResourceVector {
    uint8_t* begin{nullptr};
    uint8_t* end{nullptr};
    uint8_t* cap{nullptr};
};

// All version-sensitive struct offsets loaded per WRSR build from config/offsets.ini.
struct GameLayout {
    std::string profileName{"default"};
    bool        exactProfileMatch{false};
    uint32_t    peTimeDateStamp{0};
    uint32_t    peSizeOfImage{0};

    // ResourceRecord layout
    uint32_t resStride{0};
    uint32_t resMinHeaderSize{0};
    uint32_t resNameOff{0};
    uint32_t resKindOff{0};
    uint32_t resBuyUsdOff{0};
    uint32_t resBuyRubOff{0};
    uint32_t resSellUsdOff{0};
    uint32_t resSellRubOff{0};

    // DistTask (0x80-byte) & Building / BuildingDescriptor layout
    uint32_t taskResOff{0};
    uint32_t taskSrcBldOff{0};
    uint32_t taskDstBldOff{0};
    uint32_t taskScoreSrcDoubleOff{0};
    uint32_t taskScoreSrcFlag1Off{0};
    uint32_t taskScoreSrcFlag2Off{0};
    uint32_t taskScoreDstDoubleOff{0};
    uint32_t taskScoreDstFlag1Off{0};
    uint32_t taskScoreDstFlag2Off{0};
    uint32_t bldDescOff{0};
    uint32_t bldFactionOff{0};
    uint32_t descTypeOff{0};

    int32_t  customsBuildingType{20};
    int32_t  factionUsd{3};
    int32_t  factionRub{4};
};

struct GameAddresses {
    ResourceVector* resourceVector{nullptr};
    uint8_t*        dispatchCheckSite{nullptr};
    GameLayout      layout{};
};

extern GameAddresses g_addrs;

bool     install(uint8_t* exeBase, size_t exeSize);
void     syncLivePricesToRules() noexcept;
void     resetBlockedStats() noexcept;
uint64_t getBlockedCount() noexcept;

struct LivePrice {
    float buyRub{0.f};
    float buyUsd{0.f};
    float sellRub{0.f};
    float sellUsd{0.f};
};

std::optional<LivePrice> readLivePrice(uint32_t resourceIndex) noexcept;

} // namespace tpl::game
