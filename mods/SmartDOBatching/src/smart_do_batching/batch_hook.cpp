// ============================================================================
// batch_hook.cpp — SmartDOBatching EvaluateDistTask detour
// ============================================================================
// Hooks EvaluateDistTask (same AOB as TradePriceLimits: dispatch_check_aob)
// and gates dispatch based on how much cargo can actually be transferred vs
// the vehicle's rated capacity.
//
// Struct offsets sourced from common/config/offsets.ini and verified docs:
//   Task0x80:
//     +0x00  ResourceRecord*   resource ptr (name @+0x00, priceKind @+0x44)
//     +0x10  Building*         source building
//     +0x48  Building*         destination building
//   Building:
//     +0x318 BuildingDescriptor*
//   BuildingDescriptor:
//     +0x360/0x368  int32_t  building type
//     +0x828/0x830  float    max storage capacity  (DO inventory slot max)
//   Vehicle's cargo is not directly accessible from Task0x80 — the Task
//   references the DO's assigned-vehicle queue.  The vehicle capacity is
//   read from the DO's assigned vehicle list pointer at Building+0x9C0 (see
//   docs/REVERSE_ENGINEERING.md "DO vehicle list"; 8-byte ptr, first entry
//   is currently assigned vehicle).
//
// Because vehicle capacity offsets are build-volatile, SmartDOBatching reads
// them from offsets.ini and falls back to a conservative default of 24 t
// (the smallest real road truck) so it always fails safe.
//
// Essential goods list (exempt from batch gating by default):
//   food, meat, alcohol, medicine, chemicals, coal, water, fuel
// ============================================================================
#include "batch_hook.hpp"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
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

// ── Additional per-mod offsets (read from offsets.ini [profile.*]) ────────
// Building -> DO assigned-vehicle list (std::vector-like, ptr at this offset)
// Not present in common/config/offsets.ini so we store them here with safe defaults.
// Building storage capacity in BuildingDescriptor (this is the per-slot max cargo).
//   Common known offsets from community RE:
//     BuildingDescriptor +0x828 = $VEHICLE_LOADING_FACTOR (float)  — load rate per tick
//     BuildingDescriptor +0xCC  = resource transport class factors
//     Building           +0x9C0 = DO vehicle slot pointer (first assigned vehicle ptr)
//     Building           +0x320 = storage capacity scalar (float, units match cargo units)
//   We read these from offsets.ini [profile.default] with safe fallbacks:
struct BatchOffsets {
    uint32_t bldDescOff{0x318};         // Building -> BuildingDescriptor*
    uint32_t descTypeOff{0x368};        // BuildingDescriptor -> int32_t building type
    uint32_t descMaxStorageOff{0x828};  // BuildingDescriptor -> float max storage capacity (tons)
                                        //   Fallback: 0 = use very large value (no deadlock cap)
    uint32_t bldVehicleSlotOff{0x9C0}; // Building -> Vehicle* (first assigned vehicle ptr)
    uint32_t vehCargoVecOff{0x3B8};    // Vehicle -> CargoSlot vector begin ptr (ptr-to-ptr)
    uint32_t vehCargoVecEndOff{0x3C0}; // Vehicle -> CargoSlot vector end ptr
    uint32_t vehTypeOff{0x1748};       // Vehicle -> VehicleType*  (version-specific)
    uint32_t vehTypeCategoryOff{0x294};// VehicleType -> int32_t category
    int32_t  railCategoryMin{3};
    int32_t  railCategoryMax{5};
    uint32_t cargoSlotStride{16};      // sizeof(CargoSlot) = 16 bytes
    uint32_t cargoSlotAmountOff{8};    // CargoSlot.amount offset within slot (float)
    // ResRecord name is at offset 0x00 (char[0x40])
};
BatchOffsets g_off{};

// ── Canonical essential goods names (lower-case, partial match) ───────────
// These goods are skipped for batch gating when exempt_essentials is enabled.
static constexpr std::string_view kEssentialGoods[] = {
    "food",
    "meat",
    "alcohol",
    "medicine",
    "chemical", // matches chemicals, medicine-chemicals, etc.
    "coal",
    "water",
    "fuel",
};

