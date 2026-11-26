// ============================================================================
// sim_speed_hook.cpp — Hardware-adaptive C3D_TIMER scaling via PreSimUpdate
// ============================================================================
#include "sim_speed_hook.hpp"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "embedded_offsets.hpp"
#include "wrsr/logger.hpp"
#include "wrsr/offsets.hpp"
#include "wrsr/pe_utils.hpp"

#include "config.hpp"

namespace ass::hook {

namespace {

std::atomic<HookState> g_state{HookState::Uninitialized};
std::atomic<float>     g_singleTickMs{0.0f};
std::atomic<float>     g_activeMultiplier{1.0f};
std::atomic<uint32_t>  g_extraSubSteps{0};

char g_statusText[192]    = "Hook: Initializing...";
char g_telemetryText[192] = "Frame Cost: -- ms | Active Speed: --x | Headroom: -- ms";

using PFN_PreSimUpdate = void(__fastcall*)(void* gameObject);
PFN_PreSimUpdate g_origPreSimUpdate = nullptr;
void*            g_preSimAddr       = nullptr;

// Resolved at install() from SOVIET64.exe .text AOBs (read-only after install).
const int32_t* g_gameSpeedPtr = nullptr;
uint8_t*       g_c3dTimerBase = nullptr;

double  g_qpcFreqToMs      = 0.0;
double  g_emaFrameMs       = 0.0;
int64_t g_prevFrameQpc     = 0;
int64_t g_lastTelemetryQpc = 0;

template <typename T>
T readUnaligned(const uint8_t* p) noexcept
{
    T val{};
    std::memcpy(&val, p, sizeof(T));
    return val;
}

uint8_t* scanUniqueAob(uint8_t* scanBase, size_t scanSize, const char* pattern, const char* label) noexcept
{
    if (!scanBase || scanSize == 0 || !pattern || !*pattern) {
        return nullptr;
    }
    uint8_t* first = wrsr::pe::aobScanStr(scanBase, scanSize, pattern);
    if (!first) {
        wrsr::log::warn("sim_speed_hook: %s matched 0 sites", label);
        return nullptr;
    }
    const size_t offsetAfterFirst = static_cast<size_t>(first - scanBase) + 1;
    if (offsetAfterFirst < scanSize) {
        uint8_t* second = wrsr::pe::aobScanStr(
            scanBase + offsetAfterFirst, scanSize - offsetAfterFirst, pattern);
        if (second) {
            wrsr::log::warn("sim_speed_hook: %s matched multiple sites — refusing ambiguous hook", label);
            return nullptr;
        }
    }
    return first;
}

uint8_t* resolveRipTarget(uint8_t* instrSite, size_t dispOff, size_t nextIpOff) noexcept
{
    const int32_t disp = readUnaligned<int32_t>(instrSite + dispOff);
    return instrSite + nextIpOff + disp;
}

void maybeUpdateTelemetry(int64_t nowQpc, float frameMs, float activeMult, float headroomMs) noexcept
{
    const double sinceLastMs = static_cast<double>(nowQpc - g_lastTelemetryQpc) * g_qpcFreqToMs;
    if (g_lastTelemetryQpc == 0 || sinceLastMs >= 150.0) {
        g_lastTelemetryQpc = nowQpc;
        std::snprintf(g_telemetryText, sizeof(g_telemetryText),
                      "Frame Cost: %.2f ms  |  Active Speed: %.1fx  |  Headroom: %.1f ms",
                      frameMs, activeMult, headroomMs);
    }
}

void __fastcall detourPreSimUpdate(void* gameObject)
{
    LARGE_INTEGER nowQpc{};
    QueryPerformanceCounter(&nowQpc);

    if (g_prevFrameQpc != 0) {
        const double rawFrameMs =
            static_cast<double>(nowQpc.QuadPart - g_prevFrameQpc) * g_qpcFreqToMs;
        // Clamp outliers (alt-tab, loading screens) to 100ms so EMA recovers immediately.
        const double clampedMs = std::clamp(rawFrameMs, 0.1, 100.0);
        if (g_emaFrameMs <= 0.0) {
            g_emaFrameMs = clampedMs;
        } else {
            constexpr double kAlpha = 0.15;
            g_emaFrameMs = (kAlpha * clampedMs) + ((1.0 - kAlpha) * g_emaFrameMs);
        }
    }
    g_prevFrameQpc = nowQpc.QuadPart;

    const float frameMs = static_cast<float>(g_emaFrameMs);
    g_singleTickMs.store(frameMs, std::memory_order_relaxed);

    const auto&   cfg       = Config::get();
    const int32_t gameSpeed = g_gameSpeedPtr ? *g_gameSpeedPtr : 0;
    const bool    enabled   = cfg.isEnabled();
    const bool    only3x    = cfg.isOnlyBoost3x();

    const int   targetFps     = cfg.getTargetMinFps();
    const float reserveMs     = cfg.getRenderReserveMs();
    const float frameTargetMs = 1000.0f / static_cast<float>(targetFps);
    const float budgetMs      = (frameTargetMs > reserveMs) ? (frameTargetMs - reserveMs) : 0.0f;
    const float headroomMs    = frameTargetMs - frameMs;

    // Cheapest-exit-first: pass straight through when disabled, paused, or below >> threshold.
    if (!enabled || gameSpeed <= 0 || (only3x && gameSpeed < 2)) {
        const float vanillaMult = (gameSpeed == 0) ? 0.0f : (gameSpeed >= 2 ? 2.86f : 1.0f);
        g_activeMultiplier.store(vanillaMult, std::memory_order_relaxed);
        g_extraSubSteps.store(0, std::memory_order_relaxed);
        maybeUpdateTelemetry(nowQpc.QuadPart, frameMs, vanillaMult, headroomMs);
        g_origPreSimUpdate(gameObject);
        return;
    }

    // Scale adaptive multiplier smoothly based on available frame headroom.
    const float maxMult = cfg.getMaxSpeedMultiplier();
    float adaptiveMult = maxMult;
    if (frameMs > budgetMs && budgetMs > 0.1f) {
        // Scale down proportionally when frame time exceeds target budget.
        const float ratio = budgetMs / frameMs;
        adaptiveMult = std::clamp(maxMult * ratio, 1.0f, maxMult);
    }

    // Apply adaptive speed to C3D_TIMER::sim_fps (+0x00) using unscaled_fps (+0x04).
    // Camera/UI use +0x04 (ignorePause=true) and remain 100% 1x responsive.
    if (g_c3dTimerBase) {
        auto*       simFpsPtr = reinterpret_cast<float*>(g_c3dTimerBase + 0x00);
        const auto* rawFpsPtr = reinterpret_cast<const float*>(g_c3dTimerBase + 0x04);
        const float rawFps    = *rawFpsPtr;
        if (rawFps > 0.0f) {
            *simFpsPtr = rawFps / adaptiveMult;
        }
    }

    g_activeMultiplier.store(adaptiveMult, std::memory_order_relaxed);
    g_extraSubSteps.store(static_cast<uint32_t>(std::lround(adaptiveMult)), std::memory_order_relaxed);
    maybeUpdateTelemetry(nowQpc.QuadPart, frameMs, adaptiveMult, headroomMs);

    // Execute vanilla PreSimUpdate exactly once per frame (zero visual/render corruption).
    g_origPreSimUpdate(gameObject);
}

} // namespace

bool install(uint8_t* exeBase, size_t exeSize)
{
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    g_qpcFreqToMs = (freq.QuadPart > 0) ? (1000.0 / static_cast<double>(freq.QuadPart)) : 0.0;

    auto docOpt = wrsr::offsets::OffsetDocument::parse(wrsr::generated::kEmbeddedOffsetsIni);
    if (!docOpt) {
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR — Failed to parse embedded offsets.ini");
        g_state.store(HookState::FailedSignature, std::memory_order_relaxed);
        return false;
    }
    const auto& doc = *docOpt;

    const std::string speedAob  = std::string(doc.getString("signatures", "game_speed_ref_aob").value_or(""));
    const std::string preSimAob = std::string(doc.getString("signatures", "pre_sim_call_aob").value_or(""));

    const size_t speedDispOff   = doc.getU32("signatures", "game_speed_disp_off").value_or(2);
    const size_t speedNextIpOff = doc.getU32("signatures", "game_speed_next_ip_off").value_or(6);
    const size_t timerDispOff   = doc.getU32("signatures", "timer_paused_disp_off").value_or(13);
    const size_t timerNextIpOff = doc.getU32("signatures", "timer_paused_next_ip_off").value_or(17);
    const size_t timerStructOff = doc.getU32("signatures", "timer_paused_struct_off").value_or(0x0C);

    const size_t preSimDispOff   = doc.getU32("signatures", "pre_sim_disp_off").value_or(33);
    const size_t preSimNextIpOff = doc.getU32("signatures", "pre_sim_next_ip_off").value_or(37);

    auto textSec = wrsr::pe::getSectionRange(exeBase, ".text");
    uint8_t* scanBase = textSec.base ? textSec.base : exeBase;
    size_t   scanSize = textSec.base ? textSec.size : exeSize;

    uint8_t* speedSite     = scanUniqueAob(scanBase, scanSize, speedAob.c_str(), "game_speed_ref_aob");
    uint8_t* preSimCallSite = scanUniqueAob(scanBase, scanSize, preSimAob.c_str(), "pre_sim_call_aob");

    if (!speedSite || !preSimCallSite) {
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: Inactive — AOB signature mismatch (WRSR update?)");
        g_state.store(HookState::FailedSignature, std::memory_order_relaxed);
        wrsr::log::error("sim_speed_hook: AOB scan failed (speedSite=%p preSimCallSite=%p)",
                         speedSite, preSimCallSite);
        return false;
    }

    g_gameSpeedPtr = reinterpret_cast<const int32_t*>(
        resolveRipTarget(speedSite, speedDispOff, speedNextIpOff));
    uint8_t* timerPausedPtr = resolveRipTarget(speedSite, timerDispOff, timerNextIpOff);
    g_c3dTimerBase = timerPausedPtr - timerStructOff;

    uint8_t* preSimTarget = resolveRipTarget(preSimCallSite, preSimDispOff, preSimNextIpOff);
    g_preSimAddr = preSimTarget;

    wrsr::log::info(
        "Resolved PreSimUpdate=0x%p (RVA +0x%zX), GameSpeed=0x%p (RVA +0x%zX), C3D_TIMER=0x%p (RVA +0x%zX)",
        preSimTarget, static_cast<size_t>(preSimTarget - exeBase),
        g_gameSpeedPtr, static_cast<size_t>(reinterpret_cast<const uint8_t*>(g_gameSpeedPtr) - exeBase),
        g_c3dTimerBase, static_cast<size_t>(g_c3dTimerBase - exeBase));

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR — MH_Initialize failed (%d)", static_cast<int>(initStatus));
        g_state.store(HookState::FailedHook, std::memory_order_relaxed);
        return false;
    }

