// ============================================================================
// zip_reader.cpp — Clean-room ZIP Central Directory & RFC 1951 Inflate reader
// ============================================================================
#include "zip_reader.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wrsr/logger.hpp"

namespace mr::zip {

namespace {

[[nodiscard]] uint16_t readU16(const uint8_t* p) noexcept
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

[[nodiscard]] uint32_t readU32(const uint8_t* p) noexcept
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

struct ZipEntryMeta {
    std::string name;
    uint16_t    method{0};
    uint32_t    compSize{0};
    uint32_t    uncompSize{0};
    uint32_t    localHeaderOffset{0};
};

// Reads the entire ZIP file into memory (override packs are small texture/shader/ini archives)
bool readWholeFile(const std::wstring& path, std::vector<uint8_t>& out, uint64_t& outSize, uint64_t& outMtime)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileSize{};
    FILETIME      writeTime{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0 ||
        fileSize.QuadPart > 512LL * 1024LL * 1024LL ||
        !GetFileTime(hFile, nullptr, nullptr, &writeTime)) {
        CloseHandle(hFile);
        return false;
    }

    outSize  = static_cast<uint64_t>(fileSize.QuadPart);
    outMtime = (static_cast<uint64_t>(writeTime.dwHighDateTime) << 32) | writeTime.dwLowDateTime;

    out.resize(static_cast<size_t>(outSize));
    size_t totalRead = 0;
    while (totalRead < out.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(out.size() - totalRead, size_t{64 * 1024 * 1024}));
        DWORD       got   = 0;
        if (!ReadFile(hFile, out.data() + totalRead, chunk, &got, nullptr) || got == 0) {
            CloseHandle(hFile);
            return false;
        }
        totalRead += got;
    }

    CloseHandle(hFile);
    return true;
}

bool parseCentralDirectory(const uint8_t* data, size_t size, std::vector<ZipEntryMeta>& outEntries)
{
    if (size < 22) {
        return false;
    }

    // Locate End of Central Directory (0x06054b50) scanning backwards
    const size_t minOff = (size > 65557) ? (size - 65557) : 0;
    size_t       eocd   = SIZE_MAX;
    for (size_t i = size - 22;; --i) {
        if (data[i] == 0x50 && data[i + 1] == 0x4B && data[i + 2] == 0x05 && data[i + 3] == 0x06) {
            const uint16_t commentLen = readU16(data + i + 20);
            if (i + 22 + commentLen == size) {
                eocd = i;
                break;
            }
        }
        if (i == minOff) {
            break;
        }
    }
    if (eocd == SIZE_MAX) {
        return false;
    }

    const uint16_t totalEntries = readU16(data + eocd + 10);
    const uint32_t cdSize       = readU32(data + eocd + 12);
    const uint32_t cdOffset     = readU32(data + eocd + 16);
    if (static_cast<uint64_t>(cdOffset) + cdSize > eocd) {
        return false;
    }

    outEntries.reserve(totalEntries);
    size_t pos = cdOffset;
    const size_t cdEnd = static_cast<size_t>(cdOffset) + cdSize;
    while (pos + 46 <= cdEnd) {
        if (readU32(data + pos) != 0x02014B50u) {
            break;
        }
        const uint16_t method    = readU16(data + pos + 10);
        const uint32_t compSize  = readU32(data + pos + 20);
        const uint32_t uncompSz  = readU32(data + pos + 24);
        const uint16_t nameLen   = readU16(data + pos + 28);
        const uint16_t extraLen  = readU16(data + pos + 30);
        const uint16_t commLen   = readU16(data + pos + 32);
        const uint32_t localOff  = readU32(data + pos + 42);

        if (pos + 46 + nameLen + extraLen + commLen > cdEnd) {
            break;
        }

        ZipEntryMeta meta;
        meta.name.assign(reinterpret_cast<const char*>(data + pos + 46), nameLen);
        meta.method            = method;
        meta.compSize          = compSize;
        meta.uncompSize        = uncompSz;
        meta.localHeaderOffset = localOff;
        outEntries.push_back(std::move(meta));

        pos += 46 + nameLen + extraLen + commLen;
    }
    return !outEntries.empty();
}

