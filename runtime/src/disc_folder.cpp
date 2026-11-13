#include "disc_folder.hpp"
#include "ps2/disc.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <share.h>
#include <errno.h>
#endif

namespace ps2::disc
{
namespace
{
namespace fs = std::filesystem;

bool native_arkd_selected()
{
    const char *value = std::getenv("SDBZ_ARKD_NATIVE");
    return !value || (std::strcmp(value, "0") != 0
        && std::strcmp(value, "false") != 0);
}

struct s_direct_game_asset
{
    std::string name;
    fs::path path;
    std::uint32_t size = 0;
};

struct s_native_asset
{
    std::string name;
    std::string format;
    fs::path path;
    std::uint32_t size = 0;
};

std::vector<s_native_asset> g_native_assets;
fs::path g_native_asset_root;
std::vector<s_direct_game_asset> g_direct_game_assets;
fs::path g_direct_game_root;

std::string upper(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

std::uint64_t sectors(std::uint64_t size)
{
    return (size + kSector - 1) / kSector;
}

std::uint32_t read_u32_le(const std::uint8_t *data)
{
    return static_cast<std::uint32_t>(data[0])
        | (static_cast<std::uint32_t>(data[1]) << 8)
        | (static_cast<std::uint32_t>(data[2]) << 16)
        | (static_cast<std::uint32_t>(data[3]) << 24);
}

void write_u32_le(std::uint8_t *data, std::uint32_t value)
{
    data[0] = static_cast<std::uint8_t>(value);
    data[1] = static_cast<std::uint8_t>(value >> 8);
    data[2] = static_cast<std::uint8_t>(value >> 16);
    data[3] = static_cast<std::uint8_t>(value >> 24);
}

std::vector<std::uint8_t> encode_info(const std::vector<std::uint8_t> &decoded)
{
    std::vector<std::uint8_t> encoded = decoded;
    for (std::size_t offset = 0x10; offset < decoded.size(); ++offset)
    {
        const auto value = static_cast<std::uint8_t>(~static_cast<std::uint8_t>(decoded[offset] + decoded[offset & 15]));
        encoded[offset] = static_cast<std::uint8_t>((value << 4) | (value >> 4));
    }
    return encoded;
}

struct s_layout_entry
{
    std::uint64_t lba = 0;
    std::uint64_t size = 0;
    fs::path relative_path;
};

struct s_archive_entry
{
    std::string name;
    fs::path relative_path;
    std::uint32_t sector_offset = 0;
    std::uint32_t sector_count = 0;
    std::uint32_t byte_size = 0;
};


fs::path archive_path(const std::string &raw_name);

bool is_cp932_lead(std::uint8_t value)
{
    return (value >= 0x81 && value <= 0x9F) || (value >= 0xE0 && value <= 0xFC);
}

void normalize_cp932_separators(std::string &name)
{
    for (std::size_t index = 0; index < name.size(); ++index)
    {
        const auto value = static_cast<std::uint8_t>(name[index]);
        if (is_cp932_lead(value))
        {
            ++index; // A CP932 trail byte may itself be 0x5C ('\\').
            continue;
        }
        if (name[index] == '\\') name[index] = '/';
    }
}

std::string archive_name_key(std::string name)
{
    normalize_cp932_separators(name);
    for (std::size_t index = 0; index < name.size(); ++index)
    {
        const auto value = static_cast<std::uint8_t>(name[index]);
        if (is_cp932_lead(value))
        {
            ++index;
            continue;
        }
        if (value >= 'a' && value <= 'z') name[index] = static_cast<char>(value - ('a' - 'A'));
    }
    return name;
}

std::string archive_name_from_path(const fs::path &relative,
    std::size_t maximum_name_bytes = 0x20)
{
    std::string encoded;
#ifdef _WIN32
    std::wstring wide = relative.generic_wstring();
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    if (wide.empty() || wide.size() > static_cast<std::size_t>(INT_MAX))
    {
        throw std::runtime_error("new GAME asset path is empty or too long");
    }
    BOOL used_default = FALSE;
    const int required = WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, &used_default);
    if (required <= 0 || used_default)
    {
        throw std::runtime_error("new GAME asset path cannot be represented in CP932: " + relative.string());
    }
    encoded.resize(static_cast<std::size_t>(required));
    used_default = FALSE;
    if (WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, wide.data(), static_cast<int>(wide.size()),
            encoded.data(), required, nullptr, &used_default) != required || used_default)
    {
        throw std::runtime_error("cannot convert new GAME asset path to CP932: " + relative.string());
    }
#else
    encoded = relative.generic_string();
    std::replace(encoded.begin(), encoded.end(), '/', '\\');
#endif
    if (encoded.empty() || encoded.size() > maximum_name_bytes
        || encoded.find('\0') != std::string::npos)
    {
        throw std::runtime_error("new GAME asset path does not fit INFO.DAT's 32-byte name field: " + relative.string());
    }
    (void)archive_path(encoded);
    return encoded;
}

void append_info_record(std::vector<std::uint8_t> &decoded, std::size_t index,
    const s_archive_entry &entry, const std::vector<s_archive_entry> &existing_entries)
{
    const std::uint64_t record_offset64 = 0x30ull + static_cast<std::uint64_t>(index) * 0x30ull;
    if (record_offset64 + 0x30ull > UINT32_MAX)
    {
        throw std::runtime_error("INFO.DAT record table exceeds its 32-bit size limit");
    }
    const std::size_t record_offset = static_cast<std::size_t>(record_offset64);
    if (decoded.size() < record_offset + 0x30) decoded.resize(record_offset + 0x30, 0);

    std::uint8_t opaque[4] = {};
    if (!existing_entries.empty())
    {
        const auto extension = upper(entry.relative_path.extension().string());
        std::size_t template_index = 0;
        for (std::size_t candidate = 0; candidate < existing_entries.size(); ++candidate)
        {
            if (upper(existing_entries[candidate].relative_path.extension().string()) == extension)
            {
                template_index = candidate;
                break;
            }
        }
        const std::size_t source_offset = 0x30 + template_index * 0x30 + 0x20;
        if (source_offset + sizeof(opaque) <= decoded.size())
        {
            std::memcpy(opaque, decoded.data() + source_offset, sizeof(opaque));
        }
    }

    auto *record = decoded.data() + record_offset;
    std::memset(record, 0, 0x30);
    std::memcpy(record, entry.name.data(), entry.name.size());
    std::memcpy(record + 0x20, opaque, sizeof(opaque));
    write_u32_le(record + 0x24, entry.sector_offset);
    write_u32_le(record + 0x28, entry.sector_count);
    write_u32_le(record + 0x2C, entry.byte_size);
}


fs::path archive_path(const std::string &raw_name)
{
    std::string name = raw_name;
    normalize_cp932_separators(name);
    if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos)
    {
        throw std::runtime_error("unsafe path in INFO.DAT: " + raw_name);
    }

#ifdef _WIN32
    if (name.size() > static_cast<std::size_t>(INT_MAX))
    {
        throw std::runtime_error("INFO.DAT path is too long: " + raw_name);
    }
    const int required = MultiByteToWideChar(932, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (required <= 0)
    {
        throw std::runtime_error("invalid CP932 path in INFO.DAT: " + raw_name);
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(932, 0, name.data(), static_cast<int>(name.size()), wide.data(), required) != required)
    {
        throw std::runtime_error("cannot convert CP932 path in INFO.DAT: " + raw_name);
    }
    const fs::path relative(wide);
#else
    const fs::path relative(name);
#endif

    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
    {
        throw std::runtime_error("unsafe path in INFO.DAT: " + raw_name);
    }
    for (const auto &part : relative)
    {
        if (part.empty() || part == "." || part == "..")
        {
            throw std::runtime_error("unsafe path in INFO.DAT: " + raw_name);
        }
    }
    return relative;
}

std::vector<s_archive_entry> parse_info_entries(const std::vector<std::uint8_t> &decoded)
{
    if (decoded.size() < 0x30)
    {
        throw std::runtime_error("decoded INFO.DAT is smaller than its 0x30-byte header");
    }
    static constexpr char signature[] = "www.arika.co.jp";
    if (std::memcmp(decoded.data() + 1, signature, sizeof(signature) - 1) != 0)
    {
        throw std::runtime_error("decoded INFO.DAT signature is not www.arika.co.jp");
    }

    const std::uint32_t count = read_u32_le(decoded.data() + 0x2C);
    if (!count)
    {
        throw std::runtime_error("INFO.DAT contains no asset records");
    }
    const std::uint64_t table_end = 0x30ull + static_cast<std::uint64_t>(count) * 0x30ull;
    if (table_end > decoded.size())
    {
        throw std::runtime_error("INFO.DAT record table exceeds the file size");
    }

    std::vector<s_archive_entry> entries;
    entries.reserve(count);
    std::set<std::string> names;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const auto *record = decoded.data() + 0x30 + static_cast<std::size_t>(index) * 0x30;
        const auto *name_end = static_cast<const std::uint8_t *>(std::memchr(record, 0, 0x20));
        const std::size_t name_size = name_end ? static_cast<std::size_t>(name_end - record) : 0x20;
        if (!name_size)
        {
            throw std::runtime_error("INFO.DAT contains an empty asset name");
        }

        s_archive_entry entry;
        entry.name.assign(reinterpret_cast<const char *>(record), name_size);
        entry.relative_path = archive_path(entry.name);
        entry.sector_offset = read_u32_le(record + 0x24);
        entry.sector_count = read_u32_le(record + 0x28);
        entry.byte_size = read_u32_le(record + 0x2C);
        if (!entry.sector_count || !entry.byte_size
            || entry.byte_size > static_cast<std::uint64_t>(entry.sector_count) * kSector)
        {
            throw std::runtime_error("invalid INFO.DAT extent for " + entry.name);
        }
        if (!names.insert(archive_name_key(entry.name)).second)
        {
            throw std::runtime_error("duplicate asset path in INFO.DAT: " + entry.name);
        }
        entries.push_back(std::move(entry));
    }

