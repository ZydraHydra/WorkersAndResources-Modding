// ============================================================================
// config.cpp — CODispatchControl.ini loader & saver (mINI)
// ============================================================================
#include "config.hpp"

#include <windows.h>
#include <mini/ini.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "wrsr/logger.hpp"

namespace cdc {

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

float Config::clampFillPct(float v) noexcept
{
    if (!std::isfinite(v)) {
        return kDefaultMaterialFillPct;
    }
    return std::clamp(v, kMinMaterialFillPct, kMaxMaterialFillPct);
}

float Config::clampBusRatio(float v) noexcept
{
    if (!std::isfinite(v)) {
        return kDefaultBusWorkdayRatio;
    }
    return std::clamp(v, kMinBusWorkdayRatio, kMaxBusWorkdayRatio);
}

int Config::clampMaxTrucks(float v) noexcept
{
    if (!std::isfinite(v)) {
        return kDefaultMaxTrucksEnRoute;
    }
    const float rounded = std::round(v);
    return static_cast<int>(std::clamp(rounded, kMinMaxTrucksEnRoute, kMaxMaxTrucksEnRoute));
}

bool Config::load(const std::wstring& dllDir)
{
    std::lock_guard lock(m_mutex);

    if (!dllDir.empty()) {
        m_iniPath = dllDir + L"\\CODispatchControl.ini";
    } else {
        m_iniPath = L"CODispatchControl.ini";
    }

    const std::string utf8Path = wideToUtf8(m_iniPath);
    mINI::INIFile     file(utf8Path);
    mINI::INIStructure ini;

    float fillPct   = kDefaultMaterialFillPct;
    float busRatio  = kDefaultBusWorkdayRatio;
    int   maxTrucks = kDefaultMaxTrucksEnRoute;
    float topOff    = kDefaultTopOffFloorTons;

    if (file.read(ini) && ini.has("settings")) {
        auto& sec = ini["settings"];
        if (sec.has("min_material_fill_pct")) {
            fillPct = clampFillPct(parseFloat(sec["min_material_fill_pct"], kDefaultMaterialFillPct));
        }
        if (sec.has("min_bus_workday_ratio")) {
            busRatio = clampBusRatio(parseFloat(sec["min_bus_workday_ratio"], kDefaultBusWorkdayRatio));
        }
        if (sec.has("max_trucks_en_route")) {
            maxTrucks = clampMaxTrucks(parseFloat(
                sec["max_trucks_en_route"], static_cast<float>(kDefaultMaxTrucksEnRoute)));
        }
        if (sec.has("final_topoff_floor_tons")) {
            const float v = parseFloat(sec["final_topoff_floor_tons"], kDefaultTopOffFloorTons);
            topOff = std::clamp(v, 0.0f, 10.0f);
        }
        wrsr::log::info(
            "Loaded config from %s (fill=%.1f%% bus_ratio=%.2f max_trucks=%d topoff=%.2ft)",
            utf8Path.c_str(), fillPct, busRatio, maxTrucks, topOff);
    } else {
        wrsr::log::info(
            "No existing INI at %s — using defaults (fill=%.1f%% bus_ratio=%.2f max_trucks=%d)",
            utf8Path.c_str(), fillPct, busRatio, maxTrucks);
    }

    m_uiMinMaterialFillPct = fillPct;
    m_uiMinBusWorkdayRatio = busRatio;
    m_uiMaxTrucksEnRoute   = static_cast<float>(maxTrucks);

    m_minMaterialFillPct.store(fillPct,   std::memory_order_relaxed);
    m_minBusWorkdayRatio.store(busRatio,  std::memory_order_relaxed);
    m_maxTrucksEnRoute.store(maxTrucks,   std::memory_order_relaxed);
    m_topOffFloorTons.store(topOff,       std::memory_order_relaxed);
    return true;
}

bool Config::save()
{
    std::lock_guard lock(m_mutex);
    if (m_iniPath.empty()) {
        return false;
    }

    const float fillPct   = clampFillPct(m_uiMinMaterialFillPct);
    const float busRatio  = clampBusRatio(m_uiMinBusWorkdayRatio);
    const int   maxTrucks = clampMaxTrucks(m_uiMaxTrucksEnRoute);
    const float topOff    = m_topOffFloorTons.load(std::memory_order_relaxed);

    m_uiMinMaterialFillPct = fillPct;
    m_uiMinBusWorkdayRatio = busRatio;
    m_uiMaxTrucksEnRoute   = static_cast<float>(maxTrucks);

    m_minMaterialFillPct.store(fillPct,  std::memory_order_relaxed);
    m_minBusWorkdayRatio.store(busRatio, std::memory_order_relaxed);
    m_maxTrucksEnRoute.store(maxTrucks,  std::memory_order_relaxed);

    const std::string utf8Path = wideToUtf8(m_iniPath);
    mINI::INIFile     file(utf8Path);
    mINI::INIStructure ini;
    file.read(ini);

    ini["settings"]["min_material_fill_pct"]   = formatFloat(fillPct, "%.1f");
    ini["settings"]["min_bus_workday_ratio"]   = formatFloat(busRatio, "%.2f");
    ini["settings"]["max_trucks_en_route"]     = std::to_string(maxTrucks);
    ini["settings"]["final_topoff_floor_tons"] = formatFloat(topOff, "%.2f");

    if (!file.write(ini, true)) {
        wrsr::log::error("Failed to write config to %s", utf8Path.c_str());
        return false;
    }

    wrsr::log::info(
        "Saved config to %s (fill=%.1f%% bus_ratio=%.2f max_trucks=%d topoff=%.2ft)",
        utf8Path.c_str(), fillPct, busRatio, maxTrucks, topOff);
    return true;
}

void Config::syncFromUi() noexcept
{
    const float fillPct   = clampFillPct(m_uiMinMaterialFillPct);
    const float busRatio  = clampBusRatio(m_uiMinBusWorkdayRatio);
    const int   maxTrucks = clampMaxTrucks(m_uiMaxTrucksEnRoute);

    m_uiMaxTrucksEnRoute = static_cast<float>(maxTrucks);

    m_minMaterialFillPct.store(fillPct,  std::memory_order_relaxed);
    m_minBusWorkdayRatio.store(busRatio, std::memory_order_relaxed);
    m_maxTrucksEnRoute.store(maxTrucks,  std::memory_order_relaxed);
}

void Config::applyPreset(PresetKind preset) noexcept
{
    float fillPct   = kDefaultMaterialFillPct;
    float busRatio  = kDefaultBusWorkdayRatio;
    int   maxTrucks = kDefaultMaxTrucksEnRoute;

    switch (preset) {
    case PresetKind::Vanilla:
        fillPct   = 0.0f;
        busRatio  = 0.0f;
        maxTrucks = 10;
        break;
    case PresetKind::Balanced:
        fillPct   = 50.0f;
        busRatio  = 0.50f;
        maxTrucks = 2;
        break;
    case PresetKind::Strict:
        fillPct   = 75.0f;
        busRatio  = 1.00f;
        maxTrucks = 1;
        break;
    }

    m_uiMinMaterialFillPct = fillPct;
    m_uiMinBusWorkdayRatio = busRatio;
    m_uiMaxTrucksEnRoute   = static_cast<float>(maxTrucks);

    m_minMaterialFillPct.store(fillPct,  std::memory_order_relaxed);
    m_minBusWorkdayRatio.store(busRatio, std::memory_order_relaxed);
    m_maxTrucksEnRoute.store(maxTrucks,  std::memory_order_relaxed);
}

} // namespace cdc
