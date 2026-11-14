#include "native_texture_residency.hpp"

#include "ps2/disc.hpp"
#include "ps2/resource_pack.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ps2::gs::native_texture
{
namespace
{
constexpr std::uint32_t k_gs_blocks = (4u << 20) / 256u;
constexpr std::size_t k_max_registry_entries = 65535;
constexpr std::uint32_t k_max_native_side = 1024;

struct s_entry
{
    std::uint64_t fingerprint = 0;
    std::uint32_t psm = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string path;
    s_image image;
    bool attempted = false;
};

struct s_key
{
    std::uint64_t fingerprint;
    std::uint32_t width, height, psm;

    bool operator==(const s_key &other) const
    {
        return fingerprint == other.fingerprint && width == other.width
            && height == other.height && psm == other.psm;
    }
};

struct s_key_hash
{
    std::size_t operator()(const s_key &key) const
    {
        const std::uint64_t mixed = key.fingerprint
            ^ (static_cast<std::uint64_t>(key.width) << 32)
            ^ (static_cast<std::uint64_t>(key.height) << 16) ^ key.psm;
        return static_cast<std::size_t>(mixed ^ (mixed >> 32));
    }
};

struct s_resident
{
    std::uint32_t bp = 0;
    std::uint32_t end_bp = 0;
    std::uint32_t bw = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t psm = 0;
    std::uint64_t fingerprint = 0;
    // Named uploads originate from a loader/decode hook rather than a
    // fingerprint. Their source identity remains stable across reloads.
    std::string logical_name;
    bool named = false;
    bool valid = false;
    // Original CLUT address is part of indexed texture identity.
    // UINT32_MAX is retained for legacy fingerprint entries.
    std::uint32_t clut_bp = 0xffffffffu;
    // Verified palette-only TEX2 switches are still the *same* named image.
    // Keep both old and new CLUT addresses, as GS contexts can alternate.
    std::array<std::uint32_t, 16> clut_aliases{};
    std::uint32_t clut_alias_count = 0;
};

std::vector<s_entry> g_entries;
std::unordered_map<s_key, int, s_key_hash> g_lookup;
// Fingerprints are version-specific, but a logical filename and original
// layout identify the same replacement across different game releases.
struct s_name_key
{
    std::string path;
    std::uint32_t width, height, psm;

    bool operator==(const s_name_key &other) const
    {
        return path == other.path && width == other.width
            && height == other.height && psm == other.psm;
    }
};

struct s_name_key_hash
{
    std::size_t operator()(const s_name_key &key) const
    {
        const std::size_t a = std::hash<std::string>{}(key.path);
        const std::size_t b = static_cast<std::size_t>(key.width)
            ^ (static_cast<std::size_t>(key.height) << 16) ^ key.psm;
        return a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2));
    }
};
std::unordered_map<s_name_key, int, s_name_key_hash> g_by_name;
std::array<s_resident, 256> g_residents{};
// Direct GS base-pointer index avoids a 256-slot search per textured draw.
std::array<int, k_gs_blocks> g_resident_by_bp = []()
{
    std::array<int, k_gs_blocks> result{};
    result.fill(-1);
    return result;
}();
std::size_t g_next_resident = 0;
bool g_registry_loaded = false;

bool accepts_clut(const s_resident &resident, std::uint32_t clut_bp)
{
    if (resident.clut_bp == clut_bp)
    {
        return true;
    }
    return std::find(resident.clut_aliases.begin(),
        resident.clut_aliases.begin() + resident.clut_alias_count,
        clut_bp) != resident.clut_aliases.begin() + resident.clut_alias_count;
}

bool remember_clut(s_resident &resident, std::uint32_t clut_bp)
{
    if (accepts_clut(resident, clut_bp)
        || resident.clut_alias_count >= resident.clut_aliases.size())
    {
        return false;
    }
    resident.clut_aliases[resident.clut_alias_count++] = clut_bp;
    return true;
}

bool intersects(std::uint32_t begin, std::uint32_t end,
    std::uint32_t other_begin, std::uint32_t other_end)
{
    return begin < other_end && other_begin < end;
}

