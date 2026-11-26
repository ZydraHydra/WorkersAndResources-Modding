// ============================================================================
// vfs_index.cpp — Early-boot VFS scanner, priority resolver & conflict tracker
// ============================================================================
#include "vfs_index.hpp"
#include "config.hpp"
#include "zip_reader.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "wrsr/logger.hpp"

namespace mr {

namespace {

std::string wideToUtf8(const std::wstring& ws)
{
    if (ws.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), needed);
    return out;
}

bool dirExists(const std::wstring& path) noexcept
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

bool fileExists(const std::wstring& path) noexcept
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

std::wstring getGameDir()
{
    std::wstring path(MAX_PATH * 4, L'\0');
    const DWORD  len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (len == 0 || len >= path.size()) {
        return {};
    }
    path.resize(len);
    const size_t slash = path.find_last_of(L"\\/");
    return (slash != std::wstring::npos) ? path.substr(0, slash) : std::wstring{};
}

std::wstring canonicalizePath(const std::wstring& raw)
{
    std::wstring out(MAX_PATH * 4, L'\0');
    const DWORD  len = GetFullPathNameW(raw.c_str(), static_cast<DWORD>(out.size()), out.data(), nullptr);
    if (len == 0 || len >= out.size()) {
        return raw;
    }
    out.resize(len);
    return out;
}

std::wstring toLowerWide(std::wstring s)
{
    for (wchar_t& ch : s) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
        } else if (ch == L'/') {
            ch = L'\\';
        }
    }
    return s;
}

std::wstring normalizeRelKey(const std::wstring& rel)
{
    std::wstring out;
    out.reserve(rel.size());
    for (wchar_t ch : rel) {
        if (ch == L'/') {
            ch = L'\\';
        } else if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
        }
        if (ch == L'\\' && !out.empty() && out.back() == L'\\') {
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

std::wstring sanitizeIdForPath(const std::string& id)
{
    std::wstring out;
    out.reserve(id.size());
    for (char c : id) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            out.push_back(L'_');
        }
    }
    return out;
}

std::string parseWorkshopTitle(const std::wstring& workshopDir)
{
    const std::wstring cfgPath = workshopDir + L"\\workshopconfig.ini";
    if (!fileExists(cfgPath)) {
        return {};
    }

    HANDLE hFile = CreateFileW(cfgPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return {};
    }

    char  buf[4096]{};
    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, nullptr);
    CloseHandle(hFile);
    if (!ok || bytesRead == 0) {
        return {};
    }
    buf[bytesRead] = '\0';

    std::string content(buf, bytesRead);
    const size_t pos = content.find("$ITEM_NAME");
    if (pos == std::string::npos) {
        return {};
    }
    const size_t q1 = content.find('"', pos);
    if (q1 == std::string::npos) {
        return {};
    }
    const size_t q2 = content.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 <= q1 + 1) {
        return {};
    }
    return content.substr(q1 + 1, q2 - (q1 + 1));
}

size_t countFilesRecursive(const std::wstring& baseDir)
{
    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileW((baseDir + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return 0;
    }

    size_t count = 0;
    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) {
            continue;
        }
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            count += countFilesRecursive(baseDir + L"\\" + ffd.cFileName);
        } else {
            ++count;
        }
    } while (FindNextFileW(hFind, &ffd));

    FindClose(hFind);
    return count;
}

void collectFilesRecursive(const std::wstring& baseDir,
                           const std::wstring& relPrefix,
                           std::vector<std::pair<std::wstring, std::wstring>>& outFiles)
{
    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileW((baseDir + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring fullPath = baseDir + L"\\" + ffd.cFileName;
        const std::wstring relPath  = relPrefix + L"\\" + ffd.cFileName;

        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            collectFilesRecursive(fullPath, relPath, outFiles);
        } else {
            outFiles.emplace_back(normalizeRelKey(relPath), fullPath);
        }
    } while (FindNextFileW(hFind, &ffd));

    FindClose(hFind);
}

