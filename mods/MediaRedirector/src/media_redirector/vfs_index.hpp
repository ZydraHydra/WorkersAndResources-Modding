#pragma once
// ============================================================================
// vfs_index.hpp — Early-boot VFS scanner, priority resolver & immutable map
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mr {

class Config;

enum class PackSource {
    Local,
    Workshop,
};

struct PackInfo {
    std::string  id;           // e.g. "local:MyPack" or "ws:123456789"
    std::string  displayName;  // Human-readable name (with Workshop ID if applicable)
    PackSource   source{PackSource::Local};
    std::wstring rootDir;      // Pack root directory
    std::wstring zipPath;      // Non-empty if pack is backed by a .zip archive
    uint8_t      enabled{0};   // UI-bound POD toggle (default: disabled / manual)
    size_t       indexedFiles{0};  // Winning files in the immutable VFS map
    size_t       totalFiles{0};    // Total override files found in this pack
    size_t       conflictCount{0}; // Collisions with other enabled packs
};

struct ConflictEntry {
    std::string              relativePath;    // Normalized "media_soviet\..." (UTF-8)
    std::string              winningPack;     // Display name of winning pack
    std::vector<std::string> overriddenPacks; // Display names of lower-priority packs
};

struct TransparentWStringHash {
    using is_transparent = void;

    [[nodiscard]] size_t operator()(std::wstring_view sv) const noexcept
    {
        return std::hash<std::wstring_view>{}(sv);
    }

    [[nodiscard]] size_t operator()(const std::wstring& s) const noexcept
    {
        return std::hash<std::wstring_view>{}(s);
    }
};

using VfsMap = std::unordered_map<std::wstring, std::wstring, TransparentWStringHash, std::equal_to<>>;

class VfsIndex {
public:
    static VfsIndex& get() noexcept;

    // Builds the VFS lookup table synchronously during DllMain (DLL_PROCESS_ATTACH).
    // After build() returns, m_map is strictly immutable for the process lifetime.
    void build(Config& cfg);

    [[nodiscard]] const VfsMap& map() const noexcept { return m_map; }
    [[nodiscard]] const std::vector<PackInfo>& packs() const noexcept { return m_packs; }
    [[nodiscard]] std::vector<PackInfo>& uiPacks() noexcept { return m_packs; }
    [[nodiscard]] const std::vector<ConflictEntry>& conflicts() const noexcept { return m_conflicts; }

    [[nodiscard]] size_t indexedFileCount() const noexcept { return m_map.size(); }
    [[nodiscard]] size_t conflictCount() const noexcept { return m_conflicts.size(); }
    [[nodiscard]] bool   isBuilt() const noexcept { return m_built; }

    // Helpers to reorder UI pack list & push changes back to Config
    bool movePackUp(size_t index, Config& cfg);
    bool movePackDown(size_t index, Config& cfg);
    void syncPacksToConfig(Config& cfg) const;

private:
    VfsIndex() = default;

    VfsMap                     m_map;
    std::vector<PackInfo>      m_packs;
    std::vector<ConflictEntry> m_conflicts;
    bool                       m_built{false};
};

} // namespace mr
