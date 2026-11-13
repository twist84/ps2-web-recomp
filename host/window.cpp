#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "window.hpp"
#include "window_preferences.hpp"

#include "ps2/system.hpp"
#include "ps2/presentation.hpp"
#include "ps2/resource_pack.hpp"
#include "ps2/resource_pack_overlay.hpp"

#include <SDL.h>
#include <SDL_syswm.h>
#ifndef _WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <SDL_metal.h>
#include <mach/mach.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

#ifdef _WIN32
HANDLE g_vu_thread_handle = nullptr;
#endif

enum Button : std::uint16_t {
    kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3,
    kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7,
    kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11,
    kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15,
};

struct KeyBinding {
    SDL_Scancode key;
    std::uint16_t button;
};

constexpr KeyBinding kKeys[] = {
    {SDL_SCANCODE_UP, kUp},         {SDL_SCANCODE_DOWN, kDown},     {SDL_SCANCODE_LEFT, kLeft},
    {SDL_SCANCODE_RIGHT, kRight},   {SDL_SCANCODE_Z, kCross},       {SDL_SCANCODE_X, kCircle},
    {SDL_SCANCODE_A, kSquare},      {SDL_SCANCODE_S, kTriangle},    {SDL_SCANCODE_Q, kL1},
    {SDL_SCANCODE_W, kR1},          {SDL_SCANCODE_1, kL2},          {SDL_SCANCODE_2, kR2},
    {SDL_SCANCODE_RETURN, kStart},  {SDL_SCANCODE_BACKSPACE, kSelect},
};

struct PadBinding {
    SDL_GameControllerButton button;
    std::uint16_t mask;
};

constexpr PadBinding kPadButtons[] = {
    {SDL_CONTROLLER_BUTTON_A, kCross},          {SDL_CONTROLLER_BUTTON_B, kCircle},
    {SDL_CONTROLLER_BUTTON_X, kSquare},         {SDL_CONTROLLER_BUTTON_Y, kTriangle},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kL1},  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kR1},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, kL3},     {SDL_CONTROLLER_BUTTON_RIGHTSTICK, kR3},
    {SDL_CONTROLLER_BUTTON_BACK, kSelect},      {SDL_CONTROLLER_BUTTON_START, kStart},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, kUp},       {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDown},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kLeft},   {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kRight},
};

SDL_Window *g_window = nullptr;
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
SDL_AudioDeviceID g_audio = 0;
// the keyboard drives the first controller port along with the first game controller, a second one is player 2
SDL_GameController *g_pads[2] = {};
std::uint32_t g_tex_w = 0, g_tex_h = 0;
std::uint64_t g_last = 0;
std::vector<std::uint32_t> g_pixels;
bool g_gpu_present = false;

s_window_preferences g_preferences;
std::string g_preferences_path;
std::atomic<int> g_output_volume{100};
std::atomic<bool> g_muted{false};
std::string g_notice;
Uint64 g_notice_until = 0;
bool g_fullscreen_enter = false;
bool g_resize_pending = true;

void notify(const char *message)
{
    g_notice = message;
    g_notice_until = SDL_GetTicks64() + 3000;
    std::fprintf(stderr, "[window] %s\n", message);
    SDL_SetWindowTitle(g_window, message);
}


bool g_pack_manager_open = false;
int g_pack_column = 1;
int g_pack_row = 0;
std::vector<std::string> g_pack_enabled;
std::vector<std::string> g_pack_available;

void refresh_pack_overlay()
{
    if (!g_pack_manager_open)
    {
        ps2::resource_pack_overlay::clear();
        return;
    }
    std::vector<std::string> lines{
        "RESOURCE PACKS",
        "F6 / ESC: cancel   ENTER: save",
        "ARROWS: select / enable   PAGE UP/DOWN: move pack",
        "",
        "ENABLED - highest priority first"
    };
    for (std::size_t i = 0; i < g_pack_enabled.size() && i < 9; ++i)
    {
        lines.push_back(std::string(g_pack_column == 1
            && g_pack_row == static_cast<int>(i) ? "> " : "  ")
            + g_pack_enabled[i]);
    }
    lines.push_back("");
    lines.push_back("AVAILABLE - disabled");
    for (std::size_t i = 0; i < g_pack_available.size() && i < 9; ++i)
    {
        lines.push_back(std::string(g_pack_column == 0
            && g_pack_row == static_cast<int>(i) ? "> " : "  ")
            + g_pack_available[i]);
    }
    lines.push_back("");
    lines.push_back("Restart SDBZ to apply saved changes");
    ps2::resource_pack_overlay::set_lines(std::move(lines));
}