std::wstring findFirstMediaZipInDir(const std::wstring& dir, size_t& outZipCount)
{
    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileW((dir + L"\\*.zip").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::wstring matched;
    do {
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::wstring zipPath = dir + L"\\" + ffd.cFileName;
        const size_t       cnt     = zip::countMediaEntries(zipPath);
        if (cnt > 0) {
            matched     = zipPath;
            outZipCount = cnt;
            break;
        }
    } while (FindNextFileW(hFind, &ffd));

    FindClose(hFind);
    return matched;
}

std::wstring readRegString(HKEY root, const wchar_t* subKey, const wchar_t* valName)
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buf[MAX_PATH * 2]{};
    DWORD   bufBytes = sizeof(buf);
    DWORD   type     = 0;
    const LONG rc = RegQueryValueExW(hKey, valName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &bufBytes);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
    return std::wstring(buf);
}

std::vector<std::wstring> discoverWorkshopRoots(const std::wstring& gameDir)
{
    std::vector<std::wstring>        roots;
    std::unordered_set<std::wstring> seen;

    auto addCandidate = [&](const std::wstring& wsPath) {
        if (wsPath.empty()) {
            return;
        }
        const std::wstring canon = canonicalizePath(wsPath);
        if (!dirExists(canon)) {
            return;
        }
        const std::wstring key = toLowerWide(canon);
        if (seen.insert(key).second) {
            roots.push_back(canon);
        }
    };

    // 1. Relative to SOVIET64.exe (standard Steam install)
    addCandidate(gameDir + L"\\..\\..\\workshop\\content\\784150");

    // 2. Steam install path from Registry + libraryfolders.vdf
    std::wstring steamPath = readRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steamPath.empty()) {
        steamPath = readRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    }
    if (!steamPath.empty()) {
        addCandidate(steamPath + L"\\steamapps\\workshop\\content\\784150");

        const std::wstring vdfPath = steamPath + L"\\steamapps\\libraryfolders.vdf";
        HANDLE hFile = CreateFileW(vdfPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            std::string vdf(65536, '\0');
            DWORD       got = 0;
            if (ReadFile(hFile, vdf.data(), static_cast<DWORD>(vdf.size() - 1), &got, nullptr)) {
                vdf.resize(got);
                size_t pos = 0;
                while ((pos = vdf.find("\"path\"", pos)) != std::string::npos) {
                    pos += 6;
                    const size_t q1 = vdf.find('"', pos);
                    if (q1 == std::string::npos) break;
                    const size_t q2 = vdf.find('"', q1 + 1);
                    if (q2 == std::string::npos) break;
                    std::string rawLib = vdf.substr(q1 + 1, q2 - (q1 + 1));
                    // Unescape VDF double backslashes
                    std::string cleanLib;
                    cleanLib.reserve(rawLib.size());
                    for (size_t i = 0; i < rawLib.size(); ++i) {
                        if (rawLib[i] == '\\' && i + 1 < rawLib.size() && rawLib[i + 1] == '\\') {
                            cleanLib.push_back('\\');
                            ++i;
                        } else {
                            cleanLib.push_back(rawLib[i]);
                        }
                    }
                    addCandidate(utf8ToWide(cleanLib) + L"\\steamapps\\workshop\\content\\784150");
                    pos = q2 + 1;
                }
            }
            CloseHandle(hFile);
        }
    }

    return roots;
}

void scanLocalPacks(const std::wstring& gameDir, std::vector<PackInfo>& discovered)
{
    const std::wstring overridesDir = gameDir + L"\\media_overrides";
    if (!dirExists(overridesDir)) {
        return;
    }

    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileW((overridesDir + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) {
            continue;
        }
        if (ffd.cFileName[0] == L'.') {
            continue; // Skip hidden/internal folders like .vfs_cache
        }

        const std::wstring fullItem = overridesDir + L"\\" + ffd.cFileName;

        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            const bool hasMedia    = dirExists(fullItem + L"\\media_soviet");
            const bool hasOverride = dirExists(fullItem + L"\\override");
            size_t     zipCount    = 0;
            std::wstring zipFile;

            if (!hasMedia && !hasOverride) {
                zipFile = findFirstMediaZipInDir(fullItem, zipCount);
                if (zipFile.empty()) {
                    continue;
                }
            }

            const std::string nameUtf8 = wideToUtf8(ffd.cFileName);
            PackInfo info;
            info.id          = "local:" + nameUtf8;
            info.displayName = nameUtf8;
            info.source      = PackSource::Local;
            info.rootDir     = fullItem;
            info.zipPath     = zipFile;
            if (!zipFile.empty()) {
                info.totalFiles = zipCount;
            } else {
                info.totalFiles = (hasMedia ? countFilesRecursive(fullItem + L"\\media_soviet") : 0) +
                                  (hasOverride ? countFilesRecursive(fullItem + L"\\override") : 0);
            }
            discovered.push_back(std::move(info));
        } else {
            // Also support drop-in .zip files directly in media_overrides\MyPack.zip
            const std::wstring nameLower = toLowerWide(ffd.cFileName);
            if (nameLower.size() > 4 && nameLower.compare(nameLower.size() - 4, 4, L".zip") == 0) {
                const size_t zipCount = zip::countMediaEntries(fullItem);
                if (zipCount > 0) {
                    const std::string nameUtf8 = wideToUtf8(ffd.cFileName);
                    PackInfo info;
                    info.id          = "local:" + nameUtf8;
                    info.displayName = nameUtf8;
                    info.source      = PackSource::Local;
                    info.rootDir     = overridesDir;
                    info.zipPath     = fullItem;
                    info.totalFiles  = zipCount;
                    discovered.push_back(std::move(info));
                }
            }
        }
    } while (FindNextFileW(hFind, &ffd));

    FindClose(hFind);
}

