#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ps2::state { class Stream; }

namespace ps2::iop::arkd
{
struct s_asset
{
    std::uint32_t relative_sector = 0;
    std::uint32_t file_size = 0;
};

class c_catalog
{
public:
    bool open();
    void clear();
    std::optional<s_asset> find(std::string_view name) const;
    std::size_t read(const s_asset &asset, std::uint32_t offset,
        void *destination, std::size_t capacity) const;
    std::size_t entry_count() const;

private:
    std::map<std::string, s_asset> m_assets;
    std::uint32_t m_game_sector = 0;
};

// Asset data staged by the 0x501/0x101 sound-bank worker. These accessors
// are for subsequent SPU2 upload work, not an EE-facing game API.
bool sound_bank_ready();
std::size_t copy_sound_bank(void *destination, std::size_t capacity);

bool native_requested();
bool intercept_module(const std::string &path);
void install();
void state(ps2::state::Stream &stream);
}