void open_pack_manager()
{
    g_pack_enabled.clear();
    g_pack_available.clear();
    for (const auto &pack : ps2::resource_pack::packs())
        g_pack_enabled.push_back(pack.m_id);
    for (const auto &pack : ps2::resource_pack::discovered_packs())
    {
        if (std::find(g_pack_enabled.begin(), g_pack_enabled.end(), pack.m_id)
            == g_pack_enabled.end())
            g_pack_available.push_back(pack.m_id);
    }
    g_pack_manager_open = true;
    g_pack_column = g_pack_enabled.empty() ? 0 : 1;
    g_pack_row = 0;
    refresh_pack_overlay();
}

void close_pack_manager(bool save)
{
    if (save && !ps2::resource_pack::save_selection(g_pack_enabled))
    {
        notify("Cannot save resource pack selection");
        return;
    }
    g_pack_manager_open = false;
    refresh_pack_overlay();
    if (save) notify("Resource packs saved - restart SDBZ");
}

bool handle_pack_key(SDL_Scancode key)
{
    if (key == SDL_SCANCODE_F6)
    {
        if (g_pack_manager_open) close_pack_manager(false);
        else open_pack_manager();
        return true;
    }
    if (!g_pack_manager_open) return false;
    if (key == SDL_SCANCODE_ESCAPE)
    {
        close_pack_manager(false);
        return true;
    }
    if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER)
    {
        close_pack_manager(true);
        return true;
    }
    auto &current = g_pack_column ? g_pack_enabled : g_pack_available;
    if (key == SDL_SCANCODE_UP || key == SDL_SCANCODE_DOWN)
    {
        if (!current.empty())
            g_pack_row = std::clamp(g_pack_row
                + (key == SDL_SCANCODE_UP ? -1 : 1), 0,
                static_cast<int>(current.size()) - 1);
    }
    else if (key == SDL_SCANCODE_LEFT || key == SDL_SCANCODE_RIGHT)
    {
        const int destination = key == SDL_SCANCODE_RIGHT ? 1 : 0;
        if (destination != g_pack_column && !current.empty())
        {
            const std::string id = current[g_pack_row];
            current.erase(current.begin() + g_pack_row);
            auto &other = destination ? g_pack_enabled : g_pack_available;
            other.push_back(id);
            g_pack_column = destination;
            g_pack_row = static_cast<int>(other.size()) - 1;
        }
        else if (destination != g_pack_column)
        {
            g_pack_column = destination;
            g_pack_row = 0;
        }
    }
    else if ((key == SDL_SCANCODE_PAGEUP || key == SDL_SCANCODE_PAGEDOWN)
        && g_pack_column == 1 && !current.empty())
    {
        const int target = g_pack_row
            + (key == SDL_SCANCODE_PAGEUP ? -1 : 1);
        if (target >= 0 && target < static_cast<int>(current.size()))
        {
            std::swap(current[g_pack_row], current[target]);
            g_pack_row = target;
        }
    }
    refresh_pack_overlay();
    return true;
}

void apply_preferences()
{
    ps2::g_presentation_options.nearest.store(g_preferences.nearest != 0, std::memory_order_relaxed);
    ps2::g_presentation_options.force_blend.store(g_preferences.field_blending != 0, std::memory_order_relaxed);
    g_output_volume.store(g_preferences.volume, std::memory_order_relaxed);
    g_muted.store(g_preferences.muted != 0, std::memory_order_relaxed);
    ps2::g_presentation_options.render_scale.store(g_preferences.render_scale ? g_preferences.render_scale : 1, std::memory_order_relaxed);
    ps2::g_presentation_options.force_square.store(g_preferences.force_square != 0, std::memory_order_relaxed);
}

void load_preferences()
{
    g_preferences.width = ps2::g_options.widescreen ? 1280 : 960;
    g_preferences.widescreen = ps2::g_options.widescreen ? 1 : 0;
    const char *config_path = std::getenv("PS2_CONFIG");
    if (config_path && *config_path)
    {
        g_preferences_path = config_path;
    }
    else if (std::FILE *file = std::fopen("ps2run.ini", "r"))
    {
        g_preferences_path = "ps2run.ini";
        std::fclose(file);
    }
    else if (char *directory = SDL_GetPrefPath("ps2-web-recomp", "native"))
    {
        g_preferences_path = std::string(directory) + "window.ini";
        SDL_free(directory);
    }
    if (std::FILE *file = std::fopen(g_preferences_path.c_str(), "r"))
    {
        read_window_preferences(file, g_preferences);
        std::fclose(file);
    }
    // The previous environment switch remains a startup override.
    if (const char *value = std::getenv("PS2_FORCE_DEINTERLACE"))
    {
        if (!std::strcmp(value, "0") || !std::strcmp(value, "1"))
        {
            g_preferences.field_blending = *value == '1';
        }
    }
    ps2::g_options.widescreen = g_preferences.widescreen != 0;
    apply_preferences();
}

void save_preferences()
{
    if (g_preferences_path.empty()) return;
    std::FILE *file = std::fopen(g_preferences_path.c_str(), "w");
    if (!file)
    {
        std::fprintf(stderr, "[window] cannot save preferences: %s\n", g_preferences_path.c_str());
        return;
    }
    const bool wrote = write_window_preferences(file, g_preferences);
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed) std::fprintf(stderr, "[window] could not write window preferences\n");
}