void scanWorkshopPacks(const std::wstring& gameDir, std::vector<PackInfo>& discovered)
{
    const auto wsRoots = discoverWorkshopRoots(gameDir);
    std::unordered_set<std::string> seenIds;

    for (const auto& wsRoot : wsRoots) {
        WIN32_FIND_DATAW ffd{};
        HANDLE hFind = FindFirstFileW((wsRoot + L"\\*").c_str(), &ffd);
        if (hFind == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) {
                continue;
            }
            if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                continue;
            }

            const std::wstring packRoot    = wsRoot + L"\\" + ffd.cFileName;
            const bool         hasMedia    = dirExists(packRoot + L"\\media_soviet");
            const bool         hasOverride = dirExists(packRoot + L"\\override");
            size_t             zipCount    = 0;
            std::wstring       zipFile;

            if (!hasMedia && !hasOverride) {
                zipFile = findFirstMediaZipInDir(packRoot, zipCount);
                if (zipFile.empty()) {
                    continue;
                }
            }

            const std::string wsIdUtf8 = wideToUtf8(ffd.cFileName);
            const std::string packId   = "ws:" + wsIdUtf8;
            if (!seenIds.insert(packId).second) {
                continue;
            }

            const std::string title = parseWorkshopTitle(packRoot);

            PackInfo info;
            info.id          = packId;
            info.displayName = title.empty() ? ("Workshop " + wsIdUtf8) : (title + " [" + wsIdUtf8 + "]");
            info.source      = PackSource::Workshop;
            info.rootDir     = packRoot;
            info.zipPath     = zipFile;
            if (!zipFile.empty()) {
                info.totalFiles = zipCount;
            } else {
                info.totalFiles = (hasMedia ? countFilesRecursive(packRoot + L"\\media_soviet") : 0) +
                                  (hasOverride ? countFilesRecursive(packRoot + L"\\override") : 0);
            }
            discovered.push_back(std::move(info));
        } while (FindNextFileW(hFind, &ffd));

        FindClose(hFind);
    }
}

} // namespace

VfsIndex& VfsIndex::get() noexcept
{
    static VfsIndex instance;
    return instance;
}