// Actual PS2 GS 256-byte block layouts, shared with the tables generated
// by gs.cpp::build_tables(). Page-wide invalidation is too coarse for
// 16x16 CLUT uploads: a 1 KiB palette can share an 8 KiB page with a
// different image without overwriting any of its pixel blocks.
constexpr std::uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
constexpr std::uint8_t kBlockZ32[4][8] = {{24, 25, 28, 29, 8, 9, 12, 13}, {26, 27, 30, 31, 10, 11, 14, 15}, {16, 17, 20, 21, 0, 1, 4, 5}, {18, 19, 22, 23, 2, 3, 6, 7}};
constexpr std::uint8_t kBlock16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
constexpr std::uint8_t kBlock16S[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
constexpr std::uint8_t kBlockZ16[8][4] = {{24, 26, 16, 18}, {25, 27, 17, 19}, {28, 30, 20, 22}, {29, 31, 21, 23}, {8, 10, 0, 2}, {9, 11, 1, 3}, {12, 14, 4, 6}, {13, 15, 5, 7}};
constexpr std::uint8_t kBlockZ16S[8][4] = {{24, 26, 8, 10}, {25, 27, 9, 11}, {16, 18, 0, 2}, {17, 19, 1, 3}, {28, 30, 12, 14}, {29, 31, 13, 15}, {20, 22, 4, 6}, {21, 23, 5, 7}};

using s_transfer_bits = std::array<std::uint64_t, k_gs_blocks / 64u>;

// Mark precisely the blocks touched by a host->local GS image transfer.
// False means unknown/unsafe layout; callers must conservatively assume
// that all GS memory was touched.
bool mark_transfer_blocks(std::uint64_t bitbltbuf,
    std::uint64_t trxreg, std::uint64_t trxpos,
    s_transfer_bits &bits, std::uint32_t &last_offset)
{
    const auto psm = static_cast<std::uint32_t>(bitbltbuf >> 56) & 0x3fu;
    const auto bw = static_cast<std::uint32_t>(bitbltbuf >> 48) & 0x3fu;
    const auto width = static_cast<std::uint32_t>(trxreg) & 0xfffu;
    const auto height = static_cast<std::uint32_t>(trxreg >> 32) & 0xfffu;
    if (!width || !height)
    {
        return true;
    }
    if (!bw)
    {
        return false;
    }

    std::uint32_t page_width = 64;
    std::uint32_t page_height = 32;
    std::uint32_t tile_width = 8;
    std::uint32_t tile_height = 8;
    std::uint32_t tile_columns = 8;
    const std::uint8_t *table = &kBlock32[0][0];

    switch (psm)
    {
    case 0x00: case 0x01: case 0x1b: case 0x24: case 0x2c:
        break;
    case 0x30: case 0x31:
        table = &kBlockZ32[0][0];
        break;
    case 0x02: case 0x0a: case 0x32: case 0x3a:
        page_height = 64;
        tile_width = 16;
        tile_columns = 4;
        switch (psm)
        {
        case 0x02: table = &kBlock16[0][0]; break;
        case 0x0a: table = &kBlock16S[0][0]; break;
        case 0x32: table = &kBlockZ16[0][0]; break;
        default:   table = &kBlockZ16S[0][0]; break;
        }
        break;
    case 0x13:
        page_width = 128;
        page_height = 64;
        tile_width = 16;
        tile_height = 16;
        break;
    case 0x14:
        page_width = 128;
        page_height = 128;
        tile_width = 32;
        tile_height = 16;
        tile_columns = 4;
        table = &kBlock16[0][0];
        break;
    default:
        return false;
    }

    const auto bp = static_cast<std::uint32_t>(bitbltbuf >> 32) & 0x3fffu;
    const auto dsax = static_cast<std::uint32_t>(trxpos >> 32) & 0x7ffu;
    const auto dsay = static_cast<std::uint32_t>(trxpos >> 48) & 0x7ffu;
    const std::uint32_t columns_per_page = page_width / tile_width;
    const std::uint32_t rows_per_page = page_height / tile_height;
    const std::uint32_t pages_per_row =
        std::max(1u, bw * 64u / page_width);
    const auto first_x = dsax / tile_width;
    const auto last_x = (dsax + width - 1) / tile_width;
    const auto first_y = dsay / tile_height;
    const auto last_y = (dsay + height - 1) / tile_height;

    const auto tiles = static_cast<std::uint64_t>(last_x - first_x + 1)
        * (last_y - first_y + 1);
    if (tiles > 4u * k_gs_blocks)
    {
        return false;
    }

    for (std::uint32_t y = first_y; y <= last_y; ++y)
    {
        const std::uint32_t page_y = y / rows_per_page;
        const std::uint32_t block_y = y % rows_per_page;
        for (std::uint32_t x = first_x; x <= last_x; ++x)
        {
            const std::uint32_t page_x = x / columns_per_page;
            const std::uint32_t block_x = x % columns_per_page;
            const std::uint32_t block =
                table[block_y * tile_columns + block_x];
            const std::uint64_t offset =
                (static_cast<std::uint64_t>(page_y) * pages_per_row
                    + page_x) * 32u + block;
            last_offset = std::max(last_offset, static_cast<std::uint32_t>(
                std::min<std::uint64_t>(k_gs_blocks, offset + 1)));
            // GS local VRAM wraps at 4 MiB. Track the wrapped physical
            // address, not only an enclosing page or byte-count estimate.
            const std::uint32_t physical = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(bp) + offset)
                    & (k_gs_blocks - 1u));
            bits[physical / 64u] |=
                std::uint64_t{1} << (physical % 64u);
        }
    }
    return true;
}