void toggle_fullscreen()
{
    const bool full = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (SDL_SetWindowFullscreen(g_window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
    {
        notify(SDL_GetError());
        return;
    }
    g_preferences.fullscreen = !full;
    SDL_ShowCursor(full ? SDL_ENABLE : SDL_DISABLE);
    g_resize_pending = true;
    save_preferences();

    // SDL_WINDOWEVENT_SIZE_CHANGED often does not fire when toggling
    // fullscreen on Windows, so explicitly resize the surface to match
    // the desktop resolution. SDL_GetWindowSizeInPixels can return stale
    // data immediately after SDL_SetWindowFullscreen, so query the
    // display bounds directly.
    SDL_Rect bounds{};
    if (SDL_GetDisplayBounds(0, &bounds) == 0 && bounds.w > 0 && bounds.h > 0)
    {
        ps2::resize_window(static_cast<std::uint32_t>(bounds.w), static_cast<std::uint32_t>(bounds.h));
    }
}

bool handle_hotkey(const SDL_KeyboardEvent &key)
{
    if (key.repeat) return false;
    switch (key.keysym.scancode)
    {
    case SDL_SCANCODE_RETURN:
        if (!(key.keysym.mod & KMOD_ALT)) return false;
        g_fullscreen_enter = true;
        [[fallthrough]];
    case SDL_SCANCODE_F11:
        toggle_fullscreen();
        return true;
    case SDL_SCANCODE_F5:
    {
        g_preferences.render_scale = (g_preferences.render_scale + 1) % 5;
        apply_preferences();
        char message[64];
        std::snprintf(message, sizeof message, "GPU render scale: %s%d", g_preferences.render_scale ? "" : "auto / ",
            ps2::g_presentation_options.render_scale.load(std::memory_order_relaxed));
        notify(message);
        break;
    }
    case SDL_SCANCODE_F7:
        g_preferences.nearest = !g_preferences.nearest;
        apply_preferences();
        notify(g_preferences.nearest ? "nearest filtering" : "smooth filtering");
        break;
    case SDL_SCANCODE_F8:
        g_preferences.field_blending = !g_preferences.field_blending;
        apply_preferences();
        notify(g_preferences.field_blending ? "field blending on" : "field blending auto");
        break;
    case SDL_SCANCODE_F9:
        g_preferences.show_stats = !g_preferences.show_stats;
        if (!g_preferences.show_stats) ps2::g_stats_text[0] = '\0';
        notify(g_preferences.show_stats ? "performance title on" : "performance title off");
        break;
    case SDL_SCANCODE_F10:
        g_preferences.muted = !g_preferences.muted;
        apply_preferences();
        notify(g_preferences.muted ? "muted" : "unmuted");
        break;
    case SDL_SCANCODE_MINUS:
    case SDL_SCANCODE_EQUALS:
    case SDL_SCANCODE_KP_MINUS:
    case SDL_SCANCODE_KP_PLUS:
    {
        const bool down = key.keysym.scancode == SDL_SCANCODE_MINUS || key.keysym.scancode == SDL_SCANCODE_KP_MINUS;
        g_preferences.volume = std::clamp(g_preferences.volume + (down ? -5 : 5), 0, 100);
        apply_preferences();
        char message[64];
        std::snprintf(message, sizeof message, "volume %d%%%s", g_preferences.volume, g_preferences.muted ? " (muted)" : "");
        notify(message);
        break;
    }
    default: return false;
    }
    save_preferences();
    return true;
}

bool create_renderer() {
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_renderer) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

bool attach_gpu() {
    if (std::getenv("PS2_SDL_RENDERER")) return false;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_window, &info)) return false;
    ps2::WindowTarget target;
    int w = 0, h = 0;
    // When starting in fullscreen, SDL_GetWindowSizeInPixels can return the
    // requested windowed size rather than the actual desktop resolution.
    // Query display bounds directly when fullscreen is active.
    if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN)
    {
        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(0, &bounds) == 0 && bounds.w > 0 && bounds.h > 0)
        {
            w = bounds.w;
            h = bounds.h;
        }
    }
    if (w == 0 || h == 0)
    {
        SDL_GetWindowSizeInPixels(g_window, &w, &h);
    }
    target.width = static_cast<std::uint32_t>(w);
    target.height = static_cast<std::uint32_t>(h);
#ifdef __APPLE__
    if (info.subsystem == SDL_SYSWM_COCOA) {
        SDL_MetalView view = SDL_Metal_CreateView(g_window);
        if (!view) return false;
        target.kind = ps2::WindowTarget::Metal;
        target.surface = SDL_Metal_GetLayer(view);
        SDL_Metal_GetDrawableSize(g_window, &w, &h);
        target.width = static_cast<std::uint32_t>(w);
        target.height = static_cast<std::uint32_t>(h);
        return target.surface && ps2::attach_window(target);
    }
