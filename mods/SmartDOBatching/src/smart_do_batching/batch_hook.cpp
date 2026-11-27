// ============================================================================
// batch_hook.cpp — SmartDOBatching CanVehicleTakeDistTask detour
// ============================================================================
// Hooks CanVehicleTakeDistTask(GameObject*, Task0x80*, Vehicle*, DistOffice*)
// which runs after EvaluateDistTask has populated Task0x80's source/dest
// cargo & storage fields and receives the exact candidate Vehicle*.
//
// Verified Task0x80 fields (from SOVIET64.exe EvaluateDistTask disassembly):
//   +0x00  ResourceRecord*   resource ptr (name @+0x00)
//   +0x10  Building*         source building
//   +0x30  double            source available cargo (tons)
//   +0x48  Building*         destination building
//   +0x68  double            destination current cargo (tons)
//   +0x70  double            destination max storage capacity (tons)
//
// Vehicle capacity is queried directly via the engine's own
// GetVehicleResourceCapacity(Vehicle*, ResourceRecord*, 0, 0) -> float.
//
// Essential goods list (exempt from batch gating when exempt_essentials=true):
//   food, meat, alcohol, medicine, chemicals, coal, water, fuel
// ============================================================================
#include "batch_hook.hpp"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string_view>

#include "embedded_offsets.hpp"
#include "wrsr/game_signatures.hpp"
#include "wrsr/logger.hpp"
#include "config.hpp"

