#include "native_texture_residency.hpp"
#include "native_menu_manifest.hpp"

#include "ps2/disc.hpp"
#include "ps2/resource_pack.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ps2::gs::native_texture
{
namespace
{
constexpr std::uint32_t k_gs_blocks = (4u << 20) / 256u;


struct s_resident
{
    std::uint32_t bp = 0;
    std::uint32_t end_bp = 0;
    std::uint32_t bw = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int chunk = -1;
};

std::array<s_image, k_menu_atlases.size()> g_images;
std::array<bool, k_menu_atlases.size()> g_attempted{};
std::array<s_resident, 64> g_residents{};
std::size_t g_next_resident = 0;
bool g_available = false;
bool g_catalog_reported = false;
bool g_catalog_missing_reported = false;
std::array<bool, k_menu_atlases.size()> g_image_reported{};


bool intersects(std::uint32_t begin, std::uint32_t end,
    std::uint32_t other_begin, std::uint32_t other_end)
{
    return begin < other_end && other_begin < end;
}

std::uint32_t transfer_blocks(std::uint64_t bitbltbuf,
    std::uint64_t trxreg)
{
    const std::uint32_t psm = static_cast<std::uint32_t>(bitbltbuf >> 56) & 0x3fu;
    std::uint32_t bits = 0;
    switch (psm)
    {
    case 0x00: case 0x30: bits = 32; break;
    case 0x01: case 0x31: bits = 24; break;
    case 0x02: case 0x0a: case 0x32: case 0x3a: bits = 16; break;
    case 0x13: case 0x1b: bits = 8; break;
    case 0x14: case 0x24: case 0x2c: bits = 4; break;
    default: return k_gs_blocks;
    }
    const std::uint64_t width = trxreg & 0xfffu;
    const std::uint64_t height = (trxreg >> 32) & 0xfffu;
    const std::uint64_t bytes = (width * height * bits + 7) / 8;
    return static_cast<std::uint32_t>((bytes + 255) / 256);
}
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

bool replacements_available()
{
    if (g_available) return true;
    // The catalog is populated when a folder-backed disc is mounted, so
    // do not cache a negative result from early GS initialization.
    for (std::uint32_t index = 0; index < k_menu_atlases.size(); ++index)
    {
        char suffix[5] = {'.', static_cast<char>('0' + index / 100),
            static_cast<char>('0' + index / 10 % 10),
            static_cast<char>('0' + index % 10), 0};
        const std::string original = std::string("dis/main_menu.pix") + suffix;
        resource_pack::s_resource replacement;
        if (resource_pack::find_resource("textures/" + original + ".png",
                replacement) || disc::find_native_asset(original, "png"))
        {
            g_available = true;
            if (trace_native_textures() && !g_catalog_reported)
            {
                std::fprintf(stderr,
                    "[gs-native] catalog found dis/main_menu.pix.%03u.png\n",
                    index);
                g_catalog_reported = true;
            }
            break;
        }
    }
    if (!g_available && trace_native_textures() && disc::is_open()
        && !g_catalog_missing_reported)
    {
        std::fprintf(stderr,
            "[gs-native] no menu PNG in native asset catalog; "
            "expected disc/NATIVE/dis/main_menu.pix.001.png\n");
        g_catalog_missing_reported = true;
    }
    return g_available;
}

void reset_residency()
{
    g_residents = {};
    g_next_resident = 0;
    for (auto &resident : g_residents) resident.chunk = -1;
    g_attempted.fill(false);
    g_images = {};
    g_available = false;
    g_catalog_reported = false;
    g_catalog_missing_reported = false;
    g_image_reported.fill(false);
}

void invalidate_destination(std::uint64_t bitbltbuf, std::uint64_t trxreg)
{
    if (!g_available) return;
    const std::uint32_t start = static_cast<std::uint32_t>(bitbltbuf >> 32) & 0x3fffu;
    const std::uint32_t blocks = transfer_blocks(bitbltbuf, trxreg);
    if (!blocks) return;
    const std::uint32_t end = std::min(k_gs_blocks, start + blocks);
    for (auto &resident : g_residents)
    {
        if (resident.chunk >= 0
            && intersects(start, end, resident.bp, resident.end_bp))
        {
            if (trace_native_textures())
                std::fprintf(stderr,
                    "[gs-native] invalidate chunk=%d BP=%04X writeBP=%04X blocks=%u\n",
                    resident.chunk, resident.bp, start, blocks);
            resident.chunk = -1;
        }
    }
}

std::uint32_t atlas_scale(std::uint32_t chunk, std::uint32_t width,
    std::uint32_t height)
{
    if (chunk >= k_menu_atlases.size()) return 0;
    const std::uint32_t base = k_menu_atlases[chunk].width;
    if (width != height) return 0;
    if (width == base) return 1;
    if (width == base * 2u) return 2;
    return 0;
}

bool valid_atlas_size(std::uint32_t chunk, std::uint32_t width,
    std::uint32_t height)
{
    return atlas_scale(chunk, width, height) != 0;
}

const s_image *chunk_image(int index)
{
    if (index < 0 || index >= static_cast<int>(g_images.size())) return nullptr;
    const std::size_t at = static_cast<std::size_t>(index);
    if (!g_attempted[at])
    {
        g_attempted[at] = true;
        if (!load_fpix_chunk_png("dis/main_menu.pix",
                static_cast<std::uint32_t>(index), g_images[at]))
            g_images[at] = {};
        if (!valid_atlas_size(static_cast<std::uint32_t>(at),
                g_images[at].width, g_images[at].height)
            || g_images[at].rgba.size() !=
                static_cast<std::size_t>(g_images[at].width) * g_images[at].height * 4)
            g_images[at] = {};
        if (trace_native_textures() && !g_image_reported[at])
        {
            std::fprintf(stderr,
                "[gs-native] png chunk=%03d status=%s size=%ux%u\n",
                index, g_images[at].rgba.empty() ? "rejected" : "loaded",
                g_images[at].width, g_images[at].height);
            g_image_reported[at] = true;
        }
    }
    return g_images[at].rgba.empty() ? nullptr : &g_images[at];
}

int complete_upload(const s_texture_upload_signature &upload)
{
    if (!g_available || upload.received_bytes != upload.expected_bytes)
        return -1;
    const std::uint32_t psm = static_cast<std::uint32_t>(upload.bitbltbuf >> 56) & 0x3fu;
    if (psm != 0x13 || upload.trxpos != 0) return -1;
    const std::uint32_t width = static_cast<std::uint32_t>(upload.trxreg & 0xfffu);
    const std::uint32_t height = static_cast<std::uint32_t>((upload.trxreg >> 32) & 0xfffu);
    const std::uint32_t bw = static_cast<std::uint32_t>((upload.bitbltbuf >> 48) & 0x3fu);
    const std::uint32_t bp = static_cast<std::uint32_t>((upload.bitbltbuf >> 32) & 0x3fffu);
    if (!bw || bp >= k_gs_blocks) return -1;
    for (std::size_t i = 0; i < k_menu_atlases.size(); ++i)
    {
        const std::uint32_t expected = k_menu_atlases[i].width;
        if (width != expected || height != expected
            || upload.fnv1a64 != k_menu_atlases[i].fingerprint)
            continue;
        if (!chunk_image(static_cast<int>(i))) return -1;
        s_resident *slot = nullptr;
        for (auto &resident : g_residents)
        {
            if (resident.chunk < 0)
            {
                slot = &resident;
                break;
            }
        }
        if (!slot)
            slot = &g_residents[(g_next_resident++) % g_residents.size()];
        *slot = {bp, std::min(k_gs_blocks,
            bp + static_cast<std::uint32_t>((upload.expected_bytes + 255) / 256)),
            bw, width, height, static_cast<int>(i)};
        if (trace_native_textures() && i == 1)
            std::fprintf(stderr,
                "[gs-native] resident chunk=%03zu BP=%04X BW=%u PSM=%02X size=%ux%u\n",
                i, bp, bw, psm, width, height);
        return static_cast<int>(i);
    }
    return -1;
}

int resident_chunk(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm,
    std::uint32_t width, std::uint32_t height)
{
    if (psm != 0x13) return -1;
    for (const auto &resident : g_residents)
    {
        if (resident.chunk >= 0 && resident.bp == bp && resident.bw == bw
            && resident.width == width && resident.height == height)
            return resident.chunk;
    }
    return -1;
}

}
