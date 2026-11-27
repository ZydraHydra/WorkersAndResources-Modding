// ============================================================================
// customs_hook.cpp — Customs House cargo-transfer MinHook + PowerTime IAT hook
// ============================================================================
#include "customs_hook.hpp"

#include <windows.h>
#include <intrin.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "embedded_offsets.hpp"
#include "wrsr/game_signatures.hpp"
#include "wrsr/logger.hpp"
#include "wrsr/pe_utils.hpp"

#include "config.hpp"

namespace csc::hook {

namespace {

constexpr const char* kTimerDllName      = "C3DDLL64.dll";
constexpr const char* kPowerTimeExport   = "?PowerTime@C3D_TIMER@@QEAAMM_N0@Z";
constexpr int32_t     kTransferUnloading = 1;
constexpr int32_t     kTransferLoading   = 2;

std::atomic<HookState> g_state{HookState::Uninitialized};
std::atomic<uint64_t>  g_scaledTicks{0};
char                   g_statusText[256] = "Hook: Initializing...";

wrsr::game::GameContext g_gameCtx{};
VehicleCustomsOffsets   g_offsets{};

// Sorted list of pre-classified pre-clamp cargo-rate PowerTime return addresses
// inside VehicleCargoTransferTick (immutable after install()).
std::vector<uintptr_t> g_cargoPowerTimeReturnAddrs;

// Verified signature for SOVIET64.exe VehicleCargoTransferTick:
// bool __fastcall VehicleCargoTransferTick(Vehicle* veh, void* outLoadDone,
//     void* outUnloadDone, uint8_t arg4, uint8_t arg5, float rateScale,
//     uint8_t isLoadOnly, uint8_t isUnloadOnly)
using PFN_CargoTransferTick = bool(__fastcall*)(void*   vehicle,
                                                void*   outLoadDone,
                                                void*   outUnloadDone,
                                                uint8_t arg4,
                                                uint8_t arg5,
                                                float   rateScale,
                                                uint8_t isLoadOnly,
                                                uint8_t isUnloadOnly);
PFN_CargoTransferTick g_origCargoTransferTick = nullptr;

using PFN_PowerTime = float(__fastcall*)(void*   timer,
                                         float   baseScale,
                                         uint8_t b1,
                                         uint8_t b2);
PFN_PowerTime g_origPowerTime = nullptr;

struct ActiveTransferState {
    const uint8_t* vehicle    = nullptr;
    float          loadMult   = 1.0f;
    float          unloadMult = 1.0f;
    bool           active     = false;
};

thread_local ActiveTransferState t_active{};

struct ActiveTransferGuard {
    ActiveTransferState prev;
    explicit ActiveTransferGuard(const ActiveTransferState& next) noexcept : prev(t_active)
    {
        t_active = next;
    }
    ~ActiveTransferGuard() noexcept
    {
        t_active = prev;
    }
    ActiveTransferGuard(const ActiveTransferGuard&)            = delete;
    ActiveTransferGuard& operator=(const ActiveTransferGuard&) = delete;
};

template <typename T>
T readUnaligned(const uint8_t* p) noexcept
{
    T val{};
    std::memcpy(&val, p, sizeof(T));
    return val;
}

std::string trimCopy(std::string_view sv)
{
    size_t b = 0;
    while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) {
        ++b;
    }
    size_t e = sv.size();
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) {
        --e;
    }
    return std::string(sv.substr(b, e - b));
}

bool isPlaceholderPattern(std::string_view pat) noexcept
{
    if (pat.empty()) {
        return true;
    }
    return _strnicmp(pat.data(), "PENDING", 7) == 0 ||
           _strnicmp(pat.data(), "TODO", 4) == 0;
}

// Scans [scanBase, scanBase + scanSize) and requires exactly one unique match.
uint8_t* scanUniqueAob(uint8_t* scanBase, size_t scanSize, const char* pattern) noexcept
{
    if (!scanBase || scanSize == 0 || !pattern || !*pattern) {
        return nullptr;
    }
    uint8_t* first = wrsr::pe::aobScanStr(scanBase, scanSize, pattern);
    if (!first) {
        return nullptr;
    }
    const size_t offsetAfterFirst = static_cast<size_t>(first - scanBase) + 1;
    if (offsetAfterFirst < scanSize) {
        uint8_t* second = wrsr::pe::aobScanStr(
            scanBase + offsetAfterFirst, scanSize - offsetAfterFirst, pattern);
        if (second) {
            wrsr::log::warn("customs_hook: cargo_transfer_tick_aob matched multiple sites — refusing ambiguous hook");
            return nullptr;
        }
    }
    return first;
}

