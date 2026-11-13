#pragma once

#include <array>
#include <cstdint>

namespace ps2::gs::native_texture
{

struct s_menu_atlas_entry
{
    std::uint64_t fingerprint;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t psm;
};

inline constexpr std::array<s_menu_atlas_entry, 13> k_menu_atlases = {{
    {0x825F7B0566C19109ull, 256u, 256u, 0x13u},
    {0xB51AF81B5A15B2A7ull, 512u, 512u, 0x13u},
    {0xDFD3DFF097489FA5ull, 512u, 512u, 0x13u},
    {0xD307C2ED0BE864BDull, 512u, 512u, 0x13u},
    {0xA5A58F8B1540184Full, 512u, 512u, 0x13u},
    {0x307DBE18E48C1E8Bull, 512u, 512u, 0x13u},
    {0x12060A392E8C6231ull, 512u, 512u, 0x13u},
    {0x11359618DED2A3F6ull, 512u, 512u, 0x13u},
    {0x7B54AC6672C00390ull, 512u, 512u, 0x13u},
    {0x50E1EDC598A80DD3ull, 512u, 512u, 0x13u},
    {0x5B0B861DA2F05F13ull, 512u, 512u, 0x13u},
    {0x5141C17CB77119C4ull, 512u, 512u, 0x13u},
    {0x89A12DC009425F4Dull, 512u, 512u, 0x13u},
}};

}