#endif
    switch (info.subsystem) {
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
    case SDL_SYSWM_WINDOWS:
    {
        target.kind = ps2::WindowTarget::Win32;
        target.display = info.info.win.hinstance;
        target.surface = info.info.win.window;
        break;
    }
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
    case SDL_SYSWM_X11:
        target.kind = ps2::WindowTarget::X11;
        target.display = info.info.x11.display;
        target.window = static_cast<std::uint64_t>(info.info.x11.window);
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
        target.kind = ps2::WindowTarget::Wayland;
        target.display = info.info.wl.display;
        target.surface = info.info.wl.surface;
        break;
#endif
    default: return false;
    }
    return ps2::attach_window(target);
}

std::uint8_t axis(SDL_GameController *pad, SDL_GameControllerAxis a) {
    const int v = SDL_GameControllerGetAxis(pad, a);
    return static_cast<std::uint8_t>(std::clamp((v + 32768) >> 8, 0, 255));
}

void rumble(int port) {
    static double last_strong[2] = {};
    static bool last_weak[2] = {};
    static Uint32 sent[2] = {};
    SDL_GameController *pad = g_pads[port];
    if (!pad) return;
    const std::uint16_t motors = ps2::pad_rumble(port);
    const double strong = (motors >> 8) < 16 ? 0.0 : std::sqrt((motors >> 8) / 255.0);
    const bool weak = motors & 1u;
    const Uint32 now = SDL_GetTicks();
    if (std::fabs(strong - last_strong[port]) <= 0.08 && weak == last_weak[port] && (!(strong > 0.0 || weak) || now - sent[port] < 100)) return;
    last_strong[port] = strong;
    last_weak[port] = weak;
    sent[port] = now;
    SDL_GameControllerRumble(pad, static_cast<Uint16>(strong * 65535.0), weak ? 0xFFFF : 0, strong > 0.0 || weak ? 200 : 0);
}

// a new controller takes the first free port, the second one plugs in player 2
void open_pad(int device) {
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
    for (SDL_GameController *pad : g_pads)
        if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id) return;
    for (int port = 0; port < 2; ++port)
        if (!g_pads[port]) {
            g_pads[port] = SDL_GameControllerOpen(device);
            return;
        }
}

void poll_port(int port) {
    static bool plugged[2] = {};
    SDL_GameController *pad = g_pads[port];
    if (port > 0 && plugged[port] != (pad != nullptr)) {
        plugged[port] = pad != nullptr;
        ps2::set_pad_connected(port, plugged[port]);
    }
    std::uint16_t pressed = 0;
    std::uint8_t l2 = 0xFF, r2 = 0xFF;
    std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    if (port == 0 && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)) {
        const Uint8 *keys = SDL_GetKeyboardState(nullptr);
        for (const KeyBinding &b : kKeys)
            if (keys[b.key] && !(b.key == SDL_SCANCODE_RETURN && (g_fullscreen_enter || (SDL_GetModState() & KMOD_ALT)))) pressed |= b.button;
        if (keys[SDL_SCANCODE_LEFT]) lx = 0x00;
        if (keys[SDL_SCANCODE_RIGHT]) lx = 0xFF;
        if (keys[SDL_SCANCODE_UP]) ly = 0x00;
        if (keys[SDL_SCANCODE_DOWN]) ly = 0xFF;
    }
    if (pad) {
        for (const PadBinding &b : kPadButtons)
            if (SDL_GameControllerGetButton(pad, b.button)) pressed |= b.mask;
        const auto trigger = [&](SDL_GameControllerAxis a, std::uint16_t bit, std::uint8_t &pressure) {
            const int v = SDL_GameControllerGetAxis(pad, a);
            if (v <= 2000 || (pressed & bit)) return;
            pressed |= bit;
            pressure = static_cast<std::uint8_t>(std::max(1, v * 255 / 32767));
        };
        trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT, kL2, l2);
        trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, kR2, r2);
        if (lx == 0x80) lx = axis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        if (ly == 0x80) ly = axis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        rx = axis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
        ry = axis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
    }
    if (g_pack_manager_open)
    {
        pressed = 0;
        lx = ly = rx = ry = 0x80;
        l2 = r2 = 0xFF;
    }
    rumble(port);
    static std::uint64_t last[2] = {~0ull, ~0ull};
    const std::uint64_t state = pressed | (static_cast<std::uint64_t>(rx) << 16) | (static_cast<std::uint64_t>(ry) << 24) | (static_cast<std::uint64_t>(lx) << 32) |
                                (static_cast<std::uint64_t>(ly) << 40) | (static_cast<std::uint64_t>(l2) << 48) | (static_cast<std::uint64_t>(r2) << 56);
    if (state == last[port]) return;
    last[port] = state;
    ps2::set_pad_pressure(8, l2, port);
    ps2::set_pad_pressure(9, r2, port);
    ps2::set_pad_buttons(pressed, port);
    ps2::set_pad_axis(0, rx, port);
    ps2::set_pad_axis(1, ry, port);
    ps2::set_pad_axis(2, lx, port);
    ps2::set_pad_axis(3, ly, port);
}

