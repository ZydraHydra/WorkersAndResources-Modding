// ============================================================================
// config.cpp — MediaRedirector.ini loader & saver (mINI)
// ============================================================================
#include "config.hpp"

#include <windows.h>
#include <mini/ini.h>

#include <cctype>
#include <cstdio>
#include <string>

#include "wrsr/logger.hpp"

namespace mr {

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

} // namespace

Config& Config::get() noexcept
{
    static Config instance;
    return instance;
}

bool Config::load(const std::wstring& dllDir)
{
    std::lock_guard lock(m_mutex);

    if (!dllDir.empty()) {
        m_iniPath = dllDir + L"\\MediaRedirector.ini";
    } else {
        m_iniPath = L"MediaRedirector.ini";
    }

    const std::string utf8Path = wideToUtf8(m_iniPath);
    mINI::INIFile     file(utf8Path);
    mINI::INIStructure ini;

    bool enabled      = true;
    bool scanWorkshop = true;
    std::vector<std::string>             priorityOrder;
    std::unordered_map<std::string, bool> packEnabled;

    if (file.read(ini)) {
        if (ini.has("settings")) {
            if (ini["settings"].has("enabled")) {
                enabled = parseBool(ini["settings"]["enabled"], true);
            }
            if (ini["settings"].has("scan_workshop_overrides")) {
                scanWorkshop = parseBool(ini["settings"]["scan_workshop_overrides"], true);
            }
        }

        if (ini.has("Priority")) {
            for (const auto& kv : ini["Priority"]) {
                if (!kv.second.empty()) {
                    priorityOrder.push_back(kv.second);
                }
            }
        }

        if (ini.has("Enabled")) {
            for (const auto& kv : ini["Enabled"]) {
                if (!kv.first.empty()) {
                    packEnabled[kv.first] = parseBool(kv.second, true);
                }
            }
        }

        wrsr::log::info("Loaded config from %s (enabled=%s scan_workshop=%s packs=%zu)",
                        utf8Path.c_str(),
                        enabled ? "true" : "false",
                        scanWorkshop ? "true" : "false",
                        priorityOrder.size());
    } else {
        wrsr::log::info("No existing INI at %s — using defaults (enabled=true scan_workshop=true)",
                        utf8Path.c_str());
    }

    m_uiEnabled      = enabled ? 1u : 0u;
    m_uiScanWorkshop = scanWorkshop ? 1u : 0u;
    m_enabled.store(enabled, std::memory_order_relaxed);
    m_scanWorkshop.store(scanWorkshop, std::memory_order_relaxed);

    m_priorityOrder = std::move(priorityOrder);
    m_packEnabled   = std::move(packEnabled);
    return true;
}

bool Config::save()
{
    std::lock_guard lock(m_mutex);
    if (m_iniPath.empty()) {
        return false;
    }

    const bool enabled      = (m_uiEnabled != 0);
    const bool scanWorkshop = (m_uiScanWorkshop != 0);
    m_enabled.store(enabled, std::memory_order_relaxed);
    m_scanWorkshop.store(scanWorkshop, std::memory_order_relaxed);

    const std::string utf8Path = wideToUtf8(m_iniPath);
    mINI::INIFile     file(utf8Path);
    mINI::INIStructure ini;
    file.read(ini);

    ini["settings"]["enabled"]                 = enabled ? "true" : "false";
    ini["settings"]["scan_workshop_overrides"] = scanWorkshop ? "true" : "false";

    ini["Priority"].clear();
    for (size_t i = 0; i < m_priorityOrder.size(); ++i) {
        char keyBuf[32]{};
        std::snprintf(keyBuf, sizeof(keyBuf), "%zu", i);
        ini["Priority"][keyBuf] = m_priorityOrder[i];
    }

    ini["Enabled"].clear();
    for (const auto& id : m_priorityOrder) {
        const auto it = m_packEnabled.find(id);
        const bool isOn = (it == m_packEnabled.end()) ? false : it->second;
        ini["Enabled"][id] = isOn ? "true" : "false";
    }

    if (!file.write(ini, true)) {
        wrsr::log::error("Failed to write config to %s", utf8Path.c_str());
        return false;
    }

    wrsr::log::info("Saved config to %s (enabled=%s scan_workshop=%s packs=%zu)",
                    utf8Path.c_str(),
                    enabled ? "true" : "false",
                    scanWorkshop ? "true" : "false",
                    m_priorityOrder.size());
    return true;
}

std::vector<std::string> Config::priorityOrder() const
{
    std::lock_guard lock(m_mutex);
    return m_priorityOrder;
}

bool Config::isPackEnabled(const std::string& packId) const
{
    std::lock_guard lock(m_mutex);
    const auto it = m_packEnabled.find(packId);
    return (it == m_packEnabled.end()) ? false : it->second;
}

void Config::setPackStates(const std::vector<std::string>& orderedIds,
                           const std::unordered_map<std::string, bool>& enabledMap)
{
    std::lock_guard lock(m_mutex);
    m_priorityOrder = orderedIds;
    for (const auto& kv : enabledMap) {
        m_packEnabled[kv.first] = kv.second;
    }
}

void Config::syncFromUi() noexcept
{
    const bool enabled      = (m_uiEnabled != 0);
    const bool scanWorkshop = (m_uiScanWorkshop != 0);
    m_enabled.store(enabled, std::memory_order_relaxed);
    m_scanWorkshop.store(scanWorkshop, std::memory_order_relaxed);
}

} // namespace mr
