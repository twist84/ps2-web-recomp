#include "sdbz_texture_source_probe.hpp"

#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "gs/gs.hpp"
#include "iop/arika_dvd.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

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
// The texture handle is the first parameter of the guest TEX0 builder.
constexpr std::uint32_t k_bind_texture_eu_beta = 0x0010C3E0u;
constexpr std::uint32_t k_bind_texture_japan = 0x0010C350u;
constexpr std::uint32_t k_handle_table_eu_beta = 0x004FF8F0u;
constexpr std::uint32_t k_handle_table_japan = 0x005007F0u;
constexpr std::uint32_t k_handle_stride = 48u;
constexpr std::size_t k_max_handles = 255;
constexpr std::size_t k_max_observed_objects = 8192;

bool name_only_enabled()
{
    static const bool enabled = []()
    {
        const char *value = std::getenv("PS2_SDBZ_NAME_ONLY");
        return value && *value && *value != '0';
    }();
    return enabled;
}

bool trace_enabled()
{
    static const bool enabled =
        std::getenv("PS2_TRACE_SDBZ_TEXTURE_NAMES") != nullptr;
    return enabled;
}

struct s_observed_texture
{
    std::string guest_name;
    std::string canonical_name;
};
std::unordered_map<std::uint32_t, s_observed_texture> g_source_objects;
struct s_resolved_handle
{
    std::string name;
    bool ambiguous = false;
};
std::array<s_resolved_handle, k_max_handles + 1> g_named_handles;
std::uint64_t g_source_revision = 1;
std::uint64_t g_resolved_revision = 0;
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
    if (trace_enabled())
    {
        std::fprintf(stderr,
            "[sdbz-name] FPIX name=%s source=%08X chunks=%u bytes=%u\n",
            name.c_str(), source, count, size);
    }
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

    // Resolve the name using the original INFO/GAME index, not hashes.
    // An unknown/ambiguous basename must never select an arbitrary pack.
    const std::string canonical =
        iop::arkd::resolve_texture_name(name);
    if (g_source_objects.size() < k_max_observed_objects
        || g_source_objects.find(object) != g_source_objects.end())
    {
        g_source_objects[object] = {name, canonical};
        ++g_source_revision;
    }
    if (trace_enabled())
    {
        std::fprintf(stderr,
            "[sdbz-name] texture=%s canonical=%s TIM2=%08X pixels=%08X"
            " bytes=%u PSM=%u size=%ux%u object=%08X\n",
            name.c_str(), canonical.empty() ? "(unresolved)" : canonical.c_str(),
            tim2_address, pixel_address, image_bytes,
            image_type == 5 ? 19u : 20u, width, height, object);
    }
}
}

void rebuild_handles()
{
    if (g_resolved_revision == g_source_revision)
    {
        return;
    }
    for (auto &item : g_named_handles)
    {
        item = {};
    }
    for (const auto &[object, observed] : g_source_objects)
    {
        if (observed.canonical_name.empty())
        {
            continue;
        }
        const auto *handle_data = ee_bytes(object + 0x38u, 4);
        std::string live_name;
        if (!handle_data || !texture_name(object, live_name)
            || live_name != observed.guest_name)
        {
            continue;
        }
        const std::uint32_t handle = u32(handle_data);
        if (handle == 0 || handle > k_max_handles)
        {
            continue;
        }
        auto &entry = g_named_handles[handle];
        if (!entry.name.empty()
            && entry.name != observed.canonical_name)
        {
            entry.ambiguous = true;
        }
        else
        {
            entry.name = observed.canonical_name;
        }
    }
    g_resolved_revision = g_source_revision;
}

void inspect_binding(Cpu &cpu, std::uint32_t table_base)
{
    const std::uint32_t handle = cpu.r[4].w[0];
    if (!handle || handle > k_max_handles)
    {
        return;
    }
    rebuild_handles();
    const auto &identity = g_named_handles[handle];
    if (identity.name.empty() || identity.ambiguous)
    {
        return;
    }
    const auto *slot = ee_bytes(table_base
        + (handle - 1u) * k_handle_stride, k_handle_stride);
    if (!slot)
    {
        return;
    }
    const std::uint32_t bp = u16(slot + 2);
    const std::uint32_t psm = u16(slot + 14);
    const std::uint32_t width = u16(slot + 22);
    const std::uint32_t height = u16(slot + 24);
    // Verified TEX0 builder: the halfword at +0x04 is CLUT base pointer,
    // not texture buffer width. TBW is max(1, original_width / 64),
    // encoded in TEX0 bits 14..19.
    const std::uint32_t bw = std::max(1u, width / 64u);
    if (!bw || bw > 63 || bp >= 0x4000u
        || (psm != 0 && psm != 19 && psm != 20)
        || !width || !height || width > 1024 || height > 1024)
    {
        return;
    }

    // The game's handle lookup provides GS residency independently of
    // upload bytes; the GS queue preserves the event's relative order.
    if (name_only_enabled())
    {
        gs::publish_named_texture(identity.name, bp, bw, psm,
            width, height);
    }
    if (trace_enabled())
    {
        std::fprintf(stderr,
            "[sdbz-name] bind name=%s handle=%u bp=%04X bw=%u"
            " psm=%u size=%ux%u\n",
            identity.name.c_str(), handle, bp, bw, psm, width, height);
    }
}

void sdbz_texture_source_probe(Cpu &cpu, std::uint32_t pc)
{
    // Region-specific addresses are not interchangeable. The other region
    // can contain unrelated executable code at the same PC. Require both
    // verified entry prologues before trusting any object/handle data.
    const auto *eu_pix = ee_bytes(k_pix_initialize_eu_beta, 4);
    const auto *eu_bind = ee_bytes(k_bind_texture_eu_beta, 4);
    const auto *jp_pix = ee_bytes(k_pix_initialize_japan, 4);
    const auto *jp_bind = ee_bytes(k_bind_texture_japan, 4);
    const bool is_eu_beta = eu_pix && eu_bind
        && u32(eu_pix) == 0x27BDFF70u
        && u32(eu_bind) == 0x27BDFF30u;
    const bool is_japan = jp_pix && jp_bind
        && u32(jp_pix) == 0x27BDFF70u
        && u32(jp_bind) == 0x27BDFF30u;
    if (is_eu_beta == is_japan)
    {
        return;
    }
    if (is_eu_beta
        && pc != k_pix_initialize_eu_beta
        && pc != k_texture_initialize_eu_beta
        && pc != k_bind_texture_eu_beta)
    {
        return;
    }
    if (is_japan
        && pc != k_pix_initialize_japan
        && pc != k_texture_initialize_japan
        && pc != k_bind_texture_japan)
    {
        return;
    }
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
    case k_bind_texture_eu_beta:
        inspect_binding(cpu, k_handle_table_eu_beta);
        break;
    case k_bind_texture_japan:
        inspect_binding(cpu, k_handle_table_japan);
        break;
    default:
        break;
    }
}
}
