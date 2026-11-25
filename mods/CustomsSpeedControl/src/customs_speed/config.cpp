// ============================================================================
// config.cpp — CustomsSpeedControl.ini loader & saver (mINI)
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

namespace csc {

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
    char* end = nullptr;
    const float val = std::strtof(raw.c_str(), &end);
    if (end == raw.c_str() || !std::isfinite(val)) {
        return fallback;
    }
    return val;
}

std::string formatFloat(float v)
{
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
}

} // namespace

Config& Config::get() noexcept
{
    static Config instance;
    return instance;
}

float Config::clampMultiplier(float v) noexcept
{
    if (!std::isfinite(v)) {
        return kDefaultMultiplier;
    }
    return std::clamp(v, kMinMultiplier, kMaxMultiplier);
}

bool Config::load(const std::wstring& dllDir)
{
    std::lock_guard lock(m_mutex);

    if (!dllDir.empty()) {
        m_iniPath = dllDir + L"\\CustomsSpeedControl.ini";
    } else {
        m_iniPath = L"CustomsSpeedControl.ini";
    }

    const std::string utf8Path = wideToUtf8(m_iniPath);
    mINI::INIFile     file(utf8Path);
    mINI::INIStructure ini;

    float loadMult   = kDefaultMultiplier;
    float unloadMult = kDefaultMultiplier;
    bool  affectRail = true;
    std::string customAob;

    if (file.read(ini)) {
        const char* sec = ini.has("settings") ? "settings" : "customs_speed";
        if (ini.has(sec)) {
            if (ini[sec].has("load_multiplier")) {
                loadMult = clampMultiplier(parseFloat(ini[sec]["load_multiplier"], kDefaultMultiplier));
            }
            if (ini[sec].has("unload_multiplier")) {
                unloadMult = clampMultiplier(parseFloat(ini[sec]["unload_multiplier"], kDefaultMultiplier));
            }
            if (ini[sec].has("affect_rail")) {
                affectRail = parseBool(ini[sec]["affect_rail"], true);
            }
        }
        if (ini.has("signatures") && ini["signatures"].has("cargo_transfer_tick_aob")) {
            customAob = ini["signatures"]["cargo_transfer_tick_aob"];
        }
        wrsr::log::info("Loaded config from %s (load=%.2fx unload=%.2fx affect_rail=%s)",
                        utf8Path.c_str(), loadMult, unloadMult, affectRail ? "true" : "false");
    } else {
        wrsr::log::info("No existing INI at %s — using defaults (load=%.2fx unload=%.2fx affect_rail=%s)",
                        utf8Path.c_str(), loadMult, unloadMult, affectRail ? "true" : "false");
    }

    m_uiLoadMultiplier   = loadMult;
    m_uiUnloadMultiplier = unloadMult;
    m_uiAffectRail       = affectRail ? 1u : 0u;
    m_customSignatureAob = std::move(customAob);

    m_loadMultiplier.store(loadMult, std::memory_order_relaxed);
    m_unloadMultiplier.store(unloadMult, std::memory_order_relaxed);
    m_affectRail.store(affectRail, std::memory_order_relaxed);
    return true;
}

bool Config::save()
{
    std::lock_guard lock(m_mutex);
    if (m_iniPath.empty()) {
        return false;
    }

    const float loadMult   = clampMultiplier(m_uiLoadMultiplier);
    const float unloadMult = clampMultiplier(m_uiUnloadMultiplier);
    const bool  affectRail = (m_uiAffectRail != 0);

    m_uiLoadMultiplier   = loadMult;
    m_uiUnloadMultiplier = unloadMult;
    m_loadMultiplier.store(loadMult, std::memory_order_relaxed);
    m_unloadMultiplier.store(unloadMult, std::memory_order_relaxed);
    m_affectRail.store(affectRail, std::memory_order_relaxed);

    const std::string utf8Path = wideToUtf8(m_iniPath);
    mINI::INIFile     file(utf8Path);
    mINI::INIStructure ini;
    file.read(ini); // Preserve unrelated sections/keys (e.g. [signatures])

    ini["settings"]["load_multiplier"]   = formatFloat(loadMult);
    ini["settings"]["unload_multiplier"] = formatFloat(unloadMult);
    ini["settings"]["affect_rail"]       = affectRail ? "true" : "false";

    if (!file.write(ini, true)) {
        wrsr::log::error("Failed to write config to %s", utf8Path.c_str());
        return false;
    }

    wrsr::log::info("Saved config to %s (load=%.2fx unload=%.2fx affect_rail=%s)",
                    utf8Path.c_str(), loadMult, unloadMult, affectRail ? "true" : "false");
    return true;
}

std::string Config::customSignatureAob() const
{
    std::lock_guard lock(m_mutex);
    return m_customSignatureAob;
}

void Config::syncFromUi() noexcept
{
    const float loadMult   = clampMultiplier(m_uiLoadMultiplier);
    const float unloadMult = clampMultiplier(m_uiUnloadMultiplier);
    const bool  affectRail = (m_uiAffectRail != 0);

    m_loadMultiplier.store(loadMult, std::memory_order_relaxed);
    m_unloadMultiplier.store(unloadMult, std::memory_order_relaxed);
    m_affectRail.store(affectRail, std::memory_order_relaxed);
}

void Config::applyPreset(float multiplier) noexcept
{
    const float clamped  = clampMultiplier(multiplier);
    m_uiLoadMultiplier   = clamped;
    m_uiUnloadMultiplier = clamped;
    m_loadMultiplier.store(clamped, std::memory_order_relaxed);
    m_unloadMultiplier.store(clamped, std::memory_order_relaxed);
}

} // namespace csc
