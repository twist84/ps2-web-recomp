#include "sdbz_texture_source_probe.hpp"

#include "ps2/generated.hpp"
#include "ps2/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ps2
{
namespace
{
// Identified from the EU Beta SLES_541.61 and Japan SLPS_256.42 ELFs.
// Original guest execution is retained; these are observation points only.
constexpr std::uint32_t k_pix_initialize_eu_beta = 0x0019AA70u;
constexpr std::uint32_t k_texture_initialize_eu_beta = 0x0019A670u;
constexpr std::uint32_t k_pix_initialize_japan = 0x0019A860u;
constexpr std::uint32_t k_texture_initialize_japan = 0x0019A460u;
constexpr std::size_t k_max_name = 80;

const std::uint8_t *ee_bytes(std::uint32_t address, std::size_t size)
{
    // Only EE main-RAM addresses and their KSEG0/KSEG1 aliases are valid.
    const std::uint32_t segment = address & 0xE0000000u;
    if (segment != 0 && segment != 0x80000000u
        && segment != 0xA0000000u)
    {
        return nullptr;
    }
    const std::uint32_t offset = address & 0x1FFFFFFFu;
    if (offset >= kRamSize || size > kRamSize - offset)
    {
        return nullptr;
    }
    return g_ram + offset;
}

std::uint16_t u16(const std::uint8_t *data)
{
    return static_cast<std::uint16_t>(data[0])
        | static_cast<std::uint16_t>(data[1]) << 8;
}

std::uint32_t u32(const std::uint8_t *data)
{
    return static_cast<std::uint32_t>(data[0])
        | static_cast<std::uint32_t>(data[1]) << 8
        | static_cast<std::uint32_t>(data[2]) << 16
        | static_cast<std::uint32_t>(data[3]) << 24;
}

bool texture_name(std::uint32_t object, std::string &name)
{
    // CTexture/CPixTexture store their inline name starting at +0x10.
    // Reject non-path bytes rather than leaking arbitrary guest RAM.
    const auto *data = ee_bytes(object + 0x10u, k_max_name);
    if (!data)
    {
        return false;
    }
    name.clear();
    for (std::size_t i = 0; i < k_max_name; ++i)
    {
        const unsigned char ch = data[i];
        if (!ch)
        {
            return !name.empty();
        }
        if (!((ch >= 'a' && ch <= 'z')
            || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9')
            || ch == '_' || ch == '-' || ch == '/' || ch == '.'))
        {
            return false;
        }
        name.push_back(static_cast<char>(ch));
    }
    return false;
}

void inspect_pix(Cpu &cpu)
{
    const std::uint32_t object = cpu.r[4].w[0];
    const std::uint32_t source = cpu.r[5].w[0];
    const auto *header = ee_bytes(source, 36);
    std::string name;
    if (!header || std::memcmp(header, "FPIX", 4) != 0
        || !texture_name(object, name))
    {
        return;
    }
    const std::uint32_t size = u32(header + 4);
    const std::uint32_t count = u32(header + 8);
    if (count == 0 || count > 4096
        || size < 32u + count * 4u
        || !ee_bytes(source, size))
    {
        return;
    }
    std::fprintf(stderr,
        "[sdbz-name] FPIX name=%s source=%08X chunks=%u bytes=%u\n",
        name.c_str(), source, count, size);
}

void inspect_texture(Cpu &cpu)
{
    const std::uint32_t object = cpu.r[4].w[0];
    const std::uint32_t tim2_address = cpu.r[5].w[0];
    const auto *tim2 = ee_bytes(tim2_address, 176);
    std::string name;
    if (!tim2 || std::memcmp(tim2, "TIM2", 4) != 0
        || !texture_name(object, name))
    {
        return;
    }

    // The game's XPIX wrapper uses a TIM2 header at +0 and its first
    // picture at +128. The image data follows the picture header.
    const auto *picture = tim2 + 128;
    const std::uint32_t picture_bytes = u32(picture);
    const std::uint32_t image_bytes = u32(picture + 8);
    const std::uint16_t header_bytes = u16(picture + 12);
    const std::uint32_t width = u16(picture + 20);
    const std::uint32_t height = u16(picture + 22);
    const std::uint8_t image_type = picture[19];
    if (header_bytes < 48 || image_bytes == 0
        || image_bytes > picture_bytes - (picture_bytes >= header_bytes
            ? header_bytes : picture_bytes)
        || !width || !height || width > 1024 || height > 1024
        || (image_type != 4 && image_type != 5))
    {
        return;
    }
    const std::uint32_t pixel_address =
        tim2_address + 128u + header_bytes;
    if (!ee_bytes(pixel_address, image_bytes))
    {
        return;
    }

    // Provenance is the guest source address, not a hash of its bytes.
    // Until the texture-handle / GS transfer correlation is established,
    // do not assign this name to any GS base pointer.
    std::fprintf(stderr,
        "[sdbz-name] texture=%s TIM2=%08X pixels=%08X bytes=%u"
        " PSM=%u size=%ux%u object=%08X\n",
        name.c_str(), tim2_address, pixel_address, image_bytes,
        image_type == 5 ? 19u : 20u, width, height, object);
}
}

void sdbz_texture_source_probe(Cpu &cpu, std::uint32_t pc)
{
    switch (pc)
    {
    case k_pix_initialize_eu_beta:
    case k_pix_initialize_japan:
        inspect_pix(cpu);
        break;
    case k_texture_initialize_eu_beta:
    case k_texture_initialize_japan:
        inspect_texture(cpu);
        break;
    default:
        break;
    }
}
}
