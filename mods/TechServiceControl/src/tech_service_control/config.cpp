// ============================================================================
// config.cpp — TechServiceControl.ini loader & saver (mINI)
// ============================================================================
#include "config.hpp"

#include <windows.h>
#include <mini/ini.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "wrsr/logger.hpp"

namespace tsc {

namespace {

std::string wideToUtf8(const std::wstring& ws)
{
    if (ws.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), needed, nullptr, nullptr);
    return out;
}

float parseFloat(const std::string& raw, float fallback) noexcept
{
    if (raw.empty()) {
        return fallback;
    }
    char* end       = nullptr;
    const float val = std::strtof(raw.c_str(), &end);
    if (end == raw.c_str() || !std::isfinite(val)) {
        return fallback;
    }
    return val;
}

bool parseBool(const std::string& raw, bool fallback) noexcept
{
    if (raw.empty()) {
        return fallback;
    }
    std::string lower;
    lower.reserve(raw.size());
    for (unsigned char ch : raw) {
        lower.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
        return false;
    }
    return fallback;
}

std::string formatFloat(float v, const char* fmt)
{
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), fmt, v);
    return std::string(buf);
}

} // namespace

Config& Config::get() noexcept
{
    static Config instance;
    return instance;
}

float Config::clampRange(float v, float minV, float maxV, float fallback) noexcept
{
    if (!std::isfinite(v)) {
        return fallback;
    }
    return std::clamp(v, minV, maxV);
}

bool Config::load(const std::wstring& dllDir)
{
    std::lock_guard lock(m_mutex);

    if (!dllDir.empty()) {
        m_iniPath = dllDir + L"\\TechServiceControl.ini";
    } else {
        m_iniPath = L"TechServiceControl.ini";
    }

    const std::string utf8Path = wideToUtf8(m_iniPath);
    mINI::INIFile     file(utf8Path);
    mINI::INIStructure ini;

    bool  enabled     = true;
    float snowplowPct = kDefaultSnowplowPct;
    float garbagePct  = kDefaultGarbagePct;
    float sewagePct   = kDefaultSewagePct;
    float waterPct    = kDefaultWaterBelowPct;

    if (file.read(ini) && ini.has("settings")) {
        auto& sec = ini["settings"];
        if (sec.has("override_enabled")) {
            enabled = parseBool(sec["override_enabled"], true);
        }
        if (sec.has("snowplow_min_pct")) {
            snowplowPct = clampRange(
                parseFloat(sec["snowplow_min_pct"], kDefaultSnowplowPct),
                kMinSnowplowPct, kMaxSnowplowPct, kDefaultSnowplowPct);
        }
        if (sec.has("garbage_min_pct")) {
            garbagePct = clampRange(
                parseFloat(sec["garbage_min_pct"], kDefaultGarbagePct),
                kMinGarbagePct, kMaxGarbagePct, kDefaultGarbagePct);
        }
        if (sec.has("sewage_min_pct")) {
            sewagePct = clampRange(
                parseFloat(sec["sewage_min_pct"], kDefaultSewagePct),
                kMinSewagePct, kMaxSewagePct, kDefaultSewagePct);
        }
        if (sec.has("water_below_pct")) {
            waterPct = clampRange(
                parseFloat(sec["water_below_pct"], kDefaultWaterBelowPct),
                kMinWaterBelowPct, kMaxWaterBelowPct, kDefaultWaterBelowPct);
        }
        wrsr::log::info(
            "Loaded config from %s (enabled=%s snow=%.1f%% garbage=%.1f%% sewage=%.1f%% water<=%.1f%%)",
            utf8Path.c_str(), enabled ? "true" : "false",
            snowplowPct, garbagePct, sewagePct, waterPct);
    } else {
        wrsr::log::info(
            "No existing INI at %s — using defaults (enabled=%s snow=%.1f%% garbage=%.1f%% sewage=%.1f%% water<=%.1f%%)",
            utf8Path.c_str(), enabled ? "true" : "false",
            snowplowPct, garbagePct, sewagePct, waterPct);
    }

    m_uiOverrideEnabled = enabled ? 1 : 0;
    m_uiSnowplowMinPct  = snowplowPct;
    m_uiGarbageMinPct   = garbagePct;
    m_uiSewageMinPct    = sewagePct;
    m_uiWaterBelowPct   = waterPct;

    m_overrideEnabled.store(enabled,     std::memory_order_relaxed);
    m_snowplowMinPct.store(snowplowPct,  std::memory_order_relaxed);
    m_garbageMinPct.store(garbagePct,    std::memory_order_relaxed);
    m_sewageMinPct.store(sewagePct,      std::memory_order_relaxed);
    m_waterBelowPct.store(waterPct,      std::memory_order_relaxed);
    return true;
}