void VfsIndex::build(Config& cfg)
{
    if (m_built) {
        return;
    }

    const std::wstring gameDir = getGameDir();
    if (gameDir.empty()) {
        wrsr::log::error("VfsIndex::build — could not resolve SOVIET64.exe directory");
        m_built = true;
        return;
    }

    std::vector<PackInfo> discovered;
    scanLocalPacks(gameDir, discovered);
    if (cfg.isScanWorkshopEnabled()) {
        scanWorkshopPacks(gameDir, discovered);
    }

    // Sort discovered packs alphabetically first for deterministic ordering of new packs
    std::sort(discovered.begin(), discovered.end(), [](const PackInfo& a, const PackInfo& b) {
        return a.id < b.id;
    });

    // Apply persisted priority order from MediaRedirector.ini
    const auto configuredOrder = cfg.priorityOrder();
    std::unordered_map<std::string, size_t> idToIndex;
    for (size_t i = 0; i < discovered.size(); ++i) {
        idToIndex[discovered[i].id] = i;
    }

    std::vector<bool>     placed(discovered.size(), false);
    std::vector<PackInfo> ordered;
    ordered.reserve(discovered.size());

    for (const auto& id : configuredOrder) {
        const auto it = idToIndex.find(id);
        if (it != idToIndex.end() && !placed[it->second]) {
            placed[it->second] = true;
            ordered.push_back(std::move(discovered[it->second]));
        }
    }
    for (size_t i = 0; i < discovered.size(); ++i) {
        if (!placed[i]) {
            ordered.push_back(std::move(discovered[i]));
        }
    }

    // Never auto-enable packs: defaults to false unless explicitly enabled in INI
    for (auto& p : ordered) {
        p.enabled = cfg.isPackEnabled(p.id) ? 1u : 0u;
    }

    m_packs = std::move(ordered);
    syncPacksToConfig(cfg);

    // Index enabled packs in priority order (index 0 = highest priority, wins)
    std::unordered_map<std::wstring, size_t, TransparentWStringHash, std::equal_to<>> winnerPackIdx;
    std::unordered_map<std::wstring, size_t, TransparentWStringHash, std::equal_to<>> conflictIdxByRel;

    for (size_t packIdx = 0; packIdx < m_packs.size(); ++packIdx) {
        auto& pack = m_packs[packIdx];
        if (!pack.enabled) {
            continue;
        }

        std::vector<std::pair<std::wstring, std::wstring>> files;
        if (!pack.zipPath.empty()) {
            const std::wstring cacheDir =
                gameDir + L"\\media_overrides\\.vfs_cache\\" + sanitizeIdForPath(pack.id);
            zip::extractMediaEntries(pack.zipPath, cacheDir, files);
        } else {
            const std::wstring mediaSub    = pack.rootDir + L"\\media_soviet";
            const std::wstring overrideSub = pack.rootDir + L"\\override";

            if (dirExists(mediaSub)) {
                collectFilesRecursive(mediaSub, L"media_soviet", files);
            }
            if (dirExists(overrideSub)) {
                collectFilesRecursive(overrideSub, L"media_soviet", files);
            }
        }

        pack.totalFiles = files.size();

        for (auto& [relKey, fullTarget] : files) {
            auto existing = m_map.find(relKey);
            if (existing == m_map.end()) {
                m_map.emplace(relKey, std::move(fullTarget));
                winnerPackIdx.emplace(std::move(relKey), packIdx);
                ++pack.indexedFiles;
            } else {
                const size_t winIdx = winnerPackIdx[relKey];
                if (winIdx == packIdx) {
                    continue;
                }

                ++pack.conflictCount;
                auto cIt = conflictIdxByRel.find(relKey);
                if (cIt == conflictIdxByRel.end()) {
                    ++m_packs[winIdx].conflictCount;
                    ConflictEntry entry;
                    entry.relativePath = wideToUtf8(relKey);
                    entry.winningPack  = m_packs[winIdx].displayName;
                    entry.overriddenPacks.push_back(pack.displayName);
                    conflictIdxByRel.emplace(relKey, m_conflicts.size());
                    m_conflicts.push_back(std::move(entry));
                } else {
                    m_conflicts[cIt->second].overriddenPacks.push_back(pack.displayName);
                }
            }
        }
    }

    m_built = true;
    wrsr::log::info("VfsIndex::build complete — packs=%zu indexed_files=%zu conflicts=%zu",
                    m_packs.size(), m_map.size(), m_conflicts.size());
}

bool VfsIndex::movePackUp(size_t index, Config& cfg)
{
    if (index == 0 || index >= m_packs.size()) {
        return false;
    }
    std::swap(m_packs[index - 1], m_packs[index]);
    syncPacksToConfig(cfg);
    return true;
}

bool VfsIndex::movePackDown(size_t index, Config& cfg)
{
    if (index + 1 >= m_packs.size()) {
        return false;
    }
    std::swap(m_packs[index], m_packs[index + 1]);
    syncPacksToConfig(cfg);
    return true;
}

void VfsIndex::syncPacksToConfig(Config& cfg) const
{
    std::vector<std::string>             order;
    std::unordered_map<std::string, bool> enabledMap;
    order.reserve(m_packs.size());
    for (const auto& p : m_packs) {
        order.push_back(p.id);
        enabledMap[p.id] = (p.enabled != 0);
    }
    cfg.setPackStates(order, enabledMap);
}

} // namespace mr