bool findFunctionBoundsPdata(const uint8_t* exeBase,
                             const uint8_t* target,
                             const uint8_t*& outBegin,
                             const uint8_t*& outEnd) noexcept
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exeBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const auto& excDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (excDir.VirtualAddress == 0 || excDir.Size < sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY)) {
        return false;
    }

    const uint32_t targetRva = static_cast<uint32_t>(target - exeBase);
    const auto*    entries   = reinterpret_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY*>(exeBase + excDir.VirtualAddress);
    const size_t   count     = excDir.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);

    for (size_t i = 0; i < count; ++i) {
        if (targetRva >= entries[i].BeginAddress && targetRva < entries[i].EndAddress) {
            outBegin = exeBase + entries[i].BeginAddress;
            outEnd   = exeBase + entries[i].EndAddress;
            return true;
        }
    }
    return false;
}

uintptr_t findImportIatSlot(const uint8_t* exeBase,
                            const char*    dllName,
                            const char*    funcName) noexcept
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exeBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    const auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress == 0) {
        return 0;
    }

    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(exeBase + impDir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        const char* modName = reinterpret_cast<const char*>(exeBase + desc->Name);
        if (_stricmp(modName, dllName) != 0) {
            continue;
        }

        const uint32_t thunkRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        const auto* origThunk   = reinterpret_cast<const IMAGE_THUNK_DATA64*>(exeBase + thunkRva);
        const auto* iatBase     = reinterpret_cast<const IMAGE_THUNK_DATA64*>(exeBase + desc->FirstThunk);

        for (size_t idx = 0; origThunk[idx].u1.AddressOfData != 0; ++idx) {
            if (IMAGE_SNAP_BY_ORDINAL64(origThunk[idx].u1.Ordinal)) {
                continue;
            }
            const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                exeBase + origThunk[idx].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), funcName) == 0) {
                return reinterpret_cast<uintptr_t>(&iatBase[idx]);
            }
        }
    }
    return 0;
}

