// ============================================================================
// rules.cpp — Rule engine & INI sidecar serialization
// ============================================================================
#include "rules.hpp"

#include <windows.h>
#include <mini/ini.h>

namespace tpl {

namespace {
std::string wToU8(const std::wstring& w)
{
    if (w.empty()) {
        return {};
    }
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 1) {
        return {};
    }
    std::string s(static_cast<size_t>(sz - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), sz, nullptr, nullptr);
    return s;
}

float parseFloatSafe(const std::string& str, float fallback) noexcept
{
    try {
        return std::stof(str);
    } catch (...) {
        return fallback;
    }
}
} // namespace

float ResourceRule::effectiveMaxBuyRub() const noexcept
{
    if (mode == RuleMode::FixedCap) {
        return max_buy_rub;
    }
    if (baseline_rub != 0.f && max_buy_pct > 0.f) {
        return baseline_rub * (max_buy_pct / 100.f);
    }
    return 0.f;
}

float ResourceRule::effectiveMinSellRub() const noexcept
{
    if (mode == RuleMode::FixedCap) {
        return min_sell_rub;
    }
    const float base = (baseline_sell_rub != 0.f) ? baseline_sell_rub : baseline_rub;
    if (base != 0.f && min_sell_pct > 0.f) {
        return base * (min_sell_pct / 100.f);
    }
    return 0.f;
}

float ResourceRule::effectiveMaxBuyUsd() const noexcept
{
    if (mode == RuleMode::FixedCap) {
        return max_buy_usd;
    }
    if (baseline_usd != 0.f && max_buy_pct > 0.f) {
        return baseline_usd * (max_buy_pct / 100.f);
    }
    return 0.f;
}

float ResourceRule::effectiveMinSellUsd() const noexcept
{
    if (mode == RuleMode::FixedCap) {
        return min_sell_usd;
    }
    const float base = (baseline_sell_usd != 0.f) ? baseline_sell_usd : baseline_usd;
    if (base != 0.f && min_sell_pct > 0.f) {
        return base * (min_sell_pct / 100.f);
    }
    return 0.f;
}

bool ResourceRule::shouldBlockBuyRub(float priceRub) const noexcept
{
    const float cap = effectiveMaxBuyRub();
    return (cap != 0.f && priceRub > cap);
}

bool ResourceRule::shouldBlockSellRub(float priceRub) const noexcept
{
    const float floorVal = effectiveMinSellRub();
    return (floorVal != 0.f && priceRub < floorVal);
}

bool ResourceRule::shouldBlockBuyUsd(float priceUsd) const noexcept
{
    const float cap = effectiveMaxBuyUsd();
    return (cap != 0.f && priceUsd > cap);
}

bool ResourceRule::shouldBlockSellUsd(float priceUsd) const noexcept
{
    const float floorVal = effectiveMinSellUsd();
    return (floorVal != 0.f && priceUsd < floorVal);
}

void ResourceRule::rebase() noexcept
{
    if (live_buy_rub != 0.f) {
        baseline_rub = live_buy_rub;
    }
    if (live_sell_rub != 0.f) {
        baseline_sell_rub = live_sell_rub;
    }
    if (live_buy_usd != 0.f) {
        baseline_usd = live_buy_usd;
    }
    if (live_sell_usd != 0.f) {
        baseline_sell_usd = live_sell_usd;
    }
}

RuleEngine& RuleEngine::get() noexcept
{
    static RuleEngine instance;
    return instance;
}

void RuleEngine::setActiveSaveDir(const std::wstring& dir) noexcept
{
    std::lock_guard lock(m_mutex);
    m_saveDir = dir;
}

std::wstring RuleEngine::activeSaveDir() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_saveDir;
}

bool RuleEngine::hasSave() const noexcept
{
    std::lock_guard lock(m_mutex);
    return !m_saveDir.empty();
}

void RuleEngine::setEnforcementEnabled(bool enabled) noexcept
{
    m_enforcementEnabled.store(enabled, std::memory_order_relaxed);
}

bool RuleEngine::isEnforcementEnabled() const noexcept
{
    return m_enforcementEnabled.load(std::memory_order_relaxed);
}

bool RuleEngine::hasAnyActiveLimit() const noexcept
{
    return m_hasAnyActiveLimit.load(std::memory_order_relaxed);
}