bool Config::save()
{
    std::lock_guard lock(m_mutex);
    if (m_iniPath.empty()) {
        return false;
    }

    const bool  enabled     = (m_uiOverrideEnabled != 0);
    const float snowplowPct = clampRange(m_uiSnowplowMinPct, kMinSnowplowPct, kMaxSnowplowPct, kDefaultSnowplowPct);
    const float garbagePct  = clampRange(m_uiGarbageMinPct,  kMinGarbagePct,  kMaxGarbagePct,  kDefaultGarbagePct);
    const float sewagePct   = clampRange(m_uiSewageMinPct,   kMinSewagePct,   kMaxSewagePct,   kDefaultSewagePct);
    const float waterPct    = clampRange(m_uiWaterBelowPct,  kMinWaterBelowPct, kMaxWaterBelowPct, kDefaultWaterBelowPct);

    m_uiOverrideEnabled = enabled ? 1 : 0;
    m_uiSnowplowMinPct  = snowplowPct;
    m_uiGarbageMinPct   = garbagePct;
    m_uiSewageMinPct    = sewagePct;
    m_uiWaterBelowPct   = waterPct;

    m_overrideEnabled.store(enabled,     std::memory_order_relaxed);
    m_snowplowMinPct.store(snowplowPct,  std::memory_order_relaxed);
    m_garbageMinPct.store(garbagePct,    std::memory_order_relaxed);
    m_sewageMinPct.store(sewagePct,      std::memory_order_relaxed);
    m_waterBelowPct.store(waterPct,      std::memory_order_relaxed);

    const std::string utf8Path = wideToUtf8(m_iniPath);
    mINI::INIFile     file(utf8Path);
    mINI::INIStructure ini;
    file.read(ini);

    ini["settings"]["override_enabled"] = enabled ? "true" : "false";
    ini["settings"]["snowplow_min_pct"] = formatFloat(snowplowPct, "%.1f");
    ini["settings"]["garbage_min_pct"]  = formatFloat(garbagePct,  "%.1f");
    ini["settings"]["sewage_min_pct"]   = formatFloat(sewagePct,   "%.1f");
    ini["settings"]["water_below_pct"]  = formatFloat(waterPct,    "%.1f");

    if (!file.write(ini, true)) {
        wrsr::log::error("Failed to write config to %s", utf8Path.c_str());
        return false;
    }

    wrsr::log::info(
        "Saved config to %s (enabled=%s snow=%.1f%% garbage=%.1f%% sewage=%.1f%% water<=%.1f%%)",
        utf8Path.c_str(), enabled ? "true" : "false",
        snowplowPct, garbagePct, sewagePct, waterPct);
    return true;
}

void Config::syncFromUi() noexcept
{
    const bool  enabled     = (m_uiOverrideEnabled != 0);
    const float snowplowPct = clampRange(m_uiSnowplowMinPct, kMinSnowplowPct, kMaxSnowplowPct, kDefaultSnowplowPct);
    const float garbagePct  = clampRange(m_uiGarbageMinPct,  kMinGarbagePct,  kMaxGarbagePct,  kDefaultGarbagePct);
    const float sewagePct   = clampRange(m_uiSewageMinPct,   kMinSewagePct,   kMaxSewagePct,   kDefaultSewagePct);
    const float waterPct    = clampRange(m_uiWaterBelowPct,  kMinWaterBelowPct, kMaxWaterBelowPct, kDefaultWaterBelowPct);

    m_overrideEnabled.store(enabled,     std::memory_order_relaxed);
    m_snowplowMinPct.store(snowplowPct,  std::memory_order_relaxed);
    m_garbageMinPct.store(garbagePct,    std::memory_order_relaxed);
    m_sewageMinPct.store(sewagePct,      std::memory_order_relaxed);
    m_waterBelowPct.store(waterPct,      std::memory_order_relaxed);
}

void Config::applyPreset(PresetKind preset) noexcept
{
    bool  enabled     = true;
    float snowplowPct = kDefaultSnowplowPct;
    float garbagePct  = kDefaultGarbagePct;
    float sewagePct   = kDefaultSewagePct;
    float waterPct    = kDefaultWaterBelowPct;

    switch (preset) {
    case PresetKind::Vanilla:
        enabled     = false;
        snowplowPct = 1.0f;
        garbagePct  = 75.0f;
        sewagePct   = 70.0f;
        waterPct    = 30.0f;
        break;
    case PresetKind::FuelSaver:
        enabled     = true;
        snowplowPct = 35.0f;
        garbagePct  = 85.0f;
        sewagePct   = 85.0f;
        waterPct    = 20.0f;
        break;
    case PresetKind::DenseCity:
        enabled     = true;
        snowplowPct = 15.0f;
        garbagePct  = 50.0f;
        sewagePct   = 55.0f;
        waterPct    = 55.0f;
        break;
    }

    m_uiOverrideEnabled = enabled ? 1 : 0;
    m_uiSnowplowMinPct  = snowplowPct;
    m_uiGarbageMinPct   = garbagePct;
    m_uiSewageMinPct    = sewagePct;
    m_uiWaterBelowPct   = waterPct;

    m_overrideEnabled.store(enabled,     std::memory_order_relaxed);
    m_snowplowMinPct.store(snowplowPct,  std::memory_order_relaxed);
    m_garbageMinPct.store(garbagePct,    std::memory_order_relaxed);
    m_sewageMinPct.store(sewagePct,      std::memory_order_relaxed);
    m_waterBelowPct.store(waterPct,      std::memory_order_relaxed);
}

} // namespace tsc