    std::vector<const s_archive_entry *> extents;
    extents.reserve(entries.size());
    for (const auto &entry : entries) extents.push_back(&entry);
    std::sort(extents.begin(), extents.end(), [](const auto *a, const auto *b)
    {
        return a->sector_offset < b->sector_offset;
    });
    std::uint64_t previous_end = 0;
    for (const auto *entry : extents)
    {
        const std::uint64_t start = entry->sector_offset;
        const std::uint64_t finish = start + entry->sector_count;
        if (start < previous_end)
        {
            throw std::runtime_error("overlapping INFO.DAT extents near " + entry->name);
        }
        previous_end = finish;
    }
    return entries;
}

std::vector<s_archive_entry> read_info_entries(const fs::path &path, std::vector<std::uint8_t> &decoded)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
    {
        throw std::runtime_error("cannot open INFO.DAT: " + path.string());
    }
    const auto end = input.tellg();
    if (end < 0x30)
    {
        throw std::runtime_error("INFO.DAT is smaller than its 0x30-byte header");
    }

    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!input.read(reinterpret_cast<char *>(encoded.data()), static_cast<std::streamsize>(encoded.size())))
    {
        throw std::runtime_error("cannot read INFO.DAT: " + path.string());
    }
    static constexpr char signature[] = "www.arika.co.jp";
    if (std::memcmp(encoded.data() + 1, signature, sizeof(signature) - 1) != 0)
    {
        throw std::runtime_error("INFO.DAT signature is not www.arika.co.jp");
    }

    decoded = encoded;
    for (std::size_t offset = 0x10; offset < decoded.size(); ++offset)
    {
        const auto value = encoded[offset];
        const auto swapped = static_cast<std::uint8_t>((value << 4) | (value >> 4));
        decoded[offset] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(~swapped) - encoded[offset & 15]);
    }

    const std::uint32_t count = read_u32_le(decoded.data() + 0x2C);
    if (!count)
    {
        throw std::runtime_error("INFO.DAT contains no asset records");
    }
    const std::uint64_t table_end = 0x30ull + static_cast<std::uint64_t>(count) * 0x30ull;
    if (table_end > decoded.size())
    {
        throw std::runtime_error("INFO.DAT record table exceeds the file size");
    }

    std::vector<s_archive_entry> entries;
    entries.reserve(count);
    std::set<std::string> names;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const auto *record = decoded.data() + 0x30 + static_cast<std::size_t>(index) * 0x30;
        const auto *name_end = static_cast<const std::uint8_t *>(std::memchr(record, 0, 0x20));
        const std::size_t name_size = name_end ? static_cast<std::size_t>(name_end - record) : 0x20;
        if (!name_size)
        {
            throw std::runtime_error("INFO.DAT contains an empty asset name");
        }

        s_archive_entry entry;
        entry.name.assign(reinterpret_cast<const char *>(record), name_size);
        entry.relative_path = archive_path(entry.name);
        entry.sector_offset = read_u32_le(record + 0x24);
        entry.sector_count = read_u32_le(record + 0x28);
        entry.byte_size = read_u32_le(record + 0x2C);
        if (!entry.sector_count || !entry.byte_size
            || entry.byte_size > static_cast<std::uint64_t>(entry.sector_count) * kSector)
        {
            throw std::runtime_error("invalid INFO.DAT extent for " + entry.name);
        }
        if (!names.insert(archive_name_key(entry.name)).second)
        {
            throw std::runtime_error("duplicate asset path in INFO.DAT: " + entry.name);
        }
        entries.push_back(std::move(entry));
    }

    std::vector<const s_archive_entry *> extents;
    extents.reserve(entries.size());
    for (const auto &entry : entries) extents.push_back(&entry);
    std::sort(extents.begin(), extents.end(), [](const auto *a, const auto *b)
    {
        return a->sector_offset < b->sector_offset;
    });
    std::uint64_t previous_end = 0;
    for (const auto *entry : extents)
    {
        const std::uint64_t start = entry->sector_offset;
        const std::uint64_t finish = start + entry->sector_count;
        if (start < previous_end)
        {
            throw std::runtime_error("overlapping INFO.DAT extents near " + entry->name);
        }
        previous_end = finish;
    }
    return entries;
}

const s_layout_entry *find_layout_entry(const std::vector<s_layout_entry> &entries, const std::string &name)
{
    const std::string wanted = upper(name);
    for (const auto &entry : entries)
    {
        if (upper(entry.relative_path.generic_string()) == wanted) return &entry;
    }
    return nullptr;
}

fs::path find_directory(const fs::path &root, const std::string &name)
{
    std::error_code error;
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
    {
        if (it->is_directory(error) && !error && upper(it->path().filename().string()) == upper(name))
        {
            return it->path();
        }
    }
    return {};
}