bool dirty_resident(const s_transfer_bits &bits,
    std::uint32_t begin, std::uint32_t end)
{
    for (std::uint32_t pos = begin; pos < end;)
    {
        const std::uint32_t word = pos / 64u;
        const std::uint32_t limit = std::min(end, (word + 1u) * 64u);
        const std::uint64_t low = ~std::uint64_t{0} << (pos % 64u);
        const std::uint64_t high = (limit % 64u) == 0
            ? ~std::uint64_t{0}
            : ((std::uint64_t{1} << (limit % 64u)) - 1u);
        if ((bits[word] & low & high) != 0)
        {
            return true;
        }
        pos = limit;
    }
    return false;
}

// Source residency still needs a compact extent for its GS BP index.
// Unlike the previous page-wide estimate, the final used block is derived
// from actual GS swizzle tiles (64x64 PSMT8 uses 16, not 32, blocks).
std::uint32_t transfer_blocks(std::uint64_t bitbltbuf,
    std::uint64_t trxreg, std::uint64_t trxpos = 0)
{
    s_transfer_bits ignored{};
    std::uint32_t last = 0;
    return mark_transfer_blocks(bitbltbuf, trxreg, trxpos, ignored, last)
        ? last : k_gs_blocks;
}

bool valid_path(const std::string &name)
{
    if (name.empty() || name.front() == '/' || name.find('\\') != std::string::npos
        || name.find(':') != std::string::npos)
        return false;
    std::size_t first = 0;
    for (std::size_t i = 0; i <= name.size(); ++i)
    {
        if (i != name.size() && name[i] != '/') continue;
        const auto part = name.substr(first, i - first);
        if (part.empty() || part == "." || part == "..") return false;
        first = i + 1;
    }
    return true;
}

bool parse_line(const std::string &line, s_entry &entry)
{
    // Registry v1: 16-digit FNV64 HEX, decimal PSM, width, height,
    // and logical PNG basename, separated by tabs.
    std::istringstream stream(line);
    std::string hash, psm, width, height, name;
    if (!std::getline(stream, hash, '\t')
        || !std::getline(stream, psm, '\t')
        || !std::getline(stream, width, '\t')
        || !std::getline(stream, height, '\t')
        || !std::getline(stream, name) || !valid_path(name)
        || hash.size() != 16)
        return false;
    try
    {
        std::size_t pos = 0;
        entry.fingerprint = std::stoull(hash, &pos, 16);
        if (pos != hash.size()) return false;
        entry.psm = static_cast<std::uint32_t>(std::stoul(psm, &pos, 10));
        if (pos != psm.size()) return false;
        entry.width = static_cast<std::uint32_t>(std::stoul(width, &pos, 10));
        if (pos != width.size()) return false;
        entry.height = static_cast<std::uint32_t>(std::stoul(height, &pos, 10));
        if (pos != height.size()) return false;
    }
    catch (const std::exception &)
    {
        return false;
    }
    if (entry.width == 0 || entry.height == 0
        || entry.width > k_max_native_side || entry.height > k_max_native_side
        || (entry.psm != 0x00 && entry.psm != 0x13 && entry.psm != 0x14))
        return false;
    entry.path = std::move(name);
    return true;
}

