#pragma once
// ============================================================================
// config.hpp — TechServiceControl runtime configuration & INI persistence
// ============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace tsc {

inline constexpr float kMinSnowplowPct     = 1.0f;
inline constexpr float kMaxSnowplowPct     = 90.0f;
inline constexpr float kDefaultSnowplowPct = 20.0f;

inline constexpr float kMinGarbagePct      = 10.0f;
inline constexpr float kMaxGarbagePct      = 95.0f;
inline constexpr float kDefaultGarbagePct  = 65.0f;

inline constexpr float kMinSewagePct       = 10.0f;
inline constexpr float kMaxSewagePct       = 95.0f;
inline constexpr float kDefaultSewagePct   = 70.0f;

inline constexpr float kMinWaterBelowPct     = 5.0f;
inline constexpr float kMaxWaterBelowPct     = 90.0f;
inline constexpr float kDefaultWaterBelowPct = 40.0f;

enum class PresetKind : uintptr_t {
    Vanilla   = 0,
    FuelSaver = 1,
    DenseCity = 2,
};

class Config {
public:
    static Config& get() noexcept;

    bool load(const std::wstring& dllDir);
    bool save();

    // ── Lock-free hot-path getters ────────────────────────────────────────
    [[nodiscard]] bool isOverrideEnabled() const noexcept
    {
        return m_overrideEnabled.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getSnowplowMinPct() const noexcept
    {
        return m_snowplowMinPct.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getGarbageMinPct() const noexcept
    {
        return m_garbageMinPct.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getSewageMinPct() const noexcept
    {
        return m_sewageMinPct.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getWaterBelowPct() const noexcept
    {
        return m_waterBelowPct.load(std::memory_order_relaxed);
    }

    // ── UI-bound POD fields (owned by TechServiceControl.dll) ─────────────
    uint8_t* uiOverrideEnabledPtr() noexcept { return &m_uiOverrideEnabled; }
    float*   uiSnowplowMinPtr()     noexcept { return &m_uiSnowplowMinPct; }
    float*   uiGarbageMinPtr()      noexcept { return &m_uiGarbageMinPct; }
    float*   uiSewageMinPtr()       noexcept { return &m_uiSewageMinPct; }
    float*   uiWaterBelowPtr()      noexcept { return &m_uiWaterBelowPct; }

    void syncFromUi() noexcept;
    void applyPreset(PresetKind preset) noexcept;

    [[nodiscard]] const std::wstring& iniPath() const noexcept { return m_iniPath; }

private:
    Config() = default;

    static float clampRange(float v, float minV, float maxV, float fallback) noexcept;

    std::atomic<bool>  m_overrideEnabled{true};
    std::atomic<float> m_snowplowMinPct{kDefaultSnowplowPct};
    std::atomic<float> m_garbageMinPct{kDefaultGarbagePct};
    std::atomic<float> m_sewageMinPct{kDefaultSewagePct};
    std::atomic<float> m_waterBelowPct{kDefaultWaterBelowPct};

    // UI-thread / POD-bound backing storage
    uint8_t m_uiOverrideEnabled{1};
    float   m_uiSnowplowMinPct{kDefaultSnowplowPct};
    float   m_uiGarbageMinPct{kDefaultGarbagePct};
    float   m_uiSewageMinPct{kDefaultSewagePct};
    float   m_uiWaterBelowPct{kDefaultWaterBelowPct};

    mutable std::mutex m_mutex;
    std::wstring       m_iniPath;
};

} // namespace tsc
