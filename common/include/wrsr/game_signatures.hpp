#pragma once
// ============================================================================
// game_signatures.hpp — Shared WRSR PE profile resolution, AOB helpers & structs
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "wrsr/offsets.hpp"
#include "wrsr/pe_utils.hpp"

namespace wrsr::game {

inline constexpr size_t kMaxResources = 256;

template <typename T>
inline T readField(const uint8_t* base, uint32_t off) noexcept
{
    return *reinterpret_cast<const T*>(base + off);
}

template <typename T>
inline void writeField(uint8_t* base, uint32_t off, T value) noexcept
{
    *reinterpret_cast<T*>(base + off) = value;
}

bool readSafeCString(const char* ptr, char* out, size_t maxLen) noexcept;

struct ResourceVector {
    uint8_t* begin{nullptr};
    uint8_t* end{nullptr};
    uint8_t* cap{nullptr};
};

struct RawResourceSnapshot {
    char    name[33]{};
    int32_t priceKind{0};
    float   buyRub{0.f};
    float   sellRub{0.f};
    float   buyUsd{0.f};
    float   sellUsd{0.f};
};

struct LivePrice {
    float buyRub{0.f};
    float buyUsd{0.f};
    float sellRub{0.f};
    float sellUsd{0.f};
};

struct DistTaskBuildingProbe {
    bool    srcValid{false};
    int32_t srcBuildingType{-1};
    int32_t srcFaction{0};
    bool    dstValid{false};
    int32_t dstBuildingType{-1};
    int32_t dstFaction{0};
};

struct CommonLayout {
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

    bool loadFromProfile(const wrsr::offsets::ResolvedProfile& profile,
                         const wrsr::pe::ExecutableFingerprint& fp);
};

struct GameContext {
    uint8_t* exeBase{nullptr};
    size_t   exeSize{0};
    uint8_t* scanBase{nullptr};
    size_t   scanSize{0};

    wrsr::offsets::OffsetDocument  offsets;
    wrsr::offsets::ResolvedProfile profile;
    CommonLayout                   layout{};

    ResourceVector* resourceVector{nullptr};
    uint8_t*        evaluateDistTaskAddr{nullptr};

    // Loads common/config/offsets.ini, merges optional mod-specific INI overrides,
    // resolves the PE fingerprint profile, and detects shared engine signatures.
    static std::optional<GameContext> initialize(
        uint8_t* exeBase,
        size_t exeSize,
        std::string_view modOffsetsOverride = {});

    [[nodiscard]] uint8_t* scanSignature(
        std::string_view sigKey,
        std::string_view section = "signatures") const noexcept;

    [[nodiscard]] uint8_t* resolveRipTarget(
        std::string_view sigKey,
        std::string_view dispKey,
        std::string_view nextIpKey,
        uint8_t** outMatchSite = nullptr,
        std::string_view section = "signatures") const noexcept;

    bool resolveResourceVector() noexcept;
    bool resolveEvaluateDistTask() noexcept;

    bool tryReadResourceCount(size_t& outCount) const noexcept;
    bool tryReadResourceSnapshot(size_t index, RawResourceSnapshot& out) const noexcept;
    [[nodiscard]] std::optional<LivePrice> readLivePrice(uint32_t resourceIndex) const noexcept;

    bool tryProbeDistTaskBuildings(const uint8_t* task, DistTaskBuildingProbe& out) const noexcept;
    void zeroDistTaskScores(uint8_t* task) const noexcept;
};

// Scans `cmp dword ptr [reg+disp32], imm8` instructions for hotfix-resilient field offset extraction.
uint32_t scanCmpMemImm8Disp32(const uint8_t* site,
                              uint32_t scanWindow,
                              int32_t expectedImm8,
                              uint32_t minDisp,
                              uint32_t maxDisp,
                              uint32_t fallbackOff) noexcept;

} // namespace wrsr::game