void load_registry()
{
    if (g_registry_loaded) return;
    g_registry_loaded = true;
    g_entries.clear();
    g_lookup.clear();
    g_by_name.clear();
    // Strict name-only mode never loads fingerprint files. Names are
    // supplied by the game's own texture constructor and handle binder.
    if (name_only_mode())
    {
        return;
    }
    for (const auto &source : resource_pack::texture_registries())
    {
        std::istringstream lines(source);
        std::string line;
        while (g_entries.size() < k_max_registry_entries
            && std::getline(lines, line))
        {
            if (line.empty() || line.front() == '#') continue;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            s_entry entry;
            if (!parse_line(line, entry)) continue;
            const s_key key{entry.fingerprint, entry.width,
                entry.height, entry.psm};
            const s_name_key name_key{entry.path, entry.width,
                entry.height, entry.psm};
            // Each unique source filename/layout owns one decoded image and
            // one logical GPU slot, regardless of the number of regional
            // upload fingerprints that identify it.
            auto name_it = g_by_name.find(name_key);
            int index = -1;
            if (name_it != g_by_name.end())
            {
                index = name_it->second;
            }
            else
            {
                index = static_cast<int>(g_entries.size());
                g_entries.push_back(std::move(entry));
                g_by_name.emplace(std::move(name_key), index);
            }
            const auto [at, inserted] = g_lookup.emplace(key, index);
            if (!inserted && at->second != index)
            {
                // Distinct names with an identical GS source signature are
                // ambiguous without archive/texture-upload context. Never
                // guess which replacement to use, irrespective of order.
                at->second = -1;
            }
        }
    }
    if (trace_native_textures())
        std::fprintf(stderr,
            "[gs-native] loaded %zu source texture identities\n",
            g_entries.size());
}

int find_identity(std::uint64_t hash, std::uint32_t psm,
    std::uint32_t width, std::uint32_t height)
{
    load_registry();
    const auto it = g_lookup.find({hash, width, height, psm});
    return it == g_lookup.end() ? -1 : it->second;
}
}

bool name_only_mode()
{
    static const bool enabled = []()
    {
        const char *value = std::getenv("PS2_SDBZ_NAME_ONLY");
        return value && *value && *value != '0';
    }();
    return enabled;
}

bool trace_native_textures()
{
    static const bool enabled = []()
    {
        const char *value = std::getenv("PS2_TRACE_NATIVE_TEXTURES");
        return value && *value && *value != '0';
    }();
    return enabled;
}

std::size_t registered_texture_count()
{
    load_registry();
    return g_entries.size();
}

std::string texture_path(int index)
{
    load_registry();
    return index >= 0 && index < static_cast<int>(g_entries.size())
        ? g_entries[static_cast<std::size_t>(index)].path : std::string{};
}

bool replacements_available()
{
    if (name_only_mode())
    {
        return false;
    }
    // Even disabled packs must not prevent identifying an original upload.
    // Its source fingerprint is independent of the replacement PNG.
    return registered_texture_count() != 0;
}

void reset_residency()
{
    g_residents = {};
    g_resident_by_bp.fill(-1);
    g_next_resident = 0;
    g_entries.clear();
    g_lookup.clear();
    g_by_name.clear();
    g_registry_loaded = false;
}

void reload_replacements()
{
    // Keep original GS identities while refreshing the registry and PNG cache.
    g_entries.clear();
    g_lookup.clear();
    g_by_name.clear();
    g_registry_loaded = false;
    load_registry();
}

// Named asset identities come from the loader, not from a source
// fingerprint or registry file. Intern once; use an integer at draw time.
int find_named_identity(const std::string &name, std::uint32_t psm,
    std::uint32_t width, std::uint32_t height)
{
    load_registry();
    if (!valid_path(name) || !width || !height
        || width > k_max_native_side || height > k_max_native_side
        || (psm != 0x00 && psm != 0x13 && psm != 0x14))
    {
        return -1;
    }
    const s_name_key key{name, width, height, psm};
    const auto it = g_by_name.find(key);
    if (it != g_by_name.end())
    {
        return it->second;
    }
    if (g_entries.size() >= k_max_registry_entries)
    {
        return -1;
    }
    const int index = static_cast<int>(g_entries.size());
    s_entry entry{};
    entry.psm = psm;
    entry.width = width;
    entry.height = height;
    entry.path = name;
    g_entries.push_back(std::move(entry));
    g_by_name.emplace(key, index);
    return index;
}

