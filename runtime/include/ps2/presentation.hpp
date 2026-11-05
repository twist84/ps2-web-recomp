#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace ps2
{

enum class e_window_scaling
{
    fit,
    integer_aspect,
    integer_pixels,
    scale_2x,
    scale_3x,
    scale_4x,
    pixels_2x,
    pixels_3x,
    pixels_4x
};

struct s_presentation_options
{
    std::atomic<e_window_scaling> scaling{e_window_scaling::fit};
    std::atomic<bool> nearest{false};
    std::atomic<bool> force_blend{false};
    std::atomic<int> render_scale{1};
    std::atomic<int> active_render_scale{1};
    std::atomic<int> native_width{0};
    std::atomic<int> native_height{0};
};

inline s_presentation_options g_presentation_options;

inline int fixed_window_scale(e_window_scaling scaling)
{
    switch (scaling)
    {
    case e_window_scaling::scale_2x:
    case e_window_scaling::pixels_2x: return 2;
    case e_window_scaling::scale_3x:
    case e_window_scaling::pixels_3x: return 3;
    case e_window_scaling::scale_4x:
    case e_window_scaling::pixels_4x: return 4;
    default: return 0;
    }
}

struct s_window_viewport
{
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
};

// Integer modes use native display dimensions, independent of GPU render scale.
// If one native image cannot fit, use a fractional fit instead of cropping it.
inline s_window_viewport window_viewport(int window_width, int window_height,
    int source_width, int source_height, bool widescreen, e_window_scaling scaling)
{
    window_width = std::max(window_width, 1);
    window_height = std::max(window_height, 1);
    source_width = std::max(source_width, 1);
    source_height = std::max(source_height, 1);

    const std::int64_t aspect_width = widescreen ? 16 : 4;
    const std::int64_t aspect_height = widescreen ? 9 : 3;
    int width = window_width;
    int height = window_height;
    const bool raw_pixels = scaling == e_window_scaling::integer_pixels ||
        scaling == e_window_scaling::pixels_2x || scaling == e_window_scaling::pixels_3x || scaling == e_window_scaling::pixels_4x;
    const std::int64_t numerator = raw_pixels ? source_width : aspect_width;
    const std::int64_t denominator = raw_pixels ? source_height : aspect_height;

    if (static_cast<std::int64_t>(width) * denominator > static_cast<std::int64_t>(height) * numerator)
    {
        width = std::max(1, static_cast<int>(height * numerator / denominator));
    }
    else
    {
        height = std::max(1, static_cast<int>(width * denominator / numerator));
    }

    if (scaling != e_window_scaling::fit)
    {
        const int base_width = raw_pixels ? source_width : std::max(1,
            static_cast<int>((source_height * aspect_width + aspect_height / 2) / aspect_height));
        int scale = std::min(window_width / base_width, window_height / source_height);
        switch (scaling)
        {
        case e_window_scaling::scale_2x:
        case e_window_scaling::pixels_2x: scale = std::min(scale, 2); break;
        case e_window_scaling::scale_3x:
        case e_window_scaling::pixels_3x: scale = std::min(scale, 3); break;
        case e_window_scaling::scale_4x:
        case e_window_scaling::pixels_4x: scale = std::min(scale, 4); break;
        default: break;
        }
        if (scale >= 1)
        {
            width = base_width * scale;
            height = source_height * scale;
        }
    }

    return {(window_width - width) / 2, (window_height - height) / 2, width, height};
}

}
