#pragma once
// ============================================================================
// config.hpp — CODispatchControl runtime configuration & INI persistence
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace cdc {

inline constexpr float kMinMaterialFillPct     = 0.0f;
inline constexpr float kMaxMaterialFillPct     = 100.0f;
inline constexpr float kDefaultMaterialFillPct = 50.0f;

inline constexpr float kMinBusWorkdayRatio     = 0.0f;
inline constexpr float kMaxBusWorkdayRatio     = 2.0f;
inline constexpr float kDefaultBusWorkdayRatio = 0.5f;

inline constexpr float kMinMaxTrucksEnRoute    = 1.0f;
inline constexpr float kMaxMaxTrucksEnRoute    = 10.0f;
inline constexpr int   kDefaultMaxTrucksEnRoute = 2;

inline constexpr float kDefaultTopOffFloorTons = 1.5f;

enum class PresetKind : uintptr_t {
    Vanilla  = 0,
    Balanced = 1,
    Strict   = 2,
};

class Config {
public:
    static Config& get() noexcept;

    bool load(const std::wstring& dllDir);
    bool save();

    // ── Lock-free hot-path getters ────────────────────────────────────────
    [[nodiscard]] float getMinMaterialFillPct() const noexcept
    {
        return m_minMaterialFillPct.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getMinBusWorkdayRatio() const noexcept
    {
        return m_minBusWorkdayRatio.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int getMaxTrucksEnRoute() const noexcept
    {
        return m_maxTrucksEnRoute.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getTopOffFloorTons() const noexcept
    {
        return m_topOffFloorTons.load(std::memory_order_relaxed);
    }

    // ── UI-bound POD fields (owned by CODispatchControl.dll) ──────────────
    float* uiMinMaterialFillPtr() noexcept { return &m_uiMinMaterialFillPct; }
    float* uiMinBusWorkdayPtr()   noexcept { return &m_uiMinBusWorkdayRatio; }
    float* uiMaxTrucksEnRoutePtr() noexcept { return &m_uiMaxTrucksEnRoute; }

    void syncFromUi() noexcept;
    void applyPreset(PresetKind preset) noexcept;

    [[nodiscard]] const std::wstring& iniPath() const noexcept { return m_iniPath; }

private:
    Config() = default;

    static float clampFillPct(float v) noexcept;
    static float clampBusRatio(float v) noexcept;
    static int   clampMaxTrucks(float v) noexcept;

    std::atomic<float> m_minMaterialFillPct{kDefaultMaterialFillPct};
    std::atomic<float> m_minBusWorkdayRatio{kDefaultBusWorkdayRatio};
    std::atomic<int>   m_maxTrucksEnRoute{kDefaultMaxTrucksEnRoute};
    std::atomic<float> m_topOffFloorTons{kDefaultTopOffFloorTons};

    // UI-thread / POD-bound backing storage
    float m_uiMinMaterialFillPct{kDefaultMaterialFillPct};
    float m_uiMinBusWorkdayRatio{kDefaultBusWorkdayRatio};
    float m_uiMaxTrucksEnRoute{static_cast<float>(kDefaultMaxTrucksEnRoute)};

    mutable std::mutex m_mutex;
    std::wstring       m_iniPath;
};

} // namespace cdc
