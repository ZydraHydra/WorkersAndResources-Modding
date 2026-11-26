#pragma once
// ============================================================================
// config.hpp — MediaRedirector configuration & INI persistence (mINI)
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mr {

class Config {
public:
    static Config& get() noexcept;

    bool load(const std::wstring& dllDir);
    bool save();

    // Lock-free getter for the CreateFileW / GetFileAttributesW hot path
    [[nodiscard]] bool isEnabled() const noexcept
    {
        return m_enabled.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isScanWorkshopEnabled() const noexcept
    {
        return m_scanWorkshop.load(std::memory_order_relaxed);
    }

    // Priority & enabled state queries used during early-boot VFS indexing
    [[nodiscard]] std::vector<std::string> priorityOrder() const;
    [[nodiscard]] bool isPackEnabled(const std::string& packId) const;

    // Update persisted pack order and enabled flags (from VFS discovery or UI)
    void setPackStates(const std::vector<std::string>& orderedIds,
                       const std::unordered_map<std::string, bool>& enabledMap);

    // UI-bound POD fields (owned by MediaRedirector.dll, bound via C ABI)
    uint8_t* uiEnabledPtr() noexcept { return &m_uiEnabled; }
    uint8_t* uiScanWorkshopPtr() noexcept { return &m_uiScanWorkshop; }

    void syncFromUi() noexcept;

    [[nodiscard]] const std::wstring& iniPath() const noexcept { return m_iniPath; }

private:
    Config() = default;

    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_scanWorkshop{true};

    uint8_t m_uiEnabled{1};
    uint8_t m_uiScanWorkshop{1};

    mutable std::mutex                   m_mutex;
    std::wstring                         m_iniPath;
    std::vector<std::string>             m_priorityOrder;
    std::unordered_map<std::string, bool> m_packEnabled;
};

} // namespace mr
