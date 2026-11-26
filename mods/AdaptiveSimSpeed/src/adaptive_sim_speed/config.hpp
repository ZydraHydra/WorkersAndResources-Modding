#pragma once
// ============================================================================
// config.hpp — AdaptiveSimSpeed runtime configuration & INI persistence
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace ass {

inline constexpr float kMinSpeedMultiplier     = 3.0f;
inline constexpr float kMaxSpeedMultiplier     = 15.0f;
inline constexpr float kDefaultSpeedMultiplier = 8.0f;

inline constexpr int   kMinTargetFps     = 30;
inline constexpr int   kMaxTargetFps     = 120;
inline constexpr int   kDefaultTargetFps = 60;

inline constexpr float kMinRenderReserveMs     = 1.5f;
inline constexpr float kMaxRenderReserveMs     = 10.0f;
inline constexpr float kDefaultRenderReserveMs = 4.0f;

class Config {
public:
    static Config& get() noexcept;

    bool load(const std::wstring& dllDir);
    bool save();

    // Lock-free getters for the simulation hook hot path
    [[nodiscard]] bool isEnabled() const noexcept
    {
        return m_enabled.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool isOnlyBoost3x() const noexcept
    {
        return m_onlyBoost3x.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getMaxSpeedMultiplier() const noexcept
    {
        return m_maxSpeedMultiplier.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int getTargetMinFps() const noexcept
    {
        return m_targetMinFps.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getRenderReserveMs() const noexcept
    {
        return m_renderReserveMs.load(std::memory_order_relaxed);
    }

    // UI-bound POD fields (owned by AdaptiveSimSpeed.dll, bound via C ABI)
    uint8_t* uiEnabledPtr() noexcept { return &m_uiEnabled; }
    uint8_t* uiOnlyBoost3xPtr() noexcept { return &m_uiOnlyBoost3x; }
    float*   uiMaxSpeedMultiplierPtr() noexcept { return &m_uiMaxSpeedMultiplier; }
    float*   uiTargetMinFpsPtr() noexcept { return &m_uiTargetMinFps; }
    float*   uiRenderReserveMsPtr() noexcept { return &m_uiRenderReserveMs; }

    void syncFromUi() noexcept;
    void applyPreset(float maxMultiplier) noexcept;

    [[nodiscard]] const std::wstring& iniPath() const noexcept { return m_iniPath; }

private:
    Config() = default;

    static float clampSpeedMultiplier(float v) noexcept;
    static int   clampTargetFps(int v) noexcept;
    static float clampRenderReserveMs(float v) noexcept;

    std::atomic<bool>  m_enabled{true};
    std::atomic<bool>  m_onlyBoost3x{true};
    std::atomic<float> m_maxSpeedMultiplier{kDefaultSpeedMultiplier};
    std::atomic<int>   m_targetMinFps{kDefaultTargetFps};
    std::atomic<float> m_renderReserveMs{kDefaultRenderReserveMs};

    // UI-thread / POD-bound backing storage
    uint8_t m_uiEnabled{1};
    uint8_t m_uiOnlyBoost3x{1};
    float   m_uiMaxSpeedMultiplier{kDefaultSpeedMultiplier};
    float   m_uiTargetMinFps{static_cast<float>(kDefaultTargetFps)};
    float   m_uiRenderReserveMs{kDefaultRenderReserveMs};

    mutable std::mutex m_mutex;
    std::wstring       m_iniPath;
};

} // namespace ass
