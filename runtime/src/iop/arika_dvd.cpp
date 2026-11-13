#include "arika_dvd.hpp"

#include "kernel.hpp"
#include "sif.hpp"

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
std::uint32_t g_control_value = 0;
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
std::uint32_t g_spu_next = k_spu_base_start;
s_spu_upload g_spu_upload;

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

void reset_sound_state()
{
    g_sound = {};
    // Original IRX 0x6920-0x6970 initializes these three 14-bit
    // coefficients to 0x3FFF. Zero-initializing them would mute all
    // subsequent sceSdSetParam voice-volume updates.
    g_sound.master_volume = 0x3FFF;
    g_sound.channel_volume_left = 0x3FFF;
    g_sound.channel_volume_right = 0x3FFF;
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

void process_voice_mutes(const kernel::Job &job)
{
    // IRX 0x3020-0x3274: the 46-voice worker checks either a queued
    // per-core command bit or bit 3 of a 18-byte voice record, then
    // clears both voice volume registers through libsd ordinal 5
    // (sceSdSetParam). Other voice operations are not reproduced here.
    const std::uint32_t parity = job.a & 1u;
    if (!kernel::library_exists("libsd"))
    {
        std::fprintf(stderr,
            "[arkd-native] voice worker needs libsd for sound register writes\n");
        return;
    }
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
        const auto *record = g_sound.sound_commands[parity].data()
            + static_cast<std::size_t>(index) * 18;
        const std::uint16_t flags = static_cast<std::uint16_t>(
            record[0] | (static_cast<std::uint16_t>(record[1]) << 8));
        if (!(g_sound.command_flags[parity][mask_word] & mask_bit)
            && !(flags & 0x8u))
            continue;

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
        g_sound.active_voice_masks[mask_word] &= ~mask_bit;
        g_sound.command_flags[parity][mask_word] &= ~mask_bit;
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

    // 0x503/0x101 and 0x503/0x103 are drive commands rather than
    // file reads. In the virtual-disc backend the eight motor/status
    // operations share the same always-mounted media and completion
    // semantics. Exact mechanical error conditions remain unmodelled.
    if (transfer.command)
    {
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
    // 0x503/0x102 -> IRX 0x2410 -> worker 0x21B0 -> 0x1A58.
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
    // IRX 0x4C84 (0x501/0x11): two 16-byte sceSdEffectAttr records,
    // one per SPU2 core. It calls the libsd export #23
    // (sceSdSetEffectAttr) with (core, address_of_attribute).
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
    const std::uint32_t staging = kernel::alloc(16);
    if (!staging || (iop::phys(staging) & (iop::kRamSize - 1)) > iop::kRamSize - 16)
    {
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }
    bool success = true;
    const auto *input = ps2::g_ram + (args & (ps2::kRamSize - 1));
    for (std::uint32_t core = 0; core < 2; ++core)
    {
        std::memcpy(iop::ram_ptr(staging), input + core * 16, 16);
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

void complete_audio_command(const kernel::Job &job)
{
    if (!g_audio_worker.pending || job.a != g_audio_worker.serial)
        return;

    g_audio_worker.pending = false;
    // These commands need the original worker's 0x5228/0x5F08 SPU2
    // hardware path. Do not fabricate a successful audio operation.
    g_audio_worker.state = 0x40000064u;
    std::fprintf(stderr,
        "[arkd-native] audio command 0x501/0x%X selector %u "
        "requires unimplemented SPU2 worker\n",
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
    if (g_audio_worker.pending || g_pending_bank.valid)
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
    kernel::schedule(kernel::now() + kernel::usec_to_cycles(400),
        kernel::Job{kernel::kJobArkdAudio, g_audio_worker.serial});
    reply_u32(ret, ret_size, 0);
    return true;
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
        return;

    g_sound_bank_data.resize(asset.file_size);
    const std::size_t loaded = g_catalog.read(
        asset, 0, g_sound_bank_data.data(), g_sound_bank_data.size());
    if (loaded != asset.file_size)
    {
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
    }
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
    if (g_pending_bank.valid || g_audio_worker.pending)
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
        reply_u32(ret, ret_size, 0x80000064u);
        return true;
    }

    // Original 0x5894 copies request[0] to the active bank index at
    // 0x3334 and uses it to index per-bank allocation tables.
    const std::uint32_t bank_id = read_u32(packet);
    g_pending_bank = {*asset, bank_id, ++g_bank_serial, true};
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
        reply_u32(ret, ret_size, 0);
        return true;
    }
    case 2: // IRX 0x4AC0, OR the two command flag words.
        if (!read_input(8)) break;
        for (std::uint32_t index = 0; index < 2; ++index)
        {
            const auto *source = ps2::g_ram
                + (args & (ps2::kRamSize - 1)) + 4 * index;
            g_sound.command_flags[(g_sound.buffer_generation + 1) & 1][index]
                |= read_u32(source);
        }
        ++g_sound.submitted_generation;
        kernel::schedule(kernel::now() + kernel::usec_to_cycles(100),
            kernel::Job{kernel::kJobArkdVoice,
                (g_sound.buffer_generation + 1) & 1u});
        reply_u32(ret, ret_size, 0);
        return true;
    case 3: // IRX 0x4BB8, copy 0x33C stream command bytes.
    {
        const std::uint32_t next = (g_sound.buffer_generation + 1) & 1;
        if (!copy_input(g_sound.sound_commands[next].data(), 0x33C)) break;
        ++g_sound.buffer_generation;
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
        reply_u32(ret, ret_size, 0);
        return true;
    case 0x13: // IRX 0x5108, enable/disable mixing by request u32.
        if (!read_input(4)) break;
        if (value) g_sound.streaming_flags &= ~0x10000u;
        else g_sound.streaming_flags |= 0x10000u;
        reply_u32(ret, ret_size, 0);
        return true;
    case 0x14: // IRX 0x50A8, left/right u16 volume controls.
        if (!copy_input(&g_sound.channel_volume_left, 2)) break;
        std::memcpy(&g_sound.channel_volume_right,
            ps2::g_ram + (args & (ps2::kRamSize - 1)) + 2, 2);
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
    case 1: // IRX 0x96C4, store requested stream number + 1.
    {
        std::uint16_t stream = 0;
        if (!read16(0, stream)) break;
        g_sound.current_stream = static_cast<std::uint16_t>(stream + 1);
        reply_u32(ret, ret_size, 0);
        return true;
    }
    case 2: // IRX 0x9754, store stream configuration.
        if (!read16(0, g_sound.pending_stream)) break;
        reply_u32(ret, ret_size, 0);
        return true;
    case 3: // IRX 0x9878, toggles stream flag based on request u32.
    {
        std::uint32_t enabled = 0;
        if (!read32(enabled)) break;
        g_sound.streaming_flags = (g_sound.streaming_flags & ~0x2u)
            | (enabled ? 0u : 0x2u);
        reply_u32(ret, ret_size, 0);
        return true;
    }
    case 4: // IRX 0x991C copies a status u32 to response buffer.
        reply_u32(ret, ret_size, g_sound.stream_status);
        return true;
    case 5: // IRX 0x97DC copies two stream-channel u16 controls.
        if (!read16(0, g_sound.stream_pair_left)
            || !read16(2, g_sound.stream_pair_right)) break;
        reply_u32(ret, ret_size, 0);
        return true;
    case 0x101: // IRX 0x9548, 0x17C-byte command and update bitfield.
        if (args_size < 0x17C || !ee_range(args, 0x17C)) break;
        std::memcpy(g_sound.stream_commands.data(),
            ps2::g_ram + (args & (ps2::kRamSize - 1)), 0x17C);
        g_sound.control_flags = (g_sound.control_flags | 1u) & ~6u;
        reply_u32(ret, ret_size, 0);
        return true;
    case 0x102: // IRX 0x95C0, stores word, sets bit 1 and clears 0/2.
    {
        std::uint32_t value = 0;
        if (!read32(value)) break;
        g_sound.stream_control = value;
        g_sound.control_flags = (g_sound.control_flags | 2u) & ~5u;
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
    g_pending = {{}, 0, ++g_transfer_serial, true, 1};
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
            return native_initialize_disc(args, args_size, ret, ret_size);
        case 0x102:
            return native_full_file_read(args, args_size, ret, ret_size);
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
            reply_u32(ret, ret_size, 0);
            return true;
        case 3: // IRX 0x00D8, reinitialisation.
            g_pending.valid = false;
            g_transfer_state = 0x30000000u;
            reply_u32(ret, ret_size, 0);
            return true;
        case 0x10: // IRX 0x0110, enable control flag.
            g_control_value |= 1u;
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

void start_module()
{
    if (!g_catalog.open())
    {
        std::fprintf(stderr, "[arkd-native] INFO/GAME catalog unavailable\n");
    }
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
    std::fprintf(stderr,
        "[arkd-native] HLE servers registered (0x500-0x503, %zu indexed files)\n",
        g_catalog.entry_count());
}

} // namespace

void c_catalog::clear()
{
    m_assets.clear();
    m_game_sector = 0;
}

bool c_catalog::open()
{
    clear();
    const auto info = disc::find("INFO.DAT");
    const auto game = disc::find("GAME.DAT");
    if (!info || !game || info->directory || game->directory
        || info->size < k_header_bytes || info->size > k_max_info_size)
        return false;

    const std::uint32_t sectors = (info->size + k_sector_bytes - 1) / k_sector_bytes;
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(sectors) * k_sector_bytes);
    if (!disc::read(info->sector, sectors, encoded.data()))
        return false;
    if (encoded[0] != 0x7F
        || std::memcmp(encoded.data() + 1, "www.arika.co.jp", 15) != 0)
        return false;

    for (std::size_t offset = 16; offset < info->size; ++offset)
    {
        const std::uint8_t value = encoded[offset];
        const std::uint8_t swapped = static_cast<std::uint8_t>((value << 4) | (value >> 4));
        encoded[offset] = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(~swapped) - encoded[offset & 15]);
    }
    const std::uint32_t count = read_u32(encoded.data() + 0x2C);
    if (count > (info->size - k_header_bytes) / k_record_bytes)
        return false;

    const std::uint64_t game_sectors =
        (static_cast<std::uint64_t>(game->size) + k_sector_bytes - 1) / k_sector_bytes;
    std::map<std::string, s_asset> assets;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const std::uint8_t *record =
            encoded.data() + k_header_bytes + static_cast<std::size_t>(index) * k_record_bytes;
        const auto *end = static_cast<const std::uint8_t *>(std::memchr(record, 0, 32));
        const std::size_t name_size = end
            ? static_cast<std::size_t>(end - record) : 32;
        if (!name_size) return false;

        const std::string name(reinterpret_cast<const char *>(record), name_size);
        const std::uint32_t sector = read_u32(record + 0x24);
        const std::uint32_t allocated = read_u32(record + 0x28);
        const std::uint32_t size = read_u32(record + 0x2C);
        if (!size || !allocated
            || static_cast<std::uint64_t>(size) > static_cast<std::uint64_t>(allocated) * k_sector_bytes
            || static_cast<std::uint64_t>(sector) + allocated > game_sectors)
            return false;
        if (!assets.emplace(name_key(name), s_asset{sector, size}).second)
            return false;
    }
    m_game_sector = game->sector;
    m_assets = std::move(assets);
    return true;
}

std::optional<s_asset> c_catalog::find(std::string_view name) const
{
    const auto it = m_assets.find(name_key(name));
    if (it == m_assets.end()) return std::nullopt;
    return it->second;
}

std::size_t c_catalog::read(const s_asset &asset, std::uint32_t offset,
    void *destination, std::size_t capacity) const
{
    if (!destination || offset >= asset.file_size || !capacity)
        return 0;
    const std::size_t requested = std::min<std::size_t>(
        capacity, asset.file_size - offset);
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

std::uint32_t audio_worker_status()
{
    return g_audio_worker.state;
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
    return value && (std::strcmp(value, "1") == 0
        || std::strcmp(value, "true") == 0);
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
    g_transfer_state = 0x30000000u;
    g_transfer_serial = 0;
    g_control_value = g_transfer_block_bytes = 0;
    reset_sound_state();
    if (!native_requested()) return;
    kernel::register_job(kernel::kJobArkdRead, complete_native_read);
    kernel::register_job(kernel::kJobArkdVoice, process_voice_mutes);
    kernel::register_job(kernel::kJobArkdBank, complete_sound_bank);
    kernel::register_job(kernel::kJobArkdAudio, complete_audio_command);
    kernel::register_hle_module("ARKD_DVD", start_module);
    std::fprintf(stderr,
        "[arkd-native] experimental native module selected; "
        "unset SDBZ_ARKD_NATIVE to restore original IRX\n");
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
            g_transfer_state = 0x30000000u;
            g_transfer_serial = 0;
            g_control_value = g_transfer_block_bytes = 0;
            reset_sound_state();
            if (native_requested() && !g_catalog.open())
                throw std::runtime_error("cannot restore native ARKD asset catalog");
        }
        return;
    }

    stream.tag("arkd");
    bool native = native_requested();
    stream.pod(native);
    if (stream.loading() && native != native_requested())
        throw std::runtime_error(
            "ARKD native backend must match the savestate's SDBZ_ARKD_NATIVE setting");

    stream.pod(g_transfer_state);
    stream.pod(g_transfer_serial);
    stream.pod(g_control_value);
    stream.pod(g_transfer_block_bytes);
    stream.pod(g_pending.asset.relative_sector);
    stream.pod(g_pending.asset.file_size);
    stream.pod(g_pending.destination);
    stream.pod(g_pending.serial);
    stream.pod(g_pending.valid);
    stream.pod(g_pending.command);
    stream.pod(g_sound);

    if (stream.version() >= 8)
    {
        stream.pod(g_pending_bank.asset.relative_sector);
        stream.pod(g_pending_bank.asset.file_size);
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

    if (stream.loading() && native && !g_catalog.open())
        throw std::runtime_error("cannot restore native ARKD asset catalog");
}

}
