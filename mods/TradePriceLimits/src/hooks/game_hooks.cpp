// ============================================================================
// game_hooks.cpp — Customs House dispatch gating & market sync via wrsr::game
// ============================================================================
#include "game_hooks.hpp"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <string_view>

#include "embedded_offsets.hpp"
#include "wrsr/logger.hpp"
#include "../core/rules.hpp"

namespace tpl::game {

GameAddresses g_addrs{};

namespace {

std::atomic<uint64_t>       g_blockedCount{0};
std::atomic<const uint8_t*> g_syncedVecBegin{nullptr};
std::atomic<size_t>         g_syncedVecCount{0};

using PFN_EvaluateDistTask = void(__fastcall*)(void* gameObj, uint8_t* task);
PFN_EvaluateDistTask g_origEvaluateDistTask = nullptr;

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

        const auto& L = g_addrs.ctx.layout;

        const uint8_t* srcBld = wrsr::game::readField<const uint8_t*>(task, L.taskSrcBldOff);
        const uint8_t* dstBld = wrsr::game::readField<const uint8_t*>(task, L.taskDstBldOff);

        if (srcBld) {
            const uint8_t* desc = wrsr::game::readField<const uint8_t*>(srcBld, L.bldDescOff);
            if (desc && wrsr::game::readField<int32_t>(desc, L.descTypeOff) == L.customsBuildingType) {
                out.srcIsCustoms = true;
                out.srcFaction   = wrsr::game::readField<int32_t>(srcBld, L.bldFactionOff);
            }
        }

        if (dstBld) {
            const uint8_t* desc = wrsr::game::readField<const uint8_t*>(dstBld, L.bldDescOff);
            if (desc && wrsr::game::readField<int32_t>(desc, L.descTypeOff) == L.customsBuildingType) {
                out.dstIsCustoms = true;
                out.dstFaction   = wrsr::game::readField<int32_t>(dstBld, L.bldFactionOff);
            }
        }

        if (!out.srcIsCustoms && !out.dstIsCustoms) {
            return false;
        }

        const uint8_t* res = wrsr::game::readField<const uint8_t*>(task, L.taskResOff);
        if (!res) {
            return false;
        }

        out.buyUsd  = wrsr::game::readField<float>(res, L.resBuyUsdOff);
        out.buyRub  = wrsr::game::readField<float>(res, L.resBuyRubOff);
        out.sellUsd = wrsr::game::readField<float>(res, L.resSellUsdOff);
        out.sellRub = wrsr::game::readField<float>(res, L.resSellRubOff);

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
            if (!wrsr::game::readSafeCString(reinterpret_cast<const char*>(res + L.resNameOff),
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

void __fastcall detourEvaluateDistTask(void* gameObj, uint8_t* task)
{
    auto& engine = tpl::RuleEngine::get();
    if (engine.isEnforcementEnabled() && engine.hasAnyActiveLimit()) {
        TaskTradeProbe probe{};
        if (tryProbeTask(task, probe)) {
            if (probe.needsSync) {
                syncLivePricesToRules();
            }

            const auto& L     = g_addrs.ctx.layout;
            bool        block = false;
            if (probe.fastIndex != tpl::kInvalidFastIndex) {
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
                g_addrs.ctx.zeroDistTaskScores(task);
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
    return g_addrs.ctx.readLivePrice(resourceIndex);
}

void syncLivePricesToRules() noexcept
{
    size_t count = 0;
    if (!g_addrs.ctx.tryReadResourceCount(count) || !g_addrs.resourceVector) {
        return;
    }

    g_syncedVecBegin.store(g_addrs.resourceVector->begin, std::memory_order_relaxed);
    g_syncedVecCount.store(count, std::memory_order_relaxed);

    auto& engine = tpl::RuleEngine::get();
    for (size_t i = 0; i < count; ++i) {
        wrsr::game::RawResourceSnapshot snap{};
        if (!g_addrs.ctx.tryReadResourceSnapshot(i, snap)) {
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
    auto ctx = wrsr::game::GameContext::initialize(exeBase, exeSize, wrsr::generated::kEmbeddedOffsetsIni);
    if (!ctx) {
        return false;
    }

    const MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) {
        wrsr::log::error("game_hooks: MH_Initialize failed (%d)", static_cast<int>(mhInit));
        return false;
    }

    bool allFound = true;

    if (ctx->resolveResourceVector()) {
        g_addrs.resourceVector = ctx->resourceVector;
    } else {
        allFound = false;
    }

    if (ctx->resolveEvaluateDistTask() && ctx->evaluateDistTaskAddr) {
        if (MH_CreateHook(ctx->evaluateDistTaskAddr,
                          reinterpret_cast<void*>(detourEvaluateDistTask),
                          reinterpret_cast<void**>(&g_origEvaluateDistTask)) == MH_OK &&
            MH_EnableHook(ctx->evaluateDistTaskAddr) == MH_OK) {
            g_addrs.dispatchCheckSite = ctx->evaluateDistTaskAddr;
            wrsr::log::info("dispatch_check_aob hooked @ RVA 0x%zX (EvaluateDistTask)",
                            static_cast<size_t>(ctx->evaluateDistTaskAddr - exeBase));
        } else {
            wrsr::log::error("dispatch_check_aob MinHook install failed @ RVA 0x%zX",
                             static_cast<size_t>(ctx->evaluateDistTaskAddr - exeBase));
            allFound = false;
        }
    } else {
        allFound = false;
    }

    g_addrs.ctx = std::move(*ctx);
    return allFound;
}

} // namespace tpl::game