std::vector<int> resident_chunks()
{
    std::vector<int> chunks;
    for (const auto &resident : g_residents)
    {
        if (!resident.valid) continue;
        const int index = resident.named
            ? find_named_identity(resident.logical_name, resident.psm,
                resident.width, resident.height)
            : find_identity(resident.fingerprint, resident.psm,
                resident.width, resident.height);
        if (index >= 0
            && std::find(chunks.begin(), chunks.end(), index) == chunks.end())
            chunks.push_back(index);
    }
    return chunks;
}

void invalidate_destination(std::uint64_t bitbltbuf,
    std::uint64_t trxreg, std::uint64_t trxpos)
{
    // There is no source identity to invalidate before the first named
    // upload. Avoid building swizzle masks for every boot-time GIF upload.
    bool any_resident = false;
    for (const auto &resident : g_residents)
    {
        if (resident.valid)
        {
            any_resident = true;
            break;
        }
    }
    if (!any_resident)
    {
        return;
    }

    s_transfer_bits dirty{};
    std::uint32_t ignored = 0;
    const bool known = mark_transfer_blocks(
        bitbltbuf, trxreg, trxpos, dirty, ignored);
    for (std::size_t i = 0; i < g_residents.size(); ++i)
    {
        auto &resident = g_residents[i];
        if (!resident.valid)
        {
            continue;
        }
        // A CLUT upload should *not* evict a texture just because a
        // page-wide approximation extends into the next texture's BP.
        // Test only GS blocks the upload actually writes.
        if (!known || dirty_resident(dirty, resident.bp, resident.end_bp))
        {
            if (trace_native_textures())
            {
                static unsigned reports = 0;
                if (reports++ < 100)
                {
                    std::fprintf(stderr,
                        "[gs-native] source-overwrite name=%s bp=%04X"
                        " transfer-bp=%04X psm=%u\n",
                        resident.named ? resident.logical_name.c_str()
                            : "(fingerprint)",
                        resident.bp,
                        static_cast<unsigned>(bitbltbuf >> 32) & 0x3fffu,
                        static_cast<unsigned>(bitbltbuf >> 56) & 0x3fu);
                }
            }
            if (g_resident_by_bp[resident.bp] == static_cast<int>(i))
            {
                g_resident_by_bp[resident.bp] = -1;
            }
            resident.valid = false;
        }
    }
}

std::uint32_t atlas_scale(std::uint32_t index, std::uint32_t width,
    std::uint32_t height)
{
    load_registry();
    if (index >= g_entries.size()) return 0;
    const auto &entry = g_entries[index];
    if (!width || !height || width > k_max_native_side
        || height > k_max_native_side)
        return 0;
    if (width == entry.width && height == entry.height) return 1;
    if (width == entry.width * 2 && height == entry.height * 2) return 2;
    return 0;
}

bool valid_atlas_size(std::uint32_t index, std::uint32_t width,
    std::uint32_t height)
{
    return atlas_scale(index, width, height) != 0;
}

const s_image *chunk_image(int index)
{
    // Diagnostic A/B: retain the full GIF/name tracking path while forcing
    // the original GS pixels. This isolates replacement-PNG artifacts from
    // the emulation/queue path without disabling resource packs or UI mods.
    static const bool disable_png = []()
    {
        const char *value = std::getenv("PS2_SDBZ_SKIP_PNG");
        return value && *value && *value != '0';
    }();
    if (disable_png)
    {
        return nullptr;
    }
    load_registry();
    if (index < 0 || index >= static_cast<int>(g_entries.size()))
    {
        return nullptr;
    }
    auto &entry = g_entries[static_cast<std::size_t>(index)];

    // Targeted source-image A/B testing without removing the entire pack.
    // Resolve this once at startup: there is no parsing or filesystem work
    // on the draw path, only a lookup by the already-interned logical name.
    static const std::unordered_set<std::string> skipped_names = []()
    {
        std::unordered_set<std::string> result;
        const char *value = std::getenv("PS2_SDBZ_SKIP_PNG_NAMES");
        if (!value || !*value)
        {
            return result;
        }
        std::string source(value);
        std::size_t first = 0;
        while (first < source.size())
        {
            const std::size_t end = source.find_first_of(",;", first);
            const std::string name = source.substr(first,
                end == std::string::npos ? end : end - first);
            if (valid_path(name))
            {
                result.insert(name);
            }
            if (end == std::string::npos)
            {
                break;
            }
            first = end + 1;
        }
        return result;
    }();
    if (skipped_names.find(entry.path) != skipped_names.end())
    {
        return nullptr;
    }
    if (!entry.attempted)
    {
        entry.attempted = true;
        if (!load_png(entry.path, entry.image)
            || !valid_atlas_size(static_cast<std::uint32_t>(index),
                entry.image.width, entry.image.height)
            || entry.image.rgba.size() !=
                static_cast<std::size_t>(entry.image.width) * entry.image.height * 4)
        {
            entry.image = {};
        }
        if (trace_native_textures())
        {
            resource_pack::s_resource source{};
            const bool packed = resource_pack::find_resource(
                "textures/" + entry.path + ".png", source);
            std::size_t black_opaque = 0;
            std::size_t transparent = 0;
            const auto &pixels = entry.image.rgba;
            for (std::size_t at = 0; at + 3 < pixels.size(); at += 4)
            {
                if (!pixels[at] && !pixels[at + 1] && !pixels[at + 2]
                    && pixels[at + 3] >= 250)
                {
                    ++black_opaque;
                }
                if (!pixels[at + 3])
                {
                    ++transparent;
                }
            }
            std::fprintf(stderr,
                "[gs-native] png name=%s source=%s result=%s"
                " size=%ux%u opaque-black=%zu transparent=%zu total=%zu\n",
                entry.path.c_str(),
                packed ? source.m_pack_id.c_str() : "legacy-NATIVE/none",
                pixels.empty() ? "missing/invalid" : "loaded",
                entry.image.width, entry.image.height,
                black_opaque, transparent, pixels.size() / 4);
        }
    }
    return entry.image.rgba.empty() ? nullptr : &entry.image;
}