std::vector<uintptr_t> collectCargoPowerTimeReturnAddrs(const uint8_t*               funcBegin,
                                                        const uint8_t*               funcEnd,
                                                        uintptr_t                    powerTimeIatAddr,
                                                        const VehicleCustomsOffsets& off)
{
    std::vector<const uint8_t*> sites;
    if (!funcBegin || !funcEnd || funcEnd <= funcBegin + 6 || powerTimeIatAddr == 0) {
        return {};
    }

    for (const uint8_t* p = funcBegin; p + 6 <= funcEnd; ++p) {
        if (p[0] == 0xFF && p[1] == 0x15) {
            const int32_t   disp   = readUnaligned<int32_t>(p + 2);
            const uintptr_t target = reinterpret_cast<uintptr_t>(p + 6) + static_cast<intptr_t>(disp);
            if (target == powerTimeIatAddr) {
                sites.push_back(p);
            }
        }
    }

    // Identify worker-bonus helper calls preceded by mov r9b, 1 (41 B1 01).
    std::vector<uintptr_t> workerHelperTargets;
    for (const uint8_t* s : sites) {
        const uint8_t* postBegin = s + 6;
        const uint8_t* postEnd   = (std::min)(funcEnd, s + 61);
        for (const uint8_t* q = postBegin; q + 3 <= postEnd; ++q) {
            if (q[0] == 0x41 && q[1] == 0xB1 && q[2] == 0x01) {
                const uint8_t* e8End = (std::min)(postEnd, q + 18);
                for (const uint8_t* r = q + 3; r + 5 <= e8End; ++r) {
                    if (r[0] == 0xE8) {
                        const int32_t   rel = readUnaligned<int32_t>(r + 1);
                        const uintptr_t dst = reinterpret_cast<uintptr_t>(r + 5) + static_cast<intptr_t>(rel);
                        workerHelperTargets.push_back(dst);
                        break;
                    }
                }
                break;
            }
        }
    }

    std::vector<uintptr_t> result;
    result.reserve(sites.size());

    for (const uint8_t* s : sites) {
        const uint8_t* preBegin  = (s - funcBegin >= 55) ? (s - 55) : funcBegin;
        const uint8_t* postBegin = s + 6;
        const uint8_t* postEnd   = (std::min)(funcEnd, s + 56);

        bool exclude = false;

        // 1. Exclude worker_helper sites (followed closely by mov r9b, 1)
        for (const uint8_t* q = postBegin; q + 3 <= postEnd; ++q) {
            if (q[0] == 0x41 && q[1] == 0xB1 && q[2] == 0x01) {
                exclude = true;
                break;
            }
        }

        // 2. Exclude worker_paired sites (preceded closely by E8 call to a worker helper)
        if (!exclude && !workerHelperTargets.empty()) {
            for (const uint8_t* q = preBegin; q + 5 <= s; ++q) {
                if (q[0] == 0xE8) {
                    const int32_t   rel = readUnaligned<int32_t>(q + 1);
                    const uintptr_t dst = reinterpret_cast<uintptr_t>(q + 5) + static_cast<intptr_t>(rel);
                    if (std::find(workerHelperTargets.begin(), workerHelperTargets.end(), dst) !=
                        workerHelperTargets.end()) {
                        exclude = true;
                        break;
                    }
                }
            }
        }

        // 3. Exclude dwell/wait timers, loading animation, and electric check sites
        if (!exclude) {
            for (const uint8_t* q = postBegin; q + 4 <= postEnd; ++q) {
                const uint32_t disp = readUnaligned<uint32_t>(q);
                if (disp == off.vehWait1Off || disp == off.vehWait2Off ||
                    disp == off.vehAnimOff  || disp == off.vehElecOff) {
                    exclude = true;
                    break;
                }
            }
        }

        if (!exclude) {
            result.push_back(reinterpret_cast<uintptr_t>(s + 6));
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

// SEH-guarded probe matching VehicleCargoTransferTick's target building
// resolution (pulling+0x528 for coupled rail cars, else +0x538 fallback +0x528)
// and verifying $TYPE_CUSTOMHOUSE (BUILDINGTYPE_CUSTOMHOUSE == 20).
bool isVehicleInCustomsHouseSeh(const uint8_t* vehicle, bool& outIsRail) noexcept
{
    outIsRail = false;
    __try {
        if (!vehicle) {
            return false;
        }

        const uint8_t* pulling = wrsr::game::readField<const uint8_t*>(
            vehicle, g_offsets.vehPullingOff);

        const uint8_t* building = nullptr;
        if (pulling) {
            building = wrsr::game::readField<const uint8_t*>(
                pulling, g_offsets.vehTargetBldOff);
        } else {
            building = wrsr::game::readField<const uint8_t*>(
                vehicle, g_offsets.vehOverrideBldOff);
            if (!building) {
                building = wrsr::game::readField<const uint8_t*>(
                    vehicle, g_offsets.vehTargetBldOff);
            }
        }

        if (!building) {
            return false;
        }

        const uint8_t* desc = wrsr::game::readField<const uint8_t*>(
            building, g_offsets.bldDescOff);
        if (!desc) {
            return false;
        }

        const int32_t bldType = wrsr::game::readField<int32_t>(
            desc, g_offsets.descTypeOff);
        if (bldType != g_offsets.customsBuildingType) {
            return false;
        }

        outIsRail = (pulling != nullptr);
        if (!outIsRail) {
            const uint8_t* vehType = wrsr::game::readField<const uint8_t*>(
                vehicle, g_offsets.vehTypeOff);
            if (vehType) {
                const int32_t category = wrsr::game::readField<int32_t>(
                    vehType, g_offsets.vehTypeCategoryOff);
                outIsRail = (category >= g_offsets.railCategoryMin &&
                             category <= g_offsets.railCategoryMax);
            }
        }

        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int32_t readTransferStateSeh(const uint8_t* vehicle) noexcept
{
    __try {
        if (!vehicle) {
            return 0;
        }
        return wrsr::game::readField<int32_t>(vehicle, g_offsets.vehTransferStateOff);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

float __fastcall detourPowerTime(void*   timer,
                                 float   baseScale,
                                 uint8_t b1,
                                 uint8_t b2)
{
    float dt = g_origPowerTime(timer, baseScale, b1, b2);

    if (!t_active.active || !t_active.vehicle) {
        return dt;
    }

    const uintptr_t retAddr = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (!std::binary_search(g_cargoPowerTimeReturnAddrs.begin(),
                            g_cargoPowerTimeReturnAddrs.end(),
                            retAddr)) {
        return dt;
    }

    const int32_t transferState = readTransferStateSeh(t_active.vehicle);
    if (transferState == kTransferUnloading) {
        dt *= t_active.unloadMult;
        g_scaledTicks.fetch_add(1, std::memory_order_relaxed);
    } else if (transferState == kTransferLoading) {
        dt *= t_active.loadMult;
        g_scaledTicks.fetch_add(1, std::memory_order_relaxed);
    }

    return dt;
}

bool __fastcall detourCargoTransferTick(void*   vehicle,
                                        void*   outLoadDone,
                                        void*   outUnloadDone,
                                        uint8_t arg4,
                                        uint8_t arg5,
                                        float   rateScale,
                                        uint8_t isLoadOnly,
                                        uint8_t isUnloadOnly)
{
    ActiveTransferState nextState{};
    bool                isRail = false;
    if (isVehicleInCustomsHouseSeh(static_cast<const uint8_t*>(vehicle), isRail)) {
        const auto& cfg = Config::get();
        if (!isRail || cfg.isAffectRailEnabled()) {
            nextState.vehicle    = static_cast<const uint8_t*>(vehicle);
            nextState.loadMult   = cfg.getLoadMultiplier();
            nextState.unloadMult = cfg.getUnloadMultiplier();
            nextState.active     = true;
        }
    }

    ActiveTransferGuard guard(nextState);
    return g_origCargoTransferTick(vehicle,
                                   outLoadDone,
                                   outUnloadDone,
                                   arg4,
                                   arg5,
                                   rateScale,
                                   isLoadOnly,
                                   isUnloadOnly);
}

} // namespace

float computeScaledTransferAmount(float baseTickAmount,
                                  float remainingCapacity,
                                  bool  isUnloading) noexcept
{
    if (baseTickAmount <= 0.f || remainingCapacity <= 0.f) {
        return 0.f;
    }
    const auto& cfg    = Config::get();
    const float mult   = isUnloading ? cfg.getUnloadMultiplier() : cfg.getLoadMultiplier();
    const float scaled = baseTickAmount * mult;
    return (std::min)(scaled, remainingCapacity);
}

bool install(uint8_t* exeBase, size_t exeSize)
{
    auto ctx = wrsr::game::GameContext::initialize(exeBase, exeSize, wrsr::generated::kEmbeddedOffsetsIni);
    if (!ctx) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (GameContext init failed)");
        return false;
    }

    // Refine desc_type_off dynamically from EvaluateDistTask when available
    ctx->resolveEvaluateDistTask();

    g_offsets.bldDescOff          = ctx->layout.bldDescOff;
    g_offsets.descTypeOff         = ctx->layout.descTypeOff;
    g_offsets.customsBuildingType = ctx->layout.customsBuildingType;

    uint32_t tmpU32 = 0;
    int32_t  tmpI32 = 0;
    if (ctx->profile.getU32("veh_pulling_off", tmpU32))        g_offsets.vehPullingOff       = tmpU32;
    if (ctx->profile.getU32("veh_target_bld_off", tmpU32))     g_offsets.vehTargetBldOff     = tmpU32;
    if (ctx->profile.getU32("veh_override_bld_off", tmpU32))   g_offsets.vehOverrideBldOff   = tmpU32;
    if (ctx->profile.getU32("veh_transfer_state_off", tmpU32)) g_offsets.vehTransferStateOff = tmpU32;
    if (ctx->profile.getU32("veh_wait1_off", tmpU32))          g_offsets.vehWait1Off         = tmpU32;
    if (ctx->profile.getU32("veh_elec_off", tmpU32))           g_offsets.vehElecOff          = tmpU32;
    if (ctx->profile.getU32("veh_anim_off", tmpU32))           g_offsets.vehAnimOff          = tmpU32;
    if (ctx->profile.getU32("veh_type_off", tmpU32))           g_offsets.vehTypeOff          = tmpU32;
    if (ctx->profile.getU32("veh_wait2_off", tmpU32))          g_offsets.vehWait2Off         = tmpU32;
    if (ctx->profile.getU32("veh_type_category_off", tmpU32))  g_offsets.vehTypeCategoryOff  = tmpU32;
    if (ctx->profile.getI32("rail_category_min", tmpI32))      g_offsets.railCategoryMin     = tmpI32;
    if (ctx->profile.getI32("rail_category_max", tmpI32))      g_offsets.railCategoryMax     = tmpI32;

    std::string aobPattern = trimCopy(Config::get().customSignatureAob());
    if (aobPattern.empty()) {
        if (auto embedded = ctx->offsets.getString("signatures", "cargo_transfer_tick_aob")) {
            aobPattern = trimCopy(*embedded);
        }
    }

    g_gameCtx = std::move(*ctx);

    if (isPlaceholderPattern(aobPattern)) {
        g_state.store(HookState::StandbyNoAob, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: STANDBY (AOB pending - see docs/CUSTOMS_REVERSE_ENGINEERING.md)");
        wrsr::log::warn("customs_hook: cargo_transfer_tick_aob is PENDING — running in standby UI mode");
        return false;
    }

    uint8_t* target = nullptr;
    uintptr_t namedTarget = 0;
    if (wrsr::game::resolveNamedOffset("border_customs_tick", namedTarget) && namedTarget != 0) {
        target = reinterpret_cast<uint8_t*>(namedTarget);
    } else {
        target = scanUniqueAob(g_gameCtx.scanBase, g_gameCtx.scanSize, aobPattern.c_str());
    }
    if (!target) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: NOT MATCHED (AOB failed on current SOVIET64.exe build)");
        wrsr::log::warn("customs_hook: cargo_transfer_tick_aob did not match uniquely in .text");
        return false;
    }

    const uint8_t* funcBegin = nullptr;
    const uint8_t* funcEnd   = nullptr;
    if (!findFunctionBoundsPdata(exeBase, target, funcBegin, funcEnd)) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (.pdata bounds not found for VehicleCargoTransferTick)");
        wrsr::log::error("customs_hook: .pdata entry not found for RVA 0x%zX",
                         static_cast<size_t>(target - exeBase));
        return false;
    }

    const uintptr_t powerTimeIatAddr = findImportIatSlot(exeBase, kTimerDllName, kPowerTimeExport);
    if (powerTimeIatAddr == 0) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (C3D_TIMER::PowerTime IAT slot missing)");
        wrsr::log::error("customs_hook: could not locate %s!%s IAT slot",
                         kTimerDllName, kPowerTimeExport);
        return false;
    }

    g_cargoPowerTimeReturnAddrs = collectCargoPowerTimeReturnAddrs(
        funcBegin, funcEnd, powerTimeIatAddr, g_offsets);
    if (g_cargoPowerTimeReturnAddrs.empty()) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (0 cargo PowerTime sites matched)");
        wrsr::log::error("customs_hook: found 0 cargo PowerTime call sites inside RVA 0x%zX..0x%zX",
                         static_cast<size_t>(funcBegin - exeBase),
                         static_cast<size_t>(funcEnd - exeBase));
        return false;
    }

    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (MinHook init failed %d)", static_cast<int>(mhInit));
        wrsr::log::error("customs_hook: MH_Initialize failed (%d)", static_cast<int>(mhInit));
        return false;
    }

    if (MH_CreateHook(target, reinterpret_cast<void*>(detourCargoTransferTick),
                      reinterpret_cast<void**>(&g_origCargoTransferTick)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (MinHook attach failed)");
        wrsr::log::error("customs_hook: failed to hook cargo_transfer_tick @ RVA 0x%zX",
                         static_cast<size_t>(target - exeBase));
        return false;
    }

    if (!wrsr::pe::patchIat(exeBase,
                            kTimerDllName,
                            kPowerTimeExport,
                            reinterpret_cast<void*>(&detourPowerTime),
                            reinterpret_cast<void**>(&g_origPowerTime))) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
        g_state.store(HookState::AobNotMatched, std::memory_order_relaxed);
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR (PowerTime IAT patch failed)");
        wrsr::log::error("customs_hook: patchIat failed for %s!%s", kTimerDllName, kPowerTimeExport);
        return false;
    }

    const size_t rva = static_cast<size_t>(target - exeBase);
    g_state.store(HookState::Active, std::memory_order_relaxed);
    std::snprintf(g_statusText, sizeof(g_statusText),
                  "Hook: ACTIVE @ SOVIET64.exe+0x%zX (%zu sites, profile: %s)",
                  rva, g_cargoPowerTimeReturnAddrs.size(), g_gameCtx.layout.profileName.c_str());
    wrsr::log::info("customs_hook: hooked VehicleCargoTransferTick @ RVA 0x%zX + PowerTime IAT (%zu cargo sites, desc_type_off=0x%X, veh_type_off=0x%X)",
                    rva, g_cargoPowerTimeReturnAddrs.size(), g_offsets.descTypeOff, g_offsets.vehTypeOff);
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

uint64_t scaledTickCount() noexcept
{
    return g_scaledTicks.load(std::memory_order_relaxed);
}

} // namespace csc::hook