void poll() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { save_preferences(); ps2::halt("window closed"); }
        if (e.type == SDL_KEYDOWN && !e.key.repeat
            && !handle_pack_key(e.key.keysym.scancode))
            handle_hotkey(e.key);
        if (g_pack_manager_open && e.type == SDL_CONTROLLERBUTTONDOWN
            && g_pads[0] && e.cbutton.which
                == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[0])))
        {
            switch (e.cbutton.button)
            {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                handle_pack_key(SDL_SCANCODE_UP);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                handle_pack_key(SDL_SCANCODE_DOWN);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                handle_pack_key(SDL_SCANCODE_LEFT);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                handle_pack_key(SDL_SCANCODE_RIGHT);
                break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                handle_pack_key(SDL_SCANCODE_PAGEUP);
                break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                handle_pack_key(SDL_SCANCODE_PAGEDOWN);
                break;
            case SDL_CONTROLLER_BUTTON_A:
            case SDL_CONTROLLER_BUTTON_START:
                handle_pack_key(SDL_SCANCODE_RETURN);
                break;
            case SDL_CONTROLLER_BUTTON_B:
                handle_pack_key(SDL_SCANCODE_ESCAPE);
                break;
            default:
                break;
            }
        }
        if (g_pack_manager_open && e.type == SDL_MOUSEWHEEL)
        {
            if (e.wheel.y)
                handle_pack_key(e.wheel.y > 0
                    ? SDL_SCANCODE_UP : SDL_SCANCODE_DOWN);
        }
        if (g_pack_manager_open && e.type == SDL_MOUSEBUTTONDOWN)
        {
            int window_w = 0, window_h = 0;
            SDL_GetWindowSize(g_window, &window_w, &window_h);
            if (window_w > 0 && window_h > 0)
            {
                const float scale = std::clamp(
                    static_cast<float>(window_h) / 900.f, 0.55f, 1.5f);
                const float step = 31.f * scale;
                const float first_y = window_h * 0.10f;
                const int line = static_cast<int>(
                    (static_cast<float>(e.button.y) - first_y) / step);
                const int enabled_start = 5;
                const int available_start = 7
                    + static_cast<int>(std::min<std::size_t>(9, g_pack_enabled.size()));
                if (line >= enabled_start && line < enabled_start
                    + static_cast<int>(std::min<std::size_t>(9, g_pack_enabled.size())))
                {
                    g_pack_column = 1;
                    g_pack_row = line - enabled_start;
                }
                else if (line >= available_start && line < available_start
                    + static_cast<int>(std::min<std::size_t>(9, g_pack_available.size())))
                {
                    g_pack_column = 0;
                    g_pack_row = line - available_start;
                }
                if (e.button.button == SDL_BUTTON_RIGHT || e.button.clicks >= 2)
                    handle_pack_key(g_pack_column == 1
                        ? SDL_SCANCODE_LEFT : SDL_SCANCODE_RIGHT);
                refresh_pack_overlay();
            }
        }
        if (e.type == SDL_KEYUP && e.key.keysym.scancode == SDL_SCANCODE_RETURN) g_fullscreen_enter = false;
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) g_fullscreen_enter = false;
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED &&
            !(SDL_GetWindowFlags(g_window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)))
        {
            g_preferences.width = std::max(320, e.window.data1);
            g_preferences.height = std::max(240, e.window.data2);
        }
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && g_gpu_present) {
            int w = 0, h = 0;
#ifdef __APPLE__
            SDL_Metal_GetDrawableSize(g_window, &w, &h);
#else
            SDL_GetWindowSizeInPixels(g_window, &w, &h);
#endif
            ps2::resize_window(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) open_pad(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED)
            for (SDL_GameController *&pad : g_pads)
                if (pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
                    SDL_GameControllerClose(pad);
                    pad = nullptr;
                }
    }
    poll_port(0);
    poll_port(1);
}

