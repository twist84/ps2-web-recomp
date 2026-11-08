#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace ps2
{

// Last stats line computed by host::show_speed(); drawn on-screen by gpu::present()
extern char g_stats_text[256];

struct s_presentation_options
{
    std::atomic<bool> nearest{false};
    std::atomic<bool> force_blend{false};
    std::atomic<int> render_scale{1};
    std::atomic<int> active_render_scale{1};
    std::atomic<int> native_width{0};
    std::atomic<int> native_height{0};
};

inline s_presentation_options g_presentation_options;

struct s_window_viewport
{
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
};

// Scale to fill window height, preserving aspect ratio with side bars only.
inline s_window_viewport window_viewport(int window_width, int window_height,
    int source_width, int source_height)
{
    window_width = std::max(window_width, 1);
    window_height = std::max(window_height, 1);
    source_width = std::max(source_width, 1);
    source_height = std::max(source_height, 1);

    int height = window_height;
    int width = (source_width * window_height + source_height / 2) / source_height;
    if (width < 1) width = 1;
    if (width > window_width && window_width > 0)
    {
        width = window_width;
        height = (source_height * window_width + source_width / 2) / source_width;
        if (height < 1) height = 1;
    }

    return {(window_width - width) / 2, (window_height - height) / 2, width, height};
}

}
