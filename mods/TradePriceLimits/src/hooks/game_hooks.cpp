// ============================================================================
// game_hooks.cpp — Profile-driven AOB scanner, ResourceVector sync & dispatch gating
// ============================================================================
#include "game_hooks.hpp"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

#include "embedded_offsets.hpp"
#include "wrsr/logger.hpp"
#include "wrsr/offsets.hpp"
#include "wrsr/pe_utils.hpp"
#include "../core/rules.hpp"

namespace tpl::game {

GameAddresses g_addrs{};

namespace {

std::atomic<uint64_t>       g_blockedCount{0};
std::atomic<const uint8_t*> g_syncedVecBegin{nullptr};
std::atomic<size_t>         g_syncedVecCount{0};

using PFN_EvaluateDistTask = void(__fastcall*)(void* gameObj, uint8_t* task);
PFN_EvaluateDistTask g_origEvaluateDistTask = nullptr;

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

bool readSafeName(const char* ptr, char* out, size_t maxLen) noexcept
{
    if (!ptr || maxLen == 0) {
        return false;
    }
    size_t i = 0;
    for (; i < maxLen - 1; ++i) {
        const char c = ptr[i];
        if (c == '\0') {
            break;
        }
        if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) {
            return false;
        }
        out[i] = c;
    }
    out[i] = '\0';
    return i > 0;
}

struct RawRecordSnapshot {
    char    name[33]{};
    int32_t priceKind{0};
    float   buyRub{0.f};
    float   sellRub{0.f};
    float   buyUsd{0.f};
    float   sellUsd{0.f};
};

bool tryReadCount(const ResourceVector* vec, uint32_t stride, size_t& outCount) noexcept
{
    outCount = 0;
    __try {
        if (!vec || !vec->begin || vec->end <= vec->begin || stride == 0) {
            return false;
        }
        const size_t byteSpan = static_cast<size_t>(vec->end - vec->begin);
        const size_t count    = byteSpan / stride;
        if (count == 0 || count > tpl::kMaxFastResources) {
            return false;
        }
        outCount = count;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool tryReadSnapshot(const ResourceVector* vec, const GameLayout& L, size_t index, RawRecordSnapshot& out) noexcept
{
    __try {
        const uint8_t* rec = vec->begin + index * L.resStride;

        out.priceKind = readField<int32_t>(rec, L.resKindOff);
        if (out.priceKind == -1 || out.priceKind == -3 || out.priceKind == -4) {
            return false;
        }
        if (out.priceKind < -5 || out.priceKind > 4) {
            return false;
        }
        if (!readSafeName(reinterpret_cast<const char*>(rec + L.resNameOff), out.name, sizeof(out.name))) {
            return false;
        }
        out.buyUsd  = readField<float>(rec, L.resBuyUsdOff);
        out.buyRub  = readField<float>(rec, L.resBuyRubOff);
        out.sellUsd = readField<float>(rec, L.resSellUsdOff);
        out.sellRub = readField<float>(rec, L.resSellRubOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct TaskTradeProbe {
    bool    valid{false};
    size_t  fastIndex{tpl::kInvalidFastIndex};
    bool    needsSync{false};
    char    resName[33]{};
    float   buyRub{0.f};
    float   sellRub{0.f};
    float   buyUsd{0.f};
    float   sellUsd{0.f};
    bool    srcIsCustoms{false};
    int32_t srcFaction{0};
    bool    dstIsCustoms{false};
    int32_t dstFaction{0};
};

bool tryProbeTask(uint8_t* task, TaskTradeProbe& out) noexcept
{
    __try {
        if (!task) {
            return false;
        }

        const GameLayout& L = g_addrs.layout;

        // Fast-exit domestic tasks (majority of Distribution Office tasks) before
        // touching the resource record or price table.
        const uint8_t* srcBld = readField<const uint8_t*>(task, L.taskSrcBldOff);
        const uint8_t* dstBld = readField<const uint8_t*>(task, L.taskDstBldOff);

        if (srcBld) {
            const uint8_t* desc = readField<const uint8_t*>(srcBld, L.bldDescOff);
            if (desc && readField<int32_t>(desc, L.descTypeOff) == L.customsBuildingType) {
                out.srcIsCustoms = true;
                out.srcFaction   = readField<int32_t>(srcBld, L.bldFactionOff);
            }
        }

        if (dstBld) {
            const uint8_t* desc = readField<const uint8_t*>(dstBld, L.bldDescOff);
            if (desc && readField<int32_t>(desc, L.descTypeOff) == L.customsBuildingType) {
                out.dstIsCustoms = true;
                out.dstFaction   = readField<int32_t>(dstBld, L.bldFactionOff);
            }
        }

        if (!out.srcIsCustoms && !out.dstIsCustoms) {
            return false;
        }

        const uint8_t* res = readField<const uint8_t*>(task, L.taskResOff);
        if (!res) {
            return false;
        }

        out.buyUsd  = readField<float>(res, L.resBuyUsdOff);
        out.buyRub  = readField<float>(res, L.resBuyRubOff);
        out.sellUsd = readField<float>(res, L.resSellUsdOff);
        out.sellRub = readField<float>(res, L.resSellRubOff);

        if (g_addrs.resourceVector && L.resStride >= L.resMinHeaderSize) {
            const uint8_t* vecBegin = g_addrs.resourceVector->begin;
            const uint8_t* vecEnd   = g_addrs.resourceVector->end;
            const uint32_t stride   = L.resStride;
            if (vecBegin && vecEnd > vecBegin) {
                const size_t count = static_cast<size_t>(vecEnd - vecBegin) / stride;
                if (count > 0 && count <= tpl::kMaxFastResources) {
                    if (vecBegin != g_syncedVecBegin.load(std::memory_order_relaxed) ||
                        count != g_syncedVecCount.load(std::memory_order_relaxed)) {
                        out.needsSync = true;
                    }
                    const auto resAddr   = reinterpret_cast<uintptr_t>(res);
                    const auto beginAddr = reinterpret_cast<uintptr_t>(vecBegin);
                    const auto endAddr   = reinterpret_cast<uintptr_t>(vecEnd);
                    if (resAddr >= beginAddr && resAddr < endAddr) {
                        const size_t offset = static_cast<size_t>(resAddr - beginAddr);
                        const size_t idx    = offset / stride;
                        if ((offset % stride) == 0 && idx < count) {
                            out.fastIndex = idx;
                        }
                    }
                }
            }
        }

        if (out.fastIndex == tpl::kInvalidFastIndex) {
            if (!readSafeName(reinterpret_cast<const char*>(res + L.resNameOff),
                              out.resName, sizeof(out.resName))) {
                return false;
            }
        }

        out.valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void zeroTaskDispatchScores(uint8_t* task) noexcept
{
    __try {
        const GameLayout& L = g_addrs.layout;
        writeField<double>(task,  L.taskScoreSrcDoubleOff, 0.0);
        writeField<uint8_t>(task, L.taskScoreSrcFlag1Off,  0);
        writeField<uint8_t>(task, L.taskScoreSrcFlag2Off,  0);
        writeField<double>(task,  L.taskScoreDstDoubleOff, 0.0);
        writeField<uint8_t>(task, L.taskScoreDstFlag1Off,  0);
        writeField<uint8_t>(task, L.taskScoreDstFlag2Off,  0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool loadLayoutFromProfile(const wrsr::offsets::ResolvedProfile& profile,
                           const wrsr::pe::ExecutableFingerprint& fp,
                           GameLayout& out)
{
    out.profileName       = profile.name();
    out.exactProfileMatch = profile.isExactMatch();
    out.peTimeDateStamp   = fp.timeDateStamp;
    out.peSizeOfImage     = fp.sizeOfImage;

    bool ok = true;
    auto reqU32 = [&](const char* key, uint32_t& dst) {
        if (!profile.getU32(key, dst)) {
            wrsr::log::error("offsets.ini: missing/invalid u32 key '%s' in profile '%s'",
                             key, out.profileName.c_str());
            ok = false;
        }
    };
    auto reqI32 = [&](const char* key, int32_t& dst) {
        if (!profile.getI32(key, dst)) {
            wrsr::log::error("offsets.ini: missing/invalid i32 key '%s' in profile '%s'",
                             key, out.profileName.c_str());
            ok = false;
        }
    };

    reqU32("res_stride",                out.resStride);
    reqU32("res_min_header_size",       out.resMinHeaderSize);
    reqU32("res_name_off",              out.resNameOff);
    reqU32("res_kind_off",              out.resKindOff);
    reqU32("res_buy_usd_off",           out.resBuyUsdOff);
    reqU32("res_buy_rub_off",           out.resBuyRubOff);
    reqU32("res_sell_usd_off",          out.resSellUsdOff);
    reqU32("res_sell_rub_off",          out.resSellRubOff);
    reqU32("task_res_off",              out.taskResOff);
    reqU32("task_src_bld_off",          out.taskSrcBldOff);
    reqU32("task_dst_bld_off",          out.taskDstBldOff);
    reqU32("task_score_src_double_off", out.taskScoreSrcDoubleOff);
    reqU32("task_score_src_flag1_off",  out.taskScoreSrcFlag1Off);
    reqU32("task_score_src_flag2_off",  out.taskScoreSrcFlag2Off);
    reqU32("task_score_dst_double_off", out.taskScoreDstDoubleOff);
    reqU32("task_score_dst_flag1_off",  out.taskScoreDstFlag1Off);
    reqU32("task_score_dst_flag2_off",  out.taskScoreDstFlag2Off);
    reqU32("bld_desc_off",              out.bldDescOff);
    reqU32("bld_faction_off",           out.bldFactionOff);
    reqU32("desc_type_off",             out.descTypeOff);
    reqI32("customs_building_type",     out.customsBuildingType);
    reqI32("faction_usd",               out.factionUsd);
    reqI32("faction_rub",               out.factionRub);

    return ok;
}

uint32_t scanDescTypeOffset(const uint8_t* dispatchSite,
                            uint32_t scanWindow,
                            int32_t customsType,
                            uint32_t fallbackOff) noexcept
{
    const uint8_t customsByte = static_cast<uint8_t>(customsType & 0xFF);
    for (size_t i = 0; i + 7 <= scanWindow; ++i) {
        if (dispatchSite[i] == 0x83 &&
            dispatchSite[i + 1] >= 0xB8 && dispatchSite[i + 1] <= 0xBF &&
            dispatchSite[i + 6] == customsByte) {
            uint32_t disp = 0;
            std::memcpy(&disp, dispatchSite + i + 2, sizeof(disp));
            if (disp >= 0x300 && disp <= 0x450) {
                return disp;
            }
        }
    }
    return fallbackOff;
}

void __fastcall detourEvaluateDistTask(void* gameObj, uint8_t* task)
{
    auto& engine = tpl::RuleEngine::get();
    if (engine.isEnforcementEnabled() && engine.hasAnyActiveLimit()) {
        TaskTradeProbe probe{};
        if (tryProbeTask(task, probe)) {
            if (probe.needsSync) {
                syncLivePricesToRules();
            }

            const GameLayout& L = g_addrs.layout;
            bool block = false;
            if (probe.fastIndex != tpl::kInvalidFastIndex) {
                // O(1) lock-free, allocation-free path
                if (probe.srcIsCustoms) {
                    block = block || (probe.srcFaction == L.factionUsd
                                          ? engine.shouldBlockBuyUsdFast(probe.fastIndex, probe.buyUsd)
                                          : engine.shouldBlockBuyRubFast(probe.fastIndex, probe.buyRub));
                }
                if (probe.dstIsCustoms) {
                    block = block || (probe.dstFaction == L.factionUsd
                                          ? engine.shouldBlockSellUsdFast(probe.fastIndex, probe.sellUsd)
                                          : engine.shouldBlockSellRubFast(probe.fastIndex, probe.sellRub));
                }
            } else {
                // Fallback path (still zero heap allocation via string_view)
                const std::string_view resView(probe.resName);
                if (probe.srcIsCustoms) {
                    block = block || (probe.srcFaction == L.factionUsd
                                          ? engine.shouldBlockBuyUsd(resView, probe.buyUsd)
                                          : engine.shouldBlockBuyRub(resView, probe.buyRub));
                }
                if (probe.dstIsCustoms) {
                    block = block || (probe.dstFaction == L.factionUsd
                                          ? engine.shouldBlockSellUsd(resView, probe.sellUsd)
                                          : engine.shouldBlockSellRub(resView, probe.sellRub));
                }
            }

            if (block) {
                zeroTaskDispatchScores(task);
                g_blockedCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    g_origEvaluateDistTask(gameObj, task);
}

} // namespace

void resetBlockedStats() noexcept
{
    g_blockedCount.store(0, std::memory_order_relaxed);
    g_syncedVecCount.store(0, std::memory_order_relaxed);
}

uint64_t getBlockedCount() noexcept
{
    return g_blockedCount.load(std::memory_order_relaxed);
}

std::optional<LivePrice> readLivePrice(uint32_t resourceIndex) noexcept
{
    const GameLayout& L = g_addrs.layout;
    if (!g_addrs.resourceVector || L.resStride < L.resMinHeaderSize) {
        return std::nullopt;
    }

    size_t count = 0;
    if (!tryReadCount(g_addrs.resourceVector, L.resStride, count) || resourceIndex >= count) {
        return std::nullopt;
    }

    RawRecordSnapshot snap{};
    if (!tryReadSnapshot(g_addrs.resourceVector, L, resourceIndex, snap)) {
        return std::nullopt;
    }

    return LivePrice{ snap.buyRub, snap.buyUsd, snap.sellRub, snap.sellUsd };
}

void syncLivePricesToRules() noexcept
{
    const GameLayout& L = g_addrs.layout;
    if (!g_addrs.resourceVector || L.resStride < L.resMinHeaderSize) {
        return;
    }

    size_t count = 0;
    if (!tryReadCount(g_addrs.resourceVector, L.resStride, count)) {
        return;
    }

    g_syncedVecBegin.store(g_addrs.resourceVector->begin, std::memory_order_relaxed);
    g_syncedVecCount.store(count, std::memory_order_relaxed);

    auto& engine = tpl::RuleEngine::get();
    for (size_t i = 0; i < count; ++i) {
        RawRecordSnapshot snap{};
        if (!tryReadSnapshot(g_addrs.resourceVector, L, i, snap)) {
            continue;
        }
        engine.updateLivePrices(snap.name,
                                snap.buyRub, snap.sellRub,
                                snap.buyUsd, snap.sellUsd,
                                true, i);
    }
}

bool install(uint8_t* exeBase, size_t exeSize)
{
    if (!exeBase || exeSize == 0) {
        return false;
    }

    const auto doc = wrsr::offsets::OffsetDocument::parse(wrsr::generated::kEmbeddedOffsetsIni);
    if (!doc) {
        wrsr::log::error("game_hooks: failed to parse embedded offsets.ini");
        return false;
    }

    const auto fp      = wrsr::pe::getExecutableFingerprint(exeBase);
    const auto profile = doc->resolveProfile(fp);
    if (!profile) {
        wrsr::log::error("game_hooks: failed to resolve any offset profile");
        return false;
    }

    GameLayout layout{};
    if (!loadLayoutFromProfile(*profile, fp, layout)) {
        return false;
    }
    g_addrs.layout = layout;

    wrsr::log::info(
        "Offsets profile '%s' (%s) — PE TimeDateStamp=0x%08X SizeOfImage=0x%X",
        g_addrs.layout.profileName.c_str(),
        g_addrs.layout.exactProfileMatch ? "exact PE match" : "default fallback",
        fp.timeDateStamp,
        fp.sizeOfImage);

    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        wrsr::log::error("game_hooks: MH_Initialize failed (%d)", static_cast<int>(mhInit));
        return false;
    }

    const auto textSec  = wrsr::pe::getSectionRange(exeBase, ".text");
    uint8_t*   scanBase = textSec.base ? textSec.base : exeBase;
    size_t     scanSize = textSec.size ? textSec.size : exeSize;

    bool allFound = true;

    // 1. World-market ResourceVector + record stride
    const auto priceAob        = doc->getString("signatures", "price_table_aob");
    const auto vecDispOff      = doc->getU32("signatures", "price_table_vec_disp_off");
    const auto vecNextIpOff    = doc->getU32("signatures", "price_table_vec_next_ip_off");
    const auto strideImmOff    = doc->getU32("signatures", "price_table_stride_imm_off");
    const bool autoStride      = doc->getBool("signatures", "price_table_auto_detect_stride", true);

    if (priceAob && vecDispOff && vecNextIpOff && strideImmOff) {
        const std::string pat(*priceAob);
        if (uint8_t* site = wrsr::pe::aobScanStr(scanBase, scanSize, pat.c_str())) {
            const int32_t  disp            = *reinterpret_cast<const int32_t*>(site + *vecDispOff);
            const uint32_t detectedStride  = *reinterpret_cast<const uint32_t*>(site + *strideImmOff);
            auto*          vec             = reinterpret_cast<ResourceVector*>(site + *vecNextIpOff + disp);

            if (detectedStride >= g_addrs.layout.resMinHeaderSize && detectedStride <= 0x1000) {
                if (g_addrs.layout.exactProfileMatch && detectedStride != g_addrs.layout.resStride) {
                    wrsr::log::warn(
                        "price_table_aob operand stride=0x%X differs from profile '%s' res_stride=0x%X — using operand",
                        detectedStride, g_addrs.layout.profileName.c_str(), g_addrs.layout.resStride);
                    g_addrs.layout.resStride = detectedStride;
                } else if (!g_addrs.layout.exactProfileMatch && autoStride) {
                    g_addrs.layout.resStride = detectedStride;
                }
                g_addrs.resourceVector = vec;
                wrsr::log::info("price_table_aob matched @ RVA 0x%zX -> ResourceVector=0x%p (RVA 0x%zX), stride=0x%X",
                                static_cast<size_t>(site - exeBase),
                                vec,
                                static_cast<size_t>(reinterpret_cast<uint8_t*>(vec) - exeBase),
                                g_addrs.layout.resStride);
            } else {
                wrsr::log::warn("price_table_aob unexpected operand stride=0x%X", detectedStride);
                allFound = false;
            }
        } else {
            wrsr::log::warn("price_table_aob did not match");
            allFound = false;
        }
    } else {
        wrsr::log::error("offsets.ini: missing [signatures] entries for price_table");
        allFound = false;
    }

    // 2. Distribution Office / border-logistics dispatch validation
    const auto dispatchAob     = doc->getString("signatures", "dispatch_check_aob");
    const bool autoTypeOff     = doc->getBool("signatures", "dispatch_auto_detect_type_off", true);
    const uint32_t scanWin     = doc->getU32("signatures", "dispatch_auto_scan_window").value_or(0x280);

    if (dispatchAob) {
        const std::string pat(*dispatchAob);
        if (uint8_t* dispatchSite = wrsr::pe::aobScanStr(scanBase, scanSize, pat.c_str())) {
            const uint32_t detectedTypeOff = scanDescTypeOffset(
                dispatchSite, scanWin, g_addrs.layout.customsBuildingType, g_addrs.layout.descTypeOff);
            if (g_addrs.layout.exactProfileMatch && detectedTypeOff != g_addrs.layout.descTypeOff) {
                wrsr::log::warn(
                    "dispatch_check_aob detected desc_type_off=0x%X differs from profile '%s' (0x%X) — using detected",
                    detectedTypeOff, g_addrs.layout.profileName.c_str(), g_addrs.layout.descTypeOff);
                g_addrs.layout.descTypeOff = detectedTypeOff;
            } else if (!g_addrs.layout.exactProfileMatch && autoTypeOff) {
                g_addrs.layout.descTypeOff = detectedTypeOff;
            }

            if (MH_CreateHook(dispatchSite,
                              reinterpret_cast<void*>(detourEvaluateDistTask),
                              reinterpret_cast<void**>(&g_origEvaluateDistTask)) == MH_OK &&
                MH_EnableHook(dispatchSite) == MH_OK) {
                g_addrs.dispatchCheckSite = dispatchSite;
                wrsr::log::info("dispatch_check_aob hooked @ RVA 0x%zX (EvaluateDistTask, desc_type_off=0x%X)",
                                static_cast<size_t>(dispatchSite - exeBase),
                                g_addrs.layout.descTypeOff);
            } else {
                wrsr::log::error("dispatch_check_aob MinHook install failed @ RVA 0x%zX",
                                 static_cast<size_t>(dispatchSite - exeBase));
                allFound = false;
            }
        } else {
            wrsr::log::warn("dispatch_check_aob did not match");
            allFound = false;
        }
    } else {
        wrsr::log::error("offsets.ini: missing [signatures] entry for dispatch_check_aob");
        allFound = false;
    }

    return allFound;
}

} // namespace tpl::game
