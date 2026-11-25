// ============================================================================
// config.cpp — SmartDOBatching.ini loader & saver (mINI)
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

namespace sdb {

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
    char* end       = nullptr;
    const float val = std::strtof(raw.c_str(), &end);
    if (end == raw.c_str() || !std::isfinite(val)) {
        return fallback;
    }
    return val;
}

std::string formatFloat(float v)
{
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return std::string(buf);
}

} // namespace

Config& Config::get() noexcept
{
    static Config instance;
    return instance;
}

float Config::clampPct(float v) noexcept
{
    if (!std::isfinite(v)) {
        return 0.f;
    }
    return std::clamp(v, kMinFillPct, kMaxFillPct);
}

bool Config::load(const std::wstring& dllDir)
{
    std::lock_guard lock(m_mutex);

    if (!dllDir.empty()) {
        m_iniPath = dllDir + L"\\SmartDOBatching.ini";
    } else {
        m_iniPath = L"SmartDOBatching.ini";
    }

    const std::string utf8Path = wideToUtf8(m_iniPath);
    mINI::INIFile     file(utf8Path);
    mINI::INIStructure ini;

    float truckPct       = kDefaultTruckPct;
    float trainPct       = kDefaultTrainPct;
    bool  exemptEss      = true;
    bool  preventDeadlk  = true;

    if (file.read(ini) && ini.has("settings")) {
        auto& sec = ini["settings"];
        if (sec.has("min_truck_fill_pct")) {
            truckPct = clampPct(parseFloat(sec["min_truck_fill_pct"], kDefaultTruckPct));
        }
        if (sec.has("min_train_fill_pct")) {
            trainPct = clampPct(parseFloat(sec["min_train_fill_pct"], kDefaultTrainPct));
        }
        if (sec.has("exempt_essentials")) {
            exemptEss = parseBool(sec["exempt_essentials"], true);
        }
        if (sec.has("prevent_small_building_deadlock")) {
            preventDeadlk = parseBool(sec["prevent_small_building_deadlock"], true);
        }
        wrsr::log::info(
            "Loaded config from %s (truck=%.1f%% train=%.1f%% exempt_ess=%s prevent_deadlock=%s)",
            utf8Path.c_str(), truckPct, trainPct,
            exemptEss ? "true" : "false",
            preventDeadlk ? "true" : "false");
    } else {
        wrsr::log::info(
            "No existing INI at %s — using defaults (truck=%.1f%% train=%.1f%%)",
            utf8Path.c_str(), truckPct, trainPct);
    }

    m_uiMinTruckFill    = truckPct;
    m_uiMinTrainFill    = trainPct;
    m_uiExemptEssentials = exemptEss ? 1u : 0u;
    m_uiPreventDeadlock  = preventDeadlk ? 1u : 0u;

    m_minTruckFill.store(truckPct,      std::memory_order_relaxed);
    m_minTrainFill.store(trainPct,      std::memory_order_relaxed);
    m_exemptEssentials.store(exemptEss, std::memory_order_relaxed);
    m_preventDeadlock.store(preventDeadlk, std::memory_order_relaxed);
    return true;
}

bool Config::save()
{
    std::lock_guard lock(m_mutex);
    if (m_iniPath.empty()) {
        return false;
    }

    const float truckPct      = clampPct(m_uiMinTruckFill);
    const float trainPct      = clampPct(m_uiMinTrainFill);
    const bool  exemptEss     = (m_uiExemptEssentials != 0);
    const bool  preventDeadlk = (m_uiPreventDeadlock  != 0);

    m_uiMinTruckFill  = truckPct;
    m_uiMinTrainFill  = trainPct;
    m_minTruckFill.store(truckPct,       std::memory_order_relaxed);
    m_minTrainFill.store(trainPct,       std::memory_order_relaxed);
    m_exemptEssentials.store(exemptEss,  std::memory_order_relaxed);
    m_preventDeadlock.store(preventDeadlk, std::memory_order_relaxed);

    const std::string utf8Path = wideToUtf8(m_iniPath);
    mINI::INIFile     file(utf8Path);
    mINI::INIStructure ini;
    file.read(ini); // Preserve unrelated keys

    ini["settings"]["min_truck_fill_pct"]             = formatFloat(truckPct);
    ini["settings"]["min_train_fill_pct"]             = formatFloat(trainPct);
    ini["settings"]["exempt_essentials"]              = exemptEss ? "true" : "false";
    ini["settings"]["prevent_small_building_deadlock"] = preventDeadlk ? "true" : "false";

    if (!file.write(ini, true)) {
        wrsr::log::error("Failed to write config to %s", utf8Path.c_str());
        return false;
    }

    wrsr::log::info(
        "Saved config to %s (truck=%.1f%% train=%.1f%% exempt_ess=%s prevent_deadlock=%s)",
        utf8Path.c_str(), truckPct, trainPct,
        exemptEss ? "true" : "false",
        preventDeadlk ? "true" : "false");
    return true;
}

void Config::syncFromUi() noexcept
{
    m_minTruckFill.store(clampPct(m_uiMinTruckFill),   std::memory_order_relaxed);
    m_minTrainFill.store(clampPct(m_uiMinTrainFill),   std::memory_order_relaxed);
    m_exemptEssentials.store(m_uiExemptEssentials != 0, std::memory_order_relaxed);
    m_preventDeadlock.store(m_uiPreventDeadlock   != 0, std::memory_order_relaxed);
}

void Config::applyPreset(float pct) noexcept
{
    const float clamped  = clampPct(pct);
    m_uiMinTruckFill     = clamped;
    m_uiMinTrainFill     = clamped;
    m_minTruckFill.store(clamped, std::memory_order_relaxed);
    m_minTrainFill.store(clamped, std::memory_order_relaxed);
}

} // namespace sdb
