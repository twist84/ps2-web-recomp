#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2::resource_pack
{

struct s_pack_info
{
    std::string m_id;
    std::string m_name;
    std::int32_t m_priority = 0;
    std::filesystem::path m_root;
};

struct s_resource
{
    std::string m_pack_id;
    std::filesystem::path m_path;
    std::uint64_t m_size = 0;
};

// A disc mount supplies the game identity and its adjacent resourcepacks/
// folder. Higher priority wins; ties are resolved by stable pack ID.
void mount(const std::filesystem::path &disc_root,
    const std::filesystem::path &executable_directory = {});
void clear();
const std::vector<s_pack_info> &packs();

// Logical keys are relative to assets/sdbz, e.g.
// "textures/dis/main_menu.pix.001.png". Never accepts absolute/traversal paths.
bool read_resource(const std::string &logical_path,
    std::vector<std::uint8_t> &data);
bool find_resource(const std::string &logical_path, s_resource &resource);

}
