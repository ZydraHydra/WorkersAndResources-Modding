// ============================================================================
// bay_access_hook.cpp — SmartBayAccess RoadVehicleTick & Save-Flush Hooks
// ============================================================================
// Eliminates single-vehicle courtyard bottlenecks at multi-bay road buildings
// (cargo stations, depots, Customs Houses, bus platforms) by batching apron
// gate access while enforcing four strict engine-safety invariants:
//   1. Multi-Bay Road Buildings Only (is_road_vehicle && bay_count > 1).
//   2. External Idempotent Tracking (std::unordered_map<void*, BuildingTraffic>)
//      with automatic flush on save load/unload (CreateFileW *\media_soviet\save\*\header.bin).
//   3. Preserve Per-Bay Slot Reservations & Pin Vanilla Lock (+0xBB0) until
//      entering_vehicles.empty() && exiting_vehicles.empty().
//   4. Operating Modes (0 = Vanilla, 1 = Same-Direction Batching, 2 = Unrestricted).
// ============================================================================
#include "bay_access_hook.hpp"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "embedded_offsets.hpp"
#include "wrsr/game_signatures.hpp"
#include "wrsr/logger.hpp"
#include "config.hpp"

namespace sba::hook {

namespace {

// ── Mutable hook & diagnostic state ───────────────────────────────────────
std::atomic<HookState> g_state{HookState::Uninitialized};
std::atomic<uint32_t>  g_activeBuildings{0};
std::atomic<uint64_t>  g_batchedEntriesAllowed{0};
std::atomic<bool>      g_flushRequested{false};

char g_statusText[256]           = "Hook: Initializing...";
char g_trackedBuildingsText[96]  = "Active Tracked Buildings: 0";
char g_batchedEntriesText[96]    = "Batched Entries Allowed: 0";

wrsr::game::GameContext g_gameCtx{};

struct BayAccessOffsets {
    uint32_t vehPullingOff{0x398};
    uint32_t vehCurrBldOff{0x4F0};
    uint32_t vehEnteringBayOff{0x510};
    uint32_t vehTargetBldOff{0x528};
    uint32_t vehParkedBayOff{0x56C};
    uint32_t vehApronBldOff{0xC80};
    uint32_t vehTypeOff{0x1748};
    uint32_t vehTypeCategoryOff{0x294};
    int32_t  railCategoryMin{3};
    int32_t  railCategoryMax{5};
    uint32_t bldParkBaysBeginOff{0xA40};
    uint32_t bldParkBaysEndOff{0xA48};
    uint32_t bldBaysBeginOff{0xB78};
    uint32_t bldBaysEndOff{0xB80};
    uint32_t bldActiveMoverOff{0xBB0};
    uint32_t descNoApronLockOff{0xB89};
};
BayAccessOffsets g_off{};

// ── External idempotent tracking (Invariant 2) ────────────────────────────
struct BuildingTraffic {
    std::unordered_set<void*> entering_vehicles;
    std::unordered_set<void*> exiting_vehicles;
};

std::mutex                                  g_trafficMutex;
std::unordered_map<void*, BuildingTraffic>  g_buildings;

void updateDiagnosticLabels() noexcept
{
    const uint32_t bldCnt = static_cast<uint32_t>(g_buildings.size());
    g_activeBuildings.store(bldCnt, std::memory_order_relaxed);
    std::snprintf(g_trackedBuildingsText, sizeof(g_trackedBuildingsText),
                  "Active Tracked Buildings: %u", bldCnt);

    const uint64_t batched = g_batchedEntriesAllowed.load(std::memory_order_relaxed);
    std::snprintf(g_batchedEntriesText, sizeof(g_batchedEntriesText),
                  "Batched Entries Allowed: %llu",
                  static_cast<unsigned long long>(batched));
}

void flushTrackingMapLocked() noexcept
{
    g_buildings.clear();
    updateDiagnosticLabels();
}

// ── Engine function pointers ──────────────────────────────────────────────
using PFN_RoadVehicleTick = uint64_t(__fastcall*)(
    uint8_t* veh, uint64_t arg2, uint64_t arg3, uint64_t arg4);
PFN_RoadVehicleTick g_origRoadVehicleTick        = nullptr;
PFN_RoadVehicleTick g_origRoadVehicleBldMove     = nullptr;
PFN_RoadVehicleTick g_origRoadVehicleParkingTick = nullptr;

using PFN_CreateFileW = HANDLE(WINAPI*)(
    LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
PFN_CreateFileW g_origCreateFileW = nullptr;

// ── SEH-guarded struct helpers ────────────────────────────────────────────
bool isRoadVehicle(const uint8_t* veh) noexcept
{
    __try {
        if (!veh) {
            return false;
        }
        const uint8_t* pulling = wrsr::game::readField<const uint8_t*>(veh, g_off.vehPullingOff);
        if (pulling != nullptr) {
            return false;
        }
        const uint8_t* vehType = wrsr::game::readField<const uint8_t*>(veh, g_off.vehTypeOff);
        if (vehType) {
            const int32_t cat = wrsr::game::readField<int32_t>(vehType, g_off.vehTypeCategoryOff);
            if (cat >= g_off.railCategoryMin && cat <= g_off.railCategoryMax) {
                return false;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct BuildingProbe {
    bool     eligible{false};
    int32_t  bayCount{0};
    uint8_t* activeMover{nullptr};
};

bool probeBuilding(const uint8_t* bld, BuildingProbe& out) noexcept
{
    __try {
        if (!bld) {
            return false;
        }
        const auto& L = g_gameCtx.layout;
        const uint8_t* desc = wrsr::game::readField<const uint8_t*>(bld, L.bldDescOff);
        if (desc) {
            const uint8_t noLock = wrsr::game::readField<uint8_t>(desc, g_off.descNoApronLockOff);
            if (noLock != 0) {
                return false;
            }
        }
        const uintptr_t baysBegin = wrsr::game::readField<uintptr_t>(bld, g_off.bldBaysBeginOff);
        const uintptr_t baysEnd   = wrsr::game::readField<uintptr_t>(bld, g_off.bldBaysEndOff);
        ptrdiff_t bayCountB78 = 0;
        if (baysBegin && baysEnd > baysBegin) {
            bayCountB78 = static_cast<ptrdiff_t>((baysEnd - baysBegin) >> 5);
        }

        const uintptr_t parkBegin = wrsr::game::readField<uintptr_t>(bld, g_off.bldParkBaysBeginOff);
        const uintptr_t parkEnd   = wrsr::game::readField<uintptr_t>(bld, g_off.bldParkBaysEndOff);
        ptrdiff_t bayCountA40 = 0;
        if (parkBegin && parkEnd > parkBegin) {
            bayCountA40 = static_cast<ptrdiff_t>((parkEnd - parkBegin) >> 5);
        }

        const ptrdiff_t bayCount = (bayCountB78 > bayCountA40) ? bayCountB78 : bayCountA40;
        if (bayCount <= 1 || bayCount > 64) {
            return false;
        }
        out.bayCount    = static_cast<int32_t>(bayCount);
        out.activeMover = wrsr::game::readField<uint8_t*>(bld, g_off.bldActiveMoverOff);
        out.eligible    = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct VehicleApronState {
    bool     valid{false};
    uint8_t* currBld{nullptr};
    uint8_t* targetBld{nullptr};
    uint8_t* apronBld{nullptr};
    int32_t  enteringBay{-1};
    int32_t  parkedBay{-1};
};

bool readVehicleApronState(const uint8_t* veh, VehicleApronState& out) noexcept
{
    __try {
        if (!veh) {
            return false;
        }
        out.currBld     = wrsr::game::readField<uint8_t*>(veh, g_off.vehCurrBldOff);
        out.targetBld   = wrsr::game::readField<uint8_t*>(veh, g_off.vehTargetBldOff);
        out.apronBld    = wrsr::game::readField<uint8_t*>(veh, g_off.vehApronBldOff);
        out.enteringBay = wrsr::game::readField<int32_t>(veh, g_off.vehEnteringBayOff);
        out.parkedBay   = wrsr::game::readField<int32_t>(veh, g_off.vehParkedBayOff);
        out.valid       = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeActiveMover(uint8_t* bld, uint8_t* mover) noexcept
{
    __try {
        if (!bld) {
            return false;
        }
        *reinterpret_cast<uint8_t**>(bld + g_off.bldActiveMoverOff) = mover;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── Shared Apron Batching Wrapper ─────────────────────────────────────────
uint64_t runWithApronBatching(
    PFN_RoadVehicleTick origFn,
    uint8_t* veh, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    static thread_local int t_depth = 0;
    if (t_depth > 0) {
        return origFn(veh, arg2, arg3, arg4);
    }

    if (g_flushRequested.exchange(false, std::memory_order_relaxed)) {
        std::lock_guard lock(g_trafficMutex);
        flushTrackingMapLocked();
    }

    const BayAccessMode mode = Config::get().getMode();
    if (mode == BayAccessMode::Vanilla || !isRoadVehicle(veh)) {
        return origFn(veh, arg2, arg3, arg4);
    }

    VehicleApronState pre{};
    if (!readVehicleApronState(veh, pre)) {
        return origFn(veh, arg2, arg3, arg4);
    }

    uint8_t* const bld = pre.targetBld ? pre.targetBld
                       : (pre.currBld ? pre.currBld : pre.apronBld);
    BuildingProbe  bldProbe{};
    if (!bld || !probeBuilding(bld, bldProbe) || !bldProbe.eligible) {
        return origFn(veh, arg2, arg3, arg4);
    }

    bool     spoofedGate = false;
    bool     wasWantsEntry = false;
    bool     wasWantsExit  = false;
    uint8_t* savedMover  = bldProbe.activeMover;

    {
        std::lock_guard lock(g_trafficMutex);

        // Seed existing active mover if it started before our hook/mode switch
        if (savedMover && savedMover != veh) {
            VehicleApronState moverState{};
            if (readVehicleApronState(savedMover, moverState) && moverState.apronBld == bld) {
                auto& traffic = g_buildings[bld];
                if (!traffic.entering_vehicles.count(savedMover) &&
                    !traffic.exiting_vehicles.count(savedMover)) {
                    if (moverState.enteringBay >= 0 || moverState.parkedBay >= 0) {
                        traffic.entering_vehicles.insert(savedMover);
                    } else {
                        traffic.exiting_vehicles.insert(savedMover);
                    }
                }
            }
        }

        if (pre.apronBld == nullptr && (pre.targetBld == bld || pre.currBld == bld)) {
            wasWantsEntry = (pre.enteringBay == -1 && pre.parkedBay == -1);
            wasWantsExit  = (pre.parkedBay >= 0 || pre.parkedBay <= -10);

            if (wasWantsEntry || wasWantsExit) {
                bool allowed = false;
                if (mode == BayAccessMode::Unrestricted) {
                    allowed = true;
                } else if (mode == BayAccessMode::SameDirection) {
                    const auto it = g_buildings.find(bld);
                    if (it == g_buildings.end()) {
                        allowed = true;
                    } else if (wasWantsEntry) {
                        allowed = it->second.exiting_vehicles.empty();
                    } else {
                        allowed = it->second.entering_vehicles.empty();
                    }
                }

                if (allowed && savedMover != nullptr && savedMover != veh) {
                    if (writeActiveMover(bld, nullptr)) {
                        spoofedGate = true;
                    }
                }
            }
        }
    }

    ++t_depth;
    const uint64_t ret = origFn(veh, arg2, arg3, arg4);
    --t_depth;

    VehicleApronState post{};
    if (!readVehicleApronState(veh, post)) {
        if (spoofedGate && savedMover) {
            writeActiveMover(bld, savedMover);
        }
        return ret;
    }

    {
        std::lock_guard lock(g_trafficMutex);

        if (pre.apronBld != nullptr && post.apronBld != pre.apronBld) {
            const auto prevIt = g_buildings.find(pre.apronBld);
            if (prevIt != g_buildings.end()) {
                prevIt->second.entering_vehicles.erase(veh);
                prevIt->second.exiting_vehicles.erase(veh);
                if (prevIt->second.entering_vehicles.empty() &&
                    prevIt->second.exiting_vehicles.empty()) {
                    g_buildings.erase(prevIt);
                }
            }
        }

        if (post.apronBld == bld) {
            auto& traffic = g_buildings[bld];
            if (pre.apronBld != bld) {
                if (wasWantsExit) {
                    traffic.exiting_vehicles.insert(veh);
                } else {
                    traffic.entering_vehicles.insert(veh);
                }
                if (spoofedGate) {
                    g_batchedEntriesAllowed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else {
            const auto it = g_buildings.find(bld);
            if (it != g_buildings.end()) {
                it->second.entering_vehicles.erase(veh);
                it->second.exiting_vehicles.erase(veh);
                if (it->second.entering_vehicles.empty() &&
                    it->second.exiting_vehicles.empty()) {
                    g_buildings.erase(it);
                }
            }
        }

        const auto it = g_buildings.find(bld);
        if (it != g_buildings.end() &&
            (!it->second.entering_vehicles.empty() || !it->second.exiting_vehicles.empty())) {
            BuildingProbe postBld{};
            if (probeBuilding(bld, postBld) && postBld.activeMover == nullptr) {
                void* pinVeh = nullptr;
                if (savedMover &&
                    (it->second.entering_vehicles.count(savedMover) ||
                     it->second.exiting_vehicles.count(savedMover))) {
                    pinVeh = savedMover;
                } else if (!it->second.entering_vehicles.empty()) {
                    pinVeh = *it->second.entering_vehicles.begin();
                } else {
                    pinVeh = *it->second.exiting_vehicles.begin();
                }
                if (pinVeh) {
                    writeActiveMover(bld, static_cast<uint8_t*>(pinVeh));
                }
            }
        }

        updateDiagnosticLabels();
    }

    return ret;
}

uint64_t __fastcall detourRoadVehicleTick(
    uint8_t* veh, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    return runWithApronBatching(g_origRoadVehicleTick, veh, arg2, arg3, arg4);
}

uint64_t __fastcall detourRoadVehicleBldMove(
    uint8_t* veh, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    return runWithApronBatching(g_origRoadVehicleBldMove, veh, arg2, arg3, arg4);
}

uint64_t __fastcall detourRoadVehicleParkingTick(
    uint8_t* veh, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    return runWithApronBatching(g_origRoadVehicleParkingTick, veh, arg2, arg3, arg4);
}

// ── CreateFileW save load/unload flush hook (Invariant 2) ─────────────────
bool isSaveHeaderBinPath(const wchar_t* path) noexcept
{
    if (!path || !path[0]) {
        return false;
    }
    std::wstring lower;
    lower.reserve(128);
    for (size_t i = 0; path[i] && i < 1024; ++i) {
        wchar_t ch = path[i];
        if (ch == L'/') ch = L'\\';
        else if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
        lower.push_back(ch);
    }
    const std::wstring_view sv(lower);
    constexpr std::wstring_view kSaveMarker = L"media_soviet\\save\\";
    constexpr std::wstring_view kHeaderSuffix = L"\\header.bin";
    return sv.find(kSaveMarker) != std::wstring_view::npos &&
           sv.size() >= kHeaderSuffix.size() &&
           sv.compare(sv.size() - kHeaderSuffix.size(), kHeaderSuffix.size(), kHeaderSuffix) == 0;
}

HANDLE WINAPI detourCreateFileW(
    LPCWSTR               lpFileName,
    DWORD                 dwDesiredAccess,
    DWORD                 dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD                 dwCreationDisposition,
    DWORD                 dwFlagsAndAttributes,
    HANDLE                hTemplateFile)
{
    HANDLE h = g_origCreateFileW(
        lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
        dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    if (lpFileName && h != INVALID_HANDLE_VALUE && isSaveHeaderBinPath(lpFileName)) {
        resetTracking();
    }
    return h;
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────
bool install(uint8_t* exeBase, size_t exeSize)
{
    auto ctx = wrsr::game::GameContext::initialize(
        exeBase, exeSize, wrsr::generated::kEmbeddedOffsetsIni);
    if (!ctx) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (GameContext init failed)");
        return false;
    }

    uint32_t tmpU32 = 0;
    int32_t  tmpI32 = 0;
    if (ctx->profile.getU32("veh_pulling_off",        tmpU32)) g_off.vehPullingOff      = tmpU32;
    if (ctx->profile.getU32("veh_curr_bld_off",       tmpU32)) g_off.vehCurrBldOff      = tmpU32;
    if (ctx->profile.getU32("veh_entering_bay_off",   tmpU32)) g_off.vehEnteringBayOff  = tmpU32;
    if (ctx->profile.getU32("veh_target_bld_off",     tmpU32)) g_off.vehTargetBldOff    = tmpU32;
    if (ctx->profile.getU32("veh_parked_bay_off",     tmpU32)) g_off.vehParkedBayOff    = tmpU32;
    if (ctx->profile.getU32("veh_apron_bld_off",      tmpU32)) g_off.vehApronBldOff     = tmpU32;
    if (ctx->profile.getU32("veh_type_off",           tmpU32)) g_off.vehTypeOff         = tmpU32;
    if (ctx->profile.getU32("veh_type_category_off",  tmpU32)) g_off.vehTypeCategoryOff = tmpU32;
    if (ctx->profile.getI32("rail_category_min",      tmpI32)) g_off.railCategoryMin    = tmpI32;
    if (ctx->profile.getI32("rail_category_max",      tmpI32)) g_off.railCategoryMax    = tmpI32;
    if (ctx->profile.getU32("bld_park_bays_begin_off", tmpU32)) g_off.bldParkBaysBeginOff = tmpU32;
    if (ctx->profile.getU32("bld_park_bays_end_off",  tmpU32)) g_off.bldParkBaysEndOff  = tmpU32;
    if (ctx->profile.getU32("bld_bays_begin_off",     tmpU32)) g_off.bldBaysBeginOff    = tmpU32;
    if (ctx->profile.getU32("bld_bays_end_off",       tmpU32)) g_off.bldBaysEndOff      = tmpU32;
    if (ctx->profile.getU32("bld_active_mover_off",   tmpU32)) g_off.bldActiveMoverOff  = tmpU32;
    if (ctx->profile.getU32("desc_no_apron_lock_off", tmpU32)) g_off.descNoApronLockOff = tmpU32;

    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (MinHook init failed %d)", static_cast<int>(mhInit));
        return false;
    }

    auto resolveOrScan = [&](const char* grmlKey, const char* legacySig) -> uint8_t* {
        uintptr_t val = 0;
        if (wrsr::game::resolveNamedOffset(grmlKey, val) && val != 0) {
            return reinterpret_cast<uint8_t*>(val);
        }
        return ctx->scanSignature(legacySig);
    };
    uint8_t* tickAddr        = resolveOrScan("road_vehicle_tick", "road_vehicle_tick_aob");
    uint8_t* bldMoveAddr     = resolveOrScan("road_vehicle_bld_move", "road_vehicle_bld_move_aob");
    uint8_t* parkingTickAddr = resolveOrScan("road_vehicle_parking_tick", "road_vehicle_parking_tick_aob");
    uint8_t* entryGateAddr   = resolveOrScan("apron_entry_gate", "apron_entry_gate_aob");
    uint8_t* exitGateAddr    = resolveOrScan("apron_exit_gate", "apron_exit_gate_aob");

    if (!tickAddr) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: NOT MATCHED (RoadVehicleTick AOB failed)");
        wrsr::log::warn("sba: RoadVehicleTick AOB match failed — SmartBayAccess inactive");
        g_gameCtx = std::move(*ctx);
        return false;
    }

    if (MH_CreateHook(tickAddr,
                      reinterpret_cast<void*>(detourRoadVehicleTick),
                      reinterpret_cast<void**>(&g_origRoadVehicleTick)) != MH_OK ||
        MH_EnableHook(tickAddr) != MH_OK) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (MinHook attach failed)");
        wrsr::log::error("sba: MH_CreateHook/Enable failed for RoadVehicleTick");
        g_gameCtx = std::move(*ctx);
        return false;
    }

    if (bldMoveAddr &&
        MH_CreateHook(bldMoveAddr,
                      reinterpret_cast<void*>(detourRoadVehicleBldMove),
                      reinterpret_cast<void**>(&g_origRoadVehicleBldMove)) == MH_OK) {
        MH_EnableHook(bldMoveAddr);
        wrsr::log::info("sba: hooked RoadVehicleBldMove @ RVA 0x%zX",
                        static_cast<size_t>(bldMoveAddr - exeBase));
    }

    if (parkingTickAddr &&
        MH_CreateHook(parkingTickAddr,
                      reinterpret_cast<void*>(detourRoadVehicleParkingTick),
                      reinterpret_cast<void**>(&g_origRoadVehicleParkingTick)) == MH_OK) {
        MH_EnableHook(parkingTickAddr);
        wrsr::log::info("sba: hooked RoadVehicleParkingTick @ RVA 0x%zX",
                        static_cast<size_t>(parkingTickAddr - exeBase));
    }

    // Hook CreateFileW for automatic save load/unload tracking flush
    void* createFileTarget = nullptr;
    MH_STATUS cfSt = MH_CreateHookApiEx(
        L"kernelbase.dll", "CreateFileW",
        reinterpret_cast<void*>(detourCreateFileW),
        reinterpret_cast<void**>(&g_origCreateFileW),
        &createFileTarget);
    if (cfSt != MH_OK) {
        cfSt = MH_CreateHookApiEx(
            L"kernel32.dll", "CreateFileW",
            reinterpret_cast<void*>(detourCreateFileW),
            reinterpret_cast<void**>(&g_origCreateFileW),
            &createFileTarget);
    }
    if (cfSt == MH_OK && createFileTarget) {
        MH_EnableHook(createFileTarget);
        wrsr::log::info("sba: hooked CreateFileW for save-flush tracking");
    } else {
        wrsr::log::warn("sba: CreateFileW hook failed (%d) — manual Reset Tracking still available",
                        static_cast<int>(cfSt));
    }

    const size_t rva = static_cast<size_t>(tickAddr - exeBase);
    g_state.store(HookState::Active, std::memory_order_relaxed);
    std::snprintf(g_statusText, sizeof(g_statusText),
                  "Hook: ACTIVE @ SOVIET64.exe+0x%zX (profile: %s)",
                  rva, ctx->layout.profileName.c_str());
    wrsr::log::info(
        "sba: hooked RoadVehicleTick @ RVA 0x%zX (entryGate=%p exitGate=%p profile=%s)",
        rva, entryGateAddr, exitGateAddr, ctx->layout.profileName.c_str());

    g_gameCtx = std::move(*ctx);
    return true;
}

HookState state() noexcept
{
    return g_state.load(std::memory_order_relaxed);
}

const char* statusText() noexcept
{
    return g_statusText;
}

const char* trackedBuildingsText() noexcept
{
    return g_trackedBuildingsText;
}

const char* batchedEntriesText() noexcept
{
    return g_batchedEntriesText;
}

uint32_t activeTrackedBuildings() noexcept
{
    return g_activeBuildings.load(std::memory_order_relaxed);
}

uint64_t batchedEntriesAllowed() noexcept
{
    return g_batchedEntriesAllowed.load(std::memory_order_relaxed);
}

void resetTracking() noexcept
{
    g_flushRequested.store(true, std::memory_order_relaxed);
    std::lock_guard lock(g_trafficMutex);
    flushTrackingMapLocked();
}

} // namespace sba::hook
