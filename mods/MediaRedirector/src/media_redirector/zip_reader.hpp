#pragma once
// ============================================================================
// zip_reader.hpp — Self-contained, loader-lock-safe ZIP reader (Store/Deflate)
// ============================================================================

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace mr::zip {

// Inspects the ZIP Central Directory without decompressing any data.
// Returns the number of files inside media_soviet/ or override/ subtrees.
size_t countMediaEntries(const std::wstring& zipPath);

// Extracts media_soviet/ and override/ entries from zipPath into cacheDir\media_soviet\...
// Uses a .cache_stamp file (size + mtime) to skip re-extraction when unchanged.
// Appends (normalized_rel_key, full_extracted_path) to outFiles.
bool extractMediaEntries(const std::wstring& zipPath,
                         const std::wstring& cacheDir,
                         std::vector<std::pair<std::wstring, std::wstring>>& outFiles);

} // namespace mr::zip
