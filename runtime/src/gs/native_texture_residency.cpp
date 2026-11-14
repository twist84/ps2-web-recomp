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

bool intersects(std::uint32_t begin, std::uint32_t end,
    std::uint32_t other_begin, std::uint32_t other_end)
{
    return begin < other_end && other_begin < end;
}

// GS textures occupy 8 KiB swizzled pages; they do not occupy a tightly
// packed width*height*bpp byte interval. In particular, short sub-rectangle
// uploads can touch later pages when DBW exceeds RRW or DSAY is nonzero.
// Conservatively cover the bounding page interval using the same page
// geometry as the GPU's pages()/region() logic.
std::uint32_t transfer_blocks(std::uint64_t bitbltbuf,
    std::uint64_t trxreg, std::uint64_t trxpos = 0)
{
    const std::uint32_t psm =
        static_cast<std::uint32_t>(bitbltbuf >> 56) & 0x3fu;
    const std::uint64_t bw =
        static_cast<std::uint32_t>(bitbltbuf >> 48) & 0x3fu;
    const std::uint64_t width = trxreg & 0xfffu;
    const std::uint64_t height = (trxreg >> 32) & 0xfffu;
    if (!bw || !width || !height)
    {
        return 0;
    }

    std::uint64_t page_width = 64;
    std::uint64_t page_height = 32;
    switch (psm)
    {
    case 0x02: case 0x0a: case 0x32: case 0x3a:
        page_height = 64;
        break;
    case 0x13:
        page_width = 128;
        page_height = 64;
        break;
    case 0x14:
        page_width = 128;
        page_height = 128;
        break;
    case 0x00: case 0x01: case 0x30: case 0x31:
    case 0x1b: case 0x24: case 0x2c:
        break;
    default:
        // Unknown layouts must never leave a stale named source identity.
        return k_gs_blocks;
    }

    const std::uint64_t dsax = (trxpos >> 32) & 0x7ffu;
    const std::uint64_t dsay = (trxpos >> 48) & 0x7ffu;
    const std::uint64_t pages_per_row =
        std::max<std::uint64_t>(1, (bw * 64 + page_width - 1)
            / page_width);
    const std::uint64_t last_page =
        (dsax + width - 1) / page_width
        + ((dsay + height - 1) / page_height) * pages_per_row;
    // Each GS page spans 32 x 256-byte blocks. Clamp to all VRAM if
    // the region is larger than the available 4 MiB.
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        k_gs_blocks, (last_page + 1) * 32));
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
    const std::uint32_t start =
        static_cast<std::uint32_t>(bitbltbuf >> 32) & 0x3fffu;
    const std::uint32_t blocks =
        transfer_blocks(bitbltbuf, trxreg, trxpos);
    if (!blocks)
    {
        return;
    }
    const std::uint64_t last = static_cast<std::uint64_t>(start) + blocks;
    const bool wraps = last > k_gs_blocks;
    const std::uint32_t end = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(k_gs_blocks, last));
    for (std::size_t i = 0; i < g_residents.size(); ++i)
    {
        auto &resident = g_residents[i];
        if (!resident.valid)
        {
            continue;
        }
        // Indexed textures also depend on their CLUT. Uploading another
        // palette at the same CBP invalidates any previously decoded PNG
        // even when TEX0 and its pixel buffer have not changed.
        const bool clut_hit = resident.named
            && resident.clut_bp != 0xffffffffu
            && (intersects(start, end, resident.clut_bp,
                    std::min(k_gs_blocks, resident.clut_bp + 32u))
                || (wraps && intersects(0,
                    static_cast<std::uint32_t>(last - k_gs_blocks),
                    resident.clut_bp,
                    std::min(k_gs_blocks, resident.clut_bp + 32u))));
        if (clut_hit || intersects(start, end,
                resident.bp, resident.end_bp)
            || (wraps && intersects(0,
                    static_cast<std::uint32_t>(last - k_gs_blocks),
                    resident.bp, resident.end_bp)))
        {
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
        const auto &existing = g_residents[static_cast<std::size_t>(current)];
        if (existing.valid && existing.named
            && existing.bp == bp && existing.bw == bw
            && existing.psm == psm && existing.width == width
            && existing.height == height
            && existing.clut_bp == clut_bp
            && existing.logical_name == logical_name)
        {
            return false; // No new residency: avoid invalidating setup.
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
            && resident.clut_bp != clut_bp))
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
