#include "arika_dvd.hpp"

#include "kernel.hpp"
#include "sif.hpp"
#include "spu2.hpp"

#include "ps2/disc.hpp"
#include "ps2/memory.hpp"
#include "ps2/state.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ps2::iop::arkd
{
namespace
{
constexpr std::uint32_t k_record_bytes = 0x30;
constexpr std::uint32_t k_header_bytes = 0x30;
constexpr std::uint32_t k_sector_bytes = disc::kSector;
constexpr std::size_t k_max_info_size = 16u * 1024u * 1024u;
constexpr std::size_t k_read_chunk_sectors = 16;

c_catalog g_catalog;

// ARKD_DVD.IRX (module offsets 0x1A58 and 0x21B0) executes full-file
// requests asynchronously. These phases are observable through 0x503/2.
std::uint32_t g_transfer_state = 0x30000000u;
std::uint32_t g_transfer_serial = 0;
std::uint32_t g_control_value = 0x1000;
std::uint32_t g_transfer_block_bytes = 0;

struct s_pending_transfer
{
    s_asset asset;
    std::uint32_t destination = 0;
    std::uint32_t serial = 0;
    bool valid = false;
    // 0 = full file DMA, 1 = CD init, 2..9 = CDVD commands 1..8.
    // uint8_t preserves the previous v7 savestate field's one-byte size.
    std::uint8_t command = 0;
};

s_pending_transfer g_pending;

// 0x501/0x101 is an audio-bank load. The original IOP worker obtains
// the filename at request+4 and reads GAME.DAT through its file index.
// The native worker stages the exact declared payload for later SPU2
// processing; bank decoding and hardware upload remain separate tasks.
struct s_pending_bank
{
    s_asset asset;
    std::uint32_t bank_id = 0;
    std::uint32_t serial = 0;
    bool valid = false;
};

s_pending_bank g_pending_bank;
std::uint32_t g_bank_serial = 0;
std::uint32_t g_loaded_bank_id = 0;
bool g_bank_ready = false;
std::vector<std::uint8_t> g_sound_bank_data;

// 0x501/0x102 and /0x103 update the original IRX's per-audio-worker
// command selector and wake the same thread. The SPU2-backed worker is
// not ported yet. Track command admission and report an explicit error.
struct s_audio_worker
{
    std::uint32_t selector = 0;
    std::uint32_t serial = 0;
    std::uint32_t state = 0x30000000u;
    std::uint8_t opcode = 0;
    bool pending = false;
};
s_audio_worker g_audio_worker;

constexpr std::size_t k_max_bank_size = 64u * 1024u * 1024u;
constexpr std::uint32_t k_spu_limit = 2u * 1024u * 1024u;
// Default ARKD mode 1 reserves 0x180000 onward for streaming/effects.
constexpr std::uint32_t k_spu_bank_limit = 0x180000;
constexpr std::uint32_t k_sound_bank_slots = 16;
constexpr std::uint32_t k_iop_stage_bytes = 0x8000;
constexpr std::uint32_t k_spu_base_start = 0x5050;

// The audio-bank loader follows the original 0x5894 worker: an
// initial 0x800-byte metadata sector, then contiguous sample data.
struct s_spu_bank_allocation
{
    std::uint32_t base = 0;
    std::uint32_t size = 0;
};

struct s_spu_upload
{
    std::uint32_t bank_id = 0;
    std::uint32_t spu_address = 0;
    std::uint32_t staging_iop = 0;
    std::uint32_t data_size = 0;
    std::uint32_t offset = 0;
    std::uint32_t chunk = 0;
    std::uint32_t serial = 0;
    std::uint32_t polls = 0;
    std::uint32_t state = 0x30000000u;
    bool pending = false;
    bool waiting_for_dma = false;
};

std::array<s_spu_bank_allocation, k_sound_bank_slots> g_spu_banks{};
// Multiple sound banks remain loaded concurrently. Keep the original
// archive payload for each slot so 0x501/0x103 can re-upload an older
// bank after another bank has been loaded.
std::array<std::vector<std::uint8_t>, k_sound_bank_slots> g_cached_banks{};
constexpr std::size_t k_max_total_bank_cache = 128u * 1024u * 1024u;
std::uint32_t g_spu_next = k_spu_base_start;
// libsd DMA reads this source asynchronously. Keep it allocated for the
// lifetime of the IOP boot, including after the initial transfer completes.
std::uint32_t g_silence_staging = 0;
bool g_native_active = false;
s_spu_upload g_spu_upload;
// 0 = free, 1 = bank, 2 = stream, 3 = silence. A channel owner retains its IOP
// source until sceSdVoiceTransStatus reports completion.
std::array<std::uint8_t, 2> g_dma_owner{};

bool dma_channel_available(std::uint32_t channel)
{
    if (g_dma_owner[channel] == 3)
    {
        std::uint32_t done = 0;
        if (kernel::call_library_export("libsd", 19, channel, 0, 0, 0, done)
            && done) g_dma_owner[channel] = 0;
    }
    return g_dma_owner[channel] == 0;
}

void fill_silent_loop(std::uint8_t *bytes)
{
    std::memset(bytes, 0, 64);
    bytes[1] = 1;
    bytes[17] = bytes[33] = bytes[49] = 7;
}

struct s_ams_stream
{
    std::array<std::uint8_t, 0x17C> playlist{};
    s_asset asset{};
    std::uint32_t serial = 0, staging = 0;
    std::uint32_t segment = 0, repeats = 0, block = 0, consumed = 0;
    std::uint32_t length = 0, folded_blocks = 0, pitch = 0x1000;
    std::uint32_t channel_gain = 0x4000;
    std::uint32_t fade_total = 1, fade_step = 1, fade = 0x8000;
    std::uint32_t out_total = 0, out_step = 0, out_base = 0x8000;
    std::uint32_t error = 0, polls = 0;
    std::array<std::uint32_t, 2> half_pitch{};
    std::array<std::uint32_t, 2> half_bytes{};
    std::array<bool, 2> last_half{};
    std::uint8_t phase = 0, channel = 0, target_half = 0, read_half = 0;
    std::uint8_t request = 0; // 1 = replace playlist, 2 = stop.
    bool running = false, paused = false, waiting = false, eof = false;
};
s_ams_stream g_stream;

// The original bank-release worker compacts sample memory. Native
// allocations retain stable SPU2 addresses, reuse holes, and never move
// an allocation while voices could still reference it.
bool allocate_spu_bank(std::uint32_t id, std::uint32_t bytes)
{
    if (id >= k_sound_bank_slots || !bytes) return false;
    auto &selected = g_spu_banks[id];
    if (selected.base)
    {
        if (selected.size >= bytes) return true;
        // Moving an active bank would invalidate SPU2 start/loop
        // addresses already assigned to voices. Reject growth until
        // the game explicitly releases the allocation via 0x501/0x102.
        return false;
    }

    std::uint32_t cursor = k_spu_base_start;
    for (std::uint32_t iteration = 0; iteration <= k_sound_bank_slots;
         ++iteration)
    {
        std::uint32_t next_base = k_spu_bank_limit;
        std::uint32_t next_end = k_spu_bank_limit;
        for (std::uint32_t index = 0; index < k_sound_bank_slots; ++index)
        {
            if (index == id) continue;
            const auto &bank = g_spu_banks[index];
            if (!bank.base || bank.base < cursor
                || bank.base >= next_base)
                continue;
            next_base = bank.base;
            next_end = bank.base + bank.size;
        }

        if (cursor <= next_base && bytes <= next_base - cursor)
        {
            selected = {cursor, bytes};
            g_spu_next = std::max(g_spu_next, cursor + bytes);
            return true;
        }
        if (next_base == k_spu_bank_limit || next_end <= cursor
            || next_end > k_spu_bank_limit)
            return false;
        cursor = next_end;
    }
    return false;
}

bool release_spu_bank(std::uint32_t id)
{
    if (id >= k_sound_bank_slots || !g_spu_banks[id].base)
        return false;
    g_spu_banks[id] = {};
    g_spu_next = k_spu_base_start;
    for (const auto &bank : g_spu_banks)
    {
        if (bank.base)
            g_spu_next = std::max(g_spu_next,
                bank.base + bank.size);
    }
    return true;
}

struct s_sound_rpc_state
{
    // Original IRX 0x4A10 / 0x4BB8: alternating copies of two command
    // structures. Each update selects one of two buffers.
    std::array<std::array<std::uint8_t, 0x2E0>, 2> voice_commands{};
    std::array<std::array<std::uint8_t, 0x33C>, 2> sound_commands{};
    std::array<std::array<std::uint32_t, 2>, 2> command_flags{};
    std::array<std::uint32_t, 2> active_voice_masks{};
    std::array<std::uint8_t, 0x17C> stream_commands{};

    std::uint32_t buffer_generation = 0;
    std::uint32_t submitted_generation = 0;
    std::uint32_t control_flags = 0;
    std::uint32_t streaming_flags = 0;
    std::uint32_t stream_control = 0;
    std::uint32_t stream_status = 0;
    std::uint16_t master_volume = 0;
    std::uint16_t channel_volume_left = 0;
    std::uint16_t channel_volume_right = 0;
    std::uint16_t current_stream = 0;
    std::uint16_t pending_stream = 0;
    std::uint16_t stream_pair_left = 0;
    std::uint16_t stream_pair_right = 0;
};
s_sound_rpc_state g_sound;

// IRX globals 0x13258/0x1325A hold one pair of source gain words
// per physical voice, independent of the alternating command buffers.
std::array<std::array<std::uint16_t, 2>, 46> g_voice_source_volumes{};
// IRX 0x39C0: +14 == 0 requests a one-time silent loop address.
// Per-hardware-core masks differ from the 32-bit EE reporting masks.
std::array<std::uint32_t, 2> g_voice_end_redirect{};

void reset_sound_state()
{
    g_sound = {};
    for (auto &buffer : g_sound.voice_commands)
        for (std::size_t index = 0; index < 46; ++index)
            std::fill_n(buffer.data() + index * 16, 4, 0xFF);
    g_voice_source_volumes = {};
    g_voice_end_redirect = {};
    // Original IRX 0x6920-0x6970 initializes these three 14-bit
    // coefficients to 0x3FFF. Zero-initializing them would mute all
    // subsequent sceSdSetParam voice-volume updates.
    g_sound.master_volume = 0x3FFF;
    g_sound.channel_volume_left = 0x3FFF;
    g_sound.channel_volume_right = 0x3FFF;
    g_sound.stream_status = 0x30000000u;
    g_sound.current_stream = 0x4000;
    g_sound.pending_stream = 0x3FFF;
    g_sound.stream_pair_left = g_sound.stream_pair_right = 0x3FFF;
}

std::uint32_t read_u32(const std::uint8_t *bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool cp932_lead(std::uint8_t byte)
{
    return (byte >= 0x81 && byte <= 0x9F)
        || (byte >= 0xE0 && byte <= 0xFC);
}

std::string name_key(std::string_view source)
{
    std::string key(source);
    for (std::size_t index = 0; index < key.size(); ++index)
    {
        const auto byte = static_cast<std::uint8_t>(key[index]);
        if (cp932_lead(byte))
        {
            ++index;
            continue;
        }
        if (key[index] == '/') key[index] = '\\';
        if (byte >= 'A' && byte <= 'Z')
            key[index] = static_cast<char>(byte + ('a' - 'A'));
    }
    return key;
}

bool ee_range(std::uint32_t address, std::uint32_t length)
{
    const std::uint32_t offset = address & (ps2::kRamSize - 1);
    return length <= ps2::kRamSize - offset;
}

void reply_u32(std::uint32_t ret, std::uint32_t length, std::uint32_t value)
{
    if (length < 4 || !ee_range(ret, 4)) return;
    std::memcpy(ps2::g_ram + (ret & (ps2::kRamSize - 1)), &value, 4);
    ps2::watch_span(ret, 4, "arkd:native_rpc");
}

void reply_pair(std::uint32_t ret, std::uint32_t length,
    std::uint32_t first, std::uint32_t second)
{
    if (length >= 4) reply_u32(ret, length, first);
    if (length >= 8) reply_u32(ret + 4, length - 4, second);
}

std::uint16_t scale_voice_gain(std::uint16_t source,
    std::uint16_t master, std::uint16_t channel)
{
    // Original IRX 0x4F78-0x4FB4 and 0x4FEC-0x5028:
    // MIPS mult/mflo, logical shift by 14, multiply, shift by 14.
    // Use unsigned 32-bit arithmetic to preserve guest wrap semantics.
    const std::uint32_t first =
        (static_cast<std::uint32_t>(source) * master) >> 14;
    return static_cast<std::uint16_t>((first * channel) >> 14);
}

void refresh_live_voice_gains()
{
    // IRX 0x4DBC runs immediately when the master/channel/mixing
    // controls change, but only touches voices marked active.
    if (!(g_sound.active_voice_masks[0] | g_sound.active_voice_masks[1]))
        return;
    if (!kernel::library_exists("libsd"))
    {
        std::fprintf(stderr,
            "[arkd-native] cannot refresh voice gains without libsd\n");
        return;
    }

    for (std::uint32_t index = 0; index < g_voice_source_volumes.size(); ++index)
    {
        const std::uint32_t word = index >> 5;
        const std::uint32_t bit = 1u << (index & 31u);
        if (!(g_sound.active_voice_masks[word] & bit))
            continue;

        const std::uint32_t core = index / 24u;
        const std::uint32_t voice = index % 24u;
        const std::uint32_t reg = core | (voice << 1u);
        const auto &source = g_voice_source_volumes[index];

        // IRX 0x4EC0-0x4F50: when mono/mix-disable bit 16 is set,
        // combine both source words for the first register and zero
        // the second. Otherwise source +10 drives the first register
        // and source +8 the second register.
        const bool mono = (g_sound.streaming_flags & 0x10000u) != 0;
        const std::uint16_t first = mono
            ? static_cast<std::uint16_t>(source[0] | source[1])
            : source[1];
        const std::uint16_t second = mono ? 0u : source[0];
        const std::uint16_t first_gain = scale_voice_gain(
            first, g_sound.master_volume, g_sound.channel_volume_left);
        const std::uint16_t second_gain = scale_voice_gain(
            second, g_sound.master_volume, g_sound.channel_volume_right);

        std::uint32_t result = 0;
        if (!kernel::call_library_export("libsd", 5,
                reg, first_gain, 0, 0, result)
            || !kernel::call_library_export("libsd", 5,
                reg | 0x100u, second_gain, 0, 0, result))
        {
            std::fprintf(stderr,
                "[arkd-native] live voice gain update failed (voice %u)\n",
                index);
        }
    }
}

bool start_voice(std::uint32_t parity, std::uint32_t index,
    const s_voice_command *update = nullptr)
{
    auto &buffer = g_sound.voice_commands[parity];
    const auto command = update ? std::optional<s_voice_command>(*update)
        : inspect_voice_command(buffer.data(), buffer.size(), index);
    if (!command || command->sample_selector == UINT32_MAX) return false;
    const std::uint32_t bank_id = command->sample_selector & 0xFFFFu;
    if (bank_id >= k_sound_bank_slots || !g_spu_banks[bank_id].base
        || (g_spu_upload.pending && g_spu_upload.bank_id == bank_id))
        return false;
    const auto &source = g_cached_banks[bank_id];
    const auto bank = inspect_sound_bank(source.data(), source.size());
    if (!bank || command->control >= bank->entries.size()) return false;
    const auto &sample = bank->entries[command->control];
    const std::uint32_t address = g_spu_banks[bank_id].base + sample.spu_offset;
    static const bool trace_voices = std::getenv("SDBZ_TRACE_ARKD_VOICES") != nullptr;
    if (trace_voices)
        std::fprintf(stderr,
            "[arkd-voice] start index=%u bank=%u sample=%u restart=%u effect=%u address=0x%05X\n",
            index, bank_id, command->control, command->restart_control,
            command->start_control, address);
    const auto base_pitch = static_cast<std::uint16_t>(
        (sample.parameter * g_control_value) >> 12);
    const std::uint32_t reg = index / 24u | ((index % 24u) << 1u);
    std::uint32_t result = 0;
    // IRX 0x4420-0x4508: start address, zero next address, then pitch.
    // 0x6CCC/0x6CFC supplies these envelope defaults at initialization.
    if (!kernel::call_library_export("libsd", 9, reg | 0x2040u, address, 0, 0, result)
        || !kernel::call_library_export("libsd", 9, reg | 0x2140u,
            0, 0, 0, result)
        || !kernel::call_library_export("libsd", 5, reg | 0x200u,
            calculate_voice_pitch(base_pitch, command->pitch_request), 0, 0, result))
        return false;
    record_voice_volume_state(index, command->volume_0, command->volume_1);
    // IRX 0x3AE4 consumes the sample selector exactly once.
    if (!update) std::fill_n(buffer.data() + index * 16, 4, 0xFF);
    return true;
}

void update_voice(std::uint32_t index, const s_voice_command &command,
    std::uint16_t flags)
{
    const std::uint32_t reg = index / 24u | ((index % 24u) << 1u);
    std::uint32_t result = 0;
    // IRX 0x3BCC: bit 0 (restart) or bit 1 requests pitch.
    if (flags & 2u)
    {
        const auto bank_id = command.sample_selector & 0xFFFFu;
        if (bank_id < k_sound_bank_slots)
        {
            const auto &bytes = g_cached_banks[bank_id];
            const auto bank = inspect_sound_bank(bytes.data(), bytes.size());
            if (bank && command.control < bank->entries.size())
            {
                const auto pitch = static_cast<std::uint16_t>(
                    (bank->entries[command.control].parameter * g_control_value) >> 12);
                kernel::call_library_export("libsd", 5, reg | 0x200u,
                    calculate_voice_pitch(pitch, command.pitch_request), 0, 0, result);
            }
        }
    }
    // IRX 0x4094: bit 0 or bit 2 requests source volume updates.
    if (flags & 4u)
    {
        g_voice_source_volumes[index] = {command.volume_0, command.volume_1};
        const bool mono = (g_sound.streaming_flags & 0x10000u) != 0;
        const auto first = mono ? static_cast<std::uint16_t>(command.volume_0 | command.volume_1)
            : command.volume_1;
        const auto second = mono ? 0u : command.volume_0;
        kernel::call_library_export("libsd", 5, reg,
            scale_voice_gain(first, g_sound.master_volume, g_sound.channel_volume_left),
            0, 0, result);
        kernel::call_library_export("libsd", 5, reg | 0x100u,
            scale_voice_gain(static_cast<std::uint16_t>(second), g_sound.master_volume,
                g_sound.channel_volume_right), 0, 0, result);
    }
}

void poll_voice_status()
{
    // IRX 0x2CD8-0x2FA4: consult ENDX and loop addresses, then
    // publish 46 active bits as two 32-bit EE SREG words.
    auto reported = g_sound.active_voice_masks;
    for (std::uint32_t core = 0; core < 2; ++core)
    {
        std::uint32_t ended = 0;
        if (!kernel::call_library_export("libsd", 8, 0x1700u | core, 0, 0, 0, ended))
            continue;
        for (std::uint32_t voice = 0; voice < 24; ++voice)
        {
            const auto index = core * 24 + voice;
            if (index >= 46) break;
            const auto bit = 1u << voice;
            const auto mask_bit = 1u << (index & 31u);
            if (!(g_sound.active_voice_masks[index >> 5] & mask_bit)) continue;
            const auto reg = core | (voice << 1);
            std::uint32_t result = 0;
            if (g_voice_end_redirect[core] & bit)
            {
                if (!kernel::call_library_export("libsd", 9, reg | 0x2140u,
                        0x5010, 0, 0, result)) continue;
                g_voice_end_redirect[core] &= ~bit;
            }
            std::uint32_t loop_address = 0;
            if (!kernel::call_library_export("libsd", 10, reg | 0x2140u,
                    0, 0, 0, loop_address)) continue;
            if (loop_address < 0x5050u && (ended & bit))
            {
                g_sound.active_voice_masks[index >> 5] &= ~mask_bit;
                reported[index >> 5] &= ~mask_bit;
            }
        }
    }
    sif::publish_ee_sreg(14, reported[0]);
    sif::publish_ee_sreg(15, reported[1]);
}

void process_voice_commands(const kernel::Job &job)
{
    if (job.b == 1) { poll_voice_status(); return; }
    // IRX 0x3020-0x49EC: stop requests take precedence over starts
    // and live updates. Repeat/end handling remains a separate task.
    // IRX 0x3020-0x3060 consumes one pending submission, then selects the
    // buffer from the current global generation. It intentionally does not
    // retain the parity that was current when the worker was signalled: if
    // both alternating slots arrive before it runs, stale commands are
    // skipped in favour of the newest slot.
    if (!g_sound.submitted_generation) return;
    --g_sound.submitted_generation;
    const std::uint32_t parity = g_sound.buffer_generation & 1u;
    if (!kernel::library_exists("libsd"))
    {
        std::fprintf(stderr,
            "[arkd-native] voice worker needs libsd for sound register writes\n");
        return;
    }
    std::array<std::uint32_t, 2> key_off_masks{};
    std::array<std::uint32_t, 2> key_on_masks{};
    std::array<std::uint32_t, 2> effect_set{}, effect_clear{};
    const auto remember_effect = [&](std::uint32_t index, const s_voice_command &command)
    {
        const auto core = index / 24u, bit = 1u << (index % 24u);
        if (command.restart_control == 0) g_voice_end_redirect[core] |= bit;
        else g_voice_end_redirect[core] &= ~bit;
        // The later update of a voice supersedes an earlier start in
        // the same worker pass, matching the original per-core masks.
        if (command.start_control)
        {
            effect_set[core] |= bit;
            effect_clear[core] &= ~bit;
        }
        else
        {
            effect_clear[core] |= bit;
            effect_set[core] &= ~bit;
        }
    };
    for (std::uint32_t index = 0; index < 46; ++index)
    {
        // IRX 0x3194-0x31E8 indexes request masks by index / 32 and
        // index % 32, while 0x3110-0x316C constructs the libsd
        // register ID using index / 24 and index % 24. These are
        // deliberately different addressing schemes.
        const std::uint32_t mask_word = index >> 5;
        const std::uint32_t mask_bit = 1u << (index & 31u);
        const std::uint32_t core = index / 24;
        const std::uint32_t voice = index % 24;
        // IRX 0x3B2C/0x3B34: flags[46] precede records[46].
        auto &updates = g_sound.sound_commands[parity];
        auto *record = updates.data() + static_cast<std::size_t>(index) * 2;
        const std::uint16_t flags = static_cast<std::uint16_t>(
            record[0] | (static_cast<std::uint16_t>(record[1]) << 8));
        if (!(g_sound.command_flags[parity][mask_word] & mask_bit)
            && !(flags & 0x8u))
        {
            if (start_voice(parity, index))
            {
                key_on_masks[core] |= 1u << voice;
                remember_effect(index, *inspect_voice_command(
                    g_sound.voice_commands[parity].data(), 0x2E0, index));
            }
            if (flags)
            {
                const auto update = inspect_voice_command(updates.data() + 0x5C,
                    updates.size() - 0x5C, index);
                if (update)
                {
                    if ((flags & 1u) && start_voice(parity, index, &*update))
                    {
                        key_on_masks[core] |= 1u << voice;
                        remember_effect(index, *update);
                    }
                    update_voice(index, *update, flags);
                }
                record[0] = record[1] = 0;
            }
            continue;
        }

        const std::uint32_t register_id = core | (voice << 1);
        std::uint32_t result = 0;
        if (!kernel::call_library_export("libsd", 5,
                register_id, 0, 0, 0, result)
            || !kernel::call_library_export("libsd", 5,
                register_id | 0x100u, 0, 0, 0, result))
        {
            std::fprintf(stderr,
                "[arkd-native] libsd voice mute unavailable (voice %u)\n",
                index);
            continue;
        }
        key_off_masks[core] |= 1u << voice;
        g_sound.active_voice_masks[mask_word] &= ~mask_bit;
        g_voice_end_redirect[core] &= ~(1u << voice);
        g_sound.command_flags[parity][mask_word] &= ~mask_bit;
        record[0] &= static_cast<std::uint8_t>(~0x8u);
    }

    if (key_on_masks[0] || key_on_masks[1]) refresh_live_voice_gains();
    // IRX 0x46D4-0x4708 sends key-on before the accumulated key-offs.
    for (std::uint32_t core = 0; core < 2; ++core)
    {
        if (!key_on_masks[core]) continue;
        std::uint32_t result = 0;
        if (!kernel::call_library_export("libsd", 7, 0x1500u | core,
                key_on_masks[core], 0, 0, result))
            std::fprintf(stderr,
                "[arkd-native] libsd voice key-on unavailable (core %u)\n", core);
    }

    // The SPU2 key-on operation clears its explicit-LSAX latch so ADPCM
    // loop-start flags can establish the repeat point. Apply one-shot
    // redirects after key-on: doing this before key-on lets the first decoded
    // block replace 0x5010 and short victory voices repeat before vblank.
    for (std::uint32_t core = 0; core < 2; ++core)
    {
        const std::uint32_t redirects = key_on_masks[core]
            & g_voice_end_redirect[core];
        for (std::uint32_t voice = 0; voice < 24; ++voice)
        {
            const std::uint32_t bit = 1u << voice;
            if (!(redirects & bit)) continue;
            std::uint32_t result = 0;
            if (kernel::call_library_export("libsd", 9,
                    core | (voice << 1u) | 0x2140u,
                    0x5010u, 0, 0, result))
                g_voice_end_redirect[core] &= ~bit;
        }
    }

    // IRX 0x4730-0x4784 accumulates stopped voices per SPU2 core
    // and calls sceSdSetSwitch (libsd export #7) on SD_S_KOFF
    // (0x1600 | core). Volume zeroing alone does not stop a voice.
    for (std::uint32_t core = 0; core < 2; ++core)
    {
        if (!key_off_masks[core]) continue;
        std::uint32_t result = 0;
        if (!kernel::call_library_export("libsd", 7,
                0x1600u | core, key_off_masks[core], 0, 0, result))
            std::fprintf(stderr,
                "[arkd-native] libsd voice key-off unavailable (core %u)\n",
                core);
    }
    // IRX 0x4860-0x4994 preserves unrelated left/right effect bits.
    for (std::uint32_t core = 0; core < 2; ++core)
    {
        if (!(effect_set[core] | effect_clear[core])) continue;
        for (const std::uint32_t selector : {0x1900u, 0x1B00u})
        {
            std::uint32_t previous = 0, result = 0;
            if (kernel::call_library_export("libsd", 8, selector | core, 0, 0, 0, previous))
                kernel::call_library_export("libsd", 7, selector | core,
                    (previous & ~effect_clear[core]) | effect_set[core], 0, 0, result);
        }
    }
}

bool file_size_rpc(std::uint32_t args, std::uint32_t args_size,
    std::uint32_t ret, std::uint32_t ret_size)
{
    // 0x503 / method 3: IRX 0x1760 passes args+4 to its INFO filename
    // lookup (0x0A60), then returns file_size and its 16-byte alignment.
    if (args_size <= 4 || !ee_range(args, args_size))
    {
        reply_pair(ret, ret_size, 0x80000064u, 0);
        return true;
    }
    const auto *bytes = ps2::g_ram + (args & (ps2::kRamSize - 1)) + 4;
    const std::size_t available = std::min<std::size_t>(args_size - 4, 32);
    const void *end = std::memchr(bytes, 0, available);
    if (!end)
    {
        reply_pair(ret, ret_size, 0x80000064u, 0);
        return true;
    }
    const auto length = static_cast<const std::uint8_t *>(end) - bytes;
    const std::string name(reinterpret_cast<const char *>(bytes), length);
    const auto asset = g_catalog.find(name);
    if (!asset)
    {
        reply_pair(ret, ret_size, 0x80000064u, 0);
        return true;
    }
    reply_pair(ret, ret_size, asset->file_size,
        (asset->file_size + 15u) & ~15u);
    return true;
}

void complete_native_read(const kernel::Job &job)
{
    if (!g_pending.valid || job.a != g_pending.serial) return;
    const s_pending_transfer transfer = g_pending;
    g_pending.valid = false;

    // 0x503/0x102 and 0x503/0x103 are drive commands rather than
    // file reads. In the virtual-disc backend the eight motor/status
    // operations share the same always-mounted media and completion
    // semantics. Exact mechanical error conditions remain unmodelled.
    if (transfer.command)
    {
        if (transfer.command == 1)
        {
            const auto index = g_catalog.ee_index();
            if (!ee_range(transfer.destination, static_cast<std::uint32_t>(index.size())))
            {
                g_transfer_state = 0x40000064u;
                return;
            }
            std::memcpy(ps2::g_ram + (transfer.destination & (ps2::kRamSize - 1)),
                index.data(), index.size());
            ps2::watch_span(transfer.destination, static_cast<std::uint32_t>(index.size()),
                "arkd:index_dma");
        }
        g_transfer_state = disc::is_open() ? 0x30000000u : 0x40000190u;
        return;
    }

    if (!ee_range(transfer.destination, transfer.asset.file_size))
    {
        g_transfer_state = 0x40000064u;
        return;
    }
    g_transfer_state = 0x20000000u;
    auto *destination = ps2::g_ram + (transfer.destination & (ps2::kRamSize - 1));
    const std::size_t got = g_catalog.read(
        transfer.asset, 0, destination, transfer.asset.file_size);
    if (got != transfer.asset.file_size)
    {
        g_transfer_state = 0x40000064u;
        return;
    }
    ps2::watch_span(transfer.destination, transfer.asset.file_size,
        "arkd:native_file_dma");
    g_transfer_state = 0x30000000u;
}

bool native_full_file_read(std::uint32_t args, std::uint32_t args_size,
    std::uint32_t ret, std::uint32_t ret_size)
{
    // 0x503/0x101 -> IRX 0x2410 -> worker 0x21B0 -> 0x1A58.
    // At 0x2410 the IRX copies 0x24 bytes: EE destination (u32) and
    // the 32-byte INFO filename. At 0x1A58 it opens that filename,
    // reads it in chunks and DMAs the result to the EE destination.
    if (args_size < 0x24 || !ee_range(args, 0x24))
    {
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }
    if (g_pending.valid)
    {
        reply_u32(ret, ret_size, 0x80000065u);
        return true;
    }

    const auto *request = ps2::g_ram + (args & (ps2::kRamSize - 1));
    const auto *name = request + 4;
    const auto *end = static_cast<const std::uint8_t *>(std::memchr(name, 0, 32));
    const auto name_size = end ? static_cast<std::size_t>(end - name) : 32;
    const auto asset = g_catalog.find(
        std::string_view(reinterpret_cast<const char *>(name), name_size));
    if (!asset)
    {
        g_transfer_state = 0x40000064u;
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }

    const std::uint32_t destination = read_u32(request);
    if (!ee_range(destination, asset->file_size))
    {
        g_transfer_state = 0x40000064u;
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }

    g_pending = {*asset, destination, ++g_transfer_serial, true, 0};
    g_transfer_state = 0x10000000u;
    const auto duration = kernel::usec_to_cycles(400)
        + static_cast<std::uint64_t>(
            (asset->file_size + k_sector_bytes - 1) / k_sector_bytes)
            * kernel::usec_to_cycles(200);
    kernel::schedule(kernel::now() + duration,
        kernel::Job{kernel::kJobArkdRead, g_pending.serial});
    reply_u32(ret, ret_size, 0);
    return true;
}

bool sound_effect_rpc(std::uint32_t args, std::uint32_t args_size,
    std::uint32_t ret, std::uint32_t ret_size)
{
    // IRX 0x4C84: two 16-byte RPC descriptors become 20-byte
    // sceSdEffectAttr structures with a core word prepended.
    if (args_size < 32 || !ee_range(args, 32))
    {
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }
    if (!kernel::library_exists("libsd"))
    {
        std::fprintf(stderr,
            "[arkd-native] 0x501/0x11: libsd export unavailable\n");
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }
    const std::uint32_t staging = kernel::alloc(20);
    if (!staging || (iop::phys(staging) & (iop::kRamSize - 1)) > iop::kRamSize - 20)
    {
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }
    bool success = true;
    const auto *input = ps2::g_ram + (args & (ps2::kRamSize - 1));
    for (std::uint32_t core = 0; core < 2; ++core)
    {
        std::memcpy(iop::ram_ptr(staging), &core, 4);
        std::memcpy(iop::ram_ptr(staging) + 4, input + core * 16, 16);
        std::uint32_t result = 0;
        if (!kernel::call_library_export("libsd", 23, core, staging, 0, 0, result))
        {
            success = false;
            break;
        }
    }
    kernel::free(staging);
    reply_u32(ret, ret_size, success ? 0 : 0x80000064u);
    return true;
}

void begin_spu_upload();

bool initialize_sound_hardware()
{
    // Original 0x6B0C setup, using its default mode 1. The guest libsd
    // owns hardware layout, DMA hooks and effect initialization.
    std::uint32_t result = 0;
    if (!kernel::call_library_export("libsd", 4, 0, 0, 0, 0, result)
        || static_cast<std::int32_t>(result) < 0) return false;
    if (g_stream.staging) kernel::free(g_stream.staging);
    const auto next_stream_serial = g_stream.serial + 1;
    g_stream = {};
    g_stream.serial = next_stream_serial;
    g_dma_owner = {};
    bool success = true;
    const auto call = [&](std::uint32_t ordinal, std::uint32_t reg, std::uint32_t value)
    {
        success &= kernel::call_library_export("libsd", ordinal, reg, value, 0, 0, result);
    };
    const auto staging = kernel::alloc(20);
    if (!staging) return false;
    if ((iop::phys(staging) & (iop::kRamSize - 1)) > iop::kRamSize - 20)
    {
        kernel::free(staging);
        return false;
    }
    for (std::uint32_t core = 0; core < 2; ++core)
    {
        call(5, 0x980u | core, 0x3FFF);
        call(5, 0xA80u | core, 0x3FFF);
        call(5, 0x800u | core, core ? 0xFCC : 0xFC0);
        call(7, 0x1700u | core, 0);
        for (std::uint32_t voice = 0; voice < 24; ++voice)
        {
            const auto reg = core | (voice << 1);
            call(5, reg, 0);
            call(5, reg | 0x100u, 0);
            call(5, reg | 0x200u, 0x1000);
            call(9, reg | 0x2040u, 0x5010);
            call(5, reg | 0x300u, 0x80FF);
            call(5, reg | 0x400u, 0x1FC0);
        }
        call(11, 2u | core, 0);
        call(9, 0x1D00u | core, core ? 0x1DFFFF : 0x1FFFFF);
        const std::array<std::uint32_t, 5> effect{core, 0x105, 0xFFF00010, 10, 20};
        std::memcpy(iop::ram_ptr(staging), effect.data(), sizeof(effect));
        success &= kernel::call_library_export("libsd", 23, core, staging, 0, 0, result);
        call(11, 4u | core, 1);
        call(11, 2u | core, 1);
        call(5, 0xB80u | core, 0x3FFF);
        call(5, 0xC80u | core, 0x3FFF);
        call(7, 0x1800u | core, 0xFFFFFF);
        call(7, 0x1A00u | core, 0xFFFFFF);
        call(7, 0x1900u | core, 0);
        call(7, 0x1B00u | core, 0);
    }
    kernel::free(staging);
    if (!g_silence_staging)
        g_silence_staging = kernel::alloc(64);
    if (!g_silence_staging || iop::phys(g_silence_staging) > iop::kRamSize - 64)
        return false;
    auto *silence = iop::ram_ptr(g_silence_staging);
    fill_silent_loop(silence);
    // The original module uploads four silent ADPCM blocks at 0x5010.
    // Without their end flags, redirected voices run into the first bank.
    const bool transferred = kernel::call_library_export("libsd", 17, 0, 0,
        g_silence_staging, 0x5010, result, 64);
    success &= transferred && static_cast<std::int32_t>(result) >= 0;
    if (transferred && static_cast<std::int32_t>(result) >= 0)
        g_dma_owner[0] = 3;
    return success;
}

constexpr std::uint32_t k_stream_half = 0x8000;
constexpr std::uint32_t k_stream_left = 0x180000;
constexpr std::uint32_t k_stream_right = 0x190000;

void schedule_stream(std::uint64_t delay = 500)
{
    kernel::schedule(kernel::now() + kernel::usec_to_cycles(delay),
        kernel::Job{kernel::kJobArkdStream, g_stream.serial});
}

bool stream_call(std::uint32_t ordinal, std::uint32_t reg,
    std::uint32_t value = 0)
{
    std::uint32_t result = 0;
    return kernel::call_library_export("libsd", ordinal, reg, value, 0, 0, result);
}

void publish_stream_status()
{
    sif::publish_ee_sreg(13, (g_stream.running ? 1u : 0u)
        | (g_stream.paused ? 2u : 0u) | (g_stream.request ? 0x200u : 0u));
}

void stream_gains()
{
    // IRX 0x7FE4: three independently truncated scaling stages.
    const std::uint32_t base =
        (static_cast<std::uint32_t>(g_sound.pending_stream) * g_stream.channel_gain) >> 14;
    const auto scale = [&](std::uint16_t channel)
    {
        return static_cast<std::uint16_t>((((base * g_stream.fade) >> 15) * channel) >> 14);
    };
    const bool mono = (g_sound.streaming_flags & 2u) != 0;
    stream_call(5, 0x12D, mono ? 0 : scale(g_sound.stream_pair_right));
    stream_call(5, 0x02D, mono ? scale(g_sound.stream_pair_right) : 0);
    stream_call(5, 0x12F, 0);
    stream_call(5, 0x02F, scale(g_sound.stream_pair_left));
}

void stop_stream(std::uint32_t error = 0)
{
    if (error)
        std::fprintf(stderr,
            "[arkd-native] AMS stream error 0x%08X (phase %u, segment %u, "
            "block %u, read half %u, target half %u, DMA channel %u)\n",
            error, g_stream.phase, g_stream.segment, g_stream.block,
            g_stream.read_half, g_stream.target_half, g_stream.channel);
    stream_call(7, 0x1601, 0xC00000);
    g_stream.running = false;
    g_stream.paused = false;
    g_stream.phase = 0;
    g_stream.error = error;
    g_stream.fade = 0;
    g_stream.out_total = 0;
    g_sound.stream_status = error ? error : 0x30000000u;
    stream_gains();
    publish_stream_status();
}

bool open_stream_segment(bool reset_repeats)
{
    const auto count = read_u32(g_stream.playlist.data() + 4);
    if (!count || count > 10 || g_stream.segment >= count) return false;
    const auto *entry = g_stream.playlist.data() + 20 + g_stream.segment * 36;
    const auto *end = static_cast<const std::uint8_t *>(std::memchr(entry + 4, 0, 32));
    const auto length = end ? static_cast<std::size_t>(end - entry - 4) : 32;
    const auto asset = g_catalog.find(std::string_view(
        reinterpret_cast<const char *>(entry + 4), length));
    if (!asset || asset->file_size < 0x800) return false;
    std::array<std::uint8_t, 0x800> header{};
    if (g_catalog.read(*asset, 0, header.data(), header.size()) != header.size()) return false;
    const auto bytes = read_u32(header.data() + 8);
    const auto blocks = read_u32(header.data() + 12);
    if (!bytes || (bytes & 15u) || !blocks
        || static_cast<std::uint64_t>(bytes) > static_cast<std::uint64_t>(blocks) * 0x10000u
        || 0x800ull + static_cast<std::uint64_t>(blocks) * 0x20000u > asset->file_size)
        return false;
    g_stream.asset = *asset;
    g_stream.length = bytes;
    g_stream.folded_blocks = blocks;
    g_stream.pitch = static_cast<std::uint16_t>((read_u32(header.data() + 4) * g_control_value) >> 12);
    g_stream.block = g_stream.consumed = 0;
    if (reset_repeats) g_stream.repeats = read_u32(entry);
    return g_stream.pitch != 0;
}

bool next_stream_segment()
{
    // Zero repeats underflows just as the original unsigned decrement,
    // making it effectively indefinite rather than skipping the file.
    if (--g_stream.repeats) return open_stream_segment(false);
    const auto count = read_u32(g_stream.playlist.data() + 4);
    if (++g_stream.segment == count)
    {
        if (!read_u32(g_stream.playlist.data() + 8))
        {
            g_stream.eof = true;
            return true;
        }
        g_stream.segment = read_u32(g_stream.playlist.data() + 12);
    }
    return open_stream_segment(true);
}

bool prepare_stream_half()
{
    if (g_stream.consumed == g_stream.length && !g_stream.eof
        && !next_stream_segment()) return false;
    auto *stage = iop::ram_ptr(g_stream.staging);
    std::memset(stage, 0, k_stream_half * 2);
    const auto half = g_stream.target_half;
    g_stream.last_half[half] = false;
    g_stream.half_pitch[half] = g_stream.pitch;
    std::uint32_t valid = 0x40;
    if (!g_stream.eof)
    {
        const std::uint64_t j = g_stream.block, n = g_stream.folded_blocks;
        if (j >= n * 2) return false;
        const auto offset = j < n ? 0x800ull + j * 0x20000u
            : (2 * n - j) * 0x20000u - 0xF800u;
        if (offset > UINT32_MAX || offset + 0x10000u > g_stream.asset.file_size
            || g_catalog.read(g_stream.asset, static_cast<std::uint32_t>(offset), stage, 0x10000)
                != 0x10000) return false;
        valid = std::min(k_stream_half, g_stream.length - g_stream.consumed);
        g_stream.consumed += valid;
        ++g_stream.block;
        const auto count = read_u32(g_stream.playlist.data() + 4);
        g_stream.last_half[half] = g_stream.consumed == g_stream.length
            && g_stream.repeats == 1 && g_stream.segment + 1 == count
            && !read_u32(g_stream.playlist.data() + 8);
    }
    g_stream.half_bytes[half] = valid;
    for (unsigned channel = 0; channel < 2; ++channel)
    {
        auto *data = stage + channel * k_stream_half;
        // Original 0x77C0: full first half marks loop start; the
        // second half and partial tails mark loop end/repeat.
        if (valid == k_stream_half && half == 0) data[1] |= 6;
        else data[valid - 15] |= 3;
        if (valid < k_stream_half)
            std::memset(data + valid, 0, k_stream_half - valid);
    }
    g_stream.channel = 0;
    return true;
}

void enter_stream_half(std::uint8_t half)
{
    const auto loop_half = g_stream.half_bytes[half] < k_stream_half ? 1u - half : 0u;
    stream_call(9, 0x216F, k_stream_left + loop_half * k_stream_half);
    stream_call(9, 0x216D, k_stream_right + loop_half * k_stream_half);
    stream_call(5, 0x22F, g_stream.half_pitch[half]);
    stream_call(5, 0x22D, g_stream.half_pitch[half]);
}

void process_stream(const kernel::Job &job)
{
    if (job.a != g_stream.serial) return;
    if (job.b == 1)
    {
        if (g_stream.out_total)
        {
            if (g_stream.out_step < g_stream.out_total) ++g_stream.out_step;
            g_stream.fade = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(g_stream.out_base)
                * (g_stream.out_total - g_stream.out_step) / g_stream.out_total);
        }
        else if (g_stream.running && !g_stream.paused && g_stream.fade_step < g_stream.fade_total)
        {
            ++g_stream.fade_step;
            g_stream.fade = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(g_stream.fade_step) * 0x8000 / g_stream.fade_total);
        }
        if (g_stream.running) stream_gains();
        publish_stream_status();
        return;
    }

    // Both stereo DMA channels may run at once. Retain their shared IOP
    // staging buffer until each channel reports completion.
    if (g_stream.waiting)
    {
        bool pending = false;
        for (std::uint32_t channel = 0; channel < 2; ++channel)
        {
            if (g_dma_owner[channel] != 2) continue;
            std::uint32_t done = 0;
            if (!kernel::call_library_export("libsd", 19, channel, 0, 0, 0, done)
                || !done)
            {
                pending = true;
                continue;
            }
            g_dma_owner[channel] = 0;
        }
        g_stream.waiting = pending;
        if (pending && ++g_stream.polls == 10000)
        {
            g_stream.error = 0x40000065u;
            g_sound.stream_status = g_stream.error;
            std::fprintf(stderr, "[arkd-native] stream DMA completion timed out; retaining source RAM\n");
        }
        if (!pending) g_stream.polls = 0;
        if (pending && (g_stream.request || g_stream.error))
        {
            schedule_stream();
            return;
        }
    }
    if (g_stream.error)
    {
        stop_stream(g_stream.error);
        g_stream.request = 0;
        return;
    }
    if (g_stream.request)
    {
        if (g_stream.running && g_stream.out_total && g_stream.out_step < g_stream.out_total)
        {
            schedule_stream();
            return;
        }
        stop_stream();
        const auto request = g_stream.request;
        g_stream.request = 0;
        if (request == 2) { publish_stream_status(); return; }
        g_stream.playlist = g_sound.stream_commands;
        g_stream.segment = 0;
        g_stream.eof = false;
        if (!open_stream_segment(true)) { stop_stream(0x40000064u); return; }
        if (!g_stream.staging) g_stream.staging = kernel::alloc(k_stream_half * 2);
        if (!g_stream.staging || iop::phys(g_stream.staging) > iop::kRamSize - k_stream_half * 2)
        { stop_stream(0x40000064u); return; }
        g_stream.channel_gain = read_u32(g_stream.playlist.data() + 16) + 1u;
        g_stream.fade_total = std::max(1u, read_u32(g_stream.playlist.data()));
        g_stream.fade_step = 0;
        g_stream.target_half = 0;
        g_stream.read_half = 0;
        g_stream.phase = 1; // Upload both initial halves before key-on.
        g_sound.stream_status = 0x20000000u;
        if (!prepare_stream_half()) { stop_stream(0x40000064u); return; }
    }
    if (!g_stream.phase) return;
    if (g_stream.phase == 3)
    {
        const bool pause = (g_sound.streaming_flags & 4u) != 0;
        if (pause != g_stream.paused)
        {
            g_stream.paused = pause;
            stream_call(5, 0x22D, pause ? 0 : g_stream.half_pitch[g_stream.read_half]);
            stream_call(5, 0x22F, pause ? 0 : g_stream.half_pitch[g_stream.read_half]);
            publish_stream_status();
        }
        if (pause) { schedule_stream(); return; }
        std::uint32_t address = 0;
        const bool address_read = kernel::call_library_export(
            "libsd", 10, 0x226F, 0, 0, 0, address);
        if (!address_read || address < k_stream_left
            || address >= k_stream_left + k_stream_half * 2)
        {
            if (++g_stream.polls <= 2000)
            {
                if (g_stream.polls == 1)
                    std::fprintf(stderr,
                        "[arkd-native] waiting for stream playback address 0x%08X\n",
                        address);
                schedule_stream();
                return;
            }
            stop_stream(0x40000065u);
            return;
        }
        g_stream.polls = 0;
        const auto half = static_cast<std::uint8_t>((address - k_stream_left) / k_stream_half);
        if (half == g_stream.read_half) { schedule_stream(); return; }
        if (g_stream.last_half[g_stream.read_half]) { stop_stream(); return; }
        g_stream.target_half = g_stream.read_half;
        g_stream.read_half = half;
        enter_stream_half(half);
        if (!prepare_stream_half()) { stop_stream(0x40000064u); return; }
        g_stream.phase = 2;
    }
    if (g_stream.channel < 2)
    {
        const auto channel = g_stream.channel;
        if (!dma_channel_available(channel)) { schedule_stream(); return; }
        if (g_stream.running)
        {
            std::uint32_t address = 0;
            const bool address_read = kernel::call_library_export(
                "libsd", 10, 0x226F, 0, 0, 0, address);
            if (!address_read
                || address < k_stream_left || address >= k_stream_left + k_stream_half * 2
                || (address - k_stream_left) / k_stream_half == g_stream.target_half)
            {
                std::fprintf(stderr,
                    "[arkd-native] stream refill missed half %u: NAX 0x%08X, "
                    "channel %u owner %u, block %u\n",
                    g_stream.target_half, address, channel,
                    g_dma_owner[channel], g_stream.block);
                if (g_stream.waiting)
                {
                    g_stream.error = 0x40000065u;
                    schedule_stream();
                    return;
                }
                stop_stream(0x40000065u);
                return;
            }
        }
        const auto destination = (channel ? k_stream_right : k_stream_left)
            + g_stream.target_half * k_stream_half;
        std::uint32_t transferred = 0;
        const bool transferred_ok = kernel::call_library_export("libsd", 17, channel, 0,
                g_stream.staging + channel * k_stream_half, destination,
                transferred, k_stream_half);
        if (!transferred_ok || static_cast<std::int32_t>(transferred) < 0)
        {
            std::fprintf(stderr, "[arkd-native] stream DMA rejected: channel %u, result 0x%08X\n",
                channel, transferred);
            if (g_stream.waiting)
            {
                g_stream.error = 0x40000064u;
                schedule_stream();
                return;
            }
            stop_stream(0x40000064u);
            return;
        }
        g_dma_owner[channel] = 2;
        g_stream.waiting = true;
        ++g_stream.channel;
        schedule_stream();
        return;
    }
    if (g_stream.waiting) { schedule_stream(); return; }
    if (g_stream.phase == 1 && g_stream.target_half == 0)
    {
        g_stream.target_half = 1;
        if (!prepare_stream_half()) { stop_stream(0x40000064u); return; }
        schedule_stream();
        return;
    }
    if (!g_stream.running)
    {
        stream_call(9, 0x206F, k_stream_left);
        stream_call(9, 0x206D, k_stream_right);
        enter_stream_half(0);
        g_stream.fade_step = 1;
        g_stream.fade = 0x8000u / g_stream.fade_total;
        g_stream.running = true;
        stream_gains();
        if (!stream_call(7, 0x1501, 0xC00000)) { stop_stream(0x40000064u); return; }
        g_sound.stream_status = 0x30000000u;
        std::fprintf(stderr, "[arkd-native] AMS stream started (%u segments)\n",
            read_u32(g_stream.playlist.data() + 4));
        publish_stream_status();
    }
    g_stream.phase = 3;
    schedule_stream();
}

void queue_stream(std::uint8_t request, std::uint32_t fade_frames)
{
    ++g_stream.serial;
    g_stream.error = 0;
    g_stream.request = request;
    g_stream.out_total = g_stream.running ? fade_frames : 0;
    g_stream.out_step = 0;
    g_stream.out_base = g_stream.fade;
    g_sound.stream_status = 0x20000000u;
    publish_stream_status();
    schedule_stream(0);
}

void complete_audio_command(const kernel::Job &job)
{
    if (!g_audio_worker.pending || job.a != g_audio_worker.serial)
        return;

    g_audio_worker.pending = false;
    // IRX worker opcode 1 executes 0x5228, releasing the selected bank.
    // Keep other banks at stable SPU addresses instead of physically
    // compacting them; the allocator will reuse the freed interval.
    if (g_audio_worker.opcode == 1)
    {
        g_audio_worker.state = release_spu_bank(g_audio_worker.selector)
            ? 0x30000000u : 0x4000012Cu;
        sif::publish_ee_sreg(12, g_audio_worker.state);
        return;
    }
    // Original opcode 2 (0x5F08) uses the cached IOP sound bank,
    // releases its old SPU allocation, then re-uploads its sample data.
    // A bank for a different selector cannot be reconstructed here.
    if (g_audio_worker.opcode == 2
        && g_audio_worker.selector < k_sound_bank_slots
        && !g_cached_banks[g_audio_worker.selector].empty())
    {
        const auto selected = g_audio_worker.selector;
        g_sound_bank_data = g_cached_banks[selected];
        g_loaded_bank_id = selected;
        g_bank_ready = true;
        release_spu_bank(selected);
        begin_spu_upload();
        g_audio_worker.state = g_spu_upload.state;
        if (!g_spu_upload.pending) sif::publish_ee_sreg(12, g_audio_worker.state);
        return;
    }
    g_audio_worker.state = 0x40000064u;
    sif::publish_ee_sreg(12, g_audio_worker.state);
    std::fprintf(stderr,
        "[arkd-native] audio command 0x501/0x%X selector %u "
        "has no cached sample bank\n",
        g_audio_worker.opcode, g_audio_worker.selector);
}

bool queue_audio_command(std::uint32_t method,
    std::uint32_t args, std::uint32_t args_size,
    std::uint32_t ret, std::uint32_t ret_size)
{
    // IRX 0x65C8 and 0x6668: both methods load a single u32 selector,
    // assign command opcode 1 or 2, and wake the same worker.
    if (args_size < 4 || !ee_range(args, 4))
    {
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }
    // All three 0x501/0x101-0x103 methods wake the same IRX worker.
    if (g_audio_worker.pending || g_pending_bank.valid
        || g_spu_upload.pending)
    {
        reply_u32(ret, ret_size, 0x80000065u);
        return true;
    }

    g_audio_worker.selector = read_u32(
        ps2::g_ram + (args & (ps2::kRamSize - 1)));
    g_audio_worker.opcode = static_cast<std::uint8_t>(
        method == 0x102 ? 1 : 2);
    ++g_audio_worker.serial;
    g_audio_worker.pending = true;
    g_audio_worker.state = 0x10000000u;
    sif::publish_ee_sreg(12, g_audio_worker.state);
    kernel::schedule(kernel::now() + kernel::usec_to_cycles(400),
        kernel::Job{kernel::kJobArkdAudio, g_audio_worker.serial});
    reply_u32(ret, ret_size, 0);
    return true;
}

void complete_spu_upload(const kernel::Job &job)
{
    if (!g_spu_upload.pending || job.a != g_spu_upload.serial)
        return;

    auto &upload = g_spu_upload;
    auto fail = [&upload]()
    {
        const auto channel = (upload.offset / k_iop_stage_bytes) & 1u;
        if (g_dma_owner[channel] == 1) g_dma_owner[channel] = 0;
        upload.pending = false;
        upload.waiting_for_dma = false;
        upload.state = 0x40000064u;
        sif::publish_ee_sreg(12, upload.state);
        if (g_audio_worker.opcode == 2
            && g_audio_worker.selector == upload.bank_id)
            g_audio_worker.state = upload.state;
        if (upload.staging_iop)
        {
            kernel::free(upload.staging_iop);
            upload.staging_iop = 0;
        }
    };

    // Source RAM must remain allocated until the asynchronous SPU2 DMA
    // finishes. Poll libsd #19 before changing or freeing the staging RAM.
    if (upload.waiting_for_dma)
    {
        std::uint32_t completed = 0;
        if (!kernel::call_library_export("libsd", 19,
                (upload.offset / k_iop_stage_bytes) & 1u,
                0, 0, 0, completed))
        {
            fail();
            return;
        }
        if (!completed)
        {
            if (++upload.polls > 10000)
            {
                fail();
                return;
            }
            kernel::schedule(kernel::now() + kernel::usec_to_cycles(200),
                kernel::Job{kernel::kJobArkdSpuUpload, upload.serial});
            return;
        }
        upload.waiting_for_dma = false;
        g_dma_owner[(upload.offset / k_iop_stage_bytes) & 1u] = 0;
        upload.offset += upload.chunk;
        upload.chunk = 0;
        upload.polls = 0;
    }

    if (upload.offset == upload.data_size)
    {
        upload.pending = false;
        upload.state = 0x30000000u;
        const auto metadata = inspect_sound_bank(
            g_sound_bank_data.data(), g_sound_bank_data.size());
        // IRX 0x2CBC forwards the audio-worker result to EE SREG 12.
        // Successful bank load retains the low halfword of the bank span.
        sif::publish_ee_sreg(12, upload.state
            | (metadata ? metadata->spu_memory_bytes & 0xFFFFu : 0u));
        if (g_audio_worker.opcode == 2
            && g_audio_worker.selector == upload.bank_id)
            g_audio_worker.state = upload.state;
        kernel::free(upload.staging_iop);
        upload.staging_iop = 0;
        return;
    }
    if (upload.offset > upload.data_size
        || g_sound_bank_data.size() < 0x800u + upload.data_size)
    {
        fail();
        return;
    }

    const std::uint32_t remaining = upload.data_size - upload.offset;
    const std::uint32_t chunk = std::min(remaining, k_iop_stage_bytes);
    const std::uint32_t padded = (chunk + 15u) & ~15u;
    if (upload.spu_address > k_spu_limit
        || upload.offset > k_spu_limit - upload.spu_address
        || padded > k_spu_limit - (upload.spu_address + upload.offset))
    {
        fail();
        return;
    }

    const auto channel = (upload.offset / k_iop_stage_bytes) & 1u;
    // A stream refill has one half of audio to complete both stereo DMAs.
    // Yield this channel between bank chunks when that refill is ready.
    if (!dma_channel_available(channel)
        || ((g_stream.phase == 1 || g_stream.phase == 2)
            && g_stream.channel == channel))
    {
        kernel::schedule(kernel::now() + kernel::usec_to_cycles(200),
            kernel::Job{kernel::kJobArkdSpuUpload, upload.serial});
        return;
    }
    // Copy into a persistent IOP buffer; pad the last 16-byte block.
    // SPU2 sample data starts after the 2 KiB sound-bank header.
    std::memset(iop::ram_ptr(upload.staging_iop), 0, padded);
    std::memcpy(iop::ram_ptr(upload.staging_iop),
        g_sound_bank_data.data() + 0x800u + upload.offset, chunk);

    std::uint32_t transferred = 0;
    if (!kernel::call_library_export("libsd", 17,
            (upload.offset / k_iop_stage_bytes) & 1u,
            0, upload.staging_iop, upload.spu_address + upload.offset,
            transferred, padded)
        || static_cast<std::int32_t>(transferred) < 0)
    {
        fail();
        return;
    }

    upload.state = 0x20000000u;
    upload.chunk = chunk;
    upload.waiting_for_dma = true;
    g_dma_owner[channel] = 1;
    kernel::schedule(kernel::now() + kernel::usec_to_cycles(200),
        kernel::Job{kernel::kJobArkdSpuUpload, upload.serial});
}

void begin_spu_upload()
{
    g_spu_upload.state = 0x40000064u;
    const auto metadata = inspect_sound_bank(
        g_sound_bank_data.data(), g_sound_bank_data.size());
    if (!metadata || g_loaded_bank_id >= k_sound_bank_slots
        || !kernel::library_exists("libsd")
        || g_sound_bank_data.size() <= 0x800u
        || g_spu_upload.pending)
        return;

    const std::size_t samples = g_sound_bank_data.size() - 0x800u;
    const std::uint32_t allocation =
        (metadata->spu_memory_bytes + 63u) & ~63u;
    if (samples > allocation || samples > k_spu_limit)
        return;

    if (!allocate_spu_bank(g_loaded_bank_id, allocation))
        return;
    auto &bank = g_spu_banks[g_loaded_bank_id];

    const std::uint32_t staging = kernel::alloc(k_iop_stage_bytes);
    if (!staging || (iop::phys(staging) & (iop::kRamSize - 1))
        > iop::kRamSize - k_iop_stage_bytes)
        return;

    auto &upload = g_spu_upload;
    upload = {};
    upload.bank_id = g_loaded_bank_id;
    upload.spu_address = bank.base;
    upload.staging_iop = staging;
    upload.data_size = static_cast<std::uint32_t>(samples);
    upload.state = 0x10000000u;
    upload.serial = ++g_bank_serial;
    upload.pending = true;
    kernel::schedule(kernel::now() + kernel::usec_to_cycles(200),
        kernel::Job{kernel::kJobArkdSpuUpload, upload.serial});
}

void complete_sound_bank(const kernel::Job &job)
{
    if (!g_pending_bank.valid || job.a != g_pending_bank.serial)
        return;

    const s_asset asset = g_pending_bank.asset;
    g_pending_bank.valid = false;
    g_bank_ready = false;
    g_sound_bank_data.clear();

    if (asset.file_size > k_max_bank_size)
    {
        sif::publish_ee_sreg(12, 0x40000064u);
        return;
    }

    g_sound_bank_data.resize(asset.file_size);
    const std::size_t loaded = g_catalog.read(
        asset, 0, g_sound_bank_data.data(), g_sound_bank_data.size());
    if (loaded != asset.file_size)
    {
        sif::publish_ee_sreg(12, 0x40000064u);
        g_sound_bank_data.clear();
        std::fprintf(stderr,
            "[arkd-native] audio bank read failed (%zu / %u bytes)\n",
            loaded, asset.file_size);
        return;
    }
    g_loaded_bank_id = g_pending_bank.bank_id;
    g_bank_ready = true;
    if (const auto metadata = inspect_sound_bank(
            g_sound_bank_data.data(), g_sound_bank_data.size()))
    {
        std::fprintf(stderr,
            "[arkd-native] bank %u preloaded: %u entries, %u SPU bytes\n",
            g_loaded_bank_id, metadata->entry_count,
            metadata->spu_memory_bytes);
        begin_spu_upload();
        // Cache only banks accepted for SPU2 upload. A malformed
        // preload or failed growth must not replace an existing,
        // potentially still-playing bank's reload source.
        if (g_spu_upload.pending
            && g_loaded_bank_id < k_sound_bank_slots)
        {
            std::size_t total = 0;
            for (std::size_t index = 0; index < g_cached_banks.size(); ++index)
            {
                if (index != g_loaded_bank_id)
                    total += g_cached_banks[index].size();
            }
            if (g_sound_bank_data.size() <= k_max_total_bank_cache
                && total <= k_max_total_bank_cache - g_sound_bank_data.size())
                g_cached_banks[g_loaded_bank_id] = g_sound_bank_data;
            else
                std::fprintf(stderr,
                    "[arkd-native] total audio-bank cache limit reached; "
                    "older bank reloads may be unavailable\n");
        }
    }
    if (!g_spu_upload.pending) sif::publish_ee_sreg(12, 0x40000064u);
}

bool queue_sound_bank(std::uint32_t args, std::uint32_t args_size,
    std::uint32_t ret, std::uint32_t ret_size)
{
    // IRX 0x6528 copies a 0x24-byte packet; worker 0x5894
    // searches INFO.DAT using its 32-byte filename at +4.
    if (args_size < 0x24 || !ee_range(args, 0x24))
    {
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }
    // The bank preload and stream-control commands share an IOP worker.
    if (g_pending_bank.valid || g_audio_worker.pending
        || g_spu_upload.pending)
    {
        reply_u32(ret, ret_size, 0x80000065u);
        return true;
    }

    const auto *packet = ps2::g_ram + (args & (ps2::kRamSize - 1));
    const auto *name = packet + 4;
    const auto *end = static_cast<const std::uint8_t *>(
        std::memchr(name, 0, 32));
    const std::size_t name_size = end
        ? static_cast<std::size_t>(end - name) : 32;
    const auto asset = g_catalog.find(std::string_view(
        reinterpret_cast<const char *>(name), name_size));
    if (!asset || asset->file_size > k_max_bank_size)
    {
        sif::publish_ee_sreg(12, 0x40000064u);
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }

    // Original 0x5894 copies request[0] to the active bank index at
    // 0x3334 and uses it to index per-bank allocation tables.
    const std::uint32_t bank_id = read_u32(packet);
    g_pending_bank = {*asset, bank_id, ++g_bank_serial, true};
    // IRX 0x6588-0x65A0 publishes busy at admission, so the EE
    // cannot mistake the previous bank's completion for this one.
    sif::publish_ee_sreg(12, 0x10000000u);
    g_bank_ready = false;
    g_sound_bank_data.clear();

    const std::uint64_t sectors =
        (static_cast<std::uint64_t>(asset->file_size) + k_sector_bytes - 1)
        / k_sector_bytes;
    const std::uint64_t delay =
        kernel::usec_to_cycles(400 + 200 * sectors);
    kernel::schedule(kernel::now() + delay,
        kernel::Job{kernel::kJobArkdBank, g_pending_bank.serial});
    reply_u32(ret, ret_size, 0);
    return true;
}

bool sound_command_rpc(std::uint32_t method, std::uint32_t args,
    std::uint32_t args_size, std::uint32_t ret, std::uint32_t ret_size)
{
    const auto copy_input = [&](void *destination, std::uint32_t amount) -> bool
    {
        if (args_size < amount || !ee_range(args, amount)) return false;
        std::memcpy(destination, ps2::g_ram + (args & (ps2::kRamSize - 1)), amount);
        return true;
    };
    std::uint32_t value = 0;
    const auto read_input = [&](std::uint32_t amount) -> bool
    {
        if (args_size < amount || !ee_range(args, amount)) return false;
        value = read_u32(ps2::g_ram + (args & (ps2::kRamSize - 1)));
        return true;
    };

    switch (method)
    {
    case 1: // IRX 0x4A10, copy 0x2E0 voice command bytes to alternating buffer.
    {
        const std::uint32_t next = (g_sound.buffer_generation + 1) & 1;
        if (!copy_input(g_sound.voice_commands[next].data(), 0x2E0)) break;
        ++g_sound.buffer_generation;
        ++g_sound.submitted_generation;
        kernel::schedule(kernel::now() + kernel::usec_to_cycles(100),
            kernel::Job{kernel::kJobArkdVoice, next});
        reply_u32(ret, ret_size, 0);
        return true;
    }
    case 2: // IRX 0x4AC0, OR the two command flag words.
    {
        if (!read_input(8)) break;
        // Original 0x4AC0 increments the global generation before
        // ORing the two flag words into the selected alternating slot.
        ++g_sound.buffer_generation;
        const std::uint32_t parity = g_sound.buffer_generation & 1u;
        for (std::uint32_t index = 0; index < 2; ++index)
        {
            const auto *source = ps2::g_ram
                + (args & (ps2::kRamSize - 1)) + 4 * index;
            g_sound.command_flags[parity][index] |= read_u32(source);
        }
        ++g_sound.submitted_generation;
        kernel::schedule(kernel::now() + kernel::usec_to_cycles(100),
            kernel::Job{kernel::kJobArkdVoice, parity});
        reply_u32(ret, ret_size, 0);
        return true;
    }
    case 3: // IRX 0x4BB8, copy 0x33C stream command bytes.
    {
        const std::uint32_t next = (g_sound.buffer_generation + 1) & 1;
        if (!copy_input(g_sound.sound_commands[next].data(), 0x33C)) break;
        ++g_sound.buffer_generation;
        // Unlike 0x4A10 and 0x4AC0, original 0x4BB8 only signals
        // pending work if the completion counter is currently zero.
        if (!g_sound.submitted_generation)
            ++g_sound.submitted_generation;
        kernel::schedule(kernel::now() + kernel::usec_to_cycles(100),
            kernel::Job{kernel::kJobArkdVoice,
                g_sound.buffer_generation & 1u});
        reply_u32(ret, ret_size, 0);
        return true;
    }
    case 0x12: // IRX 0x505C, master volume u16, then update 46 voices.
        if (args_size < 2 || !ee_range(args, 2)) break;
        std::memcpy(&g_sound.master_volume,
            ps2::g_ram + (args & (ps2::kRamSize - 1)), 2);
        refresh_live_voice_gains();
        reply_u32(ret, ret_size, 0);
        return true;
    case 0x13: // IRX 0x5108, enable/disable mixing by request u32.
        if (!read_input(4)) break;
        if (value) g_sound.streaming_flags &= ~0x10000u;
        else g_sound.streaming_flags |= 0x10000u;
        refresh_live_voice_gains();
        reply_u32(ret, ret_size, 0);
        return true;
    case 0x14: // IRX 0x50A8, left/right u16 volume controls.
        // Validate both fields before copying either: the old HLE read
        // the second u16 from an unvalidated request buffer.
        if (args_size < 4 || !ee_range(args, 4)) break;
        std::memcpy(&g_sound.channel_volume_left,
            ps2::g_ram + (args & (ps2::kRamSize - 1)), 2);
        std::memcpy(&g_sound.channel_volume_right,
            ps2::g_ram + (args & (ps2::kRamSize - 1)) + 2, 2);
        refresh_live_voice_gains();
        reply_u32(ret, ret_size, 0);
        return true;
    case 0x81: // IRX 0x5194, set control bit 0.
        g_sound.streaming_flags |= 1u;
        reply_u32(ret, ret_size, 0);
        return true;
    case 0x82: // IRX 0x51C8 returns 2 live stream-status words.
        reply_pair(ret, ret_size, g_sound.stream_status,
            g_sound.stream_control);
        return true;
    default:
        return false;
    }
    reply_u32(ret, ret_size, 0x80000064u);
    return true;
}

bool stream_control_rpc(std::uint32_t method, std::uint32_t args,
    std::uint32_t args_size, std::uint32_t ret, std::uint32_t ret_size)
{
    const auto read16 = [&](std::uint32_t byte_offset, std::uint16_t &out) -> bool
    {
        if (args_size < byte_offset + 2 || !ee_range(args, args_size))
            return false;
        std::memcpy(&out,
            ps2::g_ram + (args & (ps2::kRamSize - 1)) + byte_offset, 2);
        return true;
    };
    const auto read32 = [&](std::uint32_t &out) -> bool
    {
        if (args_size < 4 || !ee_range(args, 4)) return false;
        out = read_u32(ps2::g_ram + (args & (ps2::kRamSize - 1)));
        return true;
    };

    switch (method)
    {
    case 1: // IRX 0x96C4, channel gain is the requested u16 plus one.
    {
        std::uint16_t stream = 0;
        if (!read16(0, stream)) break;
        g_sound.current_stream = static_cast<std::uint16_t>(stream + 1);
        g_stream.channel_gain = static_cast<std::uint32_t>(stream) + 1u;
        if (g_stream.running) stream_gains();
        reply_u32(ret, ret_size, 0);
        return true;
    }
    case 2: // IRX 0x9754, master stream gain.
        if (!read16(0, g_sound.pending_stream)) break;
        if (g_stream.running) stream_gains();
        reply_u32(ret, ret_size, 0);
        return true;
    case 3: // IRX 0x9878, toggles stream flag based on request u32.
    {
        std::uint32_t enabled = 0;
        if (!read32(enabled)) break;
        g_sound.streaming_flags = (g_sound.streaming_flags & ~0x2u)
            | (enabled ? 0u : 0x2u);
        if (g_stream.running) stream_gains();
        reply_u32(ret, ret_size, 0);
        return true;
    }
    case 4: // IRX 0x991C copies a status u32 to response buffer.
        reply_u32(ret, ret_size, g_sound.stream_status);
        return true;
    case 5: // IRX 0x97DC copies two stream-channel u16 controls.
        if (!read16(0, g_sound.stream_pair_left)
            || !read16(2, g_sound.stream_pair_right)) break;
        if (g_stream.running) stream_gains();
        reply_u32(ret, ret_size, 0);
        return true;
    case 0x101: // IRX 0x9548, 0x17C-byte command and update bitfield.
        if (args_size < 0x17C || !ee_range(args, 0x17C)) break;
        std::memcpy(g_sound.stream_commands.data(),
            ps2::g_ram + (args & (ps2::kRamSize - 1)), 0x17C);
        g_sound.control_flags = (g_sound.control_flags | 1u) & ~6u;
        queue_stream(1, g_stream.running ? 60 : 0);
        reply_u32(ret, ret_size, 0);
        return true;
    case 0x102: // IRX 0x95C0, stores word, sets bit 1 and clears 0/2.
    {
        std::uint32_t value = 0;
        if (!read32(value)) break;
        g_sound.stream_control = value;
        g_sound.control_flags = (g_sound.control_flags | 2u) & ~5u;
        queue_stream(2, value);
        reply_u32(ret, ret_size, 0);
        return true;
    }
    case 0x103: // IRX 0x9628, toggles stream flag and marks change.
    {
        std::uint32_t enabled = 0;
        if (!read32(enabled)) break;
        const bool previous = (g_sound.streaming_flags & 4u) != 0;
        if (enabled) g_sound.streaming_flags &= ~4u;
        else g_sound.streaming_flags |= 4u;
        if (previous != ((g_sound.streaming_flags & 4u) != 0))
            g_sound.control_flags |= 4u;
        reply_u32(ret, ret_size, 0);
        return true;
    }
    default:
        return false;
    }
    reply_u32(ret, ret_size, 0x80000064u);
    return true;
}

bool native_initialize_disc(std::uint32_t args, std::uint32_t args_size,
    std::uint32_t ret, std::uint32_t ret_size)
{
    // IRX 0x250C / 0x1DA4: a 36-byte command primes the CDVD worker
    // without transferring a file. The EE polls 0x503/2 for completion.
    if (args_size < 0x24 || !ee_range(args, 0x24))
    {
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }
    if (g_pending.valid)
    {
        reply_u32(ret, ret_size, 0x80000065u);
        return true;
    }
    const auto destination = read_u32(ps2::g_ram + (args & (ps2::kRamSize - 1)));
    g_pending = {{}, destination, ++g_transfer_serial, true, 1};
    g_transfer_state = 0x10000000u;
    kernel::schedule(kernel::now() + kernel::usec_to_cycles(400),
        kernel::Job{kernel::kJobArkdRead, g_pending.serial});
    reply_u32(ret, ret_size, 0);
    return true;
}

bool native_cdvd_control(std::uint32_t args, std::uint32_t args_size,
    std::uint32_t ret, std::uint32_t ret_size)
{
    // IRX 0x2608 accepts the 0x503/0x103 worker only when the previous
    // CDVD request has reached a success/error terminal state. A u32
    // at request+0 selects CDVD operation 1 through 8 (IRX 0x1EB0).
    if (args_size < 4 || !ee_range(args, 4))
    {
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }
    if (g_pending.valid
        || ((g_transfer_state & 0xF0000000u) != 0x30000000u
            && (g_transfer_state & 0xF0000000u) != 0x40000000u))
    {
        reply_u32(ret, ret_size, 0xFFFFFFFFu);
        return true;
    }
    const auto *bytes = ps2::g_ram + (args & (ps2::kRamSize - 1));
    const std::uint32_t command = read_u32(bytes);
    if (command < 1 || command > 8)
    {
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }

    g_pending = {{}, 0, ++g_transfer_serial, true,
        static_cast<std::uint8_t>(command + 1)};
    g_transfer_state = 0x10000000u;
    kernel::schedule(kernel::now() + kernel::usec_to_cycles(400),
        kernel::Job{kernel::kJobArkdRead, g_pending.serial});
    reply_u32(ret, ret_size, 0);
    return true;
}

bool rpc(std::uint32_t server, std::uint32_t method, std::uint32_t args,
    std::uint32_t args_size, std::uint32_t ret, std::uint32_t ret_size)
{
    if (server == 0x501)
    {
        if (method == 0x101)
            return queue_sound_bank(args, args_size, ret, ret_size);
        if (method == 0x102 || method == 0x103)
            return queue_audio_command(method, args, args_size, ret, ret_size);
        if (method == 0x11)
            return sound_effect_rpc(args, args_size, ret, ret_size);
        if (sound_command_rpc(method, args, args_size, ret, ret_size))
            return true;
    }

    if (server == 0x502
        && stream_control_rpc(method, args, args_size, ret, ret_size))
        return true;

    if (server == 0x503)
    {
        switch (method)
        {
        case 1: // IRX 0x16A4, CDVD status request.
            reply_u32(ret, ret_size, disc::is_open() ? 1u : 0u);
            return true;
        case 2:  // IRX 0x1714, returns its global transfer state.
            reply_u32(ret, ret_size, g_transfer_state);
            return true;
        case 3:
            return file_size_rpc(args, args_size, ret, ret_size);
        case 4: // IRX 0x1A10, diagnostic message.
            if (args_size && ee_range(args, args_size))
            {
                const auto *input = ps2::g_ram + (args & (ps2::kRamSize - 1));
                const std::size_t n = std::min<std::size_t>(args_size, 128);
                const auto *end = static_cast<const std::uint8_t *>(
                    std::memchr(input, 0, n));
                if (end)
                    std::fprintf(stderr, "[arkd-native] %.*s\n",
                        static_cast<int>(end - input),
                        reinterpret_cast<const char *>(input));
            }
            reply_u32(ret, ret_size, 0);
            return true;
        case 0x81: // IRX 0x183C, cancel the current transfer.
            g_pending.valid = false;
            ++g_transfer_serial;
            g_transfer_state = 0x40000065u;
            reply_u32(ret, ret_size, 0);
            return true;
        case 0x101:
            return native_full_file_read(args, args_size, ret, ret_size);
        case 0x102:
            return native_initialize_disc(args, args_size, ret, ret_size);
        case 0x103:
            return native_cdvd_control(args, args_size, ret, ret_size);
        default:
            break;
        }
    }

    if (server == 0x500)
    {
        switch (method)
        {
        case 1: // IRX 0x0030, subsystem initialisation.
            g_transfer_state = 0x30000000u;
            reset_sound_state();
            g_pending.valid = false;
            reply_u32(ret, ret_size, initialize_sound_hardware() ? 0 : 0x80000064u);
            return true;
        case 3: // IRX 0x00D8, reinitialisation.
            g_pending.valid = false;
            g_transfer_state = 0x30000000u;
            reply_u32(ret, ret_size, 0);
            return true;
        case 0x10: // IRX 0x0110 sets the separate B4AC device-mode word.
            // Native file reads have no corresponding mechanical mode.
            // B4A8 is the pitch scale and must remain independent.
            reply_u32(ret, ret_size, 0);
            return true;
        case 0x11: // IRX 0x0138, store the requested control value.
        case 0x12: // IRX 0x017C, round requested block size to 128 bytes.
            if (args_size < 4 || !ee_range(args, 4)) break;
            if (method == 0x11)
                g_control_value = read_u32(
                    ps2::g_ram + (args & (ps2::kRamSize - 1)));
            else
                g_transfer_block_bytes = (
                    read_u32(ps2::g_ram + (args & (ps2::kRamSize - 1)))
                    + 127u) & ~127u;
            reply_u32(ret, ret_size, 0);
            return true;
        default:
            break;
        }
    }

    // The original 0x501/0x502 services include streaming buffers, DMA
    // chains and playback state machines. Unsupported methods report errors
    // until packet-level parity is established; they must not return success.
    std::fprintf(stderr,
        "[arkd-native] unsupported RPC server 0x%03X method 0x%X "
        "args %u recv %u\n", server, method, args_size, ret_size);
    reply_u32(ret, ret_size, 0x80000064u);
    return true;
}

void register_rpc_servers()
{
    for (std::uint32_t server = 0x500; server <= 0x503; ++server)
    {
        sif::register_hle_server(server,
            [server](std::uint32_t method, std::uint32_t args,
                std::uint32_t args_size, std::uint32_t ret,
                std::uint32_t ret_size)
            {
                return rpc(server, method, args, args_size, ret, ret_size);
            });
    }
}

void start_module()
{
    g_native_active = true;
    if (!g_catalog.open())
    {
        std::fprintf(stderr, "[arkd-native] INFO/GAME catalog unavailable\n");
    }
    register_rpc_servers();
    std::fprintf(stderr,
        "[arkd-native] HLE servers registered (0x500-0x503, %zu indexed files)\n",
        g_catalog.entry_count());
}

} // namespace

