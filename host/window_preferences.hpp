#pragma once

#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct s_window_preferences
{
    int nearest = 1;
    int field_blending = 1;
    int show_stats = 1;
    int muted = 0;
    int volume = 100;
    int fullscreen = 0;
    int width = 960;
    int height = 720;
    int render_scale = 0;
    int widescreen = 0;
    int force_square = 0;
};

inline void read_window_preferences(std::FILE *file, s_window_preferences &settings)
{
    if (!file)
    {
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof line, file))
    {
        char key[64], text[64];
        if (std::sscanf(line, "%63[^=]=%63s", key, text) != 2)
        {
            continue;
        }
        char *name = key;
        while (std::isspace(static_cast<unsigned char>(*name))) ++name;
        char *key_end = name + std::strlen(name);
        while (key_end > name && std::isspace(static_cast<unsigned char>(key_end[-1]))) --key_end;
        *key_end = '\0';
        char *end = nullptr;
        errno = 0;
        const long parsed = std::strtol(text, &end, 10);
        if (errno || end == text || *end || parsed < INT_MIN || parsed > INT_MAX)
        {
            continue;
        }
        const int value = static_cast<int>(parsed);
        if (!std::strcmp(name, "nearest") && value >= 0 && value <= 1) settings.nearest = value;
        if (!std::strcmp(name, "field_blending") && value >= 0 && value <= 1) settings.field_blending = value;
        if (!std::strcmp(name, "show_stats") && value >= 0 && value <= 1) settings.show_stats = value;
        if (!std::strcmp(name, "muted") && value >= 0 && value <= 1) settings.muted = value;
        if (!std::strcmp(name, "volume") && value >= 0 && value <= 100) settings.volume = value;
        if (!std::strcmp(name, "fullscreen") && value >= 0 && value <= 1) settings.fullscreen = value;
        if (!std::strcmp(name, "width") && value >= 320 && value <= 16384) settings.width = value;
        if (!std::strcmp(name, "height") && value >= 240 && value <= 16384) settings.height = value;
        if (!std::strcmp(name, "render_scale") && value >= 0 && value <= 4) settings.render_scale = value;
        if (!std::strcmp(name, "widescreen") && value >= 0 && value <= 1) settings.widescreen = value;
        if (!std::strcmp(name, "force_square") && value >= 0 && value <= 1) settings.force_square = value;
    }
}

inline bool write_window_preferences(std::FILE *file, const s_window_preferences &settings)
{
    return file && std::fprintf(file,
        "; ps2run window settings. Close the game before editing.\n"
        "; Hotkeys save these settings back to this file.\n\n"
        "; Filtering: 1=nearest, 0=smooth\nnearest=%d\n\n"
        "; WebGPU field blending: 1=force on (jitter fix), 0=automatic\nfield_blending=%d\n\n"
        "; Window title statistics: 1=show, 0=hide\nshow_stats=%d\n\n"
        "; Audio: muted is 0 or 1; volume is 0..100 percent\nmuted=%d\nvolume=%d\n\n"
        "; Desktop fullscreen: 1=on, 0=off\nfullscreen=%d\n\n"
        "; Windowed client size in SDL window coordinates\nwidth=%d\nheight=%d\n\n"
        "; GPU render scale: 0=follow fixed display scale (otherwise 1x), 1..4=override\nrender_scale=%d\n\n"
        "; Aspect ratio: 0=4:3, 1=16:9\nwidescreen=%d\n\n"
        "; Force square output (448x448 NTSC): 1=on, 0=off\nforce_square=%d\n",
        settings.nearest, settings.field_blending, settings.show_stats,
        settings.muted, settings.volume, settings.fullscreen, settings.width, settings.height,
        settings.render_scale, settings.widescreen, settings.force_square) >= 0;
}
