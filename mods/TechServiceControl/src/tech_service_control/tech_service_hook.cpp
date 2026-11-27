// ============================================================================
// tech_service_hook.cpp — TechServiceControl Technical Services threshold hooks
// ============================================================================
// Installs 4 independent threshold overrides in SOVIET64.exe (each falling back
// gracefully to STANDBY if its offsets.ini signature is PENDING or missing):
//   1. Snowplow Dispatch: detours RoadNode_HasSnow(RoadSearchRequest*, RoadNode*)
//      and overrides req->minSnowByte when req->searchMode == 9.
//   2. Water Delivery: detours FindWaterTarget(GameObject*, Building* techOffice)
//      and evaluates candidate buildings against water_below_pct / 100.0f.
//   3. Sewage Pickup: detours FindSewageTarget(GameObject*, Building* techOffice)
//      and evaluates candidate buildings against sewage_min_pct / 100.0f.
//   4. Garbage / Waste Pickup: retargets FindWasteTarget's RIP-relative threshold
//      loads (0.75f mixed / 0.60f fraction) to a near-module float[2] page.
// ============================================================================
#include "tech_service_hook.hpp"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "embedded_offsets.hpp"
#include "wrsr/game_signatures.hpp"
#include "wrsr/logger.hpp"
#include "config.hpp"

namespace tsc::hook {

namespace {

// ── Mutable state ─────────────────────────────────────────────────────────
std::atomic<HookState> g_state{HookState::Uninitialized};
std::atomic<uint64_t>  g_gatedCount{0};
char                   g_statusText[256] = "Hooks: Initializing...";

wrsr::game::GameContext g_gameCtx{};

struct TechOffsets {
    uint32_t techTargetsBeginOff{0xF90};
    uint32_t techTargetsEndOff{0xF98};
    uint32_t bldPipedWaterOff{0x133D};
    uint32_t bldPipedSewageOff{0x133E};
    uint32_t bldWaterMetricOff{0x1310};
    uint32_t reqMinSnowByteOff{0x20};
    uint32_t reqSearchModeOff{0xB8};
    int32_t  snowSearchModeVal{9};
};
TechOffsets g_off{};

// ── Per-hook status ───────────────────────────────────────────────────────
HookState g_snowState{HookState::Standby};
HookState g_waterState{HookState::Standby};
HookState g_sewageState{HookState::Standby};
HookState g_wasteState{HookState::Standby};

// ── Engine function pointers ──────────────────────────────────────────────
using PFN_RoadNodeHasSnow    = bool(__fastcall*)(void* req, void* node);
using PFN_FindServiceTarget  = uint8_t*(__fastcall*)(void* gameObj, uint8_t* techOffice);
using PFN_GetWaterFillRatio  = float(__fastcall*)(void* gameObj, uint8_t* building);
using PFN_GetSewageFillRatio = float(__fastcall*)(void* gameObj, uint8_t* building, int32_t flag);
using PFN_IsTruckEnRoute     = bool(__fastcall*)(uint8_t* building);

PFN_RoadNodeHasSnow    g_origRoadNodeHasSnow  = nullptr;
PFN_FindServiceTarget  g_origFindWaterTarget  = nullptr;
PFN_FindServiceTarget  g_origFindSewageTarget = nullptr;
PFN_GetWaterFillRatio  g_getWaterFillRatio    = nullptr;
PFN_GetSewageFillRatio g_getSewageFillRatio   = nullptr;
PFN_IsTruckEnRoute     g_isWaterTruckEnRoute  = nullptr;
PFN_IsTruckEnRoute     g_isSewageTruckEnRoute = nullptr;

// ── Near-module float[2] page for FindWasteTarget threshold patch ─────────
// [0] = mixed waste / garbage stand threshold (vanilla 0.75f)
// [1] = specific waste fraction threshold     (vanilla 0.60f)
volatile float* g_wasteThresholds = nullptr;
constexpr float kVanillaMixedWasteThreshold = 0.75f;
constexpr float kVanillaFracWasteThreshold  = 0.60f;

// ── Helpers ───────────────────────────────────────────────────────────────
bool isPendingSig(const wrsr::offsets::OffsetDocument& doc, std::string_view key) noexcept
{
    const auto pat = doc.getString("signatures", key);
    return !pat || pat->empty() || *pat == "PENDING" || *pat == "pending";
}

uint8_t* resolveCallRel32(uint8_t* instrAddr, uint32_t dispOff, uint32_t nextIpOff) noexcept
{
    int32_t rel = 0;
    std::memcpy(&rel, instrAddr + dispOff, sizeof(rel));
    return instrAddr + nextIpOff + rel;
}

volatile float* allocNearModuleFloats(uint8_t* anchor) noexcept
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t base = reinterpret_cast<uintptr_t>(anchor);
    const uintptr_t minAddr = (base > 0x70000000ULL) ? (base - 0x70000000ULL) : gran;
    const uintptr_t maxAddr = base + 0x70000000ULL;

