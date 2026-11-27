// ============================================================================
// game_signatures.cpp — Shared WRSR PE profile resolution, AOB helpers & structs
// ============================================================================
#include "wrsr/game_signatures.hpp"
#include "grml_api.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

#include "embedded_common_offsets.hpp"
#include "wrsr/logger.hpp"

namespace wrsr::game {

namespace {

bool tryReadCountSeh(const ResourceVector* vec, uint32_t stride, size_t& outCount) noexcept
{
    outCount = 0;
    __try {
        if (!vec || !vec->begin || vec->end <= vec->begin || stride == 0) {
            return false;
        }
        const size_t byteSpan = static_cast<size_t>(vec->end - vec->begin);
        const size_t count    = byteSpan / stride;
        if (count == 0 || count > kMaxResources) {
            return false;
        }
        outCount = count;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool tryReadSnapshotSeh(const ResourceVector* vec,
                        const CommonLayout& L,
                        size_t index,
                        RawResourceSnapshot& out) noexcept
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
        if (!readSafeCString(reinterpret_cast<const char*>(rec + L.resNameOff),
                             out.name, sizeof(out.name))) {
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

bool tryProbeDistTaskBuildingsSeh(const uint8_t* task,
                                  const CommonLayout& L,
                                  DistTaskBuildingProbe& out) noexcept
{
    __try {
        if (!task) {
            return false;
        }
        const uint8_t* srcBld = readField<const uint8_t*>(task, L.taskSrcBldOff);
        const uint8_t* dstBld = readField<const uint8_t*>(task, L.taskDstBldOff);

        if (srcBld) {
            const uint8_t* desc = readField<const uint8_t*>(srcBld, L.bldDescOff);
            if (desc) {
                out.srcValid        = true;
                out.srcBuildingType = readField<int32_t>(desc, L.descTypeOff);
                out.srcFaction      = readField<int32_t>(srcBld, L.bldFactionOff);
            }
        }
        if (dstBld) {
            const uint8_t* desc = readField<const uint8_t*>(dstBld, L.bldDescOff);
            if (desc) {
                out.dstValid        = true;
                out.dstBuildingType = readField<int32_t>(desc, L.descTypeOff);
                out.dstFaction      = readField<int32_t>(dstBld, L.bldFactionOff);
            }
        }
        return out.srcValid || out.dstValid;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void zeroDistTaskScoresSeh(uint8_t* task, const CommonLayout& L) noexcept
{
    __try {
        writeField<double>(task,  L.taskScoreSrcDoubleOff, 0.0);
        writeField<uint8_t>(task, L.taskScoreSrcFlag1Off,  0);
        writeField<uint8_t>(task, L.taskScoreSrcFlag2Off,  0);
        writeField<double>(task,  L.taskScoreDstDoubleOff, 0.0);
        writeField<uint8_t>(task, L.taskScoreDstFlag1Off,  0);
        writeField<uint8_t>(task, L.taskScoreDstFlag2Off,  0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace

namespace {
std::atomic<const GrmlHostApi*> g_grmlHost{nullptr};
} // namespace

void setGrmlHost(const GrmlHostApi* host) noexcept
{
    g_grmlHost.store(host, std::memory_order_release);
}

const GrmlHostApi* getGrmlHost() noexcept
{
    return g_grmlHost.load(std::memory_order_acquire);
}

bool resolveNamedOffset(const char* sectionName, uintptr_t& outValue) noexcept
{
    outValue = 0;
    const GrmlHostApi* host = g_grmlHost.load(std::memory_order_acquire);
    if (!host || host->abi_version < 2u || host->struct_size < sizeof(GrmlHostApi) || !host->ResolveNamedOffset) {
        return false;
    }
    return host->ResolveNamedOffset(sectionName, &outValue) != 0 && outValue != 0;
}

bool readSafeCString(const char* ptr, char* out, size_t maxLen) noexcept
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

uint32_t scanCmpMemImm8Disp32(const uint8_t* site,
                              uint32_t scanWindow,
                              int32_t expectedImm8,
                              uint32_t minDisp,
                              uint32_t maxDisp,
                              uint32_t fallbackOff) noexcept
{
    if (!site || scanWindow < 7) {
        return fallbackOff;
    }
    const uint8_t immByte = static_cast<uint8_t>(expectedImm8 & 0xFF);
    for (size_t i = 0; i + 7 <= scanWindow; ++i) {
        if (site[i] == 0x83 &&
            site[i + 1] >= 0xB8 && site[i + 1] <= 0xBF &&
            site[i + 6] == immByte) {
            uint32_t disp = 0;
            std::memcpy(&disp, site + i + 2, sizeof(disp));
            if (disp >= minDisp && disp <= maxDisp) {
                return disp;
            }
        }
    }
    return fallbackOff;
}

bool CommonLayout::loadFromProfile(const wrsr::offsets::ResolvedProfile& profile,
                                   const wrsr::pe::ExecutableFingerprint& fp)
{
    profileName       = profile.name();
    exactProfileMatch = profile.isExactMatch();
    peTimeDateStamp   = fp.timeDateStamp;
    peSizeOfImage     = fp.sizeOfImage;

    bool ok = true;
    auto reqU32 = [&](const char* key, uint32_t& dst) {
        if (!profile.getU32(key, dst)) {
            wrsr::log::error("offsets.ini: missing/invalid u32 key '%s' in profile '%s'",
                             key, profileName.c_str());
            ok = false;
        }
    };
    auto reqI32 = [&](const char* key, int32_t& dst) {
        if (!profile.getI32(key, dst)) {
            wrsr::log::error("offsets.ini: missing/invalid i32 key '%s' in profile '%s'",
                             key, profileName.c_str());
            ok = false;
        }
    };

    reqU32("res_stride",                resStride);
    reqU32("res_min_header_size",       resMinHeaderSize);
    reqU32("res_name_off",              resNameOff);
    reqU32("res_kind_off",              resKindOff);
    reqU32("res_buy_usd_off",           resBuyUsdOff);
    reqU32("res_buy_rub_off",           resBuyRubOff);
    reqU32("res_sell_usd_off",          resSellUsdOff);
    reqU32("res_sell_rub_off",          resSellRubOff);
    reqU32("task_res_off",              taskResOff);
    reqU32("task_src_bld_off",          taskSrcBldOff);
    reqU32("task_dst_bld_off",          taskDstBldOff);
    reqU32("task_score_src_double_off", taskScoreSrcDoubleOff);
    reqU32("task_score_src_flag1_off",  taskScoreSrcFlag1Off);
    reqU32("task_score_src_flag2_off",  taskScoreSrcFlag2Off);
    reqU32("task_score_dst_double_off", taskScoreDstDoubleOff);
    reqU32("task_score_dst_flag1_off",  taskScoreDstFlag1Off);
    reqU32("task_score_dst_flag2_off",  taskScoreDstFlag2Off);
    reqU32("bld_desc_off",              bldDescOff);
    reqU32("bld_faction_off",           bldFactionOff);
    reqU32("desc_type_off",             descTypeOff);
    reqI32("customs_building_type",     customsBuildingType);
    reqI32("faction_usd",               factionUsd);
    reqI32("faction_rub",               factionRub);

    return ok;
}

std::optional<GameContext> GameContext::initialize(uint8_t* exeBase,
                                                   size_t exeSize,
                                                   std::string_view modOffsetsOverride)
{
    if (!exeBase || exeSize == 0) {
        return std::nullopt;
    }

    auto doc = wrsr::offsets::OffsetDocument::parse(wrsr::generated::kCommonOffsetsIni);
    if (!doc) {
        wrsr::log::error("wrsr_common: failed to parse embedded common offsets.ini");
        return std::nullopt;
    }

    if (!modOffsetsOverride.empty()) {
        doc->merge(modOffsetsOverride);
    }

    const auto fp      = wrsr::pe::getExecutableFingerprint(exeBase);
    const auto profile = doc->resolveProfile(fp);
    if (!profile) {
        wrsr::log::error("wrsr_common: failed to resolve any offset profile");
        return std::nullopt;
    }

    GameContext ctx;
    ctx.exeBase  = exeBase;
    ctx.exeSize  = exeSize;
    ctx.offsets  = std::move(*doc);
    ctx.profile  = *profile;

    if (!ctx.layout.loadFromProfile(ctx.profile, fp)) {
        return std::nullopt;
    }

    const auto textSec = wrsr::pe::getSectionRange(exeBase, ".text");
    ctx.scanBase       = textSec.base ? textSec.base : exeBase;
    ctx.scanSize       = textSec.size ? textSec.size : exeSize;

    wrsr::log::info(
        "Offsets profile '%s' (%s) — PE TimeDateStamp=0x%08X SizeOfImage=0x%X",
        ctx.layout.profileName.c_str(),
        ctx.layout.exactProfileMatch ? "exact PE match" : "default fallback",
        fp.timeDateStamp,
        fp.sizeOfImage);

    return ctx;
}

uint8_t* GameContext::scanSignature(std::string_view sigKey, std::string_view section) const noexcept
{
    const auto pat = offsets.getString(section, sigKey);
    if (!pat || !scanBase || scanSize == 0) {
        return nullptr;
    }
    const std::string patStr(*pat);
    return wrsr::pe::aobScanStr(scanBase, scanSize, patStr.c_str());
}

uint8_t* GameContext::resolveRipTarget(std::string_view sigKey,
                                       std::string_view dispKey,
                                       std::string_view nextIpKey,
                                       uint8_t** outMatchSite,
                                       std::string_view section) const noexcept
{
    if (outMatchSite) {
        *outMatchSite = nullptr;
    }
    const auto dispOff   = offsets.getU32(section, dispKey);
    const auto nextIpOff = offsets.getU32(section, nextIpKey);
    if (!dispOff || !nextIpOff) {
        return nullptr;
    }
    uint8_t* site = scanSignature(sigKey, section);
    if (!site) {
        return nullptr;
    }
    if (outMatchSite) {
        *outMatchSite = site;
    }
    int32_t disp = 0;
    std::memcpy(&disp, site + *dispOff, sizeof(disp));
    return site + *nextIpOff + disp;
}

bool GameContext::resolveResourceVector() noexcept
{
    uintptr_t namedVec = 0;
    if (resolveNamedOffset("resource_vector", namedVec) && namedVec != 0) {
        uintptr_t namedStride = 0;
        if (resolveNamedOffset("resource_stride", namedStride) &&
            namedStride >= layout.resMinHeaderSize && namedStride <= 0x1000) {
            layout.resStride = static_cast<uint32_t>(namedStride);
        }
        resourceVector = reinterpret_cast<ResourceVector*>(namedVec);
        wrsr::log::info("resource_vector resolved via GRML ResolveNamedOffset -> ResourceVector=0x%p (stride=0x%X)",
                        resourceVector, layout.resStride);
        return true;
    }

    const auto strideImmOff = offsets.getU32("signatures", "price_table_stride_imm_off");
    const bool autoStride   = offsets.getBool("signatures", "price_table_auto_detect_stride", true);

    uint8_t* site = nullptr;
    uint8_t* target = resolveRipTarget(
        "price_table_aob", "price_table_vec_disp_off", "price_table_vec_next_ip_off", &site);

    if (!target || !site || !strideImmOff) {
        wrsr::log::warn("price_table_aob did not match or missing offsets.ini signature metadata");
        return false;
    }

    uint32_t detectedStride = 0;
    std::memcpy(&detectedStride, site + *strideImmOff, sizeof(detectedStride));
    if (detectedStride < layout.resMinHeaderSize || detectedStride > 0x1000) {
        wrsr::log::warn("price_table_aob unexpected operand stride=0x%X", detectedStride);
        return false;
    }

    if (layout.exactProfileMatch && detectedStride != layout.resStride) {
        wrsr::log::warn(
            "price_table_aob operand stride=0x%X differs from profile '%s' res_stride=0x%X — using operand",
            detectedStride, layout.profileName.c_str(), layout.resStride);
        layout.resStride = detectedStride;
    } else if (!layout.exactProfileMatch && autoStride) {
        layout.resStride = detectedStride;
    }

    resourceVector = reinterpret_cast<ResourceVector*>(target);
    wrsr::log::info("price_table_aob matched @ RVA 0x%zX -> ResourceVector=0x%p (RVA 0x%zX), stride=0x%X",
                    static_cast<size_t>(site - exeBase),
                    resourceVector,
                    static_cast<size_t>(reinterpret_cast<uint8_t*>(resourceVector) - exeBase),
                    layout.resStride);
    return true;
}

bool GameContext::resolveEvaluateDistTask() noexcept
{
    uintptr_t namedEval = 0;
    if (resolveNamedOffset("evaluate_dist_task", namedEval) && namedEval != 0) {
        evaluateDistTaskAddr = reinterpret_cast<uint8_t*>(namedEval);
        wrsr::log::info("evaluate_dist_task resolved via GRML ResolveNamedOffset -> 0x%p",
                        evaluateDistTaskAddr);
        return true;
    }

    const bool     autoTypeOff = offsets.getBool("signatures", "dispatch_auto_detect_type_off", true);
    const uint32_t scanWin     = offsets.getU32("signatures", "dispatch_auto_scan_window").value_or(0x280);

    uint8_t* site = scanSignature("dispatch_check_aob");
    if (!site) {
        wrsr::log::warn("dispatch_check_aob did not match");
        return false;
    }

    const uint32_t detectedTypeOff = scanCmpMemImm8Disp32(
        site, scanWin, layout.customsBuildingType, 0x300, 0x450, layout.descTypeOff);

    if (layout.exactProfileMatch && detectedTypeOff != layout.descTypeOff) {
        wrsr::log::warn(
            "dispatch_check_aob detected desc_type_off=0x%X differs from profile '%s' (0x%X) — using detected",
            detectedTypeOff, layout.profileName.c_str(), layout.descTypeOff);
        layout.descTypeOff = detectedTypeOff;
    } else if (!layout.exactProfileMatch && autoTypeOff) {
        layout.descTypeOff = detectedTypeOff;
    }

    evaluateDistTaskAddr = site;
    wrsr::log::info("dispatch_check_aob matched @ RVA 0x%zX (EvaluateDistTask, desc_type_off=0x%X)",
                    static_cast<size_t>(site - exeBase),
                    layout.descTypeOff);
    return true;
}

bool GameContext::tryReadResourceCount(size_t& outCount) const noexcept
{
    return tryReadCountSeh(resourceVector, layout.resStride, outCount);
}

bool GameContext::tryReadResourceSnapshot(size_t index, RawResourceSnapshot& out) const noexcept
{
    return tryReadSnapshotSeh(resourceVector, layout, index, out);
}

std::optional<LivePrice> GameContext::readLivePrice(uint32_t resourceIndex) const noexcept
{
    if (!resourceVector || layout.resStride < layout.resMinHeaderSize) {
        return std::nullopt;
    }
    size_t count = 0;
    if (!tryReadResourceCount(count) || resourceIndex >= count) {
        return std::nullopt;
    }
    RawResourceSnapshot snap{};
    if (!tryReadResourceSnapshot(resourceIndex, snap)) {
        return std::nullopt;
    }
    return LivePrice{ snap.buyRub, snap.buyUsd, snap.sellRub, snap.sellUsd };
}

bool GameContext::tryProbeDistTaskBuildings(const uint8_t* task, DistTaskBuildingProbe& out) const noexcept
{
    return tryProbeDistTaskBuildingsSeh(task, layout, out);
}

void GameContext::zeroDistTaskScores(uint8_t* task) const noexcept
{
    zeroDistTaskScoresSeh(task, layout);
}

} // namespace wrsr::game