// Extracts normalized "media_soviet\..." relative path if entry belongs to media_soviet/ or override/
// (at archive root or nested inside a top-level folder like "ModPack/media_soviet/...").
bool extractMediaRelPath(const std::string& entryName, std::wstring& outRelWide)
{
    if (entryName.empty() || entryName.back() == '/' || entryName.back() == '\\') {
        return false;
    }

    std::string norm;
    norm.reserve(entryName.size());
    for (char c : entryName) {
        if (c == '\\') {
            c = '/';
        } else if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
        if (c == '/' && !norm.empty() && norm.back() == '/') {
            continue;
        }
        norm.push_back(c);
    }

    // Reject path traversal
    if (norm.find("..") != std::string::npos) {
        return false;
    }

    size_t tailStart = std::string::npos;
    auto checkPrefix = [&](size_t offset) -> bool {
        if (norm.compare(offset, 13, "media_soviet/") == 0) {
            tailStart = offset + 13;
            return true;
        }
        if (norm.compare(offset, 9, "override/") == 0) {
            tailStart = offset + 9;
            return true;
        }
        return false;
    };

    if (!checkPrefix(0)) {
        const size_t firstSlash = norm.find('/');
        if (firstSlash == std::string::npos || !checkPrefix(firstSlash + 1)) {
            return false;
        }
    }

    if (tailStart >= norm.size()) {
        return false;
    }

    std::string relUtf8 = "media_soviet\\" + norm.substr(tailStart);
    for (char& c : relUtf8) {
        if (c == '/') {
            c = '\\';
        }
    }

    const int needed = MultiByteToWideChar(CP_UTF8, 0, relUtf8.c_str(), -1, nullptr, 0);
    if (needed <= 1) {
        return false;
    }
    outRelWide.resize(static_cast<size_t>(needed - 1));
    MultiByteToWideChar(CP_UTF8, 0, relUtf8.c_str(), -1, outRelWide.data(), needed);
    return true;
}

// ── Clean-room RFC 1951 Inflate (Deflate decompression) ──────────────────────
struct BitReader {
    const uint8_t* data;
    size_t         size;
    size_t         bytePos{0};
    uint32_t       bitBuf{0};
    int            bitCount{0};

    bool ensure(int n) noexcept
    {
        while (bitCount < n) {
            if (bytePos >= size) {
                return false;
            }
            bitBuf |= static_cast<uint32_t>(data[bytePos++]) << bitCount;
            bitCount += 8;
        }
        return true;
    }

    bool readBits(int n, uint32_t& out) noexcept
    {
        if (n == 0) {
            out = 0;
            return true;
        }
        if (!ensure(n)) {
            return false;
        }
        out = bitBuf & ((1u << n) - 1u);
        bitBuf >>= n;
        bitCount -= n;
        return true;
    }

    void alignToByte() noexcept
    {
        const int discard = bitCount & 7;
        bitBuf >>= discard;
        bitCount -= discard;
    }
};

struct HuffTable {
    static constexpr int kMaxBits = 15;
    std::array<uint16_t, kMaxBits + 1> counts{};
    std::vector<uint16_t>              symbols;

