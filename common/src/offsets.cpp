// ============================================================================
// offsets.cpp — In-memory INI loader for per-version signatures & offsets
// ============================================================================
#include "wrsr/offsets.hpp"

#include <cctype>
#include <charconv>

namespace wrsr::offsets {

namespace {

std::string_view trim(std::string_view sv) noexcept
{
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return sv;
}

bool listMatchesU32(std::string_view list, uint32_t target) noexcept
{
    size_t pos = 0;
    while (pos < list.size()) {
        const size_t comma = list.find(',', pos);
        const std::string_view token = trim(
            comma == std::string_view::npos
                ? list.substr(pos)
                : list.substr(pos, comma - pos));
        if (!token.empty()) {
            const auto parsed = parseU32(token);
            if (parsed && *parsed == target) {
                return true;
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

bool parseBoolText(std::string_view text, bool fallback) noexcept
{
    const auto t = trim(text);
    if (t == "1" || t == "true" || t == "TRUE" || t == "yes" || t == "YES") {
        return true;
    }
    if (t == "0" || t == "false" || t == "FALSE" || t == "no" || t == "NO") {
        return false;
    }
    return fallback;
}

constexpr std::string_view kProfilePrefix = "profile.";

} // namespace

std::optional<uint32_t> parseU32(std::string_view text) noexcept
{
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }

    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) {
        return std::nullopt;
    }

    uint32_t val = 0;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), val, base);
    if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return val;
}

std::optional<int32_t> parseI32(std::string_view text) noexcept
{
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }

    bool negative = false;
    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    } else if (text.front() == '+') {
        text.remove_prefix(1);
    }

    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) {
        return std::nullopt;
    }

    if (base == 16) {
        uint32_t uval = 0;
        const auto res = std::from_chars(text.data(), text.data() + text.size(), uval, 16);
        if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) {
            return std::nullopt;
        }
        return negative ? -static_cast<int32_t>(uval) : static_cast<int32_t>(uval);
    }

    int64_t val = 0;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), val, 10);
    if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    if (negative) {
        val = -val;
    }
    if (val < INT32_MIN || val > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<int32_t>(val);
}

std::optional<std::string_view> ResolvedProfile::getString(std::string_view key) const noexcept
{
    const auto it = m_values.find(std::string(key));
    if (it == m_values.end()) {
        return std::nullopt;
    }
    return std::string_view(it->second);
}

bool ResolvedProfile::getU32(std::string_view key, uint32_t& out) const noexcept
{
    const auto sv = getString(key);
    if (!sv) {
        return false;
    }
    const auto parsed = parseU32(*sv);
    if (!parsed) {
        return false;
    }
    out = *parsed;
    return true;
}

bool ResolvedProfile::getI32(std::string_view key, int32_t& out) const noexcept
{
    const auto sv = getString(key);
    if (!sv) {
        return false;
    }
    const auto parsed = parseI32(*sv);
    if (!parsed) {
        return false;
    }
    out = *parsed;
    return true;
}

bool ResolvedProfile::getBool(std::string_view key, bool fallback) const noexcept
{
    const auto sv = getString(key);
    if (!sv) {
        return fallback;
    }
    return parseBoolText(*sv, fallback);
}

std::optional<OffsetDocument> OffsetDocument::parse(std::string_view iniText) noexcept
{
    OffsetDocument doc;
    std::string    currentSection;

    size_t pos = 0;
    while (pos < iniText.size()) {
        size_t eol = iniText.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = iniText.size();
        }
        std::string_view line = iniText.substr(pos, eol - pos);
        pos = eol + 1;

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        line = trim(line);
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            currentSection = std::string(trim(line.substr(1, line.size() - 2)));
            doc.m_sections[currentSection]; // ensure even empty sections exist
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string_view::npos || currentSection.empty()) {
            continue;
        }

        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view val = trim(line.substr(eq + 1));
        if (!key.empty()) {
            doc.m_sections[currentSection][std::string(key)] = std::string(val);
        }
    }

    if (doc.m_sections.empty()) {
        return std::nullopt;
    }
    return doc;
}

std::optional<std::string_view> OffsetDocument::getString(
    std::string_view section, std::string_view key) const noexcept
{
    const auto sit = m_sections.find(std::string(section));
    if (sit == m_sections.end()) {
        return std::nullopt;
    }
    const auto kit = sit->second.find(std::string(key));
    if (kit == sit->second.end()) {
        return std::nullopt;
    }
    return std::string_view(kit->second);
}

std::optional<uint32_t> OffsetDocument::getU32(
    std::string_view section, std::string_view key) const noexcept
{
    const auto sv = getString(section, key);
    if (!sv) {
        return std::nullopt;
    }
    return parseU32(*sv);
}

bool OffsetDocument::getBool(
    std::string_view section, std::string_view key, bool fallback) const noexcept
{
    const auto sv = getString(section, key);
    if (!sv) {
        return fallback;
    }
    return parseBoolText(*sv, fallback);
}

bool OffsetDocument::collectProfileKeys(
    std::string_view profileName, int depth, KeyMap& out) const noexcept
{
    if (depth > 8) {
        return false;
    }

    std::string sectionName(kProfilePrefix);
    sectionName.append(profileName);

    const auto sit = m_sections.find(sectionName);
    if (sit == m_sections.end()) {
        return false;
    }

    const auto baseIt = sit->second.find("base");
    if (baseIt != sit->second.end() && !baseIt->second.empty()) {
        if (!collectProfileKeys(baseIt->second, depth + 1, out)) {
            return false;
        }
    }

    for (const auto& [k, v] : sit->second) {
        if (k != "base") {
            out[k] = v;
        }
    }
    return true;
}

std::optional<ResolvedProfile> OffsetDocument::resolveProfile(
    const pe::ExecutableFingerprint& fp) const noexcept
{
    std::string matchedName;
    bool        exactMatch = false;

    if (fp.timeDateStamp != 0) {
        for (const auto& [secName, keys] : m_sections) {
            if (secName.rfind(kProfilePrefix, 0) != 0) {
                continue;
            }
            const std::string_view profName = std::string_view(secName).substr(kProfilePrefix.size());
            if (profName == "default") {
                continue;
            }

            const auto tsIt = keys.find("pe_timestamp");
            if (tsIt == keys.end() || !listMatchesU32(tsIt->second, fp.timeDateStamp)) {
                continue;
            }

            const auto szIt = keys.find("pe_size_of_image");
            if (szIt != keys.end() && !trim(szIt->second).empty() && fp.sizeOfImage != 0) {
                if (!listMatchesU32(szIt->second, fp.sizeOfImage)) {
                    continue;
                }
            }

            matchedName = std::string(profName);
            exactMatch  = true;
            break;
        }
    }

    if (matchedName.empty()) {
        if (m_sections.find("profile.default") != m_sections.end()) {
            matchedName = "default";
        } else {
            return std::nullopt;
        }
    }

    KeyMap merged;
    if (!collectProfileKeys(matchedName, 0, merged)) {
        return std::nullopt;
    }
    return ResolvedProfile(std::move(matchedName), exactMatch, std::move(merged));
}

} // namespace wrsr::offsets