    const MH_STATUS createStatus = MH_CreateHook(
        g_preSimAddr,
        reinterpret_cast<void*>(&detourPreSimUpdate),
        reinterpret_cast<void**>(&g_origPreSimUpdate));
    if (createStatus != MH_OK) {
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR — MH_CreateHook failed (%d)", static_cast<int>(createStatus));
        g_state.store(HookState::FailedHook, std::memory_order_relaxed);
        return false;
    }

    const MH_STATUS enableStatus = MH_EnableHook(g_preSimAddr);
    if (enableStatus != MH_OK) {
        std::snprintf(g_statusText, sizeof(g_statusText),
                      "Hook: ERROR — MH_EnableHook failed (%d)", static_cast<int>(enableStatus));
        g_state.store(HookState::FailedHook, std::memory_order_relaxed);
        return false;
    }

    std::snprintf(g_statusText, sizeof(g_statusText),
                  "Hook: Active (PreSimUpdate @ +0x%zX | Adaptive C3D_TIMER)",
                  static_cast<size_t>(preSimTarget - exeBase));
    g_state.store(HookState::Active, std::memory_order_relaxed);
    return true;
}

void uninstall() noexcept
{
    if (g_preSimAddr) {
        MH_DisableHook(g_preSimAddr);
        MH_RemoveHook(g_preSimAddr);
        g_preSimAddr = nullptr;
    }
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

const char* telemetryText() noexcept
{
    return g_telemetryText;
}

float lastSingleTickMs() noexcept
{
    return g_singleTickMs.load(std::memory_order_relaxed);
}

float lastActiveMultiplier() noexcept
{
    return g_activeMultiplier.load(std::memory_order_relaxed);
}

uint32_t lastExtraSubSteps() noexcept
{
    return g_extraSubSteps.load(std::memory_order_relaxed);
}

} // namespace ass::hook