void show_pixels(const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h) {
    const bool force_blend = ps2::g_presentation_options.force_blend.load(std::memory_order_relaxed);
    static std::vector<std::uint32_t> g_prev_pixels;
    std::vector<std::uint32_t> blended;
    if (force_blend && !g_prev_pixels.empty() && g_prev_pixels.size() == static_cast<std::size_t>(w) * h) {
        blended.resize(w * h);
        std::memcpy(blended.data(), rgba, w * h * 4);
        // Full-frame field blend: average current with previous frame to eliminate vertical jitter,
        // matching the WebGPU present() path behavior.
        for (std::size_t i = 0; i < g_pixels.size(); ++i) {
            const std::uint32_t cur = blended[i];
            const std::uint32_t prev = g_prev_pixels[i];
            const std::uint8_t cr = static_cast<std::uint8_t>(cur);
            const std::uint8_t cg = static_cast<std::uint8_t>(cur >> 8);
            const std::uint8_t cb = static_cast<std::uint8_t>(cur >> 16);
            const std::uint8_t pr = static_cast<std::uint8_t>(prev);
            const std::uint8_t pg = static_cast<std::uint8_t>(prev >> 8);
            const std::uint8_t pb = static_cast<std::uint8_t>(prev >> 16);
            blended[i] = ((cr + pr) >> 1) | (((cg + pg) >> 1) << 8) | (((cb + pb) >> 1) << 16) | 0xFF000000u;
        }
    }
    g_prev_pixels.assign(rgba, rgba + w * h);
    const unsigned scale = static_cast<unsigned>(std::max(1, ps2::g_presentation_options.active_render_scale.load(std::memory_order_relaxed)));
    const unsigned native_w = g_gpu_present ? w : w / scale;
    const unsigned native_h = g_gpu_present ? h : h / scale;
    ps2::g_presentation_options.native_width.store(static_cast<int>(native_w), std::memory_order_relaxed);
    ps2::g_presentation_options.native_height.store(static_cast<int>(native_h), std::memory_order_relaxed);
    if (w != g_tex_w || h != g_tex_h) {
        if (g_texture) SDL_DestroyTexture(g_texture);
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(w), static_cast<int>(h));
        g_tex_w = w;
        g_tex_h = h;
    }
    SDL_UpdateTexture(g_texture, nullptr, force_blend && !blended.empty() ? blended.data() : rgba, static_cast<int>(w * 4));
    SDL_SetTextureScaleMode(g_texture, ps2::g_presentation_options.nearest.load(std::memory_order_relaxed) ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    int output_width = 0, output_height = 0;
    SDL_GetRendererOutputSize(g_renderer, &output_width, &output_height);
    const auto viewport = ps2::window_viewport(output_width, output_height, static_cast<int>(native_w), static_cast<int>(native_h));
    const SDL_Rect destination{viewport.x, viewport.y, viewport.width, viewport.height};
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, &destination);
    SDL_RenderPresent(g_renderer);
}

void present() {
    if (g_gpu_present) {
        ps2::present_frame();
        return;
    }
    std::uint32_t w = 0, h = 0;
    if (ps2::display_frame(g_pixels, w, h)) show_pixels(g_pixels.data(), w, h);
}

constexpr Uint32 kAudioBytesPerMs = 48 * 4;

std::atomic<double> g_paced_ms{0.0};

struct PaceTimer {
    std::uint64_t start = SDL_GetPerformanceCounter();
    ~PaceTimer() {
        const double ms = static_cast<double>(SDL_GetPerformanceCounter() - start) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
        g_paced_ms.store(g_paced_ms.load() + ms);
    }
};

void pace() {
    const PaceTimer timer;
    if (g_audio) {
        // The audio device clock paces emulation: wait while more than ~80 ms of sound is queued.
        while (SDL_GetQueuedAudioSize(g_audio) > 80 * kAudioBytesPerMs) SDL_Delay(1);
        return;
    }
    const std::uint64_t frequency = SDL_GetPerformanceFrequency();
    const std::uint64_t period = frequency / (ps2::g_options.pal ? 50 : 60);
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (g_last && now - g_last < period) {
        SDL_Delay(static_cast<Uint32>((period - (now - g_last)) * 1000 / frequency));
        g_last += period;
    } else {
        g_last = now;
    }
}

double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME creation_time{}, exit_time{}, kernel_time{}, user_time{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time)) return 0.0;
    ULARGE_INTEGER kernel_ticks{}, user_ticks{};
    kernel_ticks.LowPart = kernel_time.dwLowDateTime;
    kernel_ticks.HighPart = kernel_time.dwHighDateTime;
    user_ticks.LowPart = user_time.dwLowDateTime;
    user_ticks.HighPart = user_time.dwHighDateTime;
    return (static_cast<double>(kernel_ticks.QuadPart) + static_cast<double>(user_ticks.QuadPart)) / 10000000.0;
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
#endif
}

#ifdef _WIN32
double thread_cpu_seconds(HANDLE h) {
    FILETIME creation_time{}, exit_time{}, kernel_time{}, user_time{};
    if (!GetThreadTimes(h, &creation_time, &exit_time, &kernel_time, &user_time)) return 0.0;
    ULARGE_INTEGER kernel_ticks{}, user_ticks{};
    kernel_ticks.LowPart = kernel_time.dwLowDateTime;
    kernel_ticks.HighPart = kernel_time.dwHighDateTime;
    user_ticks.LowPart = user_time.dwLowDateTime;
    user_ticks.HighPart = user_time.dwHighDateTime;
    return (static_cast<double>(kernel_ticks.QuadPart) + static_cast<double>(user_ticks.QuadPart)) / 10000000.0;
}
#else
double thread_cpu_seconds(void*) { return 0.0; }
#endif

