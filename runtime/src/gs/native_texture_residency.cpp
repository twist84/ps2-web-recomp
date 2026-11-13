#include "native_texture_residency.hpp"

#include "ps2/disc.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ps2::gs::native_texture
{
namespace
{
constexpr std::uint32_t k_gs_blocks = (4u << 20) / 256u;
constexpr std::array<std::uint64_t, 13> k_fingerprints = {
    0x825F7B0566C19109ull, 0xB51AF81B5A15B2A7ull,
    0xDFD3DFF097489FA5ull, 0xD307C2ED0BE864BDull,
    0xA5A58F8B1540184Full, 0x307DBE18E48C1E8Bull,
    0x12060A392E8C6231ull, 0x11359618DED2A3F6ull,
    0x7B54AC6672C00390ull, 0x50E1EDC598A80DD3ull,
    0x5B0B861DA2F05F13ull, 0x5141C17CB77119C4ull,
    0x89A12DC009425F4Dull,
};

struct s_resident
{
    std::uint32_t bp = 0;
    std::uint32_t end_bp = 0;
    std::uint32_t bw = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int chunk = -1;
};

std::array<s_image, 13> g_images;
std::array<bool, 13> g_attempted{};
std::array<s_resident, 13> g_residents{};
bool g_available = false;

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

bool replacements_available()
{
    if (g_available) return true;
    // The catalog is populated when a folder-backed disc is mounted, so
    // do not cache a negative result from early GS initialization.
    for (std::uint32_t index = 0; index < 13; ++index)
    {
        char suffix[5] = {'.', static_cast<char>('0' + index / 100),
            static_cast<char>('0' + index / 10 % 10),
            static_cast<char>('0' + index % 10), 0};
        if (disc::find_native_asset(std::string("dis/main_menu.pix") + suffix, "png"))
        {
            g_available = true;
            break;
        }
    }
    return g_available;
}

void reset_residency()
{
    g_residents = {};
    for (auto &resident : g_residents) resident.chunk = -1;
    g_attempted.fill(false);
    g_images = {};
    g_available = false;
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
            resident.chunk = -1;
    }
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
        const std::uint32_t expected = at == 0 ? 256u : 512u;
        if (g_images[at].width != expected || g_images[at].height != expected
            || g_images[at].rgba.size() !=
                static_cast<std::size_t>(expected) * expected * 4)
            g_images[at] = {};
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
    for (std::size_t i = 0; i < k_fingerprints.size(); ++i)
    {
        const std::uint32_t expected = i == 0 ? 256u : 512u;
        if (width != expected || height != expected
            || upload.fnv1a64 != k_fingerprints[i])
            continue;
        if (!chunk_image(static_cast<int>(i))) return -1;
        g_residents[i] = {bp, std::min(k_gs_blocks,
            bp + static_cast<std::uint32_t>(upload.expected_bytes / 256)),
            bw, width, height, static_cast<int>(i)};
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