    bool build(const uint8_t* lengths, size_t numSyms)
    {
        counts.fill(0);
        for (size_t i = 0; i < numSyms; ++i) {
            if (lengths[i] > kMaxBits) {
                return false;
            }
            ++counts[lengths[i]];
        }
        if (counts[0] == numSyms) {
            symbols.clear();
            return true;
        }

        std::array<uint16_t, kMaxBits + 1> offsets{};
        uint16_t                           total = 0;
        for (int b = 1; b <= kMaxBits; ++b) {
            offsets[b] = total;
            total      = static_cast<uint16_t>(total + counts[b]);
        }
        symbols.resize(total);
        for (size_t i = 0; i < numSyms; ++i) {
            if (lengths[i] != 0) {
                symbols[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
            }
        }
        return true;
    }

    bool decode(BitReader& br, uint16_t& outSym) const noexcept
    {
        int      code  = 0;
        int      first = 0;
        int      index = 0;
        uint32_t bit   = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            if (!br.readBits(1, bit)) {
                return false;
            }
            code |= static_cast<int>(bit);
            const int count = counts[len];
            if (code - count < first) {
                outSym = symbols[static_cast<size_t>(index + (code - first))];
                return true;
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }
};

bool inflateDeflate(const uint8_t* src, size_t srcLen, size_t expectedOutLen, std::vector<uint8_t>& out)
{
    static constexpr uint16_t kLenBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static constexpr uint8_t kLenExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static constexpr uint16_t kDistBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static constexpr uint8_t kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    static constexpr uint8_t kCodeOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    out.clear();
    out.reserve(expectedOutLen);
    BitReader br{src, srcLen};

    uint32_t bfinal = 0;
    do {
        uint32_t btype = 0;
        if (!br.readBits(1, bfinal) || !br.readBits(2, btype)) {
            return false;
        }

        if (btype == 0) {
            br.alignToByte();
            if (br.bytePos + 4 > br.size) {
                return false;
            }
            const uint16_t len  = readU16(br.data + br.bytePos);
            const uint16_t nlen = readU16(br.data + br.bytePos + 2);
            br.bytePos += 4;
            if (static_cast<uint16_t>(len ^ 0xFFFFu) != nlen || br.bytePos + len > br.size) {
                return false;
            }
            out.insert(out.end(), br.data + br.bytePos, br.data + br.bytePos + len);
            br.bytePos += len;
        } else if (btype == 1 || btype == 2) {
            HuffTable litTable;
            HuffTable distTable;

            if (btype == 1) {
                std::array<uint8_t, 288> litLens{};
                for (size_t i = 0; i <= 143; ++i) litLens[i] = 8;
                for (size_t i = 144; i <= 255; ++i) litLens[i] = 9;
                for (size_t i = 256; i <= 279; ++i) litLens[i] = 7;
                for (size_t i = 280; i <= 287; ++i) litLens[i] = 8;
                std::array<uint8_t, 32> distLens{};
                distLens.fill(5);
                if (!litTable.build(litLens.data(), 288) || !distTable.build(distLens.data(), 32)) {
                    return false;
                }
            } else {
                uint32_t hlit = 0, hdist = 0, hclen = 0;
                if (!br.readBits(5, hlit) || !br.readBits(5, hdist) || !br.readBits(4, hclen)) {
                    return false;
                }
                const size_t numLit  = hlit + 257;
                const size_t numDist = hdist + 1;
                const size_t numCode = hclen + 4;
                if (numLit > 286 || numDist > 30) {
                    return false;
                }

                std::array<uint8_t, 19> codeLens{};
                for (size_t i = 0; i < numCode; ++i) {
                    uint32_t v = 0;
                    if (!br.readBits(3, v)) {
                        return false;
                    }
                    codeLens[kCodeOrder[i]] = static_cast<uint8_t>(v);
                }

                HuffTable codeTable;
                if (!codeTable.build(codeLens.data(), 19)) {
                    return false;
                }

                std::vector<uint8_t> allLens(numLit + numDist, 0);
                size_t idx = 0;
                while (idx < allLens.size()) {
                    uint16_t sym = 0;
                    if (!codeTable.decode(br, sym)) {
                        return false;
                    }
                    if (sym < 16) {
                        allLens[idx++] = static_cast<uint8_t>(sym);
                    } else {
                        uint32_t rep = 0;
                        uint8_t  val = 0;
                        if (sym == 16) {
                            if (idx == 0 || !br.readBits(2, rep)) {
                                return false;
                            }
                            rep += 3;
                            val = allLens[idx - 1];
                        } else if (sym == 17) {
                            if (!br.readBits(3, rep)) {
                                return false;
                            }
                            rep += 3;
                        } else {
                            if (!br.readBits(7, rep)) {
                                return false;
                            }
                            rep += 11;
                        }
                        if (idx + rep > allLens.size()) {
                            return false;
                        }
                        for (uint32_t r = 0; r < rep; ++r) {
                            allLens[idx++] = val;
                        }
                    }
                }
                if (!litTable.build(allLens.data(), numLit) ||
                    !distTable.build(allLens.data() + numLit, numDist)) {
                    return false;
                }
            }

            for (;;) {
                uint16_t sym = 0;
                if (!litTable.decode(br, sym)) {
                    return false;
                }
                if (sym < 256) {
                    out.push_back(static_cast<uint8_t>(sym));
                } else if (sym == 256) {
                    break;
                } else {
                    const size_t lIdx = sym - 257;
                    if (lIdx >= 29) {
                        return false;
                    }
                    uint32_t extra = 0;
                    if (!br.readBits(kLenExtra[lIdx], extra)) {
                        return false;
                    }
                    const size_t matchLen = kLenBase[lIdx] + extra;

                    uint16_t dSym = 0;
                    if (!distTable.decode(br, dSym) || dSym >= 30) {
                        return false;
                    }
                    uint32_t dExtra = 0;
                    if (!br.readBits(kDistExtra[dSym], dExtra)) {
                        return false;
                    }
                    const size_t dist = kDistBase[dSym] + dExtra;
                    if (dist == 0 || dist > out.size()) {
                        return false;
                    }
                    for (size_t k = 0; k < matchLen; ++k) {
                        out.push_back(out[out.size() - dist]);
                    }
                }
            }
        } else {
            return false;
        }
    } while (!bfinal);

    return true;
}

void ensureDirRecursive(const std::wstring& dir)
{
    if (dir.empty()) {
        return;
    }
    const DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return;
    }
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        ensureDirRecursive(dir.substr(0, slash));
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

bool writeWholeFile(const std::wstring& path, const uint8_t* data, size_t len)
{
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        ensureDirRecursive(path.substr(0, slash));
    }
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    size_t writtenTotal = 0;
    while (writtenTotal < len) {
        const DWORD chunk = static_cast<DWORD>((std::min)(len - writtenTotal, size_t{64 * 1024 * 1024}));
        DWORD       wrote = 0;
        if (!WriteFile(hFile, data + writtenTotal, chunk, &wrote, nullptr)) {
            CloseHandle(hFile);
            return false;
        }
        writtenTotal += wrote;
    }
    CloseHandle(hFile);
    return true;
}

void collectCachedFiles(const std::wstring& baseDir,
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
        const std::wstring full = baseDir + L"\\" + ffd.cFileName;
        const std::wstring rel  = relPrefix + L"\\" + ffd.cFileName;
        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            collectCachedFiles(full, rel, outFiles);
        } else {
            outFiles.emplace_back(rel, full);
        }
    } while (FindNextFileW(hFind, &ffd));
    FindClose(hFind);
}

} // namespace

