#include "arika_dvd.hpp"

#include "kernel.hpp"
#include "sif.hpp"

#include "ps2/disc.hpp"
#include "ps2/memory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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

bool rpc(std::uint32_t server, std::uint32_t method, std::uint32_t args,
    std::uint32_t args_size, std::uint32_t ret, std::uint32_t ret_size)
{
    if (server == 0x503 && method == 3)
        return file_size_rpc(args, args_size, ret, ret_size);

    // 0x500 / 1 calls IRX initialization (0x0030); acknowledge its
    // startup control request without running the guest module.
    if (server == 0x500 && method == 1)
    {
        reply_u32(ret, ret_size, 0);
        return true;
    }

    // Do not claim unsupported audio, streaming or DMA operations worked.
    // This HLE remains opt-in until every protocol method is reconstructed.
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
    if (!native_requested()) return;
    kernel::register_hle_module("ARKD_DVD", start_module);
    std::fprintf(stderr,
        "[arkd-native] experimental native module selected; "
        "unset SDBZ_ARKD_NATIVE to restore original IRX\n");
}

}
