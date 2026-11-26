#pragma once
// ============================================================================
// config.hpp — SmartBayAccess runtime configuration & INI persistence
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace sba {

enum class BayAccessMode : int32_t {
    Vanilla       = 0,
    SameDirection = 1,
    Unrestricted  = 2,
};

inline constexpr BayAccessMode kDefaultMode = BayAccessMode::SameDirection;

class Config {
public:
    static Config& get() noexcept;

    bool load(const std::wstring& dllDir);
    bool save();

    [[nodiscard]] BayAccessMode getMode() const noexcept
    {
        return static_cast<BayAccessMode>(m_mode.load(std::memory_order_relaxed));
    }

    void setMode(BayAccessMode mode) noexcept;

    [[nodiscard]] static const char* modeName(BayAccessMode mode) noexcept;
    [[nodiscard]] const char* modeStatusText() const noexcept { return m_modeStatusText; }

    [[nodiscard]] const std::wstring& iniPath() const noexcept { return m_iniPath; }

private:
    Config() = default;

    void refreshStatusText() noexcept;

    std::atomic<int32_t> m_mode{static_cast<int32_t>(kDefaultMode)};
    char                 m_modeStatusText[96] = "Active Mode: Same-Direction Batching (Default)";

    mutable std::mutex m_mutex;
    std::wstring       m_iniPath;
};

} // namespace sba
