#pragma once
// ============================================================================
// offsets.hpp — In-memory INI loader for per-version signatures & offsets
// ============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "wrsr/pe_utils.hpp"

namespace wrsr::offsets {

class ResolvedProfile {
public:
    ResolvedProfile() = default;
    ResolvedProfile(std::string name,
                    bool exactMatch,
                    std::unordered_map<std::string, std::string> values) noexcept
        : m_name(std::move(name))
        , m_exactMatch(exactMatch)
        , m_values(std::move(values))
    {}

    const std::string& name() const noexcept { return m_name; }
    bool isExactMatch() const noexcept { return m_exactMatch; }

    std::optional<std::string_view> getString(std::string_view key) const noexcept;
    bool getU32(std::string_view key, uint32_t& out) const noexcept;
    bool getI32(std::string_view key, int32_t& out) const noexcept;
    bool getBool(std::string_view key, bool fallback = false) const noexcept;

private:
    std::string                                  m_name{"default"};
    bool                                         m_exactMatch{false};
    std::unordered_map<std::string, std::string> m_values;
};

class OffsetDocument {
public:
    using KeyMap     = std::unordered_map<std::string, std::string>;
    using SectionMap = std::unordered_map<std::string, KeyMap>;

    static std::optional<OffsetDocument> parse(std::string_view iniText) noexcept;

    std::optional<std::string_view> getString(std::string_view section,
                                              std::string_view key) const noexcept;
    std::optional<uint32_t> getU32(std::string_view section,
                                   std::string_view key) const noexcept;
    bool getBool(std::string_view section,
                 std::string_view key,
                 bool fallback = false) const noexcept;

    // Matches [profile.<name>] against pe_timestamp (+ optional pe_size_of_image),
    // resolving `base = <parent>` inheritance, or falls back to [profile.default].
    std::optional<ResolvedProfile> resolveProfile(
        const pe::ExecutableFingerprint& fp) const noexcept;

private:
    bool collectProfileKeys(std::string_view profileName,
                            int depth,
                            KeyMap& out) const noexcept;

    SectionMap m_sections;
};

// Utility parsers for hex ("0x368") or signed/unsigned decimal integers.
std::optional<uint32_t> parseU32(std::string_view text) noexcept;
std::optional<int32_t>  parseI32(std::string_view text) noexcept;

} // namespace wrsr::offsets
