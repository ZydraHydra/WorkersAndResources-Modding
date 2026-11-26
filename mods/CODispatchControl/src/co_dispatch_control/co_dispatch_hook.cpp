// ============================================================================
// co_dispatch_hook.cpp — CODispatchControl DispatchCOTask detour
// ============================================================================
// Hooks DispatchCOTask(GameObject*, COTask64*, Building* coBuilding) -> bool
// to optimize Construction Office vehicle dispatch:
//   1. Never blocks mechanism/excavator/flatbed transport.
//   2. Min Material Truck Fill %: avoids sending oversized trucks for small
//      remaining material quantities when a smaller capable truck exists in
//      the CO (exempts final top-offs below final_topoff_floor_tons).
//   3. Min Bus Workday Ratio: avoids sending large buses for tiny remaining
//      workday counts when a smaller capable bus/microbus exists in the CO.
//   4. Max Trucks En Route per Site: caps simultaneous material trucks heading
//      to the same construction site to prevent road-node traffic jams.
// ============================================================================
#include "co_dispatch_hook.hpp"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include "embedded_offsets.hpp"
#include "wrsr/game_signatures.hpp"
#include "wrsr/logger.hpp"
#include "config.hpp"

namespace cdc::hook {

namespace {

// ── Mutable state ─────────────────────────────────────────────────────────
std::atomic<HookState> g_state{HookState::Uninitialized};
std::atomic<uint64_t>  g_rejectedCount{0};
char                   g_statusText[256] = "Hook: Initializing...";

wrsr::game::GameContext g_gameCtx{};

struct CoOffsets {
    uint32_t taskSiteBldOff{0x00};
    uint32_t taskSrcBldOff{0x08};
    uint32_t taskVehOff{0x10};
    uint32_t taskCarrierOff{0x18};
    uint32_t taskResOff{0x20};
    uint32_t taskRemainingOff{0x34};
    uint32_t coVehiclesOff{0xC70};
    uint32_t gameWorkerResOff{0xC368};
    uint32_t vehCurrBldOff{0x4F0};
    uint32_t vehHomeBldOff{0x4F8};
    uint32_t vehDispatchStateOff{0x50C};
    uint32_t vehTypeOff{0x1748};
    uint32_t vehTypePaxClassOff{0x8650};
    uint32_t vehTypePaxCapOff{0x8654};
    int32_t  paxClassValue{7};
};
CoOffsets g_off{};

// ── Engine function pointers ──────────────────────────────────────────────
using PFN_DispatchCOTask = bool(__fastcall*)(
    void* gameObj, uint8_t* task, uint8_t* coBuilding);
using PFN_GetVehicleResourceCapacity = float(__fastcall*)(
    uint8_t* vehicle, const uint8_t* resource, int32_t a3, int32_t a4);

PFN_DispatchCOTask             g_origDispatchCOTask         = nullptr;
PFN_GetVehicleResourceCapacity g_getVehicleResourceCapacity = nullptr;

// ── Active en-route material truck tracker ────────────────────────────────
struct EnRouteEntry {
    uint8_t* siteBuilding{nullptr};
    uint64_t timestampMs{0};
    bool     hasLeftHome{false};
};

std::mutex                                  g_enRouteMutex;
std::unordered_map<uint8_t*, EnRouteEntry>  g_enRouteVehicles;

constexpr uint64_t kEnRouteGraceMs  = 5000ULL;   // Allow time to exit CO gate after dispatch
constexpr uint64_t kEnRouteMaxAgeMs = 300000ULL; // 5-minute safety TTL

bool isVehicleStillEnRoute(const uint8_t* veh, EnRouteEntry& entry, uint64_t now) noexcept
{
    __try {
        if (!veh) {
            return false;
        }
        const uint64_t age = now - entry.timestampMs;
        if (age > kEnRouteMaxAgeMs) {
            return false;
        }

        const uint8_t* currBld = wrsr::game::readField<const uint8_t*>(veh, g_off.vehCurrBldOff);
        const uint8_t* homeBld = wrsr::game::readField<const uint8_t*>(veh, g_off.vehHomeBldOff);

        if (homeBld && currBld != homeBld) {
            entry.hasLeftHome = true;
            return true;
        }

        // Still inside home CO: keep during initial exit grace window, prune once returned
        if (!entry.hasLeftHome && age < kEnRouteGraceMs) {
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int countActiveTrucksForSite(const uint8_t* siteBuilding) noexcept
{
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_enRouteMutex);

    int count = 0;
    for (auto it = g_enRouteVehicles.begin(); it != g_enRouteVehicles.end();) {
        if (!isVehicleStillEnRoute(it->first, it->second, now)) {
            it = g_enRouteVehicles.erase(it);
            continue;
        }
        if (it->second.siteBuilding == siteBuilding) {
            ++count;
        }
        ++it;
    }
    return count;
}

void recordDispatchedTruck(uint8_t* veh, uint8_t* siteBuilding) noexcept
{
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_enRouteMutex);
    g_enRouteVehicles[veh] = EnRouteEntry{siteBuilding, now, false};
}

// ── Vehicle capacity helpers (SEH-safe) ───────────────────────────────────
float queryPassengerCapacity(uint8_t* veh, const uint8_t* workerRes) noexcept
{
    __try {
        const uint8_t* vehType = wrsr::game::readField<const uint8_t*>(veh, g_off.vehTypeOff);
        if (vehType) {
            const int32_t cls = wrsr::game::readField<int32_t>(vehType, g_off.vehTypePaxClassOff);
            if (cls == g_off.paxClassValue) {
                const float cap = wrsr::game::readField<float>(vehType, g_off.vehTypePaxCapOff);
                if (std::isfinite(cap) && cap > 0.f) {
                    return cap;
                }
            }
        }
        if (g_getVehicleResourceCapacity && workerRes) {
            const float cap = g_getVehicleResourceCapacity(veh, workerRes, 1, 0);
            if (std::isfinite(cap) && cap > 0.f) {
                return cap;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0.f;
}

float queryMaterialCapacity(uint8_t* veh, const uint8_t* res) noexcept
{
    __try {
        if (!veh || !res || !g_getVehicleResourceCapacity) {
            return 0.f;
        }
        const float cap = g_getVehicleResourceCapacity(veh, res, 1, 0);
        return (std::isfinite(cap) && cap > 0.f) ? cap : 0.f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.f;
    }
}

bool coHasSmallerCapableVehicle(
    const uint8_t* coBuilding,
    const uint8_t* currentVeh,
    const uint8_t* res,
    bool           isWorkers,
    float          currentCap) noexcept
{
    __try {
        if (!coBuilding) {
            return false;
        }
        const auto* vecBegin = wrsr::game::readField<uint8_t* const*>(coBuilding, g_off.coVehiclesOff);
        const auto* vecEnd   = wrsr::game::readField<uint8_t* const*>(coBuilding, g_off.coVehiclesOff + 8);
        if (!vecBegin || !vecEnd || vecEnd <= vecBegin) {
            return false;
        }
        const ptrdiff_t count = vecEnd - vecBegin;
        if (count <= 1 || count > 256) {
            return false;
        }

        const float threshold = currentCap - (isWorkers ? 0.5f : 0.05f);
        for (ptrdiff_t i = 0; i < count; ++i) {
            uint8_t* cand = vecBegin[i];
            if (!cand || cand == currentVeh) {
                continue;
            }
            const float cap = isWorkers
                ? queryPassengerCapacity(cand, res)
                : queryMaterialCapacity(cand, res);
            if (cap > 0.f && cap < threshold) {
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// ── SEH-guarded task probe ────────────────────────────────────────────────
struct CoTaskProbe {
    bool           valid{false};
    bool           isMechanism{false};
    bool           isWorkers{false};
    uint8_t*       siteBuilding{nullptr};
    uint8_t*       vehicle{nullptr};
    const uint8_t* resource{nullptr};
    float          remainingNeeded{0.f};
    float          vehicleCapacity{0.f};
};

bool tryProbeCoTask(void* gameObj, uint8_t* task, CoTaskProbe& out) noexcept
{
    __try {
        if (!gameObj || !task) {
            return false;
        }

        out.siteBuilding = wrsr::game::readField<uint8_t*>(task, g_off.taskSiteBldOff);
        out.vehicle      = wrsr::game::readField<uint8_t*>(task, g_off.taskVehOff);
        uint8_t* carrier = wrsr::game::readField<uint8_t*>(task, g_off.taskCarrierOff);
        out.resource     = wrsr::game::readField<const uint8_t*>(task, g_off.taskResOff);

        if (!out.siteBuilding || !out.vehicle) {
            return false;
        }

        // Never gate mechanism / excavator / flatbed transport
        if (carrier != nullptr || out.resource == nullptr) {
            out.isMechanism = true;
            out.valid       = true;
            return true;
        }

        const float remaining = wrsr::game::readField<float>(task, g_off.taskRemainingOff);
        if (!std::isfinite(remaining) || remaining <= 0.f) {
            return false;
        }
        out.remainingNeeded = remaining;

        // Detect worker transport via GameObject+0xC368 or ResourceRecord.priceKind == -1
        const auto* gameBytes = static_cast<const uint8_t*>(gameObj);
        const uint8_t* workerRes = wrsr::game::readField<const uint8_t*>(gameBytes, g_off.gameWorkerResOff);
        const int32_t  resKind   = wrsr::game::readField<int32_t>(out.resource, g_gameCtx.layout.resKindOff);
        out.isWorkers = (out.resource == workerRes || resKind == -1);

        out.vehicleCapacity = out.isWorkers
            ? queryPassengerCapacity(out.vehicle, out.resource)
            : queryMaterialCapacity(out.vehicle, out.resource);

        out.valid = (out.vehicleCapacity > 0.f);
        return out.valid;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── The detour ────────────────────────────────────────────────────────────
bool __fastcall detourDispatchCOTask(void* gameObj, uint8_t* task, uint8_t* coBuilding)
{
    CoTaskProbe probe{};
    if (!tryProbeCoTask(gameObj, task, probe) || probe.isMechanism) {
        return g_origDispatchCOTask(gameObj, task, coBuilding);
    }

    auto& cfg = Config::get();

    if (probe.isWorkers) {
        // ── Worker transport rule: Min Bus Workday Ratio ──────────────────
        const float ratio = cfg.getMinBusWorkdayRatio();
        if (ratio > 0.f && probe.remainingNeeded < probe.vehicleCapacity * ratio) {
            if (coHasSmallerCapableVehicle(
                    coBuilding, probe.vehicle, probe.resource, true, probe.vehicleCapacity)) {
                g_rejectedCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        return g_origDispatchCOTask(gameObj, task, coBuilding);
    }

    // ── Material delivery rules ───────────────────────────────────────────
    const float minFillPct = cfg.getMinMaterialFillPct();
    const float topOffFloor = cfg.getTopOffFloorTons();
    if (minFillPct > 0.f && probe.remainingNeeded >= topOffFloor) {
        const float fillRatio = probe.remainingNeeded / probe.vehicleCapacity;
        if (fillRatio < (minFillPct / 100.f)) {
            if (coHasSmallerCapableVehicle(
                    coBuilding, probe.vehicle, probe.resource, false, probe.vehicleCapacity)) {
                g_rejectedCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
    }

    const int maxEnRoute = cfg.getMaxTrucksEnRoute();
    if (maxEnRoute > 0 && maxEnRoute < 10) {
        if (countActiveTrucksForSite(probe.siteBuilding) >= maxEnRoute) {
            g_rejectedCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    const bool dispatched = g_origDispatchCOTask(gameObj, task, coBuilding);
    if (dispatched) {
        recordDispatchedTruck(probe.vehicle, probe.siteBuilding);
    }
    return dispatched;
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
    if (ctx->profile.getU32("co_task_site_bld_off",   tmpU32)) g_off.taskSiteBldOff     = tmpU32;
    if (ctx->profile.getU32("co_task_src_bld_off",    tmpU32)) g_off.taskSrcBldOff      = tmpU32;
    if (ctx->profile.getU32("co_task_veh_off",        tmpU32)) g_off.taskVehOff         = tmpU32;
    if (ctx->profile.getU32("co_task_carrier_off",    tmpU32)) g_off.taskCarrierOff     = tmpU32;
    if (ctx->profile.getU32("co_task_res_off",        tmpU32)) g_off.taskResOff         = tmpU32;
    if (ctx->profile.getU32("co_task_remaining_off",  tmpU32)) g_off.taskRemainingOff   = tmpU32;
    if (ctx->profile.getU32("co_vehicles_off",        tmpU32)) g_off.coVehiclesOff      = tmpU32;
    if (ctx->profile.getU32("game_worker_res_off",    tmpU32)) g_off.gameWorkerResOff   = tmpU32;
    if (ctx->profile.getU32("veh_curr_bld_off",       tmpU32)) g_off.vehCurrBldOff      = tmpU32;
    if (ctx->profile.getU32("veh_home_bld_off",       tmpU32)) g_off.vehHomeBldOff      = tmpU32;
    if (ctx->profile.getU32("veh_dispatch_state_off", tmpU32)) g_off.vehDispatchStateOff = tmpU32;
    if (ctx->profile.getU32("veh_type_off",           tmpU32)) g_off.vehTypeOff         = tmpU32;
    if (ctx->profile.getU32("veh_type_pax_class_off", tmpU32)) g_off.vehTypePaxClassOff = tmpU32;
    if (ctx->profile.getU32("veh_type_pax_cap_off",   tmpU32)) g_off.vehTypePaxCapOff   = tmpU32;
    if (ctx->profile.getI32("pax_class_value",        tmpI32)) g_off.paxClassValue      = tmpI32;

    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (MinHook init failed %d)", static_cast<int>(mhInit));
        return false;
    }

    uint8_t* dispatchAddr = ctx->scanSignature("dispatch_co_task_aob");
    uint8_t* getCapAddr   = ctx->resolveRipTarget(
        "get_veh_cap_aob", "get_veh_cap_disp_off", "get_veh_cap_next_ip_off");

    if (!dispatchAddr || !getCapAddr) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: NOT MATCHED (DispatchCOTask/GetVehCap AOB failed)");
        wrsr::log::warn("cdc: AOB match failed (dispatch=%p getCap=%p) — CODispatchControl inactive",
                        dispatchAddr, getCapAddr);
        g_gameCtx = std::move(*ctx);
        return false;
    }

    g_getVehicleResourceCapacity = reinterpret_cast<PFN_GetVehicleResourceCapacity>(getCapAddr);

    if (MH_CreateHook(dispatchAddr,
                      reinterpret_cast<void*>(detourDispatchCOTask),
                      reinterpret_cast<void**>(&g_origDispatchCOTask)) != MH_OK ||
        MH_EnableHook(dispatchAddr) != MH_OK) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (MinHook attach failed)");
        wrsr::log::error("cdc: MH_CreateHook/Enable failed for DispatchCOTask");
        g_gameCtx = std::move(*ctx);
        return false;
    }

    const size_t rva    = static_cast<size_t>(dispatchAddr - exeBase);
    const size_t capRva = static_cast<size_t>(getCapAddr - exeBase);
    g_state.store(HookState::Active, std::memory_order_relaxed);
    std::snprintf(g_statusText, sizeof(g_statusText),
                  "Hook: ACTIVE @ SOVIET64.exe+0x%zX (profile: %s)",
                  rva, ctx->layout.profileName.c_str());
    wrsr::log::info(
        "cdc: hooked DispatchCOTask @ RVA 0x%zX, GetVehCap @ RVA 0x%zX (profile=%s)",
        rva, capRva, ctx->layout.profileName.c_str());

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

uint64_t rejectedCount() noexcept
{
    return g_rejectedCount.load(std::memory_order_relaxed);
}

void resetRejectedCount() noexcept
{
    g_rejectedCount.store(0, std::memory_order_relaxed);
}

} // namespace cdc::hook