bool is_inside(const fs::path &root, const fs::path &path)
{
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

// Runtime-rebuilt Arika archive (when host INFO.DAT is absent).
struct s_virtual_archive
{
    std::vector<std::uint8_t> encoded_info;
    std::vector<s_archive_entry> assets;
    fs::path loose_root;
    bool active = false;
};

s_virtual_archive g_virtual_archive;

s_virtual_archive build_virtual_archive_from_game(const fs::path &loose_root)
{
    s_virtual_archive result;
    result.loose_root = loose_root;
    std::set<std::string> name_keys;
    std::error_code scan_error;
    for (fs::recursive_directory_iterator it(loose_root, scan_error), end;
        !scan_error && it != end; it.increment(scan_error))
    {
        if (it->is_symlink() || !it->is_regular_file()) continue;
        const auto relative_path = it->path().lexically_relative(loose_root);
        std::string encoded_name;
        try
        {
            encoded_name = archive_name_from_path(relative_path);
        }
        catch (const std::exception &error)
        {
            std::fprintf(stderr, "[disc] INFO rebuild: skip (%s)\n", error.what());
            continue;
        }
        if (!name_keys.insert(archive_name_key(encoded_name)).second)
            throw std::runtime_error("duplicate asset under GAME/");
        const auto canonical_asset = fs::canonical(it->path());
        if (!is_inside(loose_root, canonical_asset))
            throw std::runtime_error("GAME asset outside GAME/");
        const std::uint64_t byte_size = fs::file_size(canonical_asset);
        if (!byte_size || byte_size > UINT32_MAX)
            throw std::runtime_error("GAME asset has unsupported size");
        s_archive_entry entry;
        entry.name = std::move(encoded_name);
        entry.relative_path = relative_path;
        entry.byte_size = static_cast<std::uint32_t>(byte_size);
        entry.sector_count = static_cast<std::uint32_t>(sectors(byte_size));
        result.assets.push_back(std::move(entry));
    }
    if (scan_error)
        throw std::runtime_error("cannot enumerate GAME/: " + scan_error.message());
    if (result.assets.empty())
        throw std::runtime_error("GAME/ has no files to rebuild INFO.DAT from");

    std::sort(result.assets.begin(), result.assets.end(),
        [](const s_archive_entry &a, const s_archive_entry &b) {
            return archive_name_key(a.name) < archive_name_key(b.name);
        });
    std::uint64_t next = 0;
    for (auto &entry : result.assets)
    {
        entry.sector_offset = static_cast<std::uint32_t>(next);
        next += entry.sector_count;
    }

    std::vector<std::uint8_t> decoded(0x30, 0);
    static constexpr std::uint8_t kMagic[16] = {
        0x7f,'w','w','w','.','a','r','i','k','a','.','c','o','.','j','p'
    };
    std::memcpy(decoded.data(), kMagic, 16);
    write_u32_le(decoded.data() + 0x2C, static_cast<std::uint32_t>(result.assets.size()));
    for (std::size_t i = 0; i < result.assets.size(); ++i)
        append_info_record(decoded, i, result.assets[i], result.assets);
    (void)parse_info_entries(decoded);
    result.encoded_info = encode_info(decoded);
    // The EU beta INFO.DAT occupies exactly 0x18000 bytes: one 0x30-byte
    // header followed by 2047 physical record slots. Original unused
    // encoded slots contain 0x40, not an encoding of decoded zeroes.
    constexpr std::size_t kEuInfoSize = 0x18000;
    if (result.encoded_info.size() < kEuInfoSize)
        result.encoded_info.resize(kEuInfoSize, 0x40);
    if (result.assets.size() >= 2047)
        std::fprintf(stderr,
            "[disc] warning: INFO has %zu records; ARKD_DVD.IRX rejects "
            "entry_count >= file=2047\n", result.assets.size());
    else if (result.assets.size() >= 2005)
        std::fprintf(stderr,
            "[disc] warning: INFO has %zu records; ARKD_DVD.IRX may only read "
            "47 table sectors with file=2047 (record 2005 not yet verified)\n",
            result.assets.size());
    result.active = true;
    std::fprintf(stderr, "[disc] rebuilt INFO.DAT from GAME/ (%zu assets, %llu sectors)\n",
        result.assets.size(), static_cast<unsigned long long>(next));
    return result;
}



struct s_region
{
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    fs::path path;
    std::vector<std::uint8_t> memory;
    std::uint64_t end() const { return offset + sectors(size) * kSector; }
};

struct s_folder
{
    std::vector<s_region> regions;
    std::vector<s_region> overlays;
    std::uint64_t size = 0;
    fs::path cached_path;
    std::FILE *cached_file = nullptr;

    ~s_folder() { if (cached_file) std::fclose(cached_file); }

    std::size_t read(std::uint64_t offset, void *output, std::size_t count)
    {
        if (!count || offset >= size) return 0;
        count = static_cast<std::size_t>(std::min<std::uint64_t>(count, size - offset));
        auto *bytes = static_cast<std::uint8_t *>(output);
        std::memset(bytes, 0, count);
        const auto read_regions = [&](const std::vector<s_region> &source_regions) -> std::size_t
        {
            auto it = std::upper_bound(source_regions.begin(), source_regions.end(), offset,
                [](std::uint64_t value, const s_region &region) { return value < region.offset; });
            if (it != source_regions.begin()) --it;
            const std::uint64_t read_end = offset + count;
            for (; it != source_regions.end() && it->offset < read_end; ++it)
            {
                const auto begin = std::max(offset, it->offset);
                const auto finish = std::min(read_end, it->offset + it->size);
                if (begin >= finish) continue;
                const auto relative = begin - it->offset;
                const auto length = static_cast<std::size_t>(finish - begin);
                auto *destination = bytes + (begin - offset);
                if (!it->memory.empty())
                {
                    std::memcpy(destination, it->memory.data() + relative, length);
                    continue;
                }
                if (it->path.empty())
                {
                    // Zero-filled virtual extent (no host file). Leave the
                    // destination as the memset(0) from above.
                    continue;
                }
                if (cached_path != it->path || !cached_file)
                {
                    if (cached_file) std::fclose(cached_file);
                    cached_path = it->path;
                    cached_file = nullptr;
#ifdef _WIN32
                    // Shared read so AV / concurrent access does not lock us out.
                    cached_file = _wfsopen(cached_path.c_str(), L"rb", _SH_DENYNO);
                    if (!cached_file)
                    {
                        const DWORD err = GetLastError();
                        std::fwprintf(stderr, L"[disc] cannot open extracted file %ls (err %lu)\n",
                            cached_path.c_str(), static_cast<unsigned long>(err));
                        return static_cast<std::size_t>(begin - offset);
                    }
#else
                    cached_file = std::fopen(cached_path.c_str(), "rb");
                    if (!cached_file)
                    {
                        std::fprintf(stderr, "[disc] cannot open extracted file %s (errno %d)\n",
                            cached_path.generic_string().c_str(), errno);
                        return static_cast<std::size_t>(begin - offset);
                    }
#endif
                }
                bool seek_ok = false;
#ifdef _WIN32
                seek_ok = _fseeki64(cached_file, static_cast<long long>(relative), SEEK_SET) == 0;
#else
                seek_ok = fseeko(cached_file, static_cast<off_t>(relative), SEEK_SET) == 0;
#endif
                if (!seek_ok)
                {
#ifdef _WIN32
                    std::fwprintf(stderr, L"[disc] cannot seek extracted file %ls to %llu\n",
                        cached_path.c_str(), static_cast<unsigned long long>(relative));
#else
                    std::fprintf(stderr, "[disc] cannot seek extracted file %s to %llu\n",
                        cached_path.generic_string().c_str(), static_cast<unsigned long long>(relative));
#endif
                    return static_cast<std::size_t>(begin - offset);
                }
                if (std::fread(destination, 1, length, cached_file) != length)
                {
#ifdef _WIN32
                    std::fwprintf(stderr, L"[disc] cannot read extracted file %ls (%zu bytes at %llu)\n",
                        cached_path.c_str(), length, static_cast<unsigned long long>(relative));
#else
                    std::fprintf(stderr, "[disc] cannot read extracted file %s (%zu bytes at %llu)\n",
                        cached_path.generic_string().c_str(), length, static_cast<unsigned long long>(relative));
#endif
                    return static_cast<std::size_t>(begin - offset);
                }
            }
            return count;
        };

        const auto base_read = read_regions(regions);
        if (base_read != count) return base_read;
        return read_regions(overlays);
    }

    void validate_regions(std::vector<s_region> &source_regions, const char *description)
    {
        std::sort(source_regions.begin(), source_regions.end(), [](const auto &a, const auto &b) { return a.offset < b.offset; });
        std::uint64_t previous = 0;
        for (const auto &region : source_regions)
        {
            if (region.offset < previous || region.end() > size)
            {
                throw std::runtime_error(std::string("overlapping or out-of-range ") + description + " extents");
            }
            previous = region.end();
        }
    }

    void validate()
    {
        validate_regions(regions, "folder");
        validate_regions(overlays, "loose GAME");
    }
};

void put32(std::uint8_t *out, std::uint32_t value, bool big = false)
{
    for (unsigned i = 0; i < 4; ++i) out[big ? 3 - i : i] = static_cast<std::uint8_t>(value >> (i * 8));
}

void both32(std::uint8_t *out, std::uint32_t value) { put32(out, value); put32(out + 4, value, true); }
void both16(std::uint8_t *out, std::uint16_t value)
{
    out[0] = out[3] = static_cast<std::uint8_t>(value);
    out[1] = out[2] = static_cast<std::uint8_t>(value >> 8);
}

std::uint32_t read_both32(const std::uint8_t *data)
{
    const auto little = read_u32_le(data);
    const auto big = (static_cast<std::uint32_t>(data[4]) << 24)
        | (static_cast<std::uint32_t>(data[5]) << 16)
        | (static_cast<std::uint32_t>(data[6]) << 8)
        | static_cast<std::uint32_t>(data[7]);
    if (little != big) throw std::runtime_error("invalid ISO 9660 both-endian 32-bit field");
    return little;
}

void patch_iso_file_size(s_folder &folder, std::uint32_t file_lba,
    std::uint32_t old_size, std::uint32_t new_size)
{
    std::uint8_t pvd[kSector] = {};
    if (folder.read(16ull * kSector, pvd, sizeof(pvd)) != sizeof(pvd)
        || std::memcmp(pvd, "\x01" "CD001", 6) != 0)
    {
        throw std::runtime_error("cannot update GAME.DAT size: invalid ISO primary descriptor");
    }

    const std::uint32_t root_lba = read_both32(pvd + 158);
    const std::uint32_t root_size = read_both32(pvd + 166);
    std::set<std::uint32_t> visited;
    const auto visit = [&](const auto &self, std::uint32_t lba, std::uint32_t size, unsigned depth) -> bool
    {
        if (depth > 32 || !visited.insert(lba).second) return false;
        const std::uint64_t padded = sectors(size) * kSector;
        if (padded > std::numeric_limits<std::size_t>::max()) return false;
        std::vector<std::uint8_t> directory_data(static_cast<std::size_t>(padded), 0);
        if (folder.read(static_cast<std::uint64_t>(lba) * kSector,
                directory_data.data(), directory_data.size()) != directory_data.size()) return false;
        std::size_t offset = 0;
        while (offset < size)
        {
            const std::uint8_t length = directory_data[offset];
            if (!length)
            {
                offset = ((offset / kSector) + 1) * kSector;
                continue;
            }
            if (length < 34 || offset + length > directory_data.size())
                throw std::runtime_error("invalid ISO directory record while locating GAME.DAT");
            const auto *record = directory_data.data() + offset;
            const std::uint8_t name_length = record[32];
            if (33u + name_length > length) throw std::runtime_error("invalid ISO directory filename length");
            std::string name(reinterpret_cast<const char *>(record + 33), name_length);
            const bool is_directory = (record[25] & 2) != 0;
            const std::uint32_t child_lba = read_both32(record + 2);
            const std::uint32_t child_size = read_both32(record + 10);
            const bool special = name_length == 1 && (name[0] == '\0' || name[0] == '\1');
            if (!special)
            {
                const auto version = name.find(';');
                if (version != std::string::npos) name.resize(version);
                if (!is_directory && upper(name) == "GAME.DAT")
                {
                    if (child_lba != file_lba || child_size != old_size)
                        throw std::runtime_error("ISO GAME.DAT directory record does not match preserved layout");
                    std::vector<std::uint8_t> size_field(8);
                    both32(size_field.data(), new_size);
                    folder.overlays.push_back({static_cast<std::uint64_t>(lba) * kSector + offset + 10,
                        size_field.size(), {}, std::move(size_field)});
                    return true;
                }
                if (is_directory && self(self, child_lba, child_size, depth + 1)) return true;
            }
            offset += length;
        }
        return false;
    };

    if (!visit(visit, root_lba, root_size, 0))
        throw std::runtime_error("could not locate GAME.DAT in ISO directory tree");
}


void patch_iso_file_extent(s_folder &folder, const char *file_name,
    std::uint32_t old_lba, std::uint32_t old_size,
    std::uint32_t new_lba, std::uint32_t new_size)
{
    std::uint8_t pvd[kSector] = {};
    if (folder.read(16ull * kSector, pvd, sizeof(pvd)) != sizeof(pvd)
        || std::memcmp(pvd, "\x01" "CD001", 6) != 0)
    {
        throw std::runtime_error(std::string("cannot update ") + file_name + ": invalid ISO primary descriptor");
    }

    const std::uint32_t root_lba = read_both32(pvd + 158);
    const std::uint32_t root_size = read_both32(pvd + 166);
    std::set<std::uint32_t> visited;
    const auto visit = [&](const auto &self, std::uint32_t lba, std::uint32_t size, unsigned depth) -> bool
    {
        if (depth > 32 || !visited.insert(lba).second) return false;
        const std::uint64_t padded = sectors(size) * kSector;
        if (padded > std::numeric_limits<std::size_t>::max()) return false;
        std::vector<std::uint8_t> directory_data(static_cast<std::size_t>(padded), 0);
        if (folder.read(static_cast<std::uint64_t>(lba) * kSector,
                directory_data.data(), directory_data.size()) != directory_data.size()) return false;
        std::size_t offset = 0;
        while (offset < size)
        {
            const std::uint8_t length = directory_data[offset];
            if (!length)
            {
                offset = ((offset / kSector) + 1) * kSector;
                continue;
            }
            if (length < 34 || offset + length > directory_data.size())
                throw std::runtime_error("invalid ISO directory record while updating file extent");
            const auto *record = directory_data.data() + offset;
            const std::uint8_t name_length = record[32];
            if (33u + name_length > length) throw std::runtime_error("invalid ISO directory filename length");
            std::string name(reinterpret_cast<const char *>(record + 33), name_length);
            const bool is_directory = (record[25] & 2) != 0;
            const std::uint32_t child_lba = read_both32(record + 2);
            const std::uint32_t child_size = read_both32(record + 10);
            const bool special = name_length == 1 && (name[0] == '\0' || name[0] == '\1');
            if (!special)
            {
                const auto version = name.find(';');
                if (version != std::string::npos) name.resize(version);
                if (!is_directory && upper(name) == file_name)
                {
                    if (child_lba != old_lba || child_size != old_size)
                    {
                        throw std::runtime_error(std::string("ISO ") + file_name
                            + " directory record does not match preserved layout");
                    }
                    std::vector<std::uint8_t> extent_field(16);
                    both32(extent_field.data(), new_lba);
                    both32(extent_field.data() + 8, new_size);
                    folder.overlays.push_back({static_cast<std::uint64_t>(lba) * kSector + offset + 2,
                        extent_field.size(), {}, std::move(extent_field)});
                    return true;
                }
                if (is_directory && self(self, child_lba, child_size, depth + 1)) return true;
            }
            offset += length;
        }
        return false;
    };

    if (!visit(visit, root_lba, root_size, 0))
        throw std::runtime_error(std::string("could not locate ") + file_name + " in ISO directory tree");
}

struct s_node
{
    fs::path path;
    std::string parent;
    std::string name;
    std::uint32_t lba = 0, size = 0;
    bool directory = false;
};

std::vector<std::uint8_t> record(const s_node &node, const std::string &name)
{
    if (name.size() > 221) throw std::runtime_error("filename is too long for ISO 9660 directory records");
    std::vector<std::uint8_t> data(34 + (name.size() & ~std::size_t(1)), 0);
    data[0] = static_cast<std::uint8_t>(data.size());
    both32(data.data() + 2, node.lba);
    both32(data.data() + 10, node.size);
    data[18] = 126; data[19] = 1; data[20] = 1;
    data[25] = node.directory ? 2 : 0;
    both16(data.data() + 28, 1);
    data[32] = static_cast<std::uint8_t>(name.size());
    std::memcpy(data.data() + 33, name.data(), name.size());
    return data;
}

std::vector<std::uint8_t> directory(const std::string &key, const std::map<std::string, s_node> &nodes)
{
    std::vector<std::uint8_t> bytes;
    const auto append = [&](const s_node &node, const std::string &name)
    {
        auto data = record(node, name);
        if (bytes.size() % kSector + data.size() > kSector) bytes.resize(static_cast<std::size_t>(sectors(bytes.size()) * kSector));
        bytes.insert(bytes.end(), data.begin(), data.end());
    };
    const auto &node = nodes.at(key);
    append(node, std::string(1, '\0'));
    append(nodes.at(node.parent), std::string(1, '\1'));
    for (const auto &[child_key, child] : nodes)
    {
        if (!child_key.empty() && child.parent == key) append(child, child.name + (child.directory ? "" : ";1"));
    }
    bytes.resize(static_cast<std::size_t>(sectors(bytes.size()) * kSector));
    return bytes;
}

void synthetic_layout(const fs::path &root, s_folder &folder, std::vector<s_layout_entry> &entries)
{
    g_virtual_archive = {};
    std::map<std::string, s_node> nodes;
    nodes[""] = {root, "", "", 0, 0, true};
    std::error_code scan_error;
    for (fs::recursive_directory_iterator it(root, scan_error), end;
        !scan_error && it != end; it.increment(scan_error))
    {
        const auto relative = it->path().lexically_relative(root);
        if (relative.empty()) continue;
        // Avoid path::string() — throws on Windows when glyphs are outside the ANSI code page.
        std::wstring top = relative.begin()->wstring();
        for (auto &c : top)
            if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - (L'a' - L'A'));
        if (top == L".PS2-DISC" || top == L"GAME" || top == L"NATIVE"
            || it->is_symlink())
        {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_directory() && !it->is_regular_file()) continue;

        auto make_key = [](const std::wstring &wide) {
            std::string out;
            out.reserve(wide.size());
            for (wchar_t c : wide)
            {
                if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - (L'a' - L'A'));
                if (c >= 0x20 && c < 0x80) out.push_back(static_cast<char>(c));
                else
                {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "+%04X", static_cast<unsigned>(c & 0xFFFF));
                    out += buf;
                }
            }
            return out;
        };
        auto iso_name = [](std::wstring wide) {
            for (auto &c : wide)
                if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - (L'a' - L'A'));
            std::string out;
            for (wchar_t c : wide)
            {
                if (c >= 0x20 && c <= 0x7E) out.push_back(static_cast<char>(c));
                else out.push_back('_');
            }
            return out.empty() ? std::string("FILE") : out;
        };

        const auto key = make_key(relative.generic_wstring());
        const auto size = it->is_directory() ? 0 : it->file_size();
        if (size > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("extracted file exceeds the PS2 disc file-size limit");
        s_node node{it->path(), make_key(relative.parent_path().generic_wstring()),
            iso_name(relative.filename().wstring()), 0, static_cast<std::uint32_t>(size), it->is_directory()};
        if (!nodes.emplace(key, std::move(node)).second)
            throw std::runtime_error("case-insensitive filename collision in extracted folder");
    }
    if (scan_error) throw std::runtime_error("cannot enumerate extracted disc folder: " + scan_error.message());

    const auto loose_game_dir = find_directory(root, "GAME");
    if (nodes.find("INFO.DAT") == nodes.end() && native_arkd_selected())
    {
        // ARKD HLE reads GAME/ directly. An archive rebuilt for the
        // original IRX would impose its 2047-record capacity needlessly.
        if (loose_game_dir.empty())
            std::fprintf(stderr,
                "[disc] native ARKD selected without GAME/; INFO absent\n");
    }
    else if (nodes.find("INFO.DAT") == nodes.end())
    {
        if (loose_game_dir.empty())
            throw std::runtime_error("INFO.DAT is missing and GAME/ is not available to rebuild it");
        g_virtual_archive = build_virtual_archive_from_game(fs::canonical(loose_game_dir));
        nodes.emplace("INFO.DAT", s_node{root / "INFO.DAT", "", "INFO.DAT", 0,
            static_cast<std::uint32_t>(g_virtual_archive.encoded_info.size()), false});
        if (nodes.find("GAME.DAT") == nodes.end())
        {
            std::uint64_t game_sectors = 0;
            for (const auto &a : g_virtual_archive.assets)
                game_sectors += a.sector_count;
            if (!game_sectors || game_sectors > std::numeric_limits<std::uint32_t>::max() / kSector)
                throw std::runtime_error("generated GAME.DAT size is invalid");
            nodes.emplace("GAME.DAT", s_node{root / "GAME.DAT", "", "GAME.DAT", 0,
                static_cast<std::uint32_t>(game_sectors * kSector), false});
        }
    }
    else if (nodes.find("GAME.DAT") == nodes.end())
    {
        if (loose_game_dir.empty())
            throw std::runtime_error("GAME.DAT is missing and no loose GAME/ extraction is available");
        std::vector<std::uint8_t> decoded;
        const auto archive_entries = read_info_entries(nodes.at("INFO.DAT").path, decoded);
        std::uint64_t required_sectors = 0;
        for (const auto &asset : archive_entries)
        {
            required_sectors = std::max(required_sectors,
                static_cast<std::uint64_t>(asset.sector_offset) + asset.sector_count);
        }
        if (!required_sectors || required_sectors > std::numeric_limits<std::uint32_t>::max() / kSector)
            throw std::runtime_error("cannot infer a valid virtual GAME.DAT size from INFO.DAT");
        nodes.emplace("GAME.DAT", s_node{root / "GAME.DAT", "", "GAME.DAT", 0,
            static_cast<std::uint32_t>(required_sectors * kSector), false});
    }

    std::vector<std::string> directories;
    std::map<std::string, std::uint16_t> ordinals;
    std::uint32_t path_bytes = 0;
    for (auto &[key, node] : nodes)
    {
        if (!node.directory) continue;
        if (directories.size() >= 65535) throw std::runtime_error("too many extracted directories");
        ordinals[key] = static_cast<std::uint16_t>(directories.size() + 1);
        directories.push_back(key);
        path_bytes += static_cast<std::uint32_t>(8 + ((key.empty() ? 1 : node.name.size()) + 1) / 2 * 2);
        node.size = static_cast<std::uint32_t>(directory(key, nodes).size());
    }
    const auto path_sectors = static_cast<std::uint32_t>(sectors(path_bytes));
    std::uint64_t next = 18ull + static_cast<std::uint64_t>(path_sectors) * 2;
    if (next > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("generated ISO path tables exceed the sector limit");
    for (const auto &key : directories)
    {
        auto &node = nodes.at(key);
        node.lba = static_cast<std::uint32_t>(next);
        next += sectors(node.size);
        if (next > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("generated ISO directories exceed the sector limit");
    }
    next = std::max<std::uint64_t>(next, 32);

    const auto assign_file = [&](const std::string &key)
    {
        auto &node = nodes.at(key);
        node.lba = static_cast<std::uint32_t>(next);
        const auto allocated = std::max<std::uint64_t>(1, sectors(node.size));
        if (allocated > std::numeric_limits<std::uint32_t>::max() - next)
            throw std::runtime_error("generated ISO files exceed the sector limit");
        next += allocated;
        entries.push_back({node.lba, node.size, node.path.lexically_relative(root)});
    };

    // Place GAME.DAT last. The archive allocator can then append new sectors
    // linearly after its original extent without covering unrelated files.
    for (const auto &[key, node] : nodes)
        if (!node.directory && key != "GAME.DAT") assign_file(key);
    if (nodes.find("GAME.DAT") != nodes.end()) assign_file("GAME.DAT");

    for (const auto &key : directories)
    {
        const auto &node = nodes.at(key);
        folder.regions.push_back({static_cast<std::uint64_t>(node.lba) * kSector,
            node.size, {}, directory(key, nodes)});
    }
    for (bool big : {false, true})
    {
        std::vector<std::uint8_t> table(path_sectors * kSector, 0);
        std::size_t at = 0;
        for (const auto &key : directories)
        {
            const auto &node = nodes.at(key);
            const std::string name = key.empty() ? std::string(1, '\0') : node.name;
            table[at] = static_cast<std::uint8_t>(name.size());
            put32(table.data() + at + 2, node.lba, big);
            const auto parent = ordinals.at(node.parent);
            table[at + (big ? 7 : 6)] = static_cast<std::uint8_t>(parent);
            table[at + (big ? 6 : 7)] = static_cast<std::uint8_t>(parent >> 8);
            std::memcpy(table.data() + at + 8, name.data(), name.size());
            at += 8 + (name.size() + 1) / 2 * 2;
        }
        folder.regions.push_back({static_cast<std::uint64_t>(18 + (big ? path_sectors : 0)) * kSector,
            static_cast<std::uint64_t>(table.size()), {}, std::move(table)});
    }
    if (next > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("generated ISO volume exceeds the sector limit");
    std::vector<std::uint8_t> pvd(kSector, 0), terminator(kSector, 0);
    pvd[0] = 1; terminator[0] = 255;
    std::memcpy(pvd.data() + 1, "CD001", 5);
    std::memcpy(terminator.data() + 1, "CD001", 5);
    pvd[6] = terminator[6] = 1;
    std::memset(pvd.data() + 8, ' ', 64);
    std::memcpy(pvd.data() + 8, "PLAYSTATION", 11);
    std::memcpy(pvd.data() + 40, "PS2_FOLDER", 10);
    both32(pvd.data() + 80, static_cast<std::uint32_t>(next));
    both16(pvd.data() + 120, 1); both16(pvd.data() + 124, 1); both16(pvd.data() + 128, kSector);
    both32(pvd.data() + 132, path_bytes);
    put32(pvd.data() + 140, 18);
    put32(pvd.data() + 148, 18 + path_sectors, true);
    for (unsigned date : {813u, 830u, 847u, 864u}) std::memset(pvd.data() + date, '0', 16);
    pvd[881] = 1;
    const auto root_record = record(nodes.at(""), std::string(1, '\0'));
    std::memcpy(pvd.data() + 156, root_record.data(), root_record.size());
    folder.regions.push_back({16 * kSector, kSector, {}, std::move(pvd)});
    folder.regions.push_back({17 * kSector, kSector, {}, std::move(terminator)});
    folder.size = next * kSector;
    std::fprintf(stderr, "[disc] synthesized ISO directory and file extents in memory; GAME.DAT is resolved by filename\n");
}

void build_disc_layout(const fs::path &root, s_folder &folder)
{
    std::vector<s_layout_entry> entries;
    synthetic_layout(root, folder, entries);
    const auto canonical_root = fs::canonical(root);

    const auto *info_entry = find_layout_entry(entries, "INFO.DAT");
    const auto *game_entry = find_layout_entry(entries, "GAME.DAT");
    const auto loose_game = find_directory(root, "GAME");
    if (!loose_game.empty() && (!info_entry || !game_entry)
        && !g_virtual_archive.active && !native_arkd_selected())
        throw std::runtime_error("loose GAME/ overrides require INFO.DAT and GAME.DAT metadata");
    const bool loose_game_active =
        !native_arkd_selected() && info_entry && game_entry && !loose_game.empty();
    bool game_dat_exists = false;
    bool expanded_info_extent = false;
    std::uint32_t expanded_info_lba = 0;
    std::uint32_t expanded_info_size = 0;
    std::uint32_t expanded_game_size = 0;
    std::uint32_t expanded_volume_sectors = 0;

    if (loose_game_active)
    {
        const auto info_path = root / info_entry->relative_path;
        std::error_code info_error;
        const bool info_file_exists = fs::exists(info_path, info_error);
        if (info_error)
        {
            throw std::runtime_error("cannot inspect INFO.DAT: " + info_path.string());
        }
        fs::path canonical_info_path;
        if (info_file_exists)
        {
            canonical_info_path = fs::canonical(info_path);
            if (!is_inside(canonical_root, canonical_info_path) || !fs::is_regular_file(canonical_info_path)
                || fs::file_size(canonical_info_path) != info_entry->size)
            {
                throw std::runtime_error("INFO.DAT does not match the preserved disc layout");
            }
        }
        const auto loose_root = fs::canonical(loose_game);
        if (!is_inside(canonical_root, loose_root) || !fs::is_directory(loose_root))
        {
            throw std::runtime_error("GAME directory resolves outside the disc folder");
        }

        const auto game_path = root / game_entry->relative_path;
        std::error_code game_error;
        const bool game_path_exists = fs::exists(game_path, game_error);
        if (game_error)
        {
            throw std::runtime_error("cannot inspect GAME.DAT: " + game_path.string());
        }
        if (game_path_exists && !fs::is_regular_file(game_path))
        {
            throw std::runtime_error("GAME.DAT exists but is not a regular file: " + game_path.string());
        }
        game_dat_exists = game_path_exists;
        if (game_dat_exists)
        {
            const auto canonical_game_dat = fs::canonical(game_path);
            if (!is_inside(canonical_root, canonical_game_dat) || fs::file_size(canonical_game_dat) != game_entry->size)
            {
                throw std::runtime_error("GAME.DAT does not match the preserved disc layout");
            }
        }

        if (!info_file_exists)
        {
            std::fprintf(stderr, "[disc] INFO.DAT is virtual; host-INFO overlay pass skipped\n");
        }
        else
        {
        std::vector<std::uint8_t> decoded_info;
        std::vector<s_archive_entry> archive_entries = read_info_entries(canonical_info_path, decoded_info);
        const std::uint64_t original_game_sector_count = sectors(game_entry->size);
        const std::uint64_t original_disc_sector_count = folder.size / kSector;
        std::uint64_t next_appended_lba = original_disc_sector_count;
        std::uint64_t required_game_sectors = original_game_sector_count;

        // Reserve every original archive extent. Prefer unindexed space inside
        // GAME.DAT, then append new sectors beyond the original ISO volume.
        std::vector<std::pair<std::uint64_t, std::uint64_t>> occupied;
        occupied.reserve(archive_entries.size());
        for (const auto &asset : archive_entries)
        {
            const std::uint64_t extent_end = static_cast<std::uint64_t>(asset.sector_offset) + asset.sector_count;
            if (extent_end > original_game_sector_count)
            {
                throw std::runtime_error("INFO.DAT asset extent exceeds GAME.DAT: " + asset.name);
            }
            occupied.emplace_back(asset.sector_offset, extent_end);
        }
        std::sort(occupied.begin(), occupied.end());

        const auto allocate_sectors = [&](std::uint64_t needed, const std::string &asset_name) -> std::uint64_t
        {
            std::uint64_t candidate = 0;
            for (const auto &extent : occupied)
            {
                if (extent.first > candidate && extent.first - candidate >= needed) break;
                candidate = std::max(candidate, extent.second);
            }
            if (candidate <= original_game_sector_count
                && needed <= original_game_sector_count - candidate)
            {
                occupied.emplace_back(candidate, candidate + needed);
                std::sort(occupied.begin(), occupied.end());
                return candidate;
            }

            // New extents begin at the old virtual disc end. Since the preserved
            // layout covers every original physical region, this range cannot
            // overlap an existing disc file. Relative offsets remain INFO.DAT's
            // 32-bit sector offsets, and the ISO file-size field is 32-bit.
            const std::uint64_t absolute_lba = next_appended_lba;
            if (absolute_lba < game_entry->lba || absolute_lba - game_entry->lba > UINT32_MAX
                || needed > UINT32_MAX - (absolute_lba - game_entry->lba))
            {
                throw std::runtime_error("virtual GAME.DAT sector offset limit exceeded for " + asset_name);
            }
            if (needed > UINT32_MAX - absolute_lba)
                throw std::runtime_error("ISO volume sector limit exceeded while extending GAME.DAT for " + asset_name);
            const std::uint64_t relative_lba = absolute_lba - game_entry->lba;
            next_appended_lba += needed;
            required_game_sectors = std::max(required_game_sectors, relative_lba + needed);
            occupied.emplace_back(relative_lba, relative_lba + needed);
            std::sort(occupied.begin(), occupied.end());
            return relative_lba;
        };

        std::size_t mapped_count = 0;
        std::size_t missing_count = 0;
        std::size_t resized_count = 0;
        std::size_t added_count = 0;
        std::set<std::string> indexed_names;
        for (const auto &asset : archive_entries)
        {
            indexed_names.insert(archive_name_key(asset.name));
        }
        for (std::size_t index = 0; index < archive_entries.size(); ++index)
        {
            auto &asset = archive_entries[index];
            const auto asset_path = loose_root / asset.relative_path;
            std::error_code asset_error;
            const bool exists = fs::exists(asset_path, asset_error);
            if (asset_error)
            {
                throw std::runtime_error("cannot inspect loose asset: " + asset_path.string());
            }
            if (!exists)
            {
                ++missing_count;
                continue;
            }
            if (!fs::is_regular_file(asset_path))
            {
                throw std::runtime_error("loose asset is not a regular file: " + asset.name);
            }

            const auto canonical_asset = fs::canonical(asset_path);
            if (!is_inside(loose_root, canonical_asset))
            {
                throw std::runtime_error("loose asset resolves outside GAME/: " + asset.name);
            }
            const std::uint64_t replacement_size = fs::file_size(canonical_asset);
            if (!replacement_size || replacement_size > UINT32_MAX)
            {
                throw std::runtime_error("loose asset has an unsupported size: " + asset.name);
            }
            const std::uint64_t replacement_sectors = sectors(replacement_size);
            if (replacement_sectors > UINT32_MAX)
            {
                throw std::runtime_error("loose asset requires too many sectors: " + asset.name);
            }

            if (replacement_size != asset.byte_size)
            {
                if (replacement_sectors > asset.sector_count)
                {
                    asset.sector_offset = static_cast<std::uint32_t>(allocate_sectors(replacement_sectors, asset.name));
                }
                asset.sector_count = static_cast<std::uint32_t>(replacement_sectors);
                asset.byte_size = static_cast<std::uint32_t>(replacement_size);
                auto *record = decoded_info.data() + 0x30 + index * 0x30;
                write_u32_le(record + 0x24, asset.sector_offset);
                write_u32_le(record + 0x28, asset.sector_count);
                write_u32_le(record + 0x2C, asset.byte_size);
                ++resized_count;
            }

            const std::uint64_t asset_lba = game_entry->lba + asset.sector_offset;
            folder.overlays.push_back({asset_lba * kSector, replacement_size, canonical_asset, {}});
            ++mapped_count;
        }

        if (!game_dat_exists && missing_count)
        {
            throw std::runtime_error("GAME.DAT is absent and GAME/ is incomplete: "
                + std::to_string(missing_count) + " of " + std::to_string(archive_entries.size()) + " assets are missing");
        }
        if (!game_dat_exists && mapped_count != archive_entries.size())
        {
            throw std::runtime_error("GAME.DAT is absent and no complete loose GAME/ extraction was found");
        }

        // New files below GAME/ are appended to INFO.DAT in memory. Original
        // entries and their extents remain untouched; the allocator uses only
        // unindexed GAME.DAT sectors or the existing appended-volume path.
        std::set<std::string> added_names;
        std::error_code scan_error;
        for (fs::recursive_directory_iterator it(loose_root, scan_error), end;
            !scan_error && it != end; it.increment(scan_error))
        {
            if (it->is_symlink())
            {
                throw std::runtime_error("symlinks are not supported under GAME/: " + it->path().string());
            }
            if (it->is_directory()) continue;
            if (!it->is_regular_file()) continue;

            const auto relative_path = it->path().lexically_relative(loose_root);
            const std::string encoded_name = archive_name_from_path(relative_path);
            const std::string name_key = archive_name_key(encoded_name);
            if (indexed_names.find(name_key) != indexed_names.end()) continue;
            if (!added_names.insert(name_key).second)
            {
                throw std::runtime_error("case-insensitive filename collision in GAME/: " + relative_path.string());
            }

            const auto canonical_asset = fs::canonical(it->path());
            if (!is_inside(loose_root, canonical_asset) || !fs::is_regular_file(canonical_asset))
            {
                throw std::runtime_error("new GAME asset resolves outside GAME/: " + relative_path.string());
            }
            const std::uint64_t replacement_size = fs::file_size(canonical_asset);
            if (!replacement_size || replacement_size > UINT32_MAX)
            {
                throw std::runtime_error("new GAME asset has an unsupported size: " + relative_path.string());
            }
            const std::uint64_t replacement_sectors = sectors(replacement_size);
            if (!replacement_sectors || replacement_sectors > UINT32_MAX)
            {
                throw std::runtime_error("new GAME asset requires an unsupported sector count: " + relative_path.string());
            }
            if (archive_entries.size() >= UINT32_MAX)
            {
                throw std::runtime_error("INFO.DAT asset record count limit exceeded");
            }

            s_archive_entry asset;
            asset.name = encoded_name;
            asset.relative_path = relative_path;
            asset.sector_offset = static_cast<std::uint32_t>(allocate_sectors(replacement_sectors, asset.name));
            asset.sector_count = static_cast<std::uint32_t>(replacement_sectors);
            asset.byte_size = static_cast<std::uint32_t>(replacement_size);
            append_info_record(decoded_info, archive_entries.size(), asset, archive_entries);
            archive_entries.push_back(asset);
            write_u32_le(decoded_info.data() + 0x2C, static_cast<std::uint32_t>(archive_entries.size()));

            const std::uint64_t asset_lba = game_entry->lba + asset.sector_offset;
            folder.overlays.push_back({asset_lba * kSector, replacement_size, canonical_asset, {}});
            ++mapped_count;
            ++added_count;
        }
        if (scan_error)
        {
            throw std::runtime_error("cannot enumerate loose GAME directory: " + scan_error.message());
        }

        if (resized_count || added_count)
        {
            if (added_count)
            {
                // ARKD_DVD.IRX constructs its 26 filename buckets from
                // contiguous first-letter groups. Appending arbitrary loose
                // filenames after the existing table can hide them from
                // lookup. Reorder only INFO records; sector assignments and
                // the existing GAME.DAT payload layout remain unchanged.
                std::vector<std::size_t> order(archive_entries.size());
                for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
                std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b)
                {
                    return archive_name_key(archive_entries[a].name)
                        < archive_name_key(archive_entries[b].name);
                });
                std::vector<std::uint8_t> records(order.size() * 0x30);
                std::vector<s_archive_entry> sorted_entries;
                sorted_entries.reserve(order.size());
                for (std::size_t i = 0; i < order.size(); ++i)
                {
                    std::memcpy(records.data() + i * 0x30,
                        decoded_info.data() + 0x30 + order[i] * 0x30, 0x30);
                    sorted_entries.push_back(std::move(archive_entries[order[i]]));
                }
                std::memcpy(decoded_info.data() + 0x30, records.data(), records.size());
                archive_entries = std::move(sorted_entries);
            }
            if (archive_entries.size() >= 2047)
                std::fprintf(stderr,
                    "[disc] warning: INFO has %zu records; ARKD_DVD.IRX rejects "
                    "entry_count >= file=2047\n", archive_entries.size());
            else if (archive_entries.size() >= 2005)
                std::fprintf(stderr,
                    "[disc] warning: INFO has %zu records; ARKD_DVD.IRX may only read "
                    "47 table sectors with file=2047 (record 2005 not yet verified)\n",
                    archive_entries.size());
            (void)parse_info_entries(decoded_info);
            auto encoded_info = encode_info(decoded_info);
            if (encoded_info.size() > UINT32_MAX)
            {
                throw std::runtime_error("expanded INFO.DAT exceeds the ISO 9660 32-bit file-size limit");
            }
            const std::uint64_t info_sectors = sectors(encoded_info.size());
            const std::uint64_t original_info_sectors = sectors(info_entry->size);
            expanded_info_lba = static_cast<std::uint32_t>(info_entry->lba);
            expanded_info_size = static_cast<std::uint32_t>(encoded_info.size());
            if (info_sectors > original_info_sectors)
            {
                if (next_appended_lba > UINT32_MAX || info_sectors > UINT32_MAX - next_appended_lba)
                {
                    throw std::runtime_error("ISO volume sector limit exceeded while relocating expanded INFO.DAT");
                }
                expanded_info_lba = static_cast<std::uint32_t>(next_appended_lba);
                next_appended_lba += info_sectors;
            }
            expanded_info_extent = expanded_info_lba != info_entry->lba || expanded_info_size != info_entry->size;
            folder.overlays.push_back({static_cast<std::uint64_t>(expanded_info_lba) * kSector,
                encoded_info.size(), {}, std::move(encoded_info)});
        }

        if (required_game_sectors > original_game_sector_count)
        {
            const std::uint64_t expanded_size = required_game_sectors * kSector;
            if (expanded_size > UINT32_MAX)
                throw std::runtime_error("expanded GAME.DAT exceeds the ISO 9660 32-bit file-size limit");
            expanded_game_size = static_cast<std::uint32_t>(expanded_size);
            std::fprintf(stderr, "[disc] virtual GAME.DAT expanded from %llu to %llu bytes (%llu appended sectors)\n",
                static_cast<unsigned long long>(game_entry->size), static_cast<unsigned long long>(expanded_size),
                static_cast<unsigned long long>(next_appended_lba - original_disc_sector_count));
        }
        if (next_appended_lba > original_disc_sector_count)
        {
            if (next_appended_lba > UINT32_MAX)
                throw std::runtime_error("expanded ISO volume exceeds the ISO 9660 32-bit sector limit");
            folder.size = next_appended_lba * kSector;
            expanded_volume_sectors = static_cast<std::uint32_t>(next_appended_lba);
        }

        std::fprintf(stderr, "[disc] loose GAME assets: %zu indexed/mapped; %zu added; %zu missing; %zu resized (%s)\n",
            mapped_count, added_count, missing_count, resized_count,
            game_dat_exists ? "GAME.DAT fallback" : "zero-filled");
        if (mapped_count)
        {
            // Confirm overlay paths are non-empty absolute host files.
            std::size_t empty_paths = 0, checked = 0;
            for (const auto &ov : folder.overlays)
            {
                if (!ov.memory.empty()) continue;
                if (ov.path.empty()) { ++empty_paths; continue; }
                if (checked < 3)
                {
#ifdef _WIN32
                    std::fwprintf(stderr, L"[disc] overlay sample: %ls (%llu bytes)\n",
                        ov.path.c_str(), static_cast<unsigned long long>(ov.size));
#else
                    std::fprintf(stderr, "[disc] overlay sample: %s (%llu bytes)\n",
                        ov.path.generic_string().c_str(), static_cast<unsigned long long>(ov.size));
#endif
                    ++checked;
                }
            }
            if (empty_paths)
                std::fprintf(stderr, "[disc] warning: %zu overlays have empty paths\n", empty_paths);
        }
        } // host INFO overlay
    }

    for (const auto &entry : entries)
    {
        const auto path = root / entry.relative_path;
        std::error_code error;
        const bool exists = fs::exists(path, error);
        if (error)
        {
            throw std::runtime_error("cannot inspect extracted file: " + entry.relative_path.string());
        }
        const auto entry_name = upper(entry.relative_path.filename().string());

        // Virtual INFO.DAT rebuilt from GAME/ (memory-backed, no host file).
        if (!exists && entry_name == "INFO.DAT" && g_virtual_archive.active)
        {
            folder.regions.push_back({entry.lba * kSector, g_virtual_archive.encoded_info.size(),
                {}, g_virtual_archive.encoded_info});
            // Map every GAME/ asset onto the virtual GAME.DAT extent.
            if (game_entry)
            {
                for (const auto &asset : g_virtual_archive.assets)
                {
                    folder.overlays.push_back({
                        (game_entry->lba + asset.sector_offset) * kSector,
                        asset.byte_size,
                        fs::canonical(g_virtual_archive.loose_root / asset.relative_path),
                        {}
                    });
                }
            }
            continue;
        }
        // Virtual GAME.DAT: zero-filled base; overlays supply asset bytes.
        if (!exists && entry_name == "GAME.DAT")
        {
            folder.regions.push_back({entry.lba * kSector, entry.size, {}, {}});
            continue;
        }
        if (!exists || !fs::is_regular_file(path))
        {
            throw std::runtime_error("missing or non-file extracted path: " + entry.relative_path.string());
        }

        const auto canonical_path = fs::canonical(path);
        if (!is_inside(canonical_root, canonical_path) || !fs::is_regular_file(canonical_path)
            || fs::file_size(canonical_path) != entry.size)
        {
            throw std::runtime_error("missing, modified-size or outside-root extracted file: " + entry.relative_path.string()
                + " (expected " + std::to_string(entry.size) + ", actual "
                + (fs::exists(canonical_path) ? std::to_string(fs::file_size(canonical_path)) : "missing")
                + ", inside=" + (is_inside(canonical_root, canonical_path) ? "yes" : "no") + ")");
        }
        if (entry.size) folder.regions.push_back({entry.lba * kSector, entry.size, canonical_path, {}});
    }

    if (expanded_game_size || expanded_info_extent || expanded_volume_sectors)
    {
        std::sort(folder.regions.begin(), folder.regions.end(),
            [](const auto &a, const auto &b) { return a.offset < b.offset; });
        std::sort(folder.overlays.begin(), folder.overlays.end(),
            [](const auto &a, const auto &b) { return a.offset < b.offset; });
        if (expanded_game_size)
        {
            patch_iso_file_size(folder, static_cast<std::uint32_t>(game_entry->lba),
                static_cast<std::uint32_t>(game_entry->size), expanded_game_size);
        }
        if (expanded_info_extent)
        {
            patch_iso_file_extent(folder, "INFO.DAT", static_cast<std::uint32_t>(info_entry->lba),
                static_cast<std::uint32_t>(info_entry->size), expanded_info_lba, expanded_info_size);
        }
        if (expanded_volume_sectors)
        {
            std::vector<std::uint8_t> pvd_patch(8);
            both32(pvd_patch.data(), expanded_volume_sectors);
            folder.overlays.push_back({16ull * kSector + 80, pvd_patch.size(), {}, std::move(pvd_patch)});
        }
    }

    if (folder.regions.empty() && folder.overlays.empty()) throw std::runtime_error("disc layout has no extents");
    std::fprintf(stderr, "[disc] folder preserves original file LBAs; virtual ISO metadata overlays are active when required\n");
}
}