    for (uintptr_t offset = gran; offset < 0x70000000ULL; offset += gran) {
        if (base > offset && (base - offset) >= minAddr) {
            const uintptr_t cand = (base - offset) & ~(gran - 1);
            void* p = VirtualAlloc(reinterpret_cast<void*>(cand), 4096,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (p) {
                return reinterpret_cast<volatile float*>(p);
            }
        }
        if ((base + offset) <= maxAddr) {
            const uintptr_t cand = (base + offset) & ~(gran - 1);
            void* p = VirtualAlloc(reinterpret_cast<void*>(cand), 4096,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (p) {
                return reinterpret_cast<volatile float*>(p);
            }
        }
    }
    return nullptr;
}

bool patchRipDisp32(uint8_t* instrAddr, uint32_t dispOff, uint32_t nextIpOff, const volatile void* target) noexcept
{
    const intptr_t delta = reinterpret_cast<intptr_t>(target) -
                           reinterpret_cast<intptr_t>(instrAddr + nextIpOff);
    if (delta < INT32_MIN || delta > INT32_MAX) {
        return false;
    }
    const int32_t disp32 = static_cast<int32_t>(delta);
    DWORD oldProt = 0;
    if (!VirtualProtect(instrAddr + dispOff, sizeof(disp32), PAGE_EXECUTE_READWRITE, &oldProt)) {
        return false;
    }
    std::memcpy(instrAddr + dispOff, &disp32, sizeof(disp32));
    DWORD dummy = 0;
    VirtualProtect(instrAddr + dispOff, sizeof(disp32), oldProt, &dummy);
    FlushInstructionCache(GetCurrentProcess(), instrAddr + dispOff, sizeof(disp32));
    return true;
}

int32_t readBuildingType(const uint8_t* building) noexcept
{
    if (!building) {
        return -1;
    }
    const uint8_t* desc = wrsr::game::readField<const uint8_t*>(
        building, g_gameCtx.layout.bldDescOff);
    if (!desc) {
        return -1;
    }
    return wrsr::game::readField<int32_t>(desc, g_gameCtx.layout.descTypeOff);
}

// ── Detour 1: Snowplow Dispatch (RoadNode_HasSnow) ────────────────────────
bool __fastcall hkRoadNodeHasSnow(void* req, void* node)
{
    if (!req || !node) {
        return g_origRoadNodeHasSnow(req, node);
    }

    auto& cfg = Config::get();
    if (!cfg.isOverrideEnabled()) {
        return g_origRoadNodeHasSnow(req, node);
    }

    auto* reqBytes = static_cast<uint8_t*>(req);
    const int32_t mode = wrsr::game::readField<int32_t>(reqBytes, g_off.reqSearchModeOff);
    if (mode != g_off.snowSearchModeVal) {
        return g_origRoadNodeHasSnow(req, node);
    }

    const float pct = cfg.getSnowplowMinPct();
    const int32_t byteThresh = std::clamp<int32_t>(
        static_cast<int32_t>(std::lround(pct * 255.0f / 100.0f)) - 1, 0, 254);

    const int32_t origMin = wrsr::game::readField<int32_t>(reqBytes, g_off.reqMinSnowByteOff);
    wrsr::game::writeField<int32_t>(reqBytes, g_off.reqMinSnowByteOff, byteThresh);

    const bool hasSnow = g_origRoadNodeHasSnow(req, node);
    wrsr::game::writeField<int32_t>(reqBytes, g_off.reqMinSnowByteOff, origMin);

    if (!hasSnow) {
        g_gatedCount.fetch_add(1, std::memory_order_relaxed);
    }
    return hasSnow;
}

// ── Detour 2: Water Delivery (FindWaterTarget) ────────────────────────────
uint8_t* __fastcall hkFindWaterTarget(void* gameObj, uint8_t* techOffice)
{
    auto& cfg = Config::get();
    if (!cfg.isOverrideEnabled() || !techOffice || !g_getWaterFillRatio || !g_isWaterTruckEnRoute) {
        return g_origFindWaterTarget(gameObj, techOffice);
    }

    __try {
        const auto* begin = wrsr::game::readField<uint8_t* const*>(techOffice, g_off.techTargetsBeginOff);
        const auto* end   = wrsr::game::readField<uint8_t* const*>(techOffice, g_off.techTargetsEndOff);
        if (!begin || !end || end <= begin) {
            return nullptr;
        }

        const float threshold = cfg.getWaterBelowPct() / 100.0f;
        uint8_t* bestBld      = nullptr;
        float    bestRatio    = -1.0f;

        for (const auto* it = begin; it < end; ++it) {
            uint8_t* bld = *it;
            if (!bld) {
                continue;
            }
            const int32_t btype = readBuildingType(bld);
            if (btype < 0 || btype == 0x14 || btype == 0x5C || btype == 0x61) {
                continue;
            }
            const uint8_t pipedWater = wrsr::game::readField<uint8_t>(bld, g_off.bldPipedWaterOff);
            if (pipedWater != 0 && btype != 0x5B) {
                continue;
            }

            const float ratio = g_getWaterFillRatio(gameObj, bld);
            if (ratio < 0.0f || !(threshold > ratio)) {
                continue;
            }
            if (!(bestRatio > ratio) && !(0.0f > bestRatio)) {
                continue;
            }
            const float metric = wrsr::game::readField<float>(bld, g_off.bldWaterMetricOff);
            if (metric > 18.0f) {
                continue;
            }
            if (g_isWaterTruckEnRoute(bld)) {
                continue;
            }
            bestRatio = ratio;
            bestBld   = bld;
        }
        return bestBld;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return g_origFindWaterTarget(gameObj, techOffice);
    }
}

// ── Detour 3: Sewage Pickup (FindSewageTarget) ────────────────────────────
uint8_t* __fastcall hkFindSewageTarget(void* gameObj, uint8_t* techOffice)
{
    auto& cfg = Config::get();
    if (!cfg.isOverrideEnabled() || !techOffice || !g_getSewageFillRatio || !g_isSewageTruckEnRoute) {
        return g_origFindSewageTarget(gameObj, techOffice);
    }

    __try {
        const auto* begin = wrsr::game::readField<uint8_t* const*>(techOffice, g_off.techTargetsBeginOff);
        const auto* end   = wrsr::game::readField<uint8_t* const*>(techOffice, g_off.techTargetsEndOff);
        if (!begin || !end || end <= begin) {
            return nullptr;
        }

        const float threshold = cfg.getSewageMinPct() / 100.0f;
        uint8_t* bestBld      = nullptr;
        float    bestRatio    = -1.0f;

        for (const auto* it = begin; it < end; ++it) {
            uint8_t* bld = *it;
            if (!bld) {
                continue;
            }
            const int32_t btype = readBuildingType(bld);
            if (btype < 0 || btype == 0x14 || btype == 0x62) {
                continue;
            }
            const uint8_t pipedSewage = wrsr::game::readField<uint8_t>(bld, g_off.bldPipedSewageOff);
            if (pipedSewage != 0 && btype != 0x5E) {
                continue;
            }

            const float ratio = g_getSewageFillRatio(gameObj, bld, 0);
            if (!(ratio > threshold)) {
                continue;
            }
            if (!(ratio > bestRatio) && !(0.0f > bestRatio)) {
                continue;
            }
            if (g_isSewageTruckEnRoute(bld)) {
                continue;
            }
            bestRatio = ratio;
            bestBld   = bld;
        }
        return bestBld;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return g_origFindSewageTarget(gameObj, techOffice);
    }
}

void updateStatusSummary() noexcept
{
    int active  = 0;
    int standby = 0;
    const HookState states[] = {g_snowState, g_waterState, g_sewageState, g_wasteState};
    for (HookState s : states) {
        if (s == HookState::Active) ++active;
        else if (s == HookState::Standby) ++standby;
    }

    if (active == 4) {
        g_state.store(HookState::Active, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hooks: 4/4 ACTIVE (Snow, Garbage, Sewage, Water | profile: %s)",
                      g_gameCtx.layout.profileName.c_str());
    } else if (active > 0) {
        g_state.store(HookState::Active, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hooks: %d/4 ACTIVE, %d STANDBY (profile: %s)",
                      active, standby, g_gameCtx.layout.profileName.c_str());
    } else {
        g_state.store(HookState::Standby, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hooks: STANDBY (0/4 active — check TechServiceControl.log)");
    }
}

} // namespace

void syncThresholds() noexcept
{
    if (!g_wasteThresholds) {
        return;
    }
    auto& cfg = Config::get();
    if (cfg.isOverrideEnabled()) {
        const float ratio = cfg.getGarbageMinPct() / 100.0f;
        g_wasteThresholds[0] = ratio;
        g_wasteThresholds[1] = ratio;
    } else {
        g_wasteThresholds[0] = kVanillaMixedWasteThreshold;
        g_wasteThresholds[1] = kVanillaFracWasteThreshold;
    }
}

bool install(uint8_t* exeBase, size_t exeSize)
{
    auto ctxOpt = wrsr::game::GameContext::initialize(
        exeBase, exeSize, wrsr::generated::kEmbeddedOffsetsIni);
    if (!ctxOpt) {
        g_state.store(HookState::Failed, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hooks: FAILED — GameContext::initialize failed");
        wrsr::log::error("TechServiceControl: GameContext::initialize failed");
        return false;
    }
    g_gameCtx = std::move(*ctxOpt);

    const auto& prof = g_gameCtx.profile;
    uint32_t tmpU32 = 0;
    int32_t  tmpI32 = 0;
    if (prof.getU32("tech_targets_begin_off", tmpU32)) g_off.techTargetsBeginOff = tmpU32;
    if (prof.getU32("tech_targets_end_off",   tmpU32)) g_off.techTargetsEndOff   = tmpU32;
    if (prof.getU32("bld_piped_water_off",    tmpU32)) g_off.bldPipedWaterOff    = tmpU32;
    if (prof.getU32("bld_piped_sewage_off",   tmpU32)) g_off.bldPipedSewageOff   = tmpU32;
    if (prof.getU32("bld_water_metric_off",   tmpU32)) g_off.bldWaterMetricOff   = tmpU32;
    if (prof.getU32("req_min_snow_byte_off",  tmpU32)) g_off.reqMinSnowByteOff   = tmpU32;
    if (prof.getU32("req_search_mode_off",    tmpU32)) g_off.reqSearchModeOff    = tmpU32;
    if (prof.getI32("snow_search_mode_val",   tmpI32)) g_off.snowSearchModeVal   = tmpI32;

    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        g_state.store(HookState::Failed, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hooks: FAILED (MH_Initialize %d)", static_cast<int>(mhInit));
        wrsr::log::error("MH_Initialize failed: %d", static_cast<int>(mhInit));
        return false;
    }

    auto resolveOrScan = [&](const char* grmlKey, const char* legacySig) -> uint8_t* {
        uintptr_t val = 0;
        if (wrsr::game::resolveNamedOffset(grmlKey, val) && val != 0) {
            return reinterpret_cast<uint8_t*>(val);
        }
        return g_gameCtx.scanSignature(legacySig);
    };

    // ── 1. Snowplow Dispatch (RoadNode_HasSnow) ───────────────────────────
    if (!isPendingSig(g_gameCtx.offsets, "road_node_has_snow_aob")) {
        uint8_t* snowAddr = resolveOrScan("road_node_has_snow", "road_node_has_snow_aob");
        if (snowAddr) {
            if (MH_CreateHook(snowAddr,
                              reinterpret_cast<void*>(&hkRoadNodeHasSnow),
                              reinterpret_cast<void**>(&g_origRoadNodeHasSnow)) == MH_OK &&
                MH_EnableHook(snowAddr) == MH_OK) {
                g_snowState = HookState::Active;
                wrsr::log::info("Snow hook ACTIVE @ SOVIET64.exe+0x%zX",
                                static_cast<size_t>(snowAddr - exeBase));
            }
        } else {
            wrsr::log::warn("Snow AOB did not match uniquely — STANDBY");
        }
    }

    // ── 2. Water Delivery (FindWaterTarget) ───────────────────────────────
    if (!isPendingSig(g_gameCtx.offsets, "find_water_target_aob")) {
        uint8_t* waterAddr = resolveOrScan("find_water_target", "find_water_target_aob");
        if (waterAddr) {
            const uint32_t ratioDispOff = g_gameCtx.offsets.getU32("signatures", "water_ratio_disp_off").value_or(170);
            const uint32_t ratioNextOff = g_gameCtx.offsets.getU32("signatures", "water_ratio_next_ip_off").value_or(174);
            const uint32_t enrDispOff   = g_gameCtx.offsets.getU32("signatures", "water_enroute_disp_off").value_or(207);
            const uint32_t enrNextOff   = g_gameCtx.offsets.getU32("signatures", "water_enroute_next_ip_off").value_or(211);
            g_getWaterFillRatio   = reinterpret_cast<PFN_GetWaterFillRatio>(
                resolveCallRel32(waterAddr, ratioDispOff, ratioNextOff));
            g_isWaterTruckEnRoute = reinterpret_cast<PFN_IsTruckEnRoute>(
                resolveCallRel32(waterAddr, enrDispOff, enrNextOff));

            if (MH_CreateHook(waterAddr,
                              reinterpret_cast<void*>(&hkFindWaterTarget),
                              reinterpret_cast<void**>(&g_origFindWaterTarget)) == MH_OK &&
                MH_EnableHook(waterAddr) == MH_OK) {
                g_waterState = HookState::Active;
                wrsr::log::info("Water hook ACTIVE @ SOVIET64.exe+0x%zX",
                                static_cast<size_t>(waterAddr - exeBase));
            }
        } else {
            wrsr::log::warn("Water AOB did not match uniquely — STANDBY");
        }
    }

    // ── 3. Sewage Pickup (FindSewageTarget) ───────────────────────────────
    if (!isPendingSig(g_gameCtx.offsets, "find_sewage_target_aob")) {
        uint8_t* sewageAddr = resolveOrScan("find_sewage_target", "find_sewage_target_aob");
        if (sewageAddr) {
            const uint32_t ratioDispOff = g_gameCtx.offsets.getU32("signatures", "sewage_ratio_disp_off").value_or(168);
            const uint32_t ratioNextOff = g_gameCtx.offsets.getU32("signatures", "sewage_ratio_next_ip_off").value_or(172);
            const uint32_t enrDispOff   = g_gameCtx.offsets.getU32("signatures", "sewage_enroute_disp_off").value_or(191);
            const uint32_t enrNextOff   = g_gameCtx.offsets.getU32("signatures", "sewage_enroute_next_ip_off").value_or(195);
            g_getSewageFillRatio   = reinterpret_cast<PFN_GetSewageFillRatio>(
                resolveCallRel32(sewageAddr, ratioDispOff, ratioNextOff));
            g_isSewageTruckEnRoute = reinterpret_cast<PFN_IsTruckEnRoute>(
                resolveCallRel32(sewageAddr, enrDispOff, enrNextOff));

            if (MH_CreateHook(sewageAddr,
                              reinterpret_cast<void*>(&hkFindSewageTarget),
                              reinterpret_cast<void**>(&g_origFindSewageTarget)) == MH_OK &&
                MH_EnableHook(sewageAddr) == MH_OK) {
                g_sewageState = HookState::Active;
                wrsr::log::info("Sewage hook ACTIVE @ SOVIET64.exe+0x%zX",
                                static_cast<size_t>(sewageAddr - exeBase));
            }
        } else {
            wrsr::log::warn("Sewage AOB did not match uniquely — STANDBY");
        }
    }

    // ── 4. Garbage / Waste Pickup (FindWasteTarget threshold load block) ──
    if (!isPendingSig(g_gameCtx.offsets, "waste_threshold_block_aob")) {
        uint8_t* wasteAddr = resolveOrScan("waste_threshold_block", "waste_threshold_block_aob");
        if (wasteAddr) {
            g_wasteThresholds = allocNearModuleFloats(wasteAddr);
            if (g_wasteThresholds) {
                syncThresholds();
                const uint32_t mixedDispOff = g_gameCtx.offsets.getU32("signatures", "waste_mixed_disp_off").value_or(5);
                const uint32_t mixedNextOff = g_gameCtx.offsets.getU32("signatures", "waste_mixed_next_ip_off").value_or(9);
                const uint32_t fracDispOff  = g_gameCtx.offsets.getU32("signatures", "waste_frac_disp_off").value_or(47);
                const uint32_t fracNextOff  = g_gameCtx.offsets.getU32("signatures", "waste_frac_next_ip_off").value_or(51);
                if (patchRipDisp32(wasteAddr, mixedDispOff, mixedNextOff, &g_wasteThresholds[0]) &&
                    patchRipDisp32(wasteAddr, fracDispOff,  fracNextOff,  &g_wasteThresholds[1])) {
                    g_wasteState = HookState::Active;
                    wrsr::log::info("Waste threshold patch ACTIVE @ SOVIET64.exe+0x%zX",
                                    static_cast<size_t>(wasteAddr - exeBase));
                }
            }
        } else {
            wrsr::log::warn("Waste AOB did not match uniquely — STANDBY");
        }
    }

    updateStatusSummary();
    return g_state.load(std::memory_order_relaxed) == HookState::Active;
}

void shutdown()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_state.store(HookState::Uninitialized, std::memory_order_relaxed);
}

HookState state() noexcept
{
    return g_state.load(std::memory_order_relaxed);
}

const char* statusText() noexcept
{
    return g_statusText;
}

uint64_t gatedCount() noexcept
{
    return g_gatedCount.load(std::memory_order_relaxed);
}

} // namespace tsc::hook
