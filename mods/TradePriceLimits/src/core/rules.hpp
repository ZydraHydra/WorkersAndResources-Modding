#pragma once
// ============================================================================
// rules.hpp — In-memory price-rule engine for TradePriceLimits
// ============================================================================
// Supports independent RUB and USD thresholds per resource in two modes:
//   Fixed Cap    — absolute RUB and USD limits
//   Baseline %   — percentage multipliers of captured baseline RUB/USD prices
//
// Thread safety: all public methods lock an internal mutex.
// ============================================================================

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tpl {

inline constexpr size_t kMaxFastResources = 256;
inline constexpr size_t kInvalidFastIndex = static_cast<size_t>(-1);

struct FastThresholdSlot {
    std::atomic<float> maxBuyRub{0.f};
    std::atomic<float> minSellRub{0.f};
    std::atomic<float> maxBuyUsd{0.f};
    std::atomic<float> minSellUsd{0.f};
};

enum class RuleMode : uint8_t {
    FixedCap    = 0,
    BaselinePct = 1,
};

struct ResourceRule {
    std::string name;
    RuleMode    mode{RuleMode::FixedCap};

    // Fixed Cap thresholds (0 = disabled / no limit)
    float max_buy_rub{0.f};
    float min_sell_rub{0.f};
    float max_buy_usd{0.f};
    float min_sell_usd{0.f};

    // Baseline Multiplier thresholds (survives multi-decade inflation)
    float baseline_rub{0.f};
    float baseline_sell_rub{0.f};
    float baseline_usd{0.f};
    float baseline_sell_usd{0.f};
    float max_buy_pct{0.f};   // e.g. 115 = allow up to 115% of baseline buy price
    float min_sell_pct{0.f};  // e.g.  85 = require at least 85% of baseline sell price

    // Live market data (populated by game_hooks or seeded mock values)
    float live_buy_rub{0.f};
    float live_sell_rub{0.f};
    float live_buy_usd{0.f};
    float live_sell_usd{0.f};
    bool  is_live{false};
    size_t fast_index{kInvalidFastIndex};

    [[nodiscard]] float effectiveMaxBuyRub() const noexcept;
    [[nodiscard]] float effectiveMinSellRub() const noexcept;
    [[nodiscard]] float effectiveMaxBuyUsd() const noexcept;
    [[nodiscard]] float effectiveMinSellUsd() const noexcept;

    [[nodiscard]] bool shouldBlockBuyRub(float priceRub) const noexcept;
    [[nodiscard]] bool shouldBlockSellRub(float priceRub) const noexcept;
    [[nodiscard]] bool shouldBlockBuyUsd(float priceUsd) const noexcept;
    [[nodiscard]] bool shouldBlockSellUsd(float priceUsd) const noexcept;

    void rebase() noexcept;
};

class RuleEngine {
public:
    static RuleEngine& get() noexcept;

    void loadGlobalConfig(const std::wstring& dllDir);
    void loadFromSave(const std::wstring& saveDir);
    void saveToSave(const std::wstring& saveDir) const;

    void                       setActiveSaveDir(const std::wstring& dir) noexcept;
    [[nodiscard]] std::wstring activeSaveDir() const noexcept;
    [[nodiscard]] bool         hasSave() const noexcept;

    void                      setEnforcementEnabled(bool enabled) noexcept;
    [[nodiscard]] bool        isEnforcementEnabled() const noexcept;
    void                      setToggleKey(uint32_t vk) noexcept;
    [[nodiscard]] uint32_t    toggleKey() const noexcept;
    [[nodiscard]] std::string toggleKeyName() const;
    [[nodiscard]] bool        hasAnyActiveLimit() const noexcept;

    void ensureResource(const std::string& name, float mockRub = 0.f, float mockUsd = 0.f);
    [[nodiscard]] std::vector<ResourceRule> rules() const;
    void updateRule(const ResourceRule& rule);
    void updateLivePrices(const std::string& name,
                          float buyRub, float sellRub,
                          float buyUsd, float sellUsd,
                          bool isLive = true,
                          size_t fastIndex = kInvalidFastIndex);

    // Lock-free O(1) dispatch-hook checks by ResourceVector index (no mutex, no allocation)
    [[nodiscard]] bool shouldBlockBuyRubFast(size_t index, float priceRub) const noexcept;
    [[nodiscard]] bool shouldBlockSellRubFast(size_t index, float priceRub) const noexcept;
    [[nodiscard]] bool shouldBlockBuyUsdFast(size_t index, float priceUsd) const noexcept;
    [[nodiscard]] bool shouldBlockSellUsdFast(size_t index, float priceUsd) const noexcept;

    // Fallback name-based checks (zero-allocation string_view under mutex)
    [[nodiscard]] bool shouldBlockBuyRub(std::string_view name, float priceRub) const noexcept;
    [[nodiscard]] bool shouldBlockSellRub(std::string_view name, float priceRub) const noexcept;
    [[nodiscard]] bool shouldBlockBuyUsd(std::string_view name, float priceUsd) const noexcept;
    [[nodiscard]] bool shouldBlockSellUsd(std::string_view name, float priceUsd) const noexcept;

private:
    RuleEngine()  = default;
    ~RuleEngine() = default;

    mutable std::mutex                             m_mutex;
    std::vector<ResourceRule>                      m_rules;
    std::wstring                                   m_saveDir;
    std::atomic<bool>                              m_enforcementEnabled{true};
    std::atomic<uint32_t>                          m_toggleKey{0x77}; // VK_F8
    std::atomic<bool>                              m_hasAnyActiveLimit{false};
    std::array<FastThresholdSlot, kMaxFastResources> m_fastSlots{};

    ResourceRule*       findMutable(std::string_view name) noexcept;
    const ResourceRule* find(std::string_view name) const noexcept;
    void                syncFastSlotLocked(const ResourceRule& rule) noexcept;
    void                rebuildAllFastSlotsLocked() noexcept;
};

} // namespace tpl