std::vector<s_direct_game_asset> enumerate_direct_game(const fs::path &root)
{
    const fs::path loose = find_directory(root, "GAME");
    if (loose.empty()) return {};
    const fs::path canonical_root = fs::canonical(root);
    const fs::path canonical_game = fs::canonical(loose);
    if (!is_inside(canonical_root, canonical_game))
        throw std::runtime_error("GAME/ escapes disc root");

    std::vector<s_direct_game_asset> assets;
    std::set<std::string> seen;
    std::error_code error;
    for (fs::recursive_directory_iterator it(canonical_game, error), end;
        !error && it != end; it.increment(error))
    {
        if (it->is_symlink() || !it->is_regular_file()) continue;
        const fs::path canonical_file = fs::canonical(it->path());
        if (!is_inside(canonical_game, canonical_file))
            throw std::runtime_error("native GAME asset escapes its directory");
        const std::uint64_t size = fs::file_size(canonical_file);
        if (!size || size > UINT32_MAX)
            throw std::runtime_error("native GAME asset has invalid size");
        const fs::path relative = it->path().lexically_relative(canonical_game);
        const std::string name = archive_name_from_path(relative, 4096);
        if (!seen.insert(archive_name_key(name)).second)
            throw std::runtime_error("duplicate case-insensitive native GAME asset");
        assets.push_back({name, canonical_file, static_cast<std::uint32_t>(size)});
    }
    if (error)
        throw std::runtime_error("cannot enumerate native GAME assets: " + error.message());
    // Native savestates refer to these assets by ID. Filesystem directory
    // iteration order is not stable, especially across Windows/Linux.
    // Give each name a deterministic ID before exposing it to the IOP.
    std::sort(assets.begin(), assets.end(),
        [](const s_direct_game_asset &left,
            const s_direct_game_asset &right)
        {
            return archive_name_key(left.name) < archive_name_key(right.name);
        });
    std::fprintf(stderr,
        "[disc] native GAME direct index: %zu assets, no INFO record cap\n",
        assets.size());
    return assets;
}

