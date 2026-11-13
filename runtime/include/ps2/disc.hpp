#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ps2::disc {

inline constexpr std::uint32_t kSector = 2048;

struct Entry {
    std::uint32_t sector;
    std::uint32_t size;
    bool directory;
};

// Host-backed GAME/ resources, independent of INFO.DAT or GAME.DAT.
// IDs are valid only for the currently mounted folder-backed disc.
struct LooseGameAsset
{
    std::uint32_t id = 0;
    std::uint32_t size = 0;
    std::string name;
};

std::vector<LooseGameAsset> loose_game_assets();
std::size_t read_loose_game_asset(std::uint32_t id,
    std::uint64_t offset, void *destination, std::size_t capacity);
void clear_loose_game_assets();

using Reader = std::function<std::size_t(std::uint64_t offset, void *out, std::size_t size)>;
using Probe = std::function<bool(std::uint64_t offset, std::size_t size)>;

bool open(const std::string &path);
bool open(const std::string &name, std::uint64_t size, Reader reader, Probe probe = {});
bool is_open();
bool read(std::uint32_t sector, std::uint32_t count, void *out);
bool ready(std::uint32_t sector, std::uint32_t count);
std::optional<Entry> find(const std::string &path);
std::uint32_t layer1_start();
std::uint64_t sector_count();

}