void c_catalog::clear()
{
    m_assets.clear();
    m_texture_paths.clear();
    m_game_sector = 0;
}

bool c_catalog::open()
{
    clear();

    // The original archive supplies unmodified assets; loose GAME/
    // files override entries and can also introduce new names. Do not
    // discard original archive entries merely because an overlay exists.
    const auto loose = disc::loose_game_assets();
    std::map<std::string, s_asset> assets;

    const auto info = disc::find("INFO.DAT");
    const auto game = disc::find("GAME.DAT");
    const auto load_archive = [&]() -> bool
    {
        if (!info || !game || info->directory || game->directory
            || info->size < k_header_bytes || info->size > k_max_info_size)
            return false;

        const std::uint32_t sectors =
            (info->size + k_sector_bytes - 1) / k_sector_bytes;
        std::vector<std::uint8_t> encoded(
            static_cast<std::size_t>(sectors) * k_sector_bytes);
        if (!disc::read(info->sector, sectors, encoded.data()))
            return false;
        if (encoded[0] != 0x7F
            || std::memcmp(encoded.data() + 1, "www.arika.co.jp", 15) != 0)
            return false;

        for (std::size_t offset = 16; offset < info->size; ++offset)
        {
            const std::uint8_t value = encoded[offset];
            const std::uint8_t swapped =
                static_cast<std::uint8_t>((value << 4) | (value >> 4));
            encoded[offset] = static_cast<std::uint8_t>(
                static_cast<std::uint8_t>(~swapped) - encoded[offset & 15]);
        }

        const std::uint32_t count = read_u32(encoded.data() + 0x2C);
        if (count > (info->size - k_header_bytes) / k_record_bytes)
            return false;

        const std::uint64_t game_sectors =
            (static_cast<std::uint64_t>(game->size) + k_sector_bytes - 1)
            / k_sector_bytes;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const std::uint8_t *record = encoded.data()
                + k_header_bytes
                + static_cast<std::size_t>(index) * k_record_bytes;
            const auto *end = static_cast<const std::uint8_t *>(
                std::memchr(record, 0, 32));
            const std::size_t name_size =
                end ? static_cast<std::size_t>(end - record) : 32;
            if (!name_size) return false;

            const std::string name(
                reinterpret_cast<const char *>(record), name_size);
            const std::uint32_t sector = read_u32(record + 0x24);
            const std::uint32_t allocated = read_u32(record + 0x28);
            const std::uint32_t size = read_u32(record + 0x2C);
            if (!size || !allocated
                || static_cast<std::uint64_t>(size)
                    > static_cast<std::uint64_t>(allocated) * k_sector_bytes
                || static_cast<std::uint64_t>(sector) + allocated
                    > game_sectors)
                return false;

            if (!assets.emplace(name_key(name),
                    s_asset{sector, size}).second)
                return false;
        }
        m_game_sector = game->sector;
        return true;
    };

    const bool archive_ok = load_archive();
    if (!archive_ok)
    {
        // A pure loose-asset mount may exceed the legacy INFO table
        // capacity (2047 records). It must remain fully accessible.
        if (loose.empty()) return false;
        assets.clear();
        m_game_sector = 0;
    }

    for (const auto &asset : loose)
    {
        // Override original archive records by normalised filename.
        // The loose_id selects direct folder reading in c_catalog::read.
        assets[name_key(asset.name)] = s_asset{0, asset.size, asset.id};
    }

    m_assets = std::move(assets);

    // Canonicalize by the archive's own filenames, not texture hashes.
    // In-game CTexture names omit the path, ".pix", and the dot before
    // the three-digit FPIX chunk index. Duplicate basenames are unsafe:
    // an empty value explicitly marks them as ambiguous.
    for (const auto &[filename, asset] : m_assets)
    {
        (void)asset;
        if (filename.size() <= 4
            || filename.compare(filename.size() - 4, 4, ".pix") != 0)
        {
            continue;
        }
        const auto separator = filename.find_last_of("/\\");
        const std::size_t begin = separator == std::string::npos
            ? 0 : separator + 1;
        const std::string stem = filename.substr(
            begin, filename.size() - begin - 4);
        if (stem.empty())
        {
            continue;
        }
        std::string canonical = filename;
        std::replace(canonical.begin(), canonical.end(), '\\', '/');
        const auto [it, inserted] =
            m_texture_paths.emplace(stem, canonical);
        if (!inserted && it->second != canonical)
        {
            it->second.clear();
        }
    }
    return !m_assets.empty();
}