// Native replacements are a separate namespace. They are not visible in
// the virtual ISO or ARKD's guest-facing GAME/ file catalog.
std::vector<s_native_asset> enumerate_native_assets(const fs::path &root)
{
    const fs::path native = find_directory(root, "NATIVE");
    if (native.empty()) return {};
    const fs::path canonical_root = fs::canonical(root);
    const fs::path canonical_native = fs::canonical(native);
    if (!is_inside(canonical_root, canonical_native))
        throw std::runtime_error("NATIVE/ escapes disc root");

    std::vector<s_native_asset> assets;
    std::set<std::string> names;
    std::error_code error;
    for (fs::recursive_directory_iterator it(canonical_native, error), end;
        !error && it != end; it.increment(error))
    {
        if (it->is_symlink())
        {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        const fs::path canonical_file = fs::canonical(it->path());
        if (!is_inside(canonical_native, canonical_file))
            throw std::runtime_error("native replacement escapes NATIVE/");
        const auto size = fs::file_size(canonical_file);
        constexpr std::uint64_t k_max_native_file = 64ull * 1024 * 1024;
        if (!size || size > k_max_native_file)
            throw std::runtime_error("native replacement is empty or exceeds 64 MiB");

        const fs::path relative = it->path().lexically_relative(canonical_native);
        const std::string full_name = archive_name_from_path(relative, 4096);
        const auto dot = full_name.find_last_of('.');
        if (dot == std::string::npos) continue;
        const std::string format = archive_name_key(full_name.substr(dot + 1));
        if (format != "PNG" && format != "WAV") continue;
        const std::string name = full_name.substr(0, dot);
        if (name.empty()) continue;
        const std::string key = archive_name_key(name) + "." + format;
        if (!names.insert(key).second)
            throw std::runtime_error("duplicate case-insensitive NATIVE/ replacement");
        assets.push_back({name, format, canonical_file,
            static_cast<std::uint32_t>(size)});
    }
    if (error)
        throw std::runtime_error("cannot enumerate NATIVE/: " + error.message());
    std::sort(assets.begin(), assets.end(),
        [](const s_native_asset &a, const s_native_asset &b)
        {
            const auto left = archive_name_key(a.name) + "." + a.format;
            const auto right = archive_name_key(b.name) + "." + b.format;
            return left < right;
        });
    return assets;
}

bool open_folder(const fs::path &root)
{
    try
    {
        auto folder = std::make_shared<s_folder>();
        build_disc_layout(root, *folder);
        folder->validate();
        std::uint8_t pvd[kSector] = {};
        if (folder->read(16 * kSector, pvd, kSector) != kSector || std::memcmp(pvd, "\x01" "CD001", 6) != 0)
            throw std::runtime_error("folder metadata has no valid ISO 9660 primary descriptor");
        const bool native = native_arkd_selected();
        std::vector<s_direct_game_asset> direct_assets;
        std::vector<s_native_asset> replacement_assets;
        if (native)
        {
            direct_assets = enumerate_direct_game(root);
            replacement_assets = enumerate_native_assets(root);
        }

        const bool mounted = open(root.string(), folder->size,
            [folder](std::uint64_t offset, void *out, std::size_t bytes)
            {
                return folder->read(offset, out, bytes);
            });
        if (mounted && native)
        {
            g_direct_game_assets = std::move(direct_assets);
            g_native_assets = std::move(replacement_assets);
            const fs::path native_dir = find_directory(root, "NATIVE");
            if (!native_dir.empty())
                g_native_asset_root = fs::canonical(native_dir);
            const fs::path loose = find_directory(root, "GAME");
            if (!loose.empty()) g_direct_game_root = fs::canonical(loose);
        }
        return mounted;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "[disc] cannot mount folder %s: %s\n", root.string().c_str(), error.what());
        return false;
    }
}