void RuleEngine::syncFastSlotLocked(const ResourceRule& rule) noexcept
{
    if (rule.fast_index >= kMaxFastResources) {
        return;
    }
    auto& slot = m_fastSlots[rule.fast_index];
    slot.maxBuyRub.store(rule.effectiveMaxBuyRub(), std::memory_order_relaxed);
    slot.minSellRub.store(rule.effectiveMinSellRub(), std::memory_order_relaxed);
    slot.maxBuyUsd.store(rule.effectiveMaxBuyUsd(), std::memory_order_relaxed);
    slot.minSellUsd.store(rule.effectiveMinSellUsd(), std::memory_order_relaxed);
}

void RuleEngine::rebuildAllFastSlotsLocked() noexcept
{
    for (auto& slot : m_fastSlots) {
        slot.maxBuyRub.store(0.f, std::memory_order_relaxed);
        slot.minSellRub.store(0.f, std::memory_order_relaxed);
        slot.maxBuyUsd.store(0.f, std::memory_order_relaxed);
        slot.minSellUsd.store(0.f, std::memory_order_relaxed);
    }

    bool anyLimit = false;
    for (const auto& r : m_rules) {
        const float mbr = r.effectiveMaxBuyRub();
        const float msr = r.effectiveMinSellRub();
        const float mbu = r.effectiveMaxBuyUsd();
        const float msu = r.effectiveMinSellUsd();
        if (mbr != 0.f || msr != 0.f || mbu != 0.f || msu != 0.f) {
            anyLimit = true;
        }
        if (r.fast_index < kMaxFastResources) {
            auto& slot = m_fastSlots[r.fast_index];
            slot.maxBuyRub.store(mbr, std::memory_order_relaxed);
            slot.minSellRub.store(msr, std::memory_order_relaxed);
            slot.maxBuyUsd.store(mbu, std::memory_order_relaxed);
            slot.minSellUsd.store(msu, std::memory_order_relaxed);
        }
    }
    m_hasAnyActiveLimit.store(anyLimit, std::memory_order_relaxed);
}

void RuleEngine::ensureResource(const std::string& name, float mockRub, float mockUsd)
{
    std::lock_guard lock(m_mutex);
    if (!findMutable(name)) {
        ResourceRule r;
        r.name              = name;
        r.live_buy_rub      = mockRub;
        r.live_sell_rub     = mockRub * 0.90f;
        r.live_buy_usd      = mockUsd;
        r.live_sell_usd     = mockUsd * 0.90f;
        r.baseline_rub      = mockRub;
        r.baseline_sell_rub = r.live_sell_rub;
        r.baseline_usd      = mockUsd;
        r.baseline_sell_usd = r.live_sell_usd;
        r.is_live           = false;
        m_rules.push_back(std::move(r));
    }
}

std::vector<ResourceRule> RuleEngine::rules() const
{
    std::lock_guard lock(m_mutex);
    return m_rules;
}

void RuleEngine::updateRule(const ResourceRule& rule)
{
    std::lock_guard lock(m_mutex);
    if (auto* r = findMutable(rule.name)) {
        const float  br   = r->live_buy_rub;
        const float  sr   = r->live_sell_rub;
        const float  bu   = r->live_buy_usd;
        const float  su   = r->live_sell_usd;
        const bool   live = r->is_live;
        const size_t idx  = r->fast_index;
        *r                = rule;
        r->live_buy_rub   = br;
        r->live_sell_rub  = sr;
        r->live_buy_usd   = bu;
        r->live_sell_usd  = su;
        r->is_live        = live;
        r->fast_index     = idx;
    } else {
        m_rules.push_back(rule);
    }
    rebuildAllFastSlotsLocked();
}