size_t countMediaEntries(const std::wstring& zipPath)
{
    std::vector<uint8_t> data;
    uint64_t             sz = 0, mt = 0;
    if (!readWholeFile(zipPath, data, sz, mt)) {
        return 0;
    }
    std::vector<ZipEntryMeta> entries;
    if (!parseCentralDirectory(data.data(), data.size(), entries)) {
        return 0;
    }
    size_t count = 0;
    std::wstring dummy;
    for (const auto& e : entries) {
        if (extractMediaRelPath(e.name, dummy)) {
            ++count;
        }
    }
    return count;
}

bool extractMediaEntries(const std::wstring& zipPath,
                         const std::wstring& cacheDir,
                         std::vector<std::pair<std::wstring, std::wstring>>& outFiles)
{
    HANDLE hFile = CreateFileW(zipPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER fsz{};
    FILETIME      ft{};
    const bool    statOk = GetFileSizeEx(hFile, &fsz) && GetFileTime(hFile, nullptr, nullptr, &ft);
    CloseHandle(hFile);
    if (!statOk) {
        return false;
    }

    const uint64_t curSize  = static_cast<uint64_t>(fsz.QuadPart);
    const uint64_t curMtime = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;

    char expectedStamp[64]{};
    std::snprintf(expectedStamp, sizeof(expectedStamp), "%llu:%llu",
                  static_cast<unsigned long long>(curSize),
                  static_cast<unsigned long long>(curMtime));

    const std::wstring stampPath = cacheDir + L"\\.cache_stamp";
    {
        HANDLE hStamp = CreateFileW(stampPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hStamp != INVALID_HANDLE_VALUE) {
            char  buf[64]{};
            DWORD got = 0;
            ReadFile(hStamp, buf, sizeof(buf) - 1, &got, nullptr);
            CloseHandle(hStamp);
            if (std::string_view(buf, got) == expectedStamp) {
                collectCachedFiles(cacheDir + L"\\media_soviet", L"media_soviet", outFiles);
                return !outFiles.empty();
            }
        }
    }

    std::vector<uint8_t> data;
    uint64_t             sz = 0, mt = 0;
    if (!readWholeFile(zipPath, data, sz, mt)) {
        return false;
    }
    std::vector<ZipEntryMeta> entries;
    if (!parseCentralDirectory(data.data(), data.size(), entries)) {
        return false;
    }

    std::vector<uint8_t> decomp;
    size_t extractedCount = 0;
    for (const auto& e : entries) {
        std::wstring relKey;
        if (!extractMediaRelPath(e.name, relKey)) {
            continue;
        }
        if (static_cast<uint64_t>(e.localHeaderOffset) + 30 > data.size() ||
            readU32(data.data() + e.localHeaderOffset) != 0x04034B50u) {
            continue;
        }
        const uint16_t lNameLen  = readU16(data.data() + e.localHeaderOffset + 26);
        const uint16_t lExtraLen = readU16(data.data() + e.localHeaderOffset + 28);
        const size_t   dataStart = static_cast<size_t>(e.localHeaderOffset) + 30 + lNameLen + lExtraLen;
        if (dataStart + e.compSize > data.size()) {
            continue;
        }

        const uint8_t* payload    = data.data() + dataStart;
        const uint8_t* writePtr   = nullptr;
        size_t         writeLen   = 0;

        if (e.method == 0) {
            writePtr = payload;
            writeLen = e.compSize;
        } else if (e.method == 8) {
            if (!inflateDeflate(payload, e.compSize, e.uncompSize, decomp)) {
                wrsr::log::warn("zip_reader: deflate failed for %s", e.name.c_str());
                continue;
            }
            writePtr = decomp.data();
            writeLen = decomp.size();
        } else {
            continue;
        }

        const std::wstring outPath = cacheDir + L"\\" + relKey;
        if (writeWholeFile(outPath, writePtr, writeLen)) {
            outFiles.emplace_back(std::move(relKey), outPath);
            ++extractedCount;
        }
    }

    if (extractedCount > 0) {
        writeWholeFile(stampPath, reinterpret_cast<const uint8_t*>(expectedStamp), std::strlen(expectedStamp));
    }
    return extractedCount > 0;
}

} // namespace mr::zip