std::vector<LooseGameAsset> loose_game_assets()
{
    std::vector<LooseGameAsset> assets;
    assets.reserve(g_direct_game_assets.size());
    for (std::size_t i = 0; i < g_direct_game_assets.size(); ++i)
    {
        const auto &asset = g_direct_game_assets[i];
        assets.push_back({static_cast<std::uint32_t>(i),
            asset.size, asset.name});
    }
    return assets;
}

std::size_t read_loose_game_asset(std::uint32_t id,
    std::uint64_t offset, void *destination, std::size_t capacity)
{
    if (!destination || id >= g_direct_game_assets.size() || !capacity)
        return 0;
    const auto &asset = g_direct_game_assets[id];
    if (offset >= asset.size) return 0;

    std::error_code error;
    const auto canonical = fs::canonical(asset.path, error);
    if (error || !is_inside(g_direct_game_root, canonical)
        || !fs::is_regular_file(canonical, error) || error)
        return 0;
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(capacity, asset.size - offset));
    std::ifstream input(canonical, std::ios::binary);
    if (!input) return 0;
    input.seekg(static_cast<std::streamoff>(offset));
    if (!input) return 0;
    input.read(static_cast<char *>(destination),
        static_cast<std::streamsize>(count));
    return static_cast<std::size_t>(input.gcount());
}