void RuleEngine::updateLivePrices(const std::string& name,
                                  float buyRub, float sellRub,
                                  float buyUsd, float sellUsd,
                                  bool isLive,
                                  size_t fastIndex)
{
    std::lock_guard lock(m_mutex);
    auto* r = findMutable(name);
    if (!r) {
        ResourceRule created;
        created.name              = name;
        created.baseline_rub      = buyRub;
        created.baseline_sell_rub = sellRub;
        created.baseline_usd      = buyUsd;
        created.baseline_sell_usd = sellUsd;
        created.fast_index        = fastIndex;
        m_rules.push_back(std::move(created));
        r = &m_rules.back();
    } else if (fastIndex != kInvalidFastIndex && r->fast_index != fastIndex) {
        r->fast_index = fastIndex;
    }

    r->live_buy_rub  = buyRub;
    r->live_sell_rub = sellRub;
    r->live_buy_usd  = buyUsd;
    r->live_sell_usd = sellUsd;
    r->is_live       = isLive;
    if (r->baseline_rub == 0.f && buyRub != 0.f) {
        r->baseline_rub = buyRub;
    }
    if (r->baseline_sell_rub == 0.f && sellRub != 0.f) {
        r->baseline_sell_rub = sellRub;
    }
    if (r->baseline_usd == 0.f && buyUsd != 0.f) {
        r->baseline_usd = buyUsd;
    }
    if (r->baseline_sell_usd == 0.f && sellUsd != 0.f) {
        r->baseline_sell_usd = sellUsd;
    }

    syncFastSlotLocked(*r);
    if (!m_hasAnyActiveLimit.load(std::memory_order_relaxed)) {
        if (r->effectiveMaxBuyRub() != 0.f || r->effectiveMinSellRub() != 0.f ||
            r->effectiveMaxBuyUsd() != 0.f || r->effectiveMinSellUsd() != 0.f) {
            m_hasAnyActiveLimit.store(true, std::memory_order_relaxed);
        }
    }
}

bool RuleEngine::shouldBlockBuyRubFast(size_t index, float priceRub) const noexcept
{
    if (index >= kMaxFastResources) {
        return false;
    }
    const float cap = m_fastSlots[index].maxBuyRub.load(std::memory_order_relaxed);
    return (cap != 0.f && priceRub > cap);
}

bool RuleEngine::shouldBlockSellRubFast(size_t index, float priceRub) const noexcept
{
    if (index >= kMaxFastResources) {
        return false;
    }
    const float floorVal = m_fastSlots[index].minSellRub.load(std::memory_order_relaxed);
    return (floorVal != 0.f && priceRub < floorVal);
}

bool RuleEngine::shouldBlockBuyUsdFast(size_t index, float priceUsd) const noexcept
{
    if (index >= kMaxFastResources) {
        return false;
    }
    const float cap = m_fastSlots[index].maxBuyUsd.load(std::memory_order_relaxed);
    return (cap != 0.f && priceUsd > cap);
}

bool RuleEngine::shouldBlockSellUsdFast(size_t index, float priceUsd) const noexcept
{
    if (index >= kMaxFastResources) {
        return false;
    }
    const float floorVal = m_fastSlots[index].minSellUsd.load(std::memory_order_relaxed);
    return (floorVal != 0.f && priceUsd < floorVal);
}

bool RuleEngine::shouldBlockBuyRub(std::string_view name, float priceRub) const noexcept
{
    std::lock_guard lock(m_mutex);
    const auto* r = find(name);
    return r ? r->shouldBlockBuyRub(priceRub) : false;
}

bool RuleEngine::shouldBlockSellRub(std::string_view name, float priceRub) const noexcept
{
    std::lock_guard lock(m_mutex);
    const auto* r = find(name);
    return r ? r->shouldBlockSellRub(priceRub) : false;
}

bool RuleEngine::shouldBlockBuyUsd(std::string_view name, float priceUsd) const noexcept
{
    std::lock_guard lock(m_mutex);
    const auto* r = find(name);
    return r ? r->shouldBlockBuyUsd(priceUsd) : false;
}

bool RuleEngine::shouldBlockSellUsd(std::string_view name, float priceUsd) const noexcept
{
    std::lock_guard lock(m_mutex);
    const auto* r = find(name);
    return r ? r->shouldBlockSellUsd(priceUsd) : false;
}

