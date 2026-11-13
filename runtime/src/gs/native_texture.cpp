#include "native_texture.hpp"

#include "ps2/disc.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

#include <zlib.h>

namespace ps2::gs::native_texture
{
namespace
{
constexpr std::uint32_t k_max_dimension = 4096;
constexpr std::size_t k_max_file_bytes = 64u * 1024u * 1024u;
constexpr std::uint8_t k_signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};

std::uint32_t read_be32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) << 24
        | static_cast<std::uint32_t>(p[1]) << 16
        | static_cast<std::uint32_t>(p[2]) << 8
        | static_cast<std::uint32_t>(p[3]);
}

int paeth(int a, int b, int c)
{
    const int p = a + b - c;
    const int da = p > a ? p - a : a - p;
    const int db = p > b ? p - b : b - p;
    const int dc = p > c ? p - c : c - p;
    if (da <= db && da <= dc) return a;
    if (db <= dc) return b;
    return c;
}
}

bool decode_png(const std::uint8_t *data, std::size_t size, s_image &output)
{
    output = {};
    if (!data || size < 8 || size > k_max_file_bytes
        || std::memcmp(data, k_signature, 8) != 0)
        return false;

    std::uint32_t width = 0, height = 0;
    unsigned channels = 0;
    bool has_header = false, has_end = false, has_data = false;
    std::vector<std::uint8_t> compressed;

    for (std::size_t at = 8; at + 12 <= size;)
    {
        const std::uint32_t length = read_be32(data + at);
        if (length > size - at - 12) return false;
        const std::uint8_t *tag = data + at + 4;
        const std::uint8_t *body = data + at + 8;

        uLong crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, tag, static_cast<uInt>(length + 4));
        if (static_cast<std::uint32_t>(crc) != read_be32(body + length))
            return false;

        if (std::memcmp(tag, "IHDR", 4) == 0)
        {
            if (has_header || length != 13 || has_data) return false;
            width = read_be32(body);
            height = read_be32(body + 4);
            const unsigned depth = body[8], color = body[9];
            if (!width || !height || width > k_max_dimension || height > k_max_dimension
                || depth != 8 || (color != 2 && color != 6)
                || body[10] != 0 || body[11] != 0 || body[12] != 0)
                return false;
            channels = color == 6 ? 4 : 3;
            has_header = true;
        }
        else if (std::memcmp(tag, "IDAT", 4) == 0)
        {
            if (!has_header || has_end || compressed.size() > k_max_file_bytes - length)
                return false;
            compressed.insert(compressed.end(), body, body + length);
            has_data = true;
        }
        else if (std::memcmp(tag, "IEND", 4) == 0)
        {
            if (!has_header || !has_data || length != 0 || at + 12 != size)
                return false;
            has_end = true;
            break;
        }
        else if ((tag[0] & 0x20u) == 0)
        {
            // Unknown critical chunks cannot be interpreted safely.
            return false;
        }

        at += static_cast<std::size_t>(length) + 12;
    }
    if (!has_end || compressed.empty()) return false;

    const std::size_t stride = static_cast<std::size_t>(width) * channels;
    const std::size_t inflated_size = (stride + 1) * height;
    std::vector<std::uint8_t> scanlines(inflated_size);
    uLongf actual = static_cast<uLongf>(scanlines.size());
    if (uncompress(scanlines.data(), &actual,
            compressed.data(), static_cast<uLong>(compressed.size())) != Z_OK
        || actual != inflated_size)
        return false;

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    std::vector<std::uint8_t> previous(stride, 0), row(stride, 0);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        const std::size_t source = static_cast<std::size_t>(y) * (stride + 1);
        const unsigned filter = scanlines[source];
        if (filter > 4) return false;
        for (std::size_t x = 0; x < stride; ++x)
        {
            const int left = x >= channels ? row[x - channels] : 0;
            const int up = previous[x];
            const int upper_left = x >= channels ? previous[x - channels] : 0;
            int predictor = 0;
            if (filter == 1) predictor = left;
            else if (filter == 2) predictor = up;
            else if (filter == 3) predictor = (left + up) / 2;
            else if (filter == 4) predictor = paeth(left, up, upper_left);
            row[x] = static_cast<std::uint8_t>(scanlines[source + 1 + x] + predictor);
        }
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::size_t src = static_cast<std::size_t>(x) * channels;
            const std::size_t dst = (static_cast<std::size_t>(y) * width + x) * 4;
            rgba[dst] = row[src];
            rgba[dst + 1] = row[src + 1];
            rgba[dst + 2] = row[src + 2];
            rgba[dst + 3] = channels == 4 ? row[src + 3] : 255;
        }
        previous.swap(row);
    }
    output.width = width;
    output.height = height;
    output.rgba = std::move(rgba);
    return true;
}

bool load_png(const std::string &game_path, s_image &output)
{
    output = {};
    const auto asset = disc::find_native_asset(game_path, "png");
    if (!asset || asset->size < 8 || asset->size > k_max_file_bytes)
        return false;
    std::vector<std::uint8_t> bytes(asset->size);
    if (disc::read_native_asset(asset->id, 0, bytes.data(), bytes.size())
        != bytes.size())
        return false;
    return decode_png(bytes.data(), bytes.size(), output);
}
}
