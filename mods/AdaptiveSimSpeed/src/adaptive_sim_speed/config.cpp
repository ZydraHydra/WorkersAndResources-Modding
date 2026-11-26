// ============================================================================
// config.cpp — AdaptiveSimSpeed configuration & mINI persistence
// ============================================================================
#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <mini/ini.h>

#include "wrsr/logger.hpp"

namespace ass {

namespace {

bool parseBool(const std::string& raw, bool fallback) noexcept
{
    if (raw.empty()) {
        return fallback;
    }
    std::string lower;
    lower.reserve(raw.size());
    for (char c : raw) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
        return false;
    }
    return fallback;
}

float parseFloat(const std::string& raw, float fallback) noexcept
{
    if (raw.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const float val = std::strtof(raw.c_str(), &end);
    if (end == raw.c_str() || !std::isfinite(val)) {
        return fallback;
    }
    return val;
}

int parseInt(const std::string& raw, int fallback) noexcept
{
    if (raw.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long val = std::strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str()) {
        return fallback;
    }
    return static_cast<int>(val);
}

std::string formatFloat(float v, const char* fmt)
{
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), fmt, v);
    return buf;
}

} // namespace

Config& Config::get() noexcept
{
    static Config instance;
    return instance;
}

float Config::clampSpeedMultiplier(float v) noexcept
{
    if (!std::isfinite(v)) {
        return kDefaultSpeedMultiplier;
    }
    return std::clamp(v, kMinSpeedMultiplier, kMaxSpeedMultiplier);
}

int Config::clampTargetFps(int v) noexcept
{
    return std::clamp(v, kMinTargetFps, kMaxTargetFps);
}

float Config::clampRenderReserveMs(float v) noexcept
{
    if (!std::isfinite(v)) {
        return kDefaultRenderReserveMs;
    }
    return std::clamp(v, kMinRenderReserveMs, kMaxRenderReserveMs);
}

bool Config::load(const std::wstring& dllDir)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_iniPath = dllDir.empty() ? L"AdaptiveSimSpeed.ini"
                               : (dllDir + L"\\AdaptiveSimSpeed.ini");

    bool  enabled     = true;
    bool  onlyBoost3x = true;
    float maxSpeed    = kDefaultSpeedMultiplier;
    int   targetFps   = kDefaultTargetFps;
    float reserveMs   = kDefaultRenderReserveMs;

    mINI::INIFile      file(m_iniPath);
    mINI::INIStructure ini;
    if (file.read(ini)) {
        if (ini.has("settings")) {
            auto& s     = ini["settings"];
            enabled     = parseBool(s["enabled"], enabled);
            onlyBoost3x = parseBool(s["only_boost_3x"], onlyBoost3x);
            maxSpeed    = clampSpeedMultiplier(parseFloat(s["max_speed_multiplier"], maxSpeed));
            targetFps   = clampTargetFps(parseInt(s["target_min_fps"], targetFps));
            reserveMs   = clampRenderReserveMs(parseFloat(s["render_reserve_ms"], reserveMs));
        }
        wrsr::log::info(
            "Loaded config: enabled=%d only_boost_3x=%d max=%.1fx target_fps=%d reserve=%.1fms",
            enabled ? 1 : 0, onlyBoost3x ? 1 : 0, maxSpeed, targetFps, reserveMs);
    } else {
        wrsr::log::info(
            "AdaptiveSimSpeed.ini not found — using defaults (enabled=1, only_boost_3x=1, max=%.1fx, fps=%d, reserve=%.1fms)",
            maxSpeed, targetFps, reserveMs);
    }

    m_enabled.store(enabled, std::memory_order_relaxed);
    m_onlyBoost3x.store(onlyBoost3x, std::memory_order_relaxed);
    m_maxSpeedMultiplier.store(maxSpeed, std::memory_order_relaxed);
    m_targetMinFps.store(targetFps, std::memory_order_relaxed);
    m_renderReserveMs.store(reserveMs, std::memory_order_relaxed);

    m_uiEnabled            = enabled ? 1 : 0;
    m_uiOnlyBoost3x        = onlyBoost3x ? 1 : 0;
    m_uiMaxSpeedMultiplier = maxSpeed;
    m_uiTargetMinFps       = static_cast<float>(targetFps);
    m_uiRenderReserveMs    = reserveMs;

    return true;
}

bool Config::save()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const bool  enabled     = m_enabled.load(std::memory_order_relaxed);
    const bool  onlyBoost3x = m_onlyBoost3x.load(std::memory_order_relaxed);
    const float maxSpeed    = m_maxSpeedMultiplier.load(std::memory_order_relaxed);
    const int   targetFps   = m_targetMinFps.load(std::memory_order_relaxed);
    const float reserveMs   = m_renderReserveMs.load(std::memory_order_relaxed);

    mINI::INIFile      file(m_iniPath.empty() ? L"AdaptiveSimSpeed.ini" : m_iniPath);
    mINI::INIStructure ini;
    file.read(ini);

    ini["settings"]["enabled"]              = enabled ? "true" : "false";
    ini["settings"]["only_boost_3x"]        = onlyBoost3x ? "true" : "false";
    ini["settings"]["max_speed_multiplier"] = formatFloat(maxSpeed, "%.1f");
    ini["settings"]["target_min_fps"]       = std::to_string(targetFps);
    ini["settings"]["render_reserve_ms"]    = formatFloat(reserveMs, "%.1f");

    if (!file.write(ini, true)) {
        wrsr::log::error("Failed to write AdaptiveSimSpeed.ini");
        return false;
    }

    wrsr::log::info(
        "Saved AdaptiveSimSpeed.ini (enabled=%d only_boost_3x=%d max=%.1fx fps=%d reserve=%.1fms)",
        enabled ? 1 : 0, onlyBoost3x ? 1 : 0, maxSpeed, targetFps, reserveMs);
    return true;
}

void Config::syncFromUi() noexcept
{
    const bool  enabled     = (m_uiEnabled != 0);
    const bool  onlyBoost3x = (m_uiOnlyBoost3x != 0);
    const float maxSpeed    = clampSpeedMultiplier(m_uiMaxSpeedMultiplier);
    const int   targetFps   = clampTargetFps(static_cast<int>(std::lround(m_uiTargetMinFps)));
    const float reserveMs   = clampRenderReserveMs(m_uiRenderReserveMs);

    m_uiMaxSpeedMultiplier = maxSpeed;
    m_uiTargetMinFps       = static_cast<float>(targetFps);
    m_uiRenderReserveMs    = reserveMs;

    m_enabled.store(enabled, std::memory_order_relaxed);
    m_onlyBoost3x.store(onlyBoost3x, std::memory_order_relaxed);
    m_maxSpeedMultiplier.store(maxSpeed, std::memory_order_relaxed);
    m_targetMinFps.store(targetFps, std::memory_order_relaxed);
    m_renderReserveMs.store(reserveMs, std::memory_order_relaxed);
}

void Config::applyPreset(float maxMultiplier) noexcept
{
    const float clamped    = clampSpeedMultiplier(maxMultiplier);
    m_uiMaxSpeedMultiplier = clamped;
    m_maxSpeedMultiplier.store(clamped, std::memory_order_relaxed);
}

} // namespace ass