void RuleEngine::loadFromSave(const std::wstring& saveDir)
{
    const std::string iniPathU8 = wToU8(saveDir + L"\\trade_price_limits.ini");
    if (iniPathU8.empty()) {
        return;
    }

    mINI::INIFile      file(iniPathU8);
    mINI::INIStructure ini;
    if (!file.read(ini)) {
        m_enforcementEnabled.store(true, std::memory_order_relaxed);
        return;
    }

    if (ini.has("__settings") && ini["__settings"].has("enforcement_enabled")) {
        const std::string& val = ini["__settings"]["enforcement_enabled"];
        m_enforcementEnabled.store(val != "0" && val != "false", std::memory_order_relaxed);
    } else {
        m_enforcementEnabled.store(true, std::memory_order_relaxed);
    }

    std::lock_guard lock(m_mutex);
    for (auto& [sectionName, section] : ini) {
        if (sectionName == "__settings") {
            continue;
        }
        ResourceRule r;
        r.name = sectionName;

        auto getVal = [&](const std::string& key, float def) -> float {
            return section.has(key) ? parseFloatSafe(section.get(key), def) : def;
        };

        const std::string modeStr = section.has("mode") ? section.get("mode") : "fixed";
        r.mode                    = (modeStr == "baseline") ? RuleMode::BaselinePct : RuleMode::FixedCap;

        r.max_buy_rub       = getVal("max_buy_rub", 0.f);
        r.min_sell_rub      = getVal("min_sell_rub", 0.f);
        r.max_buy_usd       = getVal("max_buy_usd", 0.f);
        r.min_sell_usd      = getVal("min_sell_usd", 0.f);
        r.baseline_rub      = getVal("baseline_rub", 0.f);
        r.baseline_sell_rub = getVal("baseline_sell_rub", 0.f);
        r.baseline_usd      = getVal("baseline_usd", 0.f);
        r.baseline_sell_usd = getVal("baseline_sell_usd", 0.f);
        r.max_buy_pct       = getVal("max_buy_pct", 0.f);
        r.min_sell_pct      = getVal("min_sell_pct", 0.f);

        if (auto* existing = findMutable(r.name)) {
            const float  br   = existing->live_buy_rub;
            const float  sr   = existing->live_sell_rub;
            const float  bu   = existing->live_buy_usd;
            const float  su   = existing->live_sell_usd;
            const bool   live = existing->is_live;
            const size_t idx  = existing->fast_index;
            *existing         = r;
            existing->live_buy_rub  = br;
            existing->live_sell_rub = sr;
            existing->live_buy_usd  = bu;
            existing->live_sell_usd = su;
            existing->is_live       = live;
            existing->fast_index    = idx;
        } else {
            m_rules.push_back(std::move(r));
        }
    }
    rebuildAllFastSlotsLocked();
}

void RuleEngine::saveToSave(const std::wstring& saveDir) const
{
    const std::string iniPathU8 = wToU8(saveDir + L"\\trade_price_limits.ini");
    if (iniPathU8.empty()) {
        return;
    }

    mINI::INIFile      file(iniPathU8);
    mINI::INIStructure ini;

    ini["__settings"]["enforcement_enabled"] =
        m_enforcementEnabled.load(std::memory_order_relaxed) ? "1" : "0";

    std::lock_guard lock(m_mutex);
    for (const auto& r : m_rules) {
        auto& sec          = ini[r.name];
        sec["mode"]              = (r.mode == RuleMode::BaselinePct) ? "baseline" : "fixed";
        sec["max_buy_rub"]       = std::to_string(r.max_buy_rub);
        sec["min_sell_rub"]      = std::to_string(r.min_sell_rub);
        sec["max_buy_usd"]       = std::to_string(r.max_buy_usd);
        sec["min_sell_usd"]      = std::to_string(r.min_sell_usd);
        sec["baseline_rub"]      = std::to_string(r.baseline_rub);
        sec["baseline_sell_rub"] = std::to_string(r.baseline_sell_rub);
        sec["baseline_usd"]      = std::to_string(r.baseline_usd);
        sec["baseline_sell_usd"] = std::to_string(r.baseline_sell_usd);
        sec["max_buy_pct"]       = std::to_string(r.max_buy_pct);
        sec["min_sell_pct"]      = std::to_string(r.min_sell_pct);
    }
    file.write(ini);
}

ResourceRule* RuleEngine::findMutable(std::string_view name) noexcept
{
    for (auto& r : m_rules) {
        if (r.name == name) {
            return &r;
        }
    }
    return nullptr;
}

const ResourceRule* RuleEngine::find(std::string_view name) const noexcept
{
    for (const auto& r : m_rules) {
        if (r.name == name) {
            return &r;
        }
    }
    return nullptr;
}

} // namespace tpl
