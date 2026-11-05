#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2
{

// VRAM stores scale*scale subpixel samples for each native word. Remap the
// sample grid without changing native addresses, including packed formats.
inline std::vector<std::uint32_t> rescale_vram_samples(
    const std::vector<std::uint32_t> &source, unsigned from, unsigned to)
{
    if (!from || !to || from > 4 || to > 4 || source.size() % (from * from)) return {};
    if (from == to) return source;
    const std::size_t words = source.size() / (from * from);
    std::vector<std::uint32_t> result(words * to * to);
    for (std::size_t word = 0; word < words; ++word)
    {
        for (unsigned y = 0; y < to; ++y)
        {
            const unsigned sy = ((2 * y + 1) * from) / (2 * to);
            for (unsigned x = 0; x < to; ++x)
            {
                const unsigned sx = ((2 * x + 1) * from) / (2 * to);
                result[word * to * to + y * to + x] = source[word * from * from + sy * from + sx];
            }
        }
    }
    return result;
}

}
