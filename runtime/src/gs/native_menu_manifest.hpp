#pragma once

#include <array>
#include <cstdint>

namespace ps2::gs::native_texture
{

struct s_menu_atlas_entry
{
    const char *game_path;
    std::uint32_t chunk_index;
    std::uint64_t fingerprint;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t psm;
};

inline constexpr std::array<s_menu_atlas_entry, 13> k_menu_atlases = {{
    {"dis/main_menu.pix", 0u, 0x825F7B0566C19109ull, 256u, 256u, 0x13u},
    {"dis/main_menu.pix", 1u, 0xB51AF81B5A15B2A7ull, 512u, 512u, 0x13u},
    {"dis/main_menu.pix", 2u, 0xDFD3DFF097489FA5ull, 512u, 512u, 0x13u},
    {"dis/main_menu.pix", 3u, 0xD307C2ED0BE864BDull, 512u, 512u, 0x13u},
    {"dis/main_menu.pix", 4u, 0xA5A58F8B1540184Full, 512u, 512u, 0x13u},
    {"dis/main_menu.pix", 5u, 0x307DBE18E48C1E8Bull, 512u, 512u, 0x13u},
    {"dis/main_menu.pix", 6u, 0x12060A392E8C6231ull, 512u, 512u, 0x13u},
    {"dis/main_menu.pix", 7u, 0x11359618DED2A3F6ull, 512u, 512u, 0x13u},
    {"dis/main_menu.pix", 8u, 0x7B54AC6672C00390ull, 512u, 512u, 0x13u},
    {"dis/main_menu.pix", 9u, 0x50E1EDC598A80DD3ull, 512u, 512u, 0x13u},
    {"dis/main_menu.pix", 10u, 0x5B0B861DA2F05F13ull, 512u, 512u, 0x13u},
    {"dis/main_menu.pix", 11u, 0x5141C17CB77119C4ull, 512u, 512u, 0x13u},
    {"dis/main_menu.pix", 12u, 0x89A12DC009425F4Dull, 512u, 512u, 0x13u},
}};

}
