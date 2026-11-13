#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps2::state { class Stream; }

namespace ps2::iop::arkd
{
struct s_asset
{
    std::uint32_t relative_sector = 0;
    std::uint32_t file_size = 0;
    // UINT32_MAX => legacy GAME.DAT sector allocation. Otherwise this
    // is an ID in the currently mounted direct GAME/ host-file index.
    std::uint32_t loose_id = UINT32_MAX;
};

// The original 0x501/0x101 bank reader examines the first DVD sector:
// +4 entry count, +8 declared SPU2 memory span, +0x0C pairs of u32s.
// These describe the bank's metadata, not raw PCM or decoded ADPCM.
struct s_sound_bank_entry
{
    std::uint32_t parameter = 0;
    std::uint32_t spu_offset = 0;
};

struct s_sound_bank_info
{
    std::uint32_t entry_count = 0;
    std::uint32_t spu_memory_bytes = 0;
    std::vector<s_sound_bank_entry> entries;
};

std::optional<s_sound_bank_info> inspect_sound_bank(
    const std::uint8_t *bytes, std::size_t size);

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
// Status of the separate 0x501/0x102-0x103 audio command worker.
// 0x10000000 = queued, 0x40000064 = unimplemented SPU2 operation.
std::uint32_t audio_worker_status();
bool sound_bank_ready();
std::uint32_t sound_bank_id();
std::uint32_t sound_upload_status();
std::size_t copy_sound_bank(void *destination, std::size_t capacity);

bool native_requested();
bool intercept_module(const std::string &path);
void install();
void state(ps2::state::Stream &stream);
}
