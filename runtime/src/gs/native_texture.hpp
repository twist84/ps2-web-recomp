#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2::gs::native_texture
{

// 8-bit RGBA pixels in row-major order, top to bottom.
// A failed decode leaves the output empty and does not alter GS memory.
struct s_image
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

bool decode_png(const std::uint8_t *data, std::size_t size, s_image &output);

// Look up NATIVE/<original logical game path>.png. This is a host-only
// operation; missing or invalid replacements return false and the caller
// must use the original PS2 texture.
bool load_png(const std::string &game_path, s_image &output);

}
