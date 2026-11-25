#pragma once
// ============================================================================
// game_hooks.hpp — AOB scanner targets & SOVIET64.exe resource structures
// ============================================================================

#include <atomic>
#include <cstdint>
#include <optional>

namespace tpl::game {

// Cross-version verified AOB pattern (unique match on both v1.1.1.7 and latest Steam build):
//   +11: 48 8D 0D <disp32>   LEA RCX, [ResourceVector]  (next IP at +18, disp at +14)
//   +42: 49 69 C0 <imm32>    IMUL RAX, R8, <stride>     (stride at +45: 0x340 on v1.1.1.7, 0x380 on latest)
inline constexpr const char* SIG_PRICE_TABLE =
    "48 C7 86 94 07 00 00 03 00 00 00 "
    "48 8D 0D ?? ?? ?? ?? "
    "E8 ?? ?? ?? ?? "
    "49 3B C6 "
    "0F 84 ?? ?? ?? ?? "
    "4C 63 86 98 07 00 00 "
    "49 8B C8 "
    "49 69 C0 ?? ?? 00 00 "
    "48 8B 15 ?? ?? ?? ??";

// Cross-version verified AOB pattern for EvaluateDistTask(GameObject*, Task0x80*)
// Called by both RoadDistOffice (0x1C6B81) and RailDistOffice (0x1C7BE1) in v1.1.1.7.
// 29-byte prologue with zero RIP-relative instructions.
inline constexpr const char* SIG_DISPATCH_CHECK =
    "48 8B C4 48 89 58 08 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8D 68 A1 48 81 EC 00 01 00 00";

struct ResourceVector {
    uint8_t* begin{nullptr};
    uint8_t* end{nullptr};
    uint8_t* cap{nullptr};
};

// Prefix layout of a ResourceRecord (stride is 0x340 on v1.1.1.7, 0x380 on latest)
#pragma pack(push, 1)
struct ResourceRecordHeader {
    char    name[0x40];      // +0x00: inline NUL-terminated ASCII resource name
    int32_t localisationId;  // +0x40
    int32_t priceKind;       // +0x44: -1 workers, -2 eletric, -3 vehicles, -4 trains, -5 heat, 0..4 tradeable
    void*   iconTexture;     // +0x48
    float   _unk50;          // +0x50
    float   _unk54;          // +0x54
    float   buyPriceUsd;     // +0x58
    float   buyPriceRub;     // +0x5C
    float   sellPriceUsd;    // +0x60
    float   sellPriceRub;    // +0x64
    uint8_t _pad68[0x10];    // +0x68..+0x77
    float   basePriceRub;    // +0x78
    float   basePriceUsd;    // +0x7C
};
#pragma pack(pop)
static_assert(sizeof(ResourceRecordHeader) == 0x80, "ResourceRecordHeader layout check");

struct GameAddresses {
    ResourceVector* resourceVector{nullptr};
    uint32_t        recordStride{0x340};
    uint32_t        buildingTypeOffset{0x360};
    uint8_t*        dispatchCheckSite{nullptr};
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
