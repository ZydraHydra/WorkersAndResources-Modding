#pragma once
// ============================================================================
// config.hpp — CustomsSpeedControl runtime configuration & INI persistence
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace csc {

inline constexpr float kMinMultiplier     = 0.25f;
inline constexpr float kMaxMultiplier     = 100.0f;
inline constexpr float kDefaultMultiplier = 2.0f;

class Config {
public:
    static Config& get() noexcept;

    bool load(const std::wstring& dllDir);
    bool save();

    // Lock-free getters for the simulation hook hot path
    [[nodiscard]] float getLoadMultiplier() const noexcept
    {
        return m_loadMultiplier.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getUnloadMultiplier() const noexcept
    {
        return m_unloadMultiplier.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool isAffectRailEnabled() const noexcept
    {
        return m_affectRail.load(std::memory_order_relaxed);
    }

    // Optional INI override for testing AOB signatures without recompiling
    [[nodiscard]] std::string customSignatureAob() const;

    // UI-bound POD fields (owned by CustomsSpeedControl.dll, bound via C ABI)
    float*   uiLoadMultiplierPtr() noexcept { return &m_uiLoadMultiplier; }
    float*   uiUnloadMultiplierPtr() noexcept { return &m_uiUnloadMultiplier; }
    uint8_t* uiAffectRailPtr() noexcept { return &m_uiAffectRail; }

    void syncFromUi() noexcept;
    void applyPreset(float multiplier) noexcept;

    [[nodiscard]] const std::wstring& iniPath() const noexcept { return m_iniPath; }

private:
    Config() = default;

    static float clampMultiplier(float v) noexcept;

    std::atomic<float> m_loadMultiplier{kDefaultMultiplier};
    std::atomic<float> m_unloadMultiplier{kDefaultMultiplier};
    std::atomic<bool>  m_affectRail{true};

    // UI-thread / POD-bound backing storage
    float   m_uiLoadMultiplier{kDefaultMultiplier};
    float   m_uiUnloadMultiplier{kDefaultMultiplier};
    uint8_t m_uiAffectRail{1};

    mutable std::mutex m_mutex;
    std::wstring       m_iniPath;
    std::string        m_customSignatureAob;
};

} // namespace csc