std::optional<s_asset> c_catalog::find(std::string_view name) const
{
    const auto it = m_assets.find(name_key(name));
    if (it == m_assets.end()) return std::nullopt;
    return it->second;
}

std::string c_catalog::resolve_texture_name(std::string_view source) const
{
    if (source.size() < 4)
    {
        return {};
    }
    const std::size_t suffix_at = source.size() - 3;
    for (std::size_t i = suffix_at; i < source.size(); ++i)
    {
        if (source[i] < '0' || source[i] > '9')
        {
            return {};
        }
    }
    const std::string stem = name_key(source.substr(0, suffix_at));
    const auto it = m_texture_paths.find(stem);
    if (it == m_texture_paths.end() || it->second.empty())
    {
        return {};
    }
    return it->second + "." + std::string(source.substr(suffix_at));
}

std::string resolve_texture_name(std::string_view name)
{
    return g_catalog.resolve_texture_name(name);
}

std::size_t c_catalog::read(const s_asset &asset, std::uint32_t offset,
    void *destination, std::size_t capacity) const
{
    if (!destination || offset >= asset.file_size || !capacity)
        return 0;
    const std::size_t requested = std::min<std::size_t>(
        capacity, asset.file_size - offset);
    if (asset.loose_id != UINT32_MAX)
        return disc::read_loose_game_asset(asset.loose_id, offset,
            destination, requested);
    auto *target = static_cast<std::uint8_t *>(destination);
    std::size_t copied = 0;
    std::vector<std::uint8_t> scratch(k_read_chunk_sectors * k_sector_bytes);
    while (copied < requested)
    {
        const std::uint64_t position = static_cast<std::uint64_t>(offset) + copied;
        const std::uint32_t first_sector = static_cast<std::uint32_t>(position / k_sector_bytes);
        const std::uint32_t inside = static_cast<std::uint32_t>(position % k_sector_bytes);
        const std::size_t want = std::min<std::size_t>(
            requested - copied, scratch.size() - inside);
        const std::uint32_t sector_count = static_cast<std::uint32_t>(
            (inside + want + k_sector_bytes - 1) / k_sector_bytes);
        const std::uint64_t lba = static_cast<std::uint64_t>(m_game_sector)
            + asset.relative_sector + first_sector;
        if (lba > std::numeric_limits<std::uint32_t>::max()
            || !disc::read(static_cast<std::uint32_t>(lba), sector_count, scratch.data()))
            break;
        std::memcpy(target + copied, scratch.data() + inside, want);
        copied += want;
    }
    return copied;
}

