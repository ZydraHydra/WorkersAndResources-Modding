#pragma once
// ============================================================================
// config.hpp — SmartDOBatching runtime configuration & INI persistence
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace sdb {

// Percentage clamp bounds [0, 100]
inline constexpr float kMinFillPct     = 0.0f;
inline constexpr float kMaxFillPct     = 100.0f;
inline constexpr float kDefaultTruckPct = 50.0f;
inline constexpr float kDefaultTrainPct = 60.0f;
// When clamping required cargo to building storage, use 85% of storage cap
inline constexpr float kDeadlockCapRatio = 0.85f;

class Config {
public:
    static Config& get() noexcept;

    bool load(const std::wstring& dllDir);
    bool save();

    // ── Lock-free hot-path getters ────────────────────────────────────────
    [[nodiscard]] float getMinTruckFillPct() const noexcept
    {
        return m_minTruckFill.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getMinTrainFillPct() const noexcept
    {
        return m_minTrainFill.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool isExemptEssentials() const noexcept
    {
        return m_exemptEssentials.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool isPreventDeadlock() const noexcept
    {
        return m_preventDeadlock.load(std::memory_order_relaxed);
    }

    // ── UI-bound POD fields (owned by SmartDOBatching.dll) ────────────────
    float*   uiMinTruckFillPtr()     noexcept { return &m_uiMinTruckFill; }
    float*   uiMinTrainFillPtr()     noexcept { return &m_uiMinTrainFill; }
    uint8_t* uiExemptEssentialsPtr() noexcept { return &m_uiExemptEssentials; }
    uint8_t* uiPreventDeadlockPtr()  noexcept { return &m_uiPreventDeadlock; }

    void syncFromUi() noexcept;
    void applyPreset(float pct) noexcept;

    [[nodiscard]] const std::wstring& iniPath() const noexcept { return m_iniPath; }

private:
    Config() = default;

    static float clampPct(float v) noexcept;

    std::atomic<float> m_minTruckFill{kDefaultTruckPct};
    std::atomic<float> m_minTrainFill{kDefaultTrainPct};
    std::atomic<bool>  m_exemptEssentials{true};
    std::atomic<bool>  m_preventDeadlock{true};

    // UI-thread / POD-bound backing storage
    float   m_uiMinTruckFill{kDefaultTruckPct};
    float   m_uiMinTrainFill{kDefaultTrainPct};
    uint8_t m_uiExemptEssentials{1};
    uint8_t m_uiPreventDeadlock{1};

    mutable std::mutex m_mutex;
    std::wstring       m_iniPath;
};

} // namespace sdb
