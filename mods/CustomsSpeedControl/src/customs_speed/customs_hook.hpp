#pragma once
// ============================================================================
// customs_hook.hpp — AOB scanner & MinHook detour stub for Customs transfer
// ============================================================================

#include <cstdint>

namespace csc::hook {

enum class HookState : int32_t {
    Uninitialized = 0,
    StandbyNoAob  = 1, // Pattern is PENDING or missing in offsets.ini / CustomsSpeedControl.ini
    AobNotMatched = 2, // Pattern did not match uniquely in SOVIET64.exe .text
    Active        = 3, // MinHook detour installed and active
};

struct VehicleCustomsOffsets {
    uint32_t vehPullingOff{0x398};         // Vehicle* pulling locomotive (nullptr if solo/road)
    uint32_t vehTargetBldOff{0x528};       // Building* docked station/Customs building
    uint32_t vehOverrideBldOff{0x538};     // Building* override building (when non-null on road/solo)
    uint32_t vehTransferStateOff{0x594};   // int32_t (0=idle, 1=unloading, 2=loading)
    uint32_t vehWait1Off{0x598};           // float wait/dwell timer 1
    uint32_t vehElecOff{0x10E0};           // electricity check field
    uint32_t vehAnimOff{0x1348};           // float loading animation timer (0x1310 on 1.1.1.x)
    uint32_t vehTypeOff{0x1748};           // VehicleType* (0x1708 on 1.1.1.x)
    uint32_t vehWait2Off{0x1834};          // float wait/dwell timer 2 (0x179C on 1.1.1.x)
    uint32_t vehTypeCategoryOff{0x294};    // 3=wagon, 4=locomotive, 5=rail service
    int32_t  railCategoryMin{3};
    int32_t  railCategoryMax{5};
    uint32_t bldDescOff{0x318};            // BuildingDescriptor*
    uint32_t descTypeOff{0x368};           // BUILDINGTYPE_* (0x360 on 1.1.1.7..13, 0x368 on 1.2.0.8)
    int32_t  customsBuildingType{20};      // $TYPE_CUSTOMHOUSE == 20
};

// Initializes GameContext, scans for cargo_transfer_tick_aob, and hooks it.
// Returns true if hooked, false if running in standby/UI-only mode.
bool install(uint8_t* exeBase, size_t exeSize);

// Current hook state & persistent UI status string buffer
HookState   state() noexcept;
const char* statusText() noexcept;
uint64_t    scaledTickCount() noexcept;

// Helper exposed for testing / documentation alignment:
// Applies load/unload multiplier before clamping to remaining capacity.
float computeScaledTransferAmount(float baseTickAmount,
                                  float remainingCapacity,
                                  bool  isUnloading) noexcept;

} // namespace csc::hook