namespace sdb::hook {

namespace {

// ── Mutable state ─────────────────────────────────────────────────────────
std::atomic<HookState>  g_state{HookState::Uninitialized};
std::atomic<uint64_t>   g_rejectedCount{0};
char                    g_statusText[256] = "Hook: Initializing...";

// Game context (owns layout offsets)
wrsr::game::GameContext g_gameCtx{};

struct BatchOffsets {
    uint32_t taskSrcAvailOff{0x30};
    uint32_t taskDstCurrOff{0x68};
    uint32_t taskDstMaxOff{0x70};
    uint32_t vehPullingOff{0x398};
    uint32_t vehTypeOff{0x1748};
    uint32_t vehTypeCategoryOff{0x294};
    int32_t  railCategoryMin{3};
    int32_t  railCategoryMax{5};
};
BatchOffsets g_off{};

// ── Canonical essential goods names (lower-case, partial match) ───────────
static constexpr std::string_view kEssentialGoods[] = {
    "food",
    "meat",
    "alcohol",
    "medicine",
    "chemical",
    "coal",
    "water",
    "fuel",
};

bool isEssential(const char* resName) noexcept
{
    if (!resName || !resName[0]) {
        return false;
    }
    char lower[64]{};
    for (size_t i = 0; i < 63 && resName[i]; ++i) {
        lower[i] = static_cast<char>(
            (resName[i] >= 'A' && resName[i] <= 'Z')
                ? (resName[i] + ('a' - 'A'))
                : resName[i]);
    }
    const std::string_view sv(lower);
    for (const auto& e : kEssentialGoods) {
        if (sv.find(e) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

// ── Engine function pointers ──────────────────────────────────────────────
using PFN_CanVehicleTakeDistTask = bool(__fastcall*)(
    void* gameObj, uint8_t* task, uint8_t* vehicle, uint8_t* distOffice);
using PFN_GetVehicleResourceCapacity = float(__fastcall*)(
    uint8_t* vehicle, const uint8_t* resource, int32_t a3, int32_t a4);

PFN_CanVehicleTakeDistTask     g_origCanVehicleTakeDistTask = nullptr;
PFN_GetVehicleResourceCapacity g_getVehicleResourceCapacity = nullptr;

// ── SEH-guarded struct probe ──────────────────────────────────────────────
struct BatchProbe {
    bool   valid{false};
    bool   isRail{false};
    float  vehicleMaxCargo{0.f};
    double sourceAvailCargo{0.0};
    double destMaxStorage{0.0};
    double destFreeStorage{0.0};
    bool   srcIsCustoms{false};
    bool   dstIsCustoms{false};
    char   resName[64]{};
};

bool tryProbeBatch(uint8_t* task, uint8_t* veh, BatchProbe& out) noexcept
{
    __try {
        if (!task || !veh || !g_getVehicleResourceCapacity) {
            return false;
        }

        const auto& L = g_gameCtx.layout;

        // Resource record & name
        const uint8_t* res = wrsr::game::readField<const uint8_t*>(task, L.taskResOff);
        if (!res) {
            return false;
        }
        wrsr::game::readSafeCString(
            reinterpret_cast<const char*>(res + L.resNameOff),
            out.resName, sizeof(out.resName));

        // Road vs rail detection
        const uint8_t* pulling = wrsr::game::readField<const uint8_t*>(veh, g_off.vehPullingOff);
        out.isRail = (pulling != nullptr);
        if (!out.isRail) {
            const uint8_t* vehType = wrsr::game::readField<const uint8_t*>(veh, g_off.vehTypeOff);
            if (vehType) {
                const int32_t cat = wrsr::game::readField<int32_t>(vehType, g_off.vehTypeCategoryOff);
                out.isRail = (cat >= g_off.railCategoryMin && cat <= g_off.railCategoryMax);
            }
        }

        // Exact vehicle max capacity for this resource (tons)
        const float vehCap = g_getVehicleResourceCapacity(veh, res, 0, 0);
        if (!std::isfinite(vehCap) || vehCap <= 0.f) {
            return false;
        }
        out.vehicleMaxCargo = vehCap;

        // Check whether source or destination is a Customs House (unlimited supply/demand)
        const uint8_t* srcBld = wrsr::game::readField<const uint8_t*>(task, L.taskSrcBldOff);
        const uint8_t* dstBld = wrsr::game::readField<const uint8_t*>(task, L.taskDstBldOff);
        if (srcBld) {
            const uint8_t* desc = wrsr::game::readField<const uint8_t*>(srcBld, L.bldDescOff);
            if (desc && wrsr::game::readField<int32_t>(desc, L.descTypeOff) == L.customsBuildingType) {
                out.srcIsCustoms = true;
            }
        }
        if (dstBld) {
            const uint8_t* desc = wrsr::game::readField<const uint8_t*>(dstBld, L.bldDescOff);
            if (desc && wrsr::game::readField<int32_t>(desc, L.descTypeOff) == L.customsBuildingType) {
                out.dstIsCustoms = true;
            }
        }

        // Source available cargo & destination free storage from Task0x80
        const double srcAvail = wrsr::game::readField<double>(task, g_off.taskSrcAvailOff);
        const double dstCurr  = wrsr::game::readField<double>(task, g_off.taskDstCurrOff);
        const double dstMax   = wrsr::game::readField<double>(task, g_off.taskDstMaxOff);

        out.sourceAvailCargo = out.srcIsCustoms
            ? static_cast<double>(vehCap)
            : (std::isfinite(srcAvail) ? std::max(0.0, srcAvail) : static_cast<double>(vehCap));

        if (out.dstIsCustoms) {
            out.destMaxStorage  = static_cast<double>(vehCap);
            out.destFreeStorage = static_cast<double>(vehCap);
        } else {
            out.destMaxStorage  = (std::isfinite(dstMax) && dstMax > 0.0) ? dstMax : 0.0;
            out.destFreeStorage = (std::isfinite(dstMax) && std::isfinite(dstCurr))
                ? std::max(0.0, dstMax - dstCurr)
                : static_cast<double>(vehCap);
        }

        out.valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── The detour ────────────────────────────────────────────────────────────
bool __fastcall detourCanVehicleTakeDistTask(
    void* gameObj, uint8_t* task, uint8_t* vehicle, uint8_t* distOffice)
{
    // 1. Let vanilla's storage-threshold and compatibility checks run first
    if (!g_origCanVehicleTakeDistTask(gameObj, task, vehicle, distOffice)) {
        return false;
    }

    auto& cfg = Config::get();
    const float truckPct      = cfg.getMinTruckFillPct();
    const float trainPct      = cfg.getMinTrainFillPct();
    const bool  exemptEss     = cfg.isExemptEssentials();
    const bool  preventDeadlk = cfg.isPreventDeadlock();

    if (truckPct <= 0.f && trainPct <= 0.f) {
        return true;
    }

    BatchProbe probe{};
    if (!tryProbeBatch(task, vehicle, probe)) {
        return true;
    }

    // 2. Essentials exemption
    if (exemptEss && isEssential(probe.resName)) {
        return true;
    }

    // 3. Road vs rail fill ratio
    const float minFillRatio = (probe.isRail ? trainPct : truckPct) / 100.f;
    if (minFillRatio <= 0.f) {
        return true;
    }

    // 4. Required cargo & small-building deadlock prevention
    double requiredCargo = static_cast<double>(probe.vehicleMaxCargo) * static_cast<double>(minFillRatio);
    if (preventDeadlk && !probe.dstIsCustoms && probe.destMaxStorage > 0.0) {
        const double destCap = probe.destMaxStorage * static_cast<double>(kDeadlockCapRatio);
        if (requiredCargo > destCap) {
            requiredCargo = destCap;
        }
    }

    // 5. Available transferable cargo
    const double availableTransfer = std::min(probe.sourceAvailCargo, probe.destFreeStorage);
    if (availableTransfer + 1e-4 < requiredCargo) {
        g_rejectedCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    return true;
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
    if (ctx->profile.getU32("task_src_avail_off",    tmpU32)) g_off.taskSrcAvailOff    = tmpU32;
    if (ctx->profile.getU32("task_dst_curr_off",     tmpU32)) g_off.taskDstCurrOff     = tmpU32;
    if (ctx->profile.getU32("task_dst_max_off",      tmpU32)) g_off.taskDstMaxOff      = tmpU32;
    if (ctx->profile.getU32("veh_pulling_off",       tmpU32)) g_off.vehPullingOff      = tmpU32;
    if (ctx->profile.getU32("veh_type_off",          tmpU32)) g_off.vehTypeOff         = tmpU32;
    if (ctx->profile.getU32("veh_type_category_off", tmpU32)) g_off.vehTypeCategoryOff = tmpU32;
    if (ctx->profile.getI32("rail_category_min",     tmpI32)) g_off.railCategoryMin    = tmpI32;
    if (ctx->profile.getI32("rail_category_max",     tmpI32)) g_off.railCategoryMax    = tmpI32;

    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (MinHook init failed %d)", static_cast<int>(mhInit));
        return false;
    }

    uintptr_t namedCanTake = 0, namedGetCap = 0;
    uint8_t* canTakeAddr = (wrsr::game::resolveNamedOffset("can_vehicle_take_task", namedCanTake) && namedCanTake)
        ? reinterpret_cast<uint8_t*>(namedCanTake)
        : ctx->scanSignature("can_vehicle_take_task_aob");
    uint8_t* getCapAddr = (wrsr::game::resolveNamedOffset("get_veh_cap", namedGetCap) && namedGetCap)
        ? reinterpret_cast<uint8_t*>(namedGetCap)
        : ctx->resolveRipTarget("get_veh_cap_aob", "get_veh_cap_disp_off", "get_veh_cap_next_ip_off");

    if (!canTakeAddr || !getCapAddr) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: NOT MATCHED (CanVehicleTakeDistTask/GetVehCap AOB failed)");
        wrsr::log::warn("sdb: AOB match failed (canTake=%p getCap=%p) — SmartDOBatching inactive",
                        canTakeAddr, getCapAddr);
        g_gameCtx = std::move(*ctx);
        return false;
    }

    g_getVehicleResourceCapacity = reinterpret_cast<PFN_GetVehicleResourceCapacity>(getCapAddr);

    if (MH_CreateHook(canTakeAddr,
                      reinterpret_cast<void*>(detourCanVehicleTakeDistTask),
                      reinterpret_cast<void**>(&g_origCanVehicleTakeDistTask)) != MH_OK ||
        MH_EnableHook(canTakeAddr) != MH_OK) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (MinHook attach failed)");
        wrsr::log::error("sdb: MH_CreateHook/Enable failed for CanVehicleTakeDistTask");
        g_gameCtx = std::move(*ctx);
        return false;
    }

    const size_t rva    = static_cast<size_t>(canTakeAddr - exeBase);
    const size_t capRva = static_cast<size_t>(getCapAddr - exeBase);
    g_state.store(HookState::Active, std::memory_order_relaxed);
    std::snprintf(g_statusText, sizeof(g_statusText),
                  "Hook: ACTIVE @ SOVIET64.exe+0x%zX (profile: %s)",
                  rva, ctx->layout.profileName.c_str());
    wrsr::log::info(
        "sdb: hooked CanVehicleTakeDistTask @ RVA 0x%zX, GetVehCap @ RVA 0x%zX (profile=%s)",
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

} // namespace sdb::hook
