#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2::resource_pack
{

struct s_zip_entry
{
    std::string m_path;
    std::uint32_t m_offset = 0;
    std::uint32_t m_compressed_size = 0;
    std::uint32_t m_size = 0;
    std::uint32_t m_crc32 = 0;
    std::uint16_t m_method = 0;
};

struct s_pack_info
{
    std::string m_id;
    std::string m_name;
    std::int32_t m_priority = 0;
    std::filesystem::path m_root;
    bool m_archive = false;
    std::vector<s_zip_entry> m_entries;
};

struct s_resource
{
    std::string m_pack_id;
    std::filesystem::path m_path;
    std::uint64_t m_size = 0;
    bool m_archive = false;
    s_zip_entry m_entry;
};

// A disc mount supplies the game identity and its adjacent resourcepacks/
// folder. Higher priority wins; ties are resolved by stable pack ID.
void mount(const std::filesystem::path &disc_root,
    const std::filesystem::path &executable_directory = {});
void clear();
const std::vector<s_pack_info> &packs();

// All discovered compatible packs, including those disabled in load_order.txt.
// This list is a stable snapshot from the most recent mount.
const std::vector<s_pack_info> &discovered_packs();

// Persist the enabled packs in highest-to-lowest order and all remaining
// discovered packs as disabled. The new order takes effect on next boot.
bool save_selection(const std::vector<std::string> &enabled_ids);

// The SDL thread stages a successful save. Only the GS render thread may
// apply it, so currently-rendered textures never read a changing pack list.
// Returns true once when a new selection was installed.
bool selection_pending();
bool apply_pending_selection();

// Thread-safe snapshot for the SDL pack manager.
std::vector<s_pack_info> active_packs_snapshot();

// Logical keys are relative to assets/sdbz, e.g.
// "textures/dis/main_menu.pix.001.png". Never accepts absolute/traversal paths.
bool read_resource(const std::string &logical_path,
    std::vector<std::uint8_t> &data);
bool find_resource(const std::string &logical_path, s_resource &resource);

}