double resident_mb() {
#ifdef __APPLE__
    mach_task_basic_info_data_t task{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task), &count) != KERN_SUCCESS) return 0.0;
    return static_cast<double>(task.resident_size) / (1024.0 * 1024.0);
#endif
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return 0.0;
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#else
    long pages = 0, resident = 0;
    if (std::FILE *f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
}

void show_speed() {
    static std::uint64_t start = 0, frames = 0;
    static double cpu_start = 0.0, gpu_start = 0.0, emu_start = 0.0, vu_start = 0.0, gs_start = 0.0;
#ifdef _WIN32
    static double main_cpu_start = 0.0, vu_cpu_start = 0.0;
#endif
    static std::uint64_t interpreted_start = 0;
    const auto emu_idle = [] { return ps2::thread_waited_ms(0) + g_paced_ms.load(); };
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (!start) {
        start = now;
        cpu_start = process_cpu_seconds();
        gpu_start = ps2::gpu_busy_ms();
        emu_start = emu_idle();
        vu_start = ps2::vu_thread_idle_ms();
        gs_start = ps2::thread_waited_ms(2);
        interpreted_start = ps2::vu1_interpreted_calls();
#ifdef _WIN32
        main_cpu_start = thread_cpu_seconds(GetCurrentThread());
        vu_cpu_start = g_vu_thread_handle ? thread_cpu_seconds(g_vu_thread_handle) : 0.0;
#endif
    }
    ++frames;
    const double seconds = static_cast<double>(now - start) / static_cast<double>(SDL_GetPerformanceFrequency());
    if (seconds < 1.0) return;
    const double cpu = process_cpu_seconds(), gpu = ps2::gpu_busy_ms(), emu = emu_idle(), vu = ps2::vu_thread_idle_ms(), gs = ps2::thread_waited_ms(2);
    const std::uint64_t interpreted = ps2::vu1_interpreted_calls();
    char title[192];
    const double queued = g_audio ? SDL_GetQueuedAudioSize(g_audio) / static_cast<double>(kAudioBytesPerMs) : 0.0;
    const double vram_mb = ps2::gpu_vram_mb();
    const int render_width = ps2::g_presentation_options.native_width.load(std::memory_order_relaxed) * ps2::g_presentation_options.active_render_scale.load(std::memory_order_relaxed);
    const int render_height = ps2::g_presentation_options.native_height.load(std::memory_order_relaxed) * ps2::g_presentation_options.active_render_scale.load(std::memory_order_relaxed);
#ifdef _WIN32
    const double main_cpu = thread_cpu_seconds(GetCurrentThread()) - main_cpu_start;
    const double vu_cpu = g_vu_thread_handle ? thread_cpu_seconds(g_vu_thread_handle) - vu_cpu_start : 0.0;
    std::snprintf(title, sizeof title, "render %dx%d (%dx)\naudio %.0f ms\n%.1f fps\nMain %.0f%%\nVU %.0f%%\nRAM %.0f MB\nGPU %.0f%%\nVRAM %.0f MB",
#else
    std::snprintf(title, sizeof title, "render %dx%d (%dx)\naudio %.0f ms\n%.1f fps\nCPU %.0f%%\nRAM %.0f MB\nGPU %.0f%%\nVRAM %.0f MB",
#endif
        render_width, render_height, ps2::g_presentation_options.active_render_scale.load(std::memory_order_relaxed),
        queued,
        static_cast<double>(frames) / seconds,
#ifdef _WIN32
        main_cpu / seconds * 100.0, vu_cpu / seconds * 100.0, resident_mb(),
#else
        (cpu - cpu_start) / seconds * 100.0, resident_mb(),
#endif
        (gpu - gpu_start) / (seconds * 10.0), vram_mb);
    if (SDL_GetTicks64() < g_notice_until)
    {
        SDL_SetWindowTitle(g_window, g_notice.c_str());
    }
    else
    {
        if (g_preferences.show_stats)
        {
            SDL_SetWindowTitle(g_window, "Super Dragon Ball Z (EU BETA) - recomp");
            std::strncpy(ps2::g_stats_text, title, sizeof(ps2::g_stats_text) - 1);
            ps2::g_stats_text[sizeof(ps2::g_stats_text) - 1] = '\0';
        }
        else
        {
            SDL_SetWindowTitle(g_window, "Super Dragon Ball Z (EU BETA) - recomp");
            ps2::g_stats_text[0] = '\0';
        }
    }
    start = now;
    frames = 0;
    emu_start = emu;
    vu_start = vu;
    gs_start = gs;
    interpreted_start = interpreted;
    cpu_start = cpu;
    gpu_start = gpu;
#ifdef _WIN32
    main_cpu_start = thread_cpu_seconds(GetCurrentThread());
    vu_cpu_start = g_vu_thread_handle ? thread_cpu_seconds(g_vu_thread_handle) : 0.0;
#endif
}

void on_vblank() {
    show_speed();
    poll();
    present();
    pace();
}

void queue_audio(const std::int16_t *samples, std::size_t frames) {
    if (SDL_GetQueuedAudioSize(g_audio) > 500 * kAudioBytesPerMs) return;
    const int volume = g_muted.load(std::memory_order_relaxed) ? 0 : g_output_volume.load(std::memory_order_relaxed);
    if (volume == 100)
    {
        SDL_QueueAudio(g_audio, samples, static_cast<Uint32>(frames * 4));
        return;
    }
    // Queue silence when muted so the audio clock still paces emulation.
    thread_local std::vector<std::int16_t> adjusted;
    adjusted.resize(frames * 2);
    for (std::size_t i = 0; i < adjusted.size(); ++i)
    {
        adjusted[i] = static_cast<std::int16_t>(static_cast<int>(samples[i]) * volume / 100);
    }
    SDL_QueueAudio(g_audio, adjusted.data(), static_cast<Uint32>(frames * 4));
}

}