int complete_upload(const s_texture_upload_signature &upload)
{
    if (upload.received_bytes != upload.expected_bytes || upload.trxpos != 0)
        return -1;
    const auto psm = static_cast<std::uint32_t>(upload.bitbltbuf >> 56) & 0x3fu;
    const auto width = static_cast<std::uint32_t>(upload.trxreg & 0xfffu);
    const auto height = static_cast<std::uint32_t>(upload.trxreg >> 32) & 0xfffu;
    const auto bw = static_cast<std::uint32_t>(upload.bitbltbuf >> 48) & 0x3fu;
    const auto bp = static_cast<std::uint32_t>(upload.bitbltbuf >> 32) & 0x3fffu;
    if (!bw || bp >= k_gs_blocks) return -1;
    const int index = find_identity(upload.fnv1a64, psm, width, height);
    if (index < 0) return -1;
    s_resident *slot = nullptr;
    for (auto &resident : g_residents)
    {
        if (!resident.valid)
        {
            slot = &resident;
            break;
        }
    }
    if (!slot)
        slot = &g_residents[(g_next_resident++) % g_residents.size()];
    const std::size_t slot_index = static_cast<std::size_t>(slot - g_residents.data());
    if (slot->valid && g_resident_by_bp[slot->bp] == static_cast<int>(slot_index))
    {
        g_resident_by_bp[slot->bp] = -1;
    }
    // Most recent completed upload supersedes any previous base-pointer owner.
    const int previous = g_resident_by_bp[bp];
    if (previous >= 0 && static_cast<std::size_t>(previous) != slot_index)
    {
        g_residents[static_cast<std::size_t>(previous)].valid = false;
    }
    *slot = {bp, std::min(k_gs_blocks,
        bp + static_cast<std::uint32_t>((upload.expected_bytes + 255) / 256)),
        bw, width, height, psm, upload.fnv1a64, {}, false, true};
    g_resident_by_bp[bp] = static_cast<int>(slot_index);
    return index;
}

bool allow_named_palette_change(std::uint32_t bp, std::uint32_t bw,
    std::uint32_t psm, std::uint32_t width, std::uint32_t height,
    std::uint32_t previous_clut_bp, std::uint32_t next_clut_bp)
{
    if (bp >= k_gs_blocks || !bw || previous_clut_bp >= k_gs_blocks
        || next_clut_bp >= k_gs_blocks)
    {
        return false;
    }
    const int index = g_resident_by_bp[bp];
    if (index < 0)
    {
        return false;
    }
    auto &resident = g_residents[static_cast<std::size_t>(index)];
    if (!resident.valid || !resident.named || resident.bp != bp
        || resident.bw != bw || resident.psm != psm
        || resident.width != width || resident.height != height
        || !accepts_clut(resident, previous_clut_bp))
    {
        return false;
    }
    return remember_clut(resident, next_clut_bp);
}