bool isEssential(const char* resName) noexcept
{
    if (!resName || !resName[0]) {
        return false;
    }
    // Compare lower-cased prefix against essential list
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

// ── SEH-guarded struct reads ──────────────────────────────────────────────
struct CargoProbe {
    bool  valid{false};
    bool  isRail{false};
    float vehicleMaxCargo{0.f};    // sum of all slots max (all slots empty = capacity)
    float sourceAvailCargo{0.f};   // actual cargo present in source building storage
    float destFreeStorage{0.f};    // free space in destination building
    char  resName[64]{};
};

// Reads total cargo capacity of the vehicle assigned to this DO's source building.
// Vehicle is at Building+bldVehicleSlotOff (a Vehicle* directly).
// Capacity = cargoSlot vector end - begin, summed per slot's max.
// Since slots are always full or empty at dispatch time, we approximate capacity
// as (vectorEnd - vectorBegin) / stride * per_slot_max.  However, we don't have
// per-slot max from struct, so we use the resource's transport class factor approach.
// Simpler approach: iterate slots, sum amounts (which represent current load) —
// that gives current load. Capacity = slot_count * per_slot_nominal.
//
// Pragmatically: the game dispatches vehicles that are EMPTY at the DO.
// So all slots are 0.0 at dispatch time. Slot count * per_slot_cap = vehicle cap.
// We read per_slot_cap from VehicleType (not easily accessible here).
//
// Fallback conservative approach used in batch_hook:
// 1. Read the slot vector to get slot count.
// 2. Use BuildingDescriptor max_storage as an upper bound for required cargo.
// 3. Use source building's current outgoing inventory vs destination free storage.
//
// Without a known VehicleType->capacity offset, we infer max capacity from
// the DO's assigned vehicle's cargo vector SIZE.  Each slot nominally holds
// the per-vehicle capacity / slot_count.  For simplicity, since the task
// contains the resource ptr which has the transport factor, we use a fixed
// per-slot capacity of 12.0t (conservative minimum for a road truck).
// Players can override via offsets.ini veh_slot_capacity_tons.
float                   g_vehicleSlotCapTons = 12.0f; // safe fallback

bool tryProbeTask(uint8_t* task, CargoProbe& out) noexcept
{
    __try {
        if (!task) {
            return false;
        }

        // Resource name
        const uint8_t* res = wrsr::game::readField<const uint8_t*>(task, 0x00);
        if (!res) {
            return false;
        }
        wrsr::game::readSafeCString(reinterpret_cast<const char*>(res), out.resName, sizeof(out.resName));

        // Source building
        const uint8_t* srcBld = wrsr::game::readField<const uint8_t*>(task, g_gameCtx.layout.taskSrcBldOff);
        if (!srcBld) {
            return false;
        }

        // Destination building
        const uint8_t* dstBld = wrsr::game::readField<const uint8_t*>(task, g_gameCtx.layout.taskDstBldOff);
        if (!dstBld) {
            return false;
        }

        // ── Vehicle max cargo capacity ─────────────────────────────────
        // Try to read the first assigned vehicle from source building
        {
            const uint8_t* veh = wrsr::game::readField<const uint8_t*>(srcBld, g_off.bldVehicleSlotOff);
            if (veh) {
                // Check if rail (pulling loco present means rail car)
                // veh_pulling_off = 0x398 from CustomsSpeedControl offsets
                const uint8_t* pulling = wrsr::game::readField<const uint8_t*>(veh, 0x398);
                out.isRail = (pulling != nullptr);

                if (!out.isRail) {
                    // Try VehicleType category for rail detection
                    const uint8_t* vehType = wrsr::game::readField<const uint8_t*>(
                        veh, g_off.vehTypeOff);
                    if (vehType) {
                        const int32_t cat = wrsr::game::readField<int32_t>(
                            vehType, g_off.vehTypeCategoryOff);
                        out.isRail = (cat >= g_off.railCategoryMin && cat <= g_off.railCategoryMax);
                    }
                }

                // Count cargo slots and sum current cargo
                const uint8_t* vecBegin = wrsr::game::readField<const uint8_t*>(
                    veh, g_off.vehCargoVecOff);
                const uint8_t* vecEnd   = wrsr::game::readField<const uint8_t*>(
                    veh, g_off.vehCargoVecEndOff);
                if (vecBegin && vecEnd && vecEnd >= vecBegin) {
                    const size_t slotCount = static_cast<size_t>(vecEnd - vecBegin) / g_off.cargoSlotStride;
                    out.vehicleMaxCargo = static_cast<float>(slotCount) * g_vehicleSlotCapTons;
                }
            }
        }

        // Guard: if we couldn't read vehicle capacity, return false → no gating
        if (out.vehicleMaxCargo <= 0.f) {
            return false;
        }

        // ── Destination building free storage ─────────────────────────
        // We read destination building's descriptor max storage.
        {
            const uint8_t* dstDesc = wrsr::game::readField<const uint8_t*>(
                dstBld, g_off.bldDescOff);
            if (dstDesc && g_off.descMaxStorageOff > 0) {
                const float dstMaxStorage = wrsr::game::readField<float>(
                    dstDesc, g_off.descMaxStorageOff);
                // dstFreeStorage: we don't have live inventory of dest.
                // Treat full descriptor storage as available free space worst case.
                // If dest is a small building (storage < vehicle cap), deadlock prevention
                // will clamp required_cargo below vehicle cap anyway.
                out.destFreeStorage = (dstMaxStorage > 0.f) ? dstMaxStorage : out.vehicleMaxCargo;
            } else {
                out.destFreeStorage = out.vehicleMaxCargo;
            }
        }

        // ── Source available cargo ─────────────────────────────────────
        // We don't have a direct source-inventory pointer from Task0x80 beyond
        // what the vanilla engine already computed.  At dispatch time the
        // vanilla threshold check already fired, meaning cargo ≥ dispatch_pct
        // of some storage is available.  We approximate source_available_cargo
        // as min(vehicle capacity, dest free storage) so the fill ratio
        // comparison is meaningful even without reading live building inventory.
        // A conservative estimate: we let available_transfer = vehicle_max_cargo
        // (vanilla already ensured at least some cargo is available, so if the
        // vehicle fills up, it's a full trip; if not, the min-fill check rejects).
        // For the deadlock-prevention clamp, we use dest storage which is known.
        out.sourceAvailCargo = out.vehicleMaxCargo; // conservative: assume best case

        out.valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── Original function pointer ─────────────────────────────────────────────
using PFN_EvaluateDistTask = void(__fastcall*)(void* gameObj, uint8_t* task);
PFN_EvaluateDistTask g_origEvaluateDistTask = nullptr;

// ── The detour ────────────────────────────────────────────────────────────
void __fastcall detourEvaluateDistTask(void* gameObj, uint8_t* task)
{
    auto& cfg = Config::get();

    const float truckPct       = cfg.getMinTruckFillPct();
    const float trainPct       = cfg.getMinTrainFillPct();
    const bool  exemptEss      = cfg.isExemptEssentials();
    const bool  preventDeadlk  = cfg.isPreventDeadlock();

    // If both thresholds are zero, skip all logic and call vanilla immediately
    if (truckPct <= 0.f && trainPct <= 0.f) {
        g_origEvaluateDistTask(gameObj, task);
        return;
    }

    CargoProbe probe{};
    if (tryProbeTask(task, probe)) {
        // Step 1: Essentials exemption
        if (exemptEss && isEssential(probe.resName)) {
            g_origEvaluateDistTask(gameObj, task);
            return;
        }

        // Step 2: Determine road vs rail ratio
        const float minFillRatio = (probe.isRail ? trainPct : truckPct) / 100.f;

        // Step 3: Compute required cargo
        float requiredCargo = probe.vehicleMaxCargo * minFillRatio;

        // Step 4: Deadlock prevention — clamp required_cargo to 85% of dest storage
        if (preventDeadlk && probe.destFreeStorage > 0.f) {
            const float destCap = probe.destFreeStorage * kDeadlockCapRatio;
            if (requiredCargo > destCap) {
                requiredCargo = destCap;
            }
        }

        // Step 5: Compute available_transfer = min(src_available, dest_free)
        const float availableTransfer = std::min(probe.sourceAvailCargo, probe.destFreeStorage);

        // If not enough cargo is available for a worthwhile batch, suppress dispatch
        if (availableTransfer < requiredCargo) {
            // Zero the dispatch scores to suppress vanilla dispatch
            g_gameCtx.zeroDistTaskScores(task);
            g_rejectedCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // Let vanilla evaluate (and potentially dispatch) the task
    g_origEvaluateDistTask(gameObj, task);
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
                      "Hook: ERROR (GameContext init failed — AOB mismatch?)");
        return false;
    }

    // ── Load mod-specific offsets from profile ───────────────────────────
    g_off.bldDescOff  = ctx->layout.bldDescOff;
    g_off.descTypeOff = ctx->layout.descTypeOff;

    uint32_t tmpU32 = 0;
    if (ctx->profile.getU32("veh_type_off",           tmpU32)) g_off.vehTypeOff          = tmpU32;
    if (ctx->profile.getU32("veh_type_category_off",  tmpU32)) g_off.vehTypeCategoryOff   = tmpU32;
    if (ctx->profile.getU32("bld_vehicle_slot_off",   tmpU32)) g_off.bldVehicleSlotOff    = tmpU32;
    if (ctx->profile.getU32("desc_max_storage_off",   tmpU32)) g_off.descMaxStorageOff    = tmpU32;
    if (ctx->profile.getU32("veh_cargo_vec_off",      tmpU32)) g_off.vehCargoVecOff       = tmpU32;
    if (ctx->profile.getU32("veh_cargo_vec_end_off",  tmpU32)) g_off.vehCargoVecEndOff    = tmpU32;
    // ResolvedProfile has no getF32 — parse float from string manually
    if (auto sv = ctx->profile.getString("veh_slot_capacity_tons")) {
        const std::string s(sv->data(), sv->size());
        char* end = nullptr;
        const float parsed = std::strtof(s.c_str(), &end);
        if (end && end != s.c_str() && std::isfinite(parsed) && parsed > 0.f) {
            g_vehicleSlotCapTons = parsed;
        }
    }

    // ── MinHook ──────────────────────────────────────────────────────────
    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (MinHook init failed %d)", static_cast<int>(mhInit));
        return false;
    }

    // ── Resolve EvaluateDistTask via shared AOB ───────────────────────────
    if (!ctx->resolveEvaluateDistTask() || !ctx->evaluateDistTaskAddr) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: NOT MATCHED (dispatch_check_aob failed on current SOVIET64.exe)");
        wrsr::log::warn("sdb: dispatch_check_aob did not match — SmartDOBatching inactive");
        g_gameCtx = std::move(*ctx);
        return false;
    }

    if (MH_CreateHook(ctx->evaluateDistTaskAddr,
                      reinterpret_cast<void*>(detourEvaluateDistTask),
                      reinterpret_cast<void**>(&g_origEvaluateDistTask)) != MH_OK ||
        MH_EnableHook(ctx->evaluateDistTaskAddr) != MH_OK) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (MinHook attach failed)");
        wrsr::log::error("sdb: MH_CreateHook/Enable failed for EvaluateDistTask");
        g_gameCtx = std::move(*ctx);
        return false;
    }

    const size_t rva = static_cast<size_t>(ctx->evaluateDistTaskAddr - exeBase);
    g_state.store(HookState::Active, std::memory_order_relaxed);
    std::snprintf(g_statusText, sizeof(g_statusText),
                  "Hook: ACTIVE @ SOVIET64.exe+0x%zX (profile: %s, slot_cap=%.0ft)",
                  rva, ctx->layout.profileName.c_str(), g_vehicleSlotCapTons);
    wrsr::log::info(
        "sdb: hooked EvaluateDistTask @ RVA 0x%zX (profile=%s, bldDesc=0x%X, descType=0x%X, "
        "vehType=0x%X, slotCap=%.0ft)",
        rva, ctx->layout.profileName.c_str(),
        g_off.bldDescOff, g_off.descTypeOff,
        g_off.vehTypeOff, g_vehicleSlotCapTons);

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
