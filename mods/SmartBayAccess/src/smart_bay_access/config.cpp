// ============================================================================
// config.cpp — SmartBayAccess.ini loader & saver (mINI)
// ============================================================================
#include "config.hpp"

#include <windows.h>
#include <mini/ini.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "wrsr/logger.hpp"

namespace sba {

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

int32_t parseInt(const std::string& raw, int32_t fallback) noexcept
{
    if (raw.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long val = std::strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str()) {
        return fallback;
    }
    return static_cast<int32_t>(val);
}

BayAccessMode clampMode(int32_t raw) noexcept
{
    if (raw <= 0) return BayAccessMode::Vanilla;
    if (raw >= 2) return BayAccessMode::Unrestricted;
    return BayAccessMode::SameDirection;
}

} // namespace

Config& Config::get() noexcept
{
    static Config instance;
    return instance;
}

const char* Config::modeName(BayAccessMode mode) noexcept
{
    switch (mode) {
        case BayAccessMode::Vanilla:       return "Vanilla";
        case BayAccessMode::SameDirection: return "Same-Direction Batching";
        case BayAccessMode::Unrestricted:  return "Unrestricted Bay Entry";
        default:                           return "Same-Direction Batching";
    }
}

void Config::refreshStatusText() noexcept
{
    const auto mode = getMode();
    std::snprintf(m_modeStatusText, sizeof(m_modeStatusText),
                  "Active Mode: %s (mode = %d)",
                  modeName(mode), static_cast<int>(mode));
}

void Config::setMode(BayAccessMode mode) noexcept
{
    m_mode.store(static_cast<int32_t>(mode), std::memory_order_relaxed);
    refreshStatusText();
}

bool Config::load(const std::wstring& dllDir)
{
    std::lock_guard lock(m_mutex);

    if (!dllDir.empty()) {
        m_iniPath = dllDir + L"\\SmartBayAccess.ini";
    } else {
        m_iniPath = L"SmartBayAccess.ini";
    }

    const std::string utf8Path = wideToUtf8(m_iniPath);
    mINI::INIFile file(utf8Path);
    mINI::INIStructure ini;

    if (!file.read(ini)) {
        wrsr::log::info("sba: SmartBayAccess.ini not found, using defaults (mode=1)");
        setMode(kDefaultMode);
        return false;
    }

    const int32_t rawMode = parseInt(
        ini["SmartBayAccess"]["mode"], static_cast<int32_t>(kDefaultMode));
    setMode(clampMode(rawMode));

    wrsr::log::info("sba: loaded SmartBayAccess.ini (mode=%d: %s)",
                    static_cast<int>(getMode()), modeName(getMode()));
    return true;
}

bool Config::save()
{
    std::lock_guard lock(m_mutex);

    if (m_iniPath.empty()) {
        m_iniPath = L"SmartBayAccess.ini";
    }

    const std::string utf8Path = wideToUtf8(m_iniPath);
    mINI::INIFile file(utf8Path);
    mINI::INIStructure ini;
    file.read(ini);

    ini["SmartBayAccess"]["mode"] = std::to_string(static_cast<int>(getMode()));

    if (!file.write(ini, true)) {
        wrsr::log::error("sba: failed to write SmartBayAccess.ini");
        return false;
    }

    wrsr::log::info("sba: saved SmartBayAccess.ini (mode=%d)", static_cast<int>(getMode()));
    return true;
}

} // namespace sba