bool open_window(bool fullscreen) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    load_preferences();
    fullscreen = fullscreen || g_preferences.fullscreen != 0;
    g_preferences.fullscreen = fullscreen;
#ifdef __APPLE__
    const Uint32 metal = std::getenv("PS2_SDL_RENDERER") ? 0u : SDL_WINDOW_METAL;
#else
    const Uint32 metal = 0u;
#endif
    const Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | metal | (fullscreen ? static_cast<Uint32>(SDL_WINDOW_FULLSCREEN_DESKTOP) : 0u);
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0)
    {
        g_preferences.width = std::clamp(g_preferences.width, 320, std::max(320, usable.w - 32));
        g_preferences.height = std::clamp(g_preferences.height, 240, std::max(240, usable.h - 32));
        // Make internal bounds square: use the smaller of the two clamped
        // dimensions so width == height regardless of display bounds.
        const int square = std::min(g_preferences.width, g_preferences.height);
        g_preferences.width = square;
        g_preferences.height = square;
    }
    g_window = SDL_CreateWindow("Super Dragon Ball Z (EU BETA) - recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, g_preferences.width, g_preferences.height, flags);
    if (!g_window) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetWindowMinimumSize(g_window, 320, 240);
    save_preferences();
    std::fprintf(stderr, "[window] F5 GPU render scale | F7 filter | F8 field blending | F9 stats | F10 mute | +/- volume | F11 / Alt+Enter fullscreen\n");
    std::fprintf(stderr, "[window] F6 resource pack manager (restart required)\n");
    std::fprintf(stderr, "[window] settings: %s\n", g_preferences_path.c_str());
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio) {
        std::fprintf(stderr, "[window] audio output: %s, %d Hz, %u channels\n",
            SDL_GetCurrentAudioDriver(), have.freq, have.channels);
        SDL_PauseAudioDevice(g_audio, 0);
        ps2::set_audio_sink(queue_audio);
    }
    else std::fprintf(stderr, "[window] audio output unavailable: %s\n", SDL_GetError());
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) open_pad(i);
    if (fullscreen) SDL_ShowCursor(SDL_DISABLE);
    return true;
}

int run_window(const std::function<int()> &emulate) {
    ps2::init_graphics();
    g_gpu_present = attach_gpu();
    std::fprintf(stderr, "[window] %s\n", g_gpu_present ? "presenting through WebGPU" : "presenting through SDL");
    if (!g_gpu_present && !create_renderer()) return 1;
    const bool threads = g_gpu_present && !std::getenv("PS2_ONE_THREAD") && !std::getenv("PS2_TIME") && ps2::g_options.frames_dir.empty();
    if (!threads) {
        ps2::g_options.on_vblank = on_vblank;
        return emulate();
    }
    ps2::g_options.on_vblank = [] {
        ps2::post_present();
        pace();
    };
    ps2::start_render_thread();
    if (!std::getenv("PS2_NO_VU_THREAD")) {
        ps2::start_vu_thread();
#ifdef _WIN32
        std::thread vu_thread(ps2::run_vu_thread);
        g_vu_thread_handle = vu_thread.native_handle();
        vu_thread.detach();
#else
        std::thread(ps2::run_vu_thread).detach();
#endif
    }
    std::atomic<bool> done{false};
    int result = 0;
    std::thread emulation([&] {
        result = emulate();
        done.store(true);
        ps2::wake_render_thread();
    });
    const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> copy = show_pixels, none;
    while (!done.load()) {
        poll();
        if (ps2::render_pending(g_gpu_present, g_gpu_present ? none : copy, show_speed, 8.0) == ps2::Rendered::idle) ps2::wait_render_work(4);
    }
    emulation.join();
    SDL_Quit();
    std::fflush(nullptr);
    std::_Exit(result);
}
