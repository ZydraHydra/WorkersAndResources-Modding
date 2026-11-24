// ============================================================================
// game_hooks.cpp — AOB scanner, live ResourceVector sync & dispatch gating
// ============================================================================
#include "game_hooks.hpp"

#include <windows.h>
#include <MinHook.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

#include "wrsr/logger.hpp"
#include "wrsr/pe_utils.hpp"
#include "../core/rules.hpp"

namespace tpl::game {

GameAddresses g_addrs{};

namespace {

constexpr int32_t kBuildingTypeCustoms = 20;   // BUILDINGTYPE_CUSTOMHOUSE (0x14)
constexpr int32_t kFactionUsd          = 3;    // NATO / Western border (USD)
constexpr int32_t kFactionRub          = 4;    // Soviet border (RUB)

// Rolling 60-second ring buffer of blocked dispatches, advanced only while
// the simulation is actively running (paused when the game is paused).
std::array<std::atomic<uint32_t>, kBlockedWindowSeconds> g_blockedBuckets{};
std::atomic<uint32_t>                                    g_currentBucket{0};
std::atomic<void*>                                       g_lastGameObj{nullptr};
std::atomic<bool>                                        g_simTickObserved{false};
std::atomic<bool>                                        g_resetClockState{false};
std::atomic<const uint8_t*>                              g_syncedVecBegin{nullptr};
std::atomic<size_t>                                      g_syncedVecCount{0};

using PFN_EvaluateDistTask = void(__fastcall*)(void* gameObj, uint8_t* task);
PFN_EvaluateDistTask g_origEvaluateDistTask = nullptr;

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
        if (!vec || !vec->begin || vec->end <= vec->begin) {
            return false;
        }
        const size_t byteSpan = static_cast<size_t>(vec->end - vec->begin);
        const size_t count    = byteSpan / stride;
        if (count == 0 || count > 256) {
            return false;
        }
        outCount = count;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool tryReadSnapshot(const ResourceVector* vec, uint32_t stride, size_t index, RawRecordSnapshot& out) noexcept
{
    __try {
        const auto* rec = reinterpret_cast<const ResourceRecordHeader*>(
            vec->begin + index * stride);

        out.priceKind = rec->priceKind;
        if (out.priceKind == -1 || out.priceKind == -3 || out.priceKind == -4) {
            return false;
        }
        if (out.priceKind < -5 || out.priceKind > 4) {
            return false;
        }
        if (!readSafeName(rec->name, out.name, sizeof(out.name))) {
            return false;
        }
        out.buyUsd  = rec->buyPriceUsd;
        out.buyRub  = rec->buyPriceRub;
        out.sellUsd = rec->sellPriceUsd;
        out.sellRub = rec->sellPriceRub;
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

        // Fast-exit domestic tasks (majority of Distribution Office tasks) before
        // touching the resource record or price table.
        const uint8_t* srcBld = *reinterpret_cast<const uint8_t* const*>(task + 0x10);
        const uint8_t* dstBld = *reinterpret_cast<const uint8_t* const*>(task + 0x48);

        if (srcBld) {
            const uint8_t* desc = *reinterpret_cast<const uint8_t* const*>(srcBld + 0x318);
            if (desc && *reinterpret_cast<const int32_t*>(desc + 0x360) == kBuildingTypeCustoms) {
                out.srcIsCustoms = true;
                out.srcFaction   = *reinterpret_cast<const int32_t*>(srcBld + 0x9EC);
            }
        }

        if (dstBld) {
            const uint8_t* desc = *reinterpret_cast<const uint8_t* const*>(dstBld + 0x318);
            if (desc && *reinterpret_cast<const int32_t*>(desc + 0x360) == kBuildingTypeCustoms) {
                out.dstIsCustoms = true;
                out.dstFaction   = *reinterpret_cast<const int32_t*>(dstBld + 0x9EC);
            }
        }

        if (!out.srcIsCustoms && !out.dstIsCustoms) {
            return false;
        }

        const auto* res = *reinterpret_cast<const ResourceRecordHeader* const*>(task + 0x00);
        if (!res) {
            return false;
        }

        out.buyUsd  = res->buyPriceUsd;
        out.buyRub  = res->buyPriceRub;
        out.sellUsd = res->sellPriceUsd;
        out.sellRub = res->sellPriceRub;

        if (g_addrs.resourceVector && g_addrs.recordStride >= sizeof(ResourceRecordHeader)) {
            const uint8_t* vecBegin = g_addrs.resourceVector->begin;
            const uint8_t* vecEnd   = g_addrs.resourceVector->end;
            const uint32_t stride   = g_addrs.recordStride;
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
            if (!readSafeName(res->name, out.resName, sizeof(out.resName))) {
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
        *reinterpret_cast<double*>(task + 0x28)  = 0.0;
        *reinterpret_cast<uint8_t*>(task + 0x40) = 0;
        *reinterpret_cast<uint8_t*>(task + 0x42) = 0;
        *reinterpret_cast<double*>(task + 0x60)  = 0.0;
        *reinterpret_cast<uint8_t*>(task + 0x78) = 0;
        *reinterpret_cast<uint8_t*>(task + 0x7A) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool tryReadSimClock(void* gameObj, int32_t& outDay, float& outTimer) noexcept
{
    __try {
        if (!gameObj) {
            return false;
        }
        const auto* base = reinterpret_cast<const uint8_t*>(gameObj);
        const int32_t day   = *reinterpret_cast<const int32_t*>(base + 0x590);
        const float   timer = *reinterpret_cast<const float*>(base + 0x59C);
        if (day < 0 || day > 366 || !std::isfinite(timer) || timer < 0.f || timer > 120.f) {
            return false;
        }
        outDay   = day;
        outTimer = timer;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall detourEvaluateDistTask(void* gameObj, uint8_t* task)
{
    if (!g_lastGameObj.load(std::memory_order_relaxed) && gameObj) {
        g_lastGameObj.store(gameObj, std::memory_order_relaxed);
    }
    if (!g_simTickObserved.load(std::memory_order_relaxed)) {
        g_simTickObserved.store(true, std::memory_order_relaxed);
    }

    auto& engine = tpl::RuleEngine::get();
    if (engine.isEnforcementEnabled() && engine.hasAnyActiveLimit()) {
        TaskTradeProbe probe{};
        if (tryProbeTask(task, probe)) {
            if (probe.needsSync) {
                syncLivePricesToRules();
            }

            bool block = false;
            if (probe.fastIndex != tpl::kInvalidFastIndex) {
                // O(1) lock-free, allocation-free path
                if (probe.srcIsCustoms) {
                    block = block || (probe.srcFaction == kFactionUsd
                                          ? engine.shouldBlockBuyUsdFast(probe.fastIndex, probe.buyUsd)
                                          : engine.shouldBlockBuyRubFast(probe.fastIndex, probe.buyRub));
                }
                if (probe.dstIsCustoms) {
                    block = block || (probe.dstFaction == kFactionUsd
                                          ? engine.shouldBlockSellUsdFast(probe.fastIndex, probe.sellUsd)
                                          : engine.shouldBlockSellRubFast(probe.fastIndex, probe.sellRub));
                }
            } else {
                // Fallback path (still zero heap allocation via string_view)
                const std::string_view resView(probe.resName);
                if (probe.srcIsCustoms) {
                    block = block || (probe.srcFaction == kFactionUsd
                                          ? engine.shouldBlockBuyUsd(resView, probe.buyUsd)
                                          : engine.shouldBlockBuyRub(resView, probe.buyRub));
                }
                if (probe.dstIsCustoms) {
                    block = block || (probe.dstFaction == kFactionUsd
                                          ? engine.shouldBlockSellUsd(resView, probe.sellUsd)
                                          : engine.shouldBlockSellRub(resView, probe.sellRub));
                }
            }

            if (block) {
                zeroTaskDispatchScores(task);
                const uint32_t slot = g_currentBucket.load(std::memory_order_relaxed) % kBlockedWindowSeconds;
                g_blockedBuckets[slot].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    g_origEvaluateDistTask(gameObj, task);
}

} // namespace

void resetBlockedStats() noexcept
{
    for (auto& bucket : g_blockedBuckets) {
        bucket.store(0, std::memory_order_relaxed);
    }
    g_currentBucket.store(0, std::memory_order_relaxed);
    g_simTickObserved.store(false, std::memory_order_relaxed);
    g_resetClockState.store(true, std::memory_order_relaxed);
    g_syncedVecCount.store(0, std::memory_order_relaxed);
}

void tickBlockedWindow() noexcept
{
    // Called from Present() on the render thread; self-throttled to ~100 ms.
    using clock = std::chrono::steady_clock;
    static clock::time_point s_lastCheck{};
    static int64_t           s_activeMsAccum = 0;
    static bool              s_hasPrevClock  = false;
    static int32_t           s_prevDay       = 0;
    static float             s_prevTimer     = 0.f;

    if (g_resetClockState.exchange(false, std::memory_order_relaxed)) {
        s_activeMsAccum = 0;
        s_hasPrevClock  = false;
        s_prevDay       = 0;
        s_prevTimer     = 0.f;
    }

    const auto now = clock::now();
    if (s_lastCheck.time_since_epoch().count() == 0) {
        s_lastCheck = now;
        return;
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastCheck).count();
    if (elapsedMs < 100) {
        return;
    }
    s_lastCheck = now;

    bool    simRunning = false;
    int32_t day        = 0;
    float   timer      = 0.f;
    void*   gameObj    = g_lastGameObj.load(std::memory_order_relaxed);
    if (tryReadSimClock(gameObj, day, timer)) {
        simRunning     = s_hasPrevClock && (day != s_prevDay || timer != s_prevTimer);
        s_prevDay      = day;
        s_prevTimer    = timer;
        s_hasPrevClock = true;
        g_simTickObserved.store(false, std::memory_order_relaxed);
    } else {
        simRunning = g_simTickObserved.exchange(false, std::memory_order_relaxed);
    }

    if (!simRunning) {
        return;
    }

    const int64_t clampedMs = (elapsedMs > 5000) ? 5000 : elapsedMs;
    s_activeMsAccum += clampedMs;

    while (s_activeMsAccum >= 1000) {
        s_activeMsAccum -= 1000;
        const uint32_t cur  = g_currentBucket.load(std::memory_order_relaxed);
        const uint32_t next = (cur + 1u) % static_cast<uint32_t>(kBlockedWindowSeconds);
        g_blockedBuckets[next].store(0, std::memory_order_relaxed);
        g_currentBucket.store(next, std::memory_order_relaxed);
    }
}

uint32_t getBlockedLast60s() noexcept
{
    uint32_t total = 0;
    for (const auto& bucket : g_blockedBuckets) {
        total += bucket.load(std::memory_order_relaxed);
    }
    return total;
}

std::optional<LivePrice> readLivePrice(uint32_t resourceIndex) noexcept
{
    if (!g_addrs.resourceVector || g_addrs.recordStride < sizeof(ResourceRecordHeader)) {
        return std::nullopt;
    }

    size_t count = 0;
    if (!tryReadCount(g_addrs.resourceVector, g_addrs.recordStride, count) || resourceIndex >= count) {
        return std::nullopt;
    }

    RawRecordSnapshot snap{};
    if (!tryReadSnapshot(g_addrs.resourceVector, g_addrs.recordStride, resourceIndex, snap)) {
        return std::nullopt;
    }

    return LivePrice{ snap.buyRub, snap.buyUsd, snap.sellRub, snap.sellUsd };
}

void syncLivePricesToRules() noexcept
{
    if (!g_addrs.resourceVector || g_addrs.recordStride < sizeof(ResourceRecordHeader)) {
        return;
    }

    size_t count = 0;
    if (!tryReadCount(g_addrs.resourceVector, g_addrs.recordStride, count)) {
        return;
    }

    g_syncedVecBegin.store(g_addrs.resourceVector->begin, std::memory_order_relaxed);
    g_syncedVecCount.store(count, std::memory_order_relaxed);

    auto& engine = tpl::RuleEngine::get();
    for (size_t i = 0; i < count; ++i) {
        RawRecordSnapshot snap{};
        if (!tryReadSnapshot(g_addrs.resourceVector, g_addrs.recordStride, i, snap)) {
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
    if (uint8_t* site = wrsr::pe::aobScanStr(scanBase, scanSize, SIG_PRICE_TABLE)) {
        const int32_t  disp   = *reinterpret_cast<const int32_t*>(site + 14);
        const uint32_t stride = *reinterpret_cast<const uint32_t*>(site + 45);
        auto*          vec    = reinterpret_cast<ResourceVector*>(site + 18 + disp);

        if (stride >= sizeof(ResourceRecordHeader) && stride <= 0x1000) {
            g_addrs.resourceVector = vec;
            g_addrs.recordStride   = stride;
            wrsr::log::info("SIG_PRICE_TABLE matched @ RVA 0x%zX -> ResourceVector=0x%p (RVA 0x%zX), stride=0x%X",
                            static_cast<size_t>(site - exeBase),
                            vec,
                            static_cast<size_t>(reinterpret_cast<uint8_t*>(vec) - exeBase),
                            stride);
        } else {
            wrsr::log::warn("SIG_PRICE_TABLE unexpected stride=0x%X", stride);
            allFound = false;
        }
    } else {
        wrsr::log::warn("SIG_PRICE_TABLE did not match");
        allFound = false;
    }

    // 2. Distribution Office / border-logistics dispatch validation
    if (uint8_t* dispatchSite = wrsr::pe::aobScanStr(scanBase, scanSize, SIG_DISPATCH_CHECK)) {
        if (MH_CreateHook(dispatchSite,
                          reinterpret_cast<void*>(detourEvaluateDistTask),
                          reinterpret_cast<void**>(&g_origEvaluateDistTask)) == MH_OK &&
            MH_EnableHook(dispatchSite) == MH_OK) {
            g_addrs.dispatchCheckSite = dispatchSite;
            wrsr::log::info("SIG_DISPATCH_CHECK hooked @ RVA 0x%zX (EvaluateDistTask)",
                            static_cast<size_t>(dispatchSite - exeBase));
        } else {
            wrsr::log::error("SIG_DISPATCH_CHECK MinHook install failed @ RVA 0x%zX",
                             static_cast<size_t>(dispatchSite - exeBase));
            allFound = false;
        }
    } else {
        wrsr::log::warn("SIG_DISPATCH_CHECK did not match");
        allFound = false;
    }

    return allFound;
}

} // namespace tpl::game