bool register_named_upload(const std::string &logical_name,
    std::uint64_t bitbltbuf, std::uint64_t trxreg,
    std::uint64_t trxpos, std::uint32_t clut_bp)
{
    if (trxpos != 0)
    {
        return false;
    }
    const auto psm = static_cast<std::uint32_t>(bitbltbuf >> 56) & 0x3fu;
    const auto width = static_cast<std::uint32_t>(trxreg & 0xfffu);
    const auto height = static_cast<std::uint32_t>(trxreg >> 32) & 0xfffu;
    const auto bw = static_cast<std::uint32_t>(bitbltbuf >> 48) & 0x3fu;
    const auto bp = static_cast<std::uint32_t>(bitbltbuf >> 32) & 0x3fffu;
    if (!bw || bp >= k_gs_blocks
        || (clut_bp > 0x3fffu && clut_bp != 0xffffffffu)
        || find_named_identity(logical_name, psm, width, height) < 0)
    {
        return false;
    }
    // Published by the verified EE guest texture handle binder and consumed
    // on the GS render thread, in queue order relative to GIF transfers.
    // Do not infer the name from matching pixel bytes.
    const int current = g_resident_by_bp[bp];
    if (current >= 0)
    {
        auto &existing = g_residents[static_cast<std::size_t>(current)];
        if (existing.valid && existing.named
            && existing.bp == bp && existing.bw == bw
            && existing.psm == psm && existing.width == width
            && existing.height == height
            && existing.logical_name == logical_name)
        {
            // The same verified game filename may bind with a different
            // palette for selection/highlight effects. No pixels changed.
            if (clut_bp == 0xffffffffu
                || accepts_clut(existing, clut_bp))
            {
                return false; // Avoid redundant setup invalidations.
            }
            if (clut_bp < k_gs_blocks && remember_clut(existing, clut_bp))
            {
                return true;
            }
        }
    }
    s_resident *slot = nullptr;
    for (auto &resident : g_residents)
    {
        if (!resident.valid)
        {
            slot = &resident;
            break;
        }
    }
    if (!slot)
    {
        slot = &g_residents[(g_next_resident++) % g_residents.size()];
    }
    const std::size_t slot_index =
        static_cast<std::size_t>(slot - g_residents.data());
    if (slot->valid && g_resident_by_bp[slot->bp] == static_cast<int>(slot_index))
    {
        g_resident_by_bp[slot->bp] = -1;
    }
    const int previous = g_resident_by_bp[bp];
    if (previous >= 0 && static_cast<std::size_t>(previous) != slot_index)
    {
        g_residents[static_cast<std::size_t>(previous)].valid = false;
    }
    *slot = {bp, std::min(k_gs_blocks,
        bp + transfer_blocks(bitbltbuf, trxreg)),
        bw, width, height, psm, 0, logical_name, true, true};
    slot->clut_bp = clut_bp;
    g_resident_by_bp[bp] = static_cast<int>(slot_index);
    return true;
}

bool revoke_named_upload(const std::string &logical_name,
    std::uint32_t bp)
{
    if (bp >= k_gs_blocks)
    {
        return false;
    }
    const int slot = g_resident_by_bp[bp];
    if (slot < 0)
    {
        return false;
    }
    auto &resident = g_residents[static_cast<std::size_t>(slot)];
    if (!resident.valid || !resident.named || resident.bp != bp
        || resident.logical_name != logical_name)
    {
        return false;
    }
    g_resident_by_bp[bp] = -1;
    resident.valid = false;
    return true;
}

int resident_chunk(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm,
    std::uint32_t width, std::uint32_t height, std::uint32_t clut_bp)
{
    if (bp >= k_gs_blocks)
    {
        return -1;
    }
    const int slot = g_resident_by_bp[bp];
    if (slot < 0)
    {
        return -1;
    }
    const auto &resident = g_residents[static_cast<std::size_t>(slot)];
    if (!resident.valid || resident.bp != bp || resident.bw != bw
        || resident.psm != psm || resident.width != width
        || resident.height != height
        || (resident.named && resident.clut_bp != 0xffffffffu
            && clut_bp != 0xffffffffu
            && !accepts_clut(resident, clut_bp)))
    {
        return -1;
    }
    return resident.named
        ? find_named_identity(resident.logical_name, resident.psm,
            resident.width, resident.height)
        : find_identity(resident.fingerprint, resident.psm,
            resident.width, resident.height);
}
}