std::size_t c_catalog::entry_count() const
{
    return m_assets.size();
}

std::vector<std::uint8_t> c_catalog::ee_index() const
{
    // 0x1DA4 transfers the decoded 48-byte index records to the EE,
    // excluding the on-disc header. Loose assets need the same shape.
    std::vector<std::uint8_t> result(m_assets.size() * k_record_bytes);
    std::size_t offset = 0;
    for (const auto &[name, asset] : m_assets)
    {
        auto *record = result.data() + offset;
        std::memcpy(record, name.data(), std::min<std::size_t>(name.size(), 32));
        for (std::size_t byte = 0; byte < std::min<std::size_t>(name.size(), 32); ++byte)
        {
            if (cp932_lead(record[byte])) { ++byte; continue; }
            if (record[byte] == '\\') record[byte] = '/';
        }
        const auto put = [&](std::size_t field, std::uint32_t value)
        {
            for (unsigned byte = 0; byte < 4; ++byte)
                record[field + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
        };
        put(0x24, asset.relative_sector);
        put(0x28, (asset.file_size + k_sector_bytes - 1) / k_sector_bytes);
        put(0x2C, asset.file_size);
        offset += k_record_bytes;
    }
    return result;
}

std::uint32_t audio_worker_status()
{
    return g_audio_worker.state;
}

s_pitch_interval decode_pitch_interval(std::int16_t pitch_request)
{
    // IRX 0x33C4 tests signed +6 with blez. Positive requests use
    // the value itself; zero and negative requests use its negation.
    // The MIPS 0x2AAAAAAB multiply-high sequence divides by 12
    // (truncation toward zero), with two separate computations:
    // (magnitude + 12) / 12 and magnitude % 12.
    const std::int32_t signed_value = pitch_request;
    const bool non_positive = signed_value <= 0;
    const std::uint32_t magnitude = static_cast<std::uint32_t>(
        non_positive ? -signed_value : signed_value);
    return s_pitch_interval{
        (magnitude + 12u) / 12u,
        magnitude % 12u,
        non_positive
    };
}

std::uint16_t calculate_voice_pitch(std::uint16_t base_pitch,
    std::int16_t pitch_request)
{
    const auto interval = decode_pitch_interval(pitch_request);
    const std::uint32_t count = interval.octave_count; // Always >= 1.
    const std::uint32_t remainder = interval.semitone_remainder;
    if (!interval.non_positive)
    {
        // IRX 0x34B4-0x35D0 stores the difference between adjacent
        // multiples in a halfword, then reloads the low halfword of
        // base * count before adding the interpolated difference.
        const std::uint16_t lower = static_cast<std::uint16_t>(
            static_cast<std::uint32_t>(base_pitch) * count);
        return static_cast<std::uint16_t>(lower
            + (static_cast<std::uint32_t>(base_pitch) * remainder) / 12u);
    }

    // IRX 0x36C8-0x37F8 interpolates between truncated quotients,
    // rather than dividing a rational interpolation of the base.
    const std::uint32_t upper = base_pitch / count;
    const std::uint32_t lower = base_pitch / (count + 1u);
    return static_cast<std::uint16_t>(
        upper - ((upper - lower) * remainder) / 12u);
}

std::optional<s_voice_command> inspect_voice_command(
    const std::uint8_t *bytes, std::size_t size,
    std::uint32_t voice_index)
{
    constexpr std::size_t k_record_bytes = 16;
    constexpr std::uint32_t k_voice_count = 46;

    if (!bytes || voice_index >= k_voice_count
        || size / k_record_bytes <= voice_index)
        return std::nullopt;

    const std::uint8_t *entry =
        bytes + static_cast<std::size_t>(voice_index) * k_record_bytes;
    const auto u16 = [](const std::uint8_t *p) -> std::uint16_t
    {
        return static_cast<std::uint16_t>(
            p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
    };

    s_voice_command result;
    result.sample_selector = read_u32(entry);
    result.control = u16(entry + 4);
    result.pitch_request = static_cast<std::int16_t>(u16(entry + 6));
    result.volume_0 = u16(entry + 8);
    result.volume_1 = u16(entry + 10);
    result.remaining = read_u32(entry + 12);
    result.start_control = u16(entry + 12);
    result.restart_control = u16(entry + 14);
    return result;
}

std::optional<s_sound_bank_info> inspect_sound_bank(
    const std::uint8_t *bytes, std::size_t size)
{
    // The IRX reads the first sector at 0x5894-0x5934 and then consults
    // +4 (number of entries), +8 (SPU2 allocation span) and the
    // 8-byte entry records beginning at +0x0C (0x5AC0-0x5C04).
    // This only checks the bounded header shape. It does not claim
    // to decode sample data or validate all entry semantics.
    constexpr std::size_t header_size = 0x800;
    constexpr std::size_t entry_offset = 0x0C;
    constexpr std::size_t entry_size = 8;
    constexpr std::uint32_t max_spu_bytes = 2u * 1024u * 1024u;

    if (!bytes || size < header_size)
        return std::nullopt;

    const std::uint32_t count = read_u32(bytes + 4);
    const std::uint32_t span = read_u32(bytes + 8);
    if (!count || count > (header_size - entry_offset) / entry_size)
        return std::nullopt;
    if (!span || span > max_spu_bytes)
        return std::nullopt;

    s_sound_bank_info result;
    result.entry_count = count;
    result.spu_memory_bytes = span;
    result.entries.reserve(count);

    for (std::uint32_t index = 0; index < count; ++index)
    {
        const auto *record = bytes + entry_offset
            + static_cast<std::size_t>(index) * entry_size;
        const std::uint32_t parameter = read_u32(record);
        const std::uint32_t offset = read_u32(record + 4);
        // IRX 0x5B40-0x5B68 adds this offset to the allocated SPU
        // region base. An offset beyond the bank span would address
        // another allocation and must not reach a native transfer.
        if (offset >= span)
            return std::nullopt;
        result.entries.push_back({parameter, offset});
    }
    return result;
}

std::uint32_t sound_upload_status()
{
    return g_spu_upload.state;
}

std::optional<std::uint32_t> sound_bank_spu_address(std::uint32_t bank_id)
{
    if (bank_id >= k_sound_bank_slots || !g_spu_banks[bank_id].base)
        return std::nullopt;
    return g_spu_banks[bank_id].base;
}

bool record_voice_volume_state(std::uint32_t index,
    std::uint16_t source_volume_0, std::uint16_t source_volume_1)
{
    if (index >= g_voice_source_volumes.size())
        return false;
    g_voice_source_volumes[index] = {source_volume_0, source_volume_1};
    g_sound.active_voice_masks[index >> 5] |= 1u << (index & 31u);
    return true;
}

bool sound_bank_ready()
{
    return g_bank_ready;
}

std::uint32_t sound_bank_id()
{
    return g_loaded_bank_id;
}

std::size_t copy_sound_bank(void *destination, std::size_t capacity)
{
    if (!g_bank_ready || !destination
        || capacity < g_sound_bank_data.size())
        return 0;
    if (!g_sound_bank_data.empty())
        std::memcpy(destination, g_sound_bank_data.data(),
            g_sound_bank_data.size());
    return g_sound_bank_data.size();
}

bool native_requested()
{
    const char *value = std::getenv("SDBZ_ARKD_NATIVE");
    return !value || (std::strcmp(value, "0") != 0
        && std::strcmp(value, "false") != 0);
}

void on_vblank()
{
    if (native_requested() && g_catalog.entry_count()
        && kernel::library_exists("libsd"))
    {
        kernel::schedule(kernel::now(),
            kernel::Job{kernel::kJobArkdVoice, 0, 1});
        kernel::schedule(kernel::now(),
            kernel::Job{kernel::kJobArkdStream, g_stream.serial, 1});
    }
}

bool intercept_module(const std::string &path)
{
    if (!native_requested()) return false;
    const auto last = path.find_last_of("/\\:");
    std::string name = path.substr(last == std::string::npos ? 0 : last + 1);
    for (char &letter : name)
        if (letter >= 'a' && letter <= 'z')
            letter = static_cast<char>(letter - ('a' - 'A'));
    return name == "ARKD_DVD.IRX" || name == "ARKD_DVD.IRX;1";
}

void install()
{
    g_catalog.clear();
    g_pending = {};
    g_pending_bank = {};
    g_bank_serial = 0;
    g_loaded_bank_id = 0;
    g_bank_ready = false;
    g_sound_bank_data.clear();
    g_audio_worker = {};
    g_spu_upload = {};
    g_stream = {};
    g_dma_owner = {};
    g_spu_banks = {};
    g_cached_banks = {};
    g_spu_next = k_spu_base_start;
    g_silence_staging = 0;
    g_native_active = false;
    g_transfer_state = 0x30000000u;
    g_transfer_serial = 0;
    g_control_value = 0x1000;
    g_transfer_block_bytes = 0;
    reset_sound_state();
    if (!native_requested()) return;
    kernel::register_job(kernel::kJobArkdRead, complete_native_read);
    kernel::register_job(kernel::kJobArkdVoice, process_voice_commands);
    kernel::register_job(kernel::kJobArkdBank, complete_sound_bank);
    kernel::register_job(kernel::kJobArkdAudio, complete_audio_command);
    kernel::register_job(kernel::kJobArkdSpuUpload, complete_spu_upload);
    kernel::register_job(kernel::kJobArkdStream, process_stream);
    kernel::register_hle_module("ARKD_DVD", start_module);
    std::fprintf(stderr,
        "[arkd-native] native module selected; "
        "set SDBZ_ARKD_NATIVE=0 to use the original IRX\n");
}

void state(ps2::state::Stream &stream)
{
    // Existing v2-v6 state files have no native ARKD section. A new v7
    // section includes the selected backend, in-flight transfer serial,
    // pending destination, and the complete alternating sound buffers.
    if (stream.version() < 7)
    {
        if (stream.loading())
        {
            g_pending = {};
            g_pending_bank = {};
            g_bank_serial = 0;
            g_loaded_bank_id = 0;
            g_bank_ready = false;
            g_sound_bank_data.clear();
            g_audio_worker = {};
            g_spu_upload = {};
            g_stream = {};
            g_dma_owner = {};
            g_spu_banks = {};
            g_cached_banks = {};
            g_spu_next = k_spu_base_start;
            g_transfer_state = 0x30000000u;
            g_transfer_serial = 0;
            g_control_value = 0x1000;
            g_transfer_block_bytes = 0;
            reset_sound_state();
            if (native_requested() && !g_catalog.open())
                throw std::runtime_error("cannot restore native ARKD asset catalog");
        }
        return;
    }

    stream.tag("arkd");
    bool native = stream.loading() ? false : g_native_active;
    stream.pod(native);
    if (stream.loading())
    {
        bool original_arkd_loaded = false;
        for (const auto &entry : kernel::modules())
            if (entry.second.name == "ARKD_DVD") original_arkd_loaded = true;
        if (native != native_requested() && (native || original_arkd_loaded))
            throw std::runtime_error(
                "ARKD native backend must match the savestate's SDBZ_ARKD_NATIVE setting");
        g_native_active = native;
    }

    stream.pod(g_transfer_state);
    stream.pod(g_transfer_serial);
    stream.pod(g_control_value);
    stream.pod(g_transfer_block_bytes);
    stream.pod(g_pending.asset.relative_sector);
    stream.pod(g_pending.asset.file_size);
    if (stream.version() >= 11)
        stream.pod(g_pending.asset.loose_id);
    else if (stream.loading())
        g_pending.asset.loose_id = UINT32_MAX;
    stream.pod(g_pending.destination);
    stream.pod(g_pending.serial);
    stream.pod(g_pending.valid);
    stream.pod(g_pending.command);
    stream.pod(g_sound);

    if (stream.version() >= 8)
    {
        stream.pod(g_pending_bank.asset.relative_sector);
        stream.pod(g_pending_bank.asset.file_size);
        if (stream.version() >= 11)
            stream.pod(g_pending_bank.asset.loose_id);
        else if (stream.loading())
            g_pending_bank.asset.loose_id = UINT32_MAX;
        stream.pod(g_pending_bank.bank_id);
        stream.pod(g_pending_bank.serial);
        stream.pod(g_pending_bank.valid);
        stream.pod(g_bank_serial);
        stream.pod(g_loaded_bank_id);
        stream.pod(g_bank_ready);
        std::uint32_t bank_size =
            static_cast<std::uint32_t>(g_sound_bank_data.size());
        stream.pod(bank_size);
        // Validate the length BEFORE resizing the vector. A malformed
        // savestate must not allocate arbitrary gigabytes of host RAM.
        if (bank_size > k_max_bank_size)
            throw std::runtime_error(
                "native ARKD bank savestate exceeds size limit");
        if (stream.loading()) g_sound_bank_data.resize(bank_size);
        if (bank_size)
            stream.bytes(g_sound_bank_data.data(), bank_size);
    }
    else if (stream.loading())
    {
        g_pending_bank = {};
        g_bank_serial = 0;
        g_loaded_bank_id = 0;
        g_bank_ready = false;
        g_sound_bank_data.clear();
    }

    if (stream.version() >= 9)
    {
        stream.pod(g_audio_worker);
    }
    else if (stream.loading())
    {
        g_audio_worker = {};
    }

    if (stream.version() >= 10)
    {
        stream.pod(g_spu_next);
        stream.pod(g_spu_banks);
        stream.pod(g_spu_upload);
        if (stream.loading() && native)
            for (const auto &bank : g_spu_banks)
                if (bank.base && (bank.base < k_spu_base_start
                    || bank.base >= k_spu_bank_limit
                    || bank.size > k_spu_bank_limit - bank.base))
                    throw std::runtime_error("native ARKD bank overlaps streaming/effect memory");
    }
    else if (stream.loading())
    {
        g_spu_upload = {};
        g_spu_banks = {};
        g_spu_next = k_spu_base_start;
    }

    if (stream.version() >= 12)
    {
        std::size_t total = 0;
        for (auto &bank : g_cached_banks)
        {
            std::uint32_t count = static_cast<std::uint32_t>(bank.size());
            stream.pod(count);
            // Validate before allocation: a damaged savestate cannot
            // force an unbounded number of audio-bank bytes into RAM.
            if (count > k_max_bank_size
                || total > k_max_total_bank_cache - count)
                throw std::runtime_error(
                    "native ARKD cached sound banks exceed size limit");
            total += count;
            if (stream.loading()) bank.resize(count);
            if (count) stream.bytes(bank.data(), count);
        }
    }
    else if (stream.loading())
    {
        g_cached_banks = {};
        if (g_loaded_bank_id < k_sound_bank_slots && g_bank_ready)
            g_cached_banks[g_loaded_bank_id] = g_sound_bank_data;
    }

    if (stream.version() >= 13)
    {
        stream.pod(g_voice_source_volumes);
    }
    else if (stream.loading())
    {
        // Older experimental HLE builds never populated active voice
        // bookkeeping; their source volumes are not available to
        // reconstruct gain writes after restoration.
        g_voice_source_volumes = {};
        g_sound.active_voice_masks = {};
    }

    if (stream.version() >= 14)
        stream.pod(g_voice_end_redirect);
    else if (stream.loading())
        g_voice_end_redirect = {};

    if (stream.version() >= 15)
    {
        stream.pod(g_stream);
        stream.pod(g_dma_owner);
        if (stream.loading())
        {
            // V15 recorded the channel currently in flight. V16 records
            // the next channel to submit so left and right can overlap.
            if (stream.version() == 15 && g_stream.waiting
                && g_stream.channel < 2
                && g_dma_owner[g_stream.channel] == 2)
                ++g_stream.channel;
            const bool stream_dma_pending = g_dma_owner[0] == 2
                || g_dma_owner[1] == 2;
            if (g_stream.phase > 3 || g_stream.channel > 2 || g_stream.target_half > 1
                || g_stream.read_half > 1 || g_stream.request > 2
                || (g_stream.staging && iop::phys(g_stream.staging) > iop::kRamSize - 0x10000u)
                || (g_stream.waiting != stream_dma_pending))
                throw std::runtime_error("invalid native AMS stream state");
            for (auto owner : g_dma_owner)
                if (owner > 3) throw std::runtime_error("invalid native audio DMA owner");
            if (g_stream.phase && (!g_stream.staging || !g_stream.length
                || g_stream.consumed > g_stream.length
                || !read_u32(g_stream.playlist.data() + 4)
                || read_u32(g_stream.playlist.data() + 4) > 10))
                throw std::runtime_error("invalid native AMS playlist state");
        }
    }
    else if (stream.loading())
    {
        g_stream = {};
        g_dma_owner = {};
        if (g_spu_upload.pending && g_spu_upload.waiting_for_dma)
            g_dma_owner[(g_spu_upload.offset / k_iop_stage_bytes) & 1u] = 1;
        // These fields existed as inert bookkeeping before streams ran.
        // Newly implemented hardware controls begin at original defaults.
        g_sound.current_stream = 0x4000;
        g_sound.pending_stream = 0x3FFF;
        g_sound.stream_pair_left = g_sound.stream_pair_right = 0x3FFF;
        g_sound.stream_status = 0x30000000u;
    }

    if (stream.loading() && native)
    {
        // Older native snapshots predate the silent-loop upload. SPU2
        // state has already been restored, so repair its reserved region
        // before any redirected one-shot voice can advance into a bank.
        fill_silent_loop(reinterpret_cast<std::uint8_t *>(spu2::ram()) + 0x5010);
        if (!g_catalog.open())
            throw std::runtime_error("cannot restore native ARKD asset catalog");
        // A fresh process has not loaded the intercepted IRX yet. The
        // following SIF state section needs these callback bindings,
        // but hardware initialization would destroy the restored SPU2.
        register_rpc_servers();
    }
}

}
