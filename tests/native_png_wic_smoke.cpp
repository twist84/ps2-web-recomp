#include "gs/native_texture.hpp"
#include "ps2/disc.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ps2::disc
{
std::optional<NativeAsset> find_native_asset(
    const std::string &, const std::string &)
{
    return std::nullopt;
}

std::size_t read_native_asset(std::uint32_t, std::uint64_t,
    void *, std::size_t)
{
    return 0;
}
}

int main()
{
    using ps2::gs::native_texture::s_image;
    using ps2::gs::native_texture::decode_png;
    static constexpr std::uint8_t png[] = {
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,
        0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,
        0x08,0x06,0x00,0x00,0x00,0x72,0xB6,0x0D,0x24,0x00,0x00,0x00,
        0x16,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0xF8,0xCF,0xC0,0xF0,
        0x1F,0x08,0x1B,0x18,0x40,0xB4,0x88,0x86,0xCD,0x7F,0x00,0x38,
        0x20,0x05,0xF4,0x35,0x83,0xAF,0x46,0x00,0x00,0x00,0x00,0x49,
        0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
    };
    const std::uint8_t expected[] = {
        255, 0, 0, 255,
        0, 255, 0, 128,
        0, 0, 255, 0,
        20, 40, 60, 255
    };

    s_image image;
    if (!decode_png(png, sizeof png, image))
        return 1;
    if (image.width != 2 || image.height != 2
        || image.rgba.size() != sizeof expected)
        return 2;
    for (std::size_t i = 0; i < sizeof expected; ++i)
        if (image.rgba[i] != expected[i])
            return 3;
    if (decode_png(png, sizeof png - 8, image))
        return 4;
    if (decode_png(nullptr, 0, image))
        return 5;
    return 0;
}