std::vector<NativeAsset> native_assets()
{
    std::vector<NativeAsset> assets;
    assets.reserve(g_native_assets.size());
    for (std::size_t i = 0; i < g_native_assets.size(); ++i)
    {
        const auto &asset = g_native_assets[i];
        assets.push_back({static_cast<std::uint32_t>(i), asset.size,
            asset.name, asset.format});
    }
    return assets;
}

std::optional<NativeAsset> find_native_asset(
    const std::string &game_path, const std::string &format)
{
    if (format != "png" && format != "PNG"
        && format != "wav" && format != "WAV") return std::nullopt;
    std::string key;
    try
    {
        // Reject absolute paths, traversal and drive prefixes.
        key = archive_name_key(archive_path(game_path).generic_string());
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
    const std::string requested_format = archive_name_key(format);
    for (std::size_t i = 0; i < g_native_assets.size(); ++i)
    {
        const auto &asset = g_native_assets[i];
        if (asset.format == requested_format
            && archive_name_key(asset.name) == key)
            return NativeAsset{static_cast<std::uint32_t>(i), asset.size,
                asset.name, asset.format};
    }
    return std::nullopt;
}

std::size_t read_native_asset(std::uint32_t id,
    std::uint64_t offset, void *destination, std::size_t capacity)
{
    if (!destination || id >= g_native_assets.size() || !capacity) return 0;
    const auto &asset = g_native_assets[id];
    if (offset >= asset.size) return 0;
    std::error_code error;
    const auto canonical = fs::canonical(asset.path, error);
    if (error || !is_inside(g_native_asset_root, canonical)
        || !fs::is_regular_file(canonical, error) || error)
        return 0;
    // If an asset is changed while running, never return bytes past
    // the size admitted when the disc was mounted.
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(capacity, asset.size - offset));
    std::ifstream input(canonical, std::ios::binary);
    if (!input) return 0;
    input.seekg(static_cast<std::streamoff>(offset));
    if (!input) return 0;
    input.read(static_cast<char *>(destination),
        static_cast<std::streamsize>(count));
    return static_cast<std::size_t>(input.gcount());
}

void clear_loose_game_assets()
{
    g_native_assets.clear();
    g_native_asset_root.clear();
    g_direct_game_assets.clear();
    g_direct_game_root.clear();
}
}
