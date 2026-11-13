#include "resource_pack_zip.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <zlib.h>

namespace ps2::resource_pack::zip
{
namespace
{
constexpr std::uint64_t k_max_archive = 512ull * 1024 * 1024;
constexpr std::uint32_t k_max_file = 64u * 1024 * 1024;
constexpr std::uint32_t k_max_entries = 4096;
constexpr std::uint32_t k_max_central = 8u * 1024 * 1024;

std::uint16_t u16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t u32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0])
        | (static_cast<std::uint32_t>(p[1]) << 8)
        | (static_cast<std::uint32_t>(p[2]) << 16)
        | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool read_at(std::ifstream &input, std::uint64_t offset,
    void *output, std::size_t size)
{
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset));
    return input.good() && static_cast<bool>(input.read(
        static_cast<char *>(output), static_cast<std::streamsize>(size)));
}

bool safe_name(const std::string &path)
{
    if (path.empty() || path.front() == '/' || path.back() == '/'
        || path.find('\\') != std::string::npos
        || path.find(':') != std::string::npos) return false;
    std::size_t start = 0;
    for (std::size_t pos = 0; pos <= path.size(); ++pos)
    {
        if (pos != path.size() && path[pos] != '/') continue;
        const std::string component = path.substr(start, pos - start);
        if (component.empty() || component == "." || component == "..")
            return false;
        for (unsigned char ch : component)
            if (!(std::isalnum(ch) || ch == '_' || ch == '.'
                || ch == '-' || ch == ' ')) return false;
        start = pos + 1;
    }
    return true;
}

bool archive_size(const std::filesystem::path &path, std::uint64_t &size)
{
    std::error_code error;
    size = std::filesystem::file_size(path, error);
    return !error && size >= 22 && size <= k_max_archive;
}

} // namespace

bool index(const std::filesystem::path &archive,
    std::vector<s_zip_entry> &entries)
{
    entries.clear();
    std::uint64_t length = 0;
    if (!archive_size(archive, length)) return false;
    std::ifstream input(archive, std::ios::binary);
    if (!input) return false;

    // EOCD must be within the last 65,557 bytes, including ZIP comment.
    const std::uint64_t tail_at = length > 65557 ? length - 65557 : 0;
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(length - tail_at));
    if (!read_at(input, tail_at, tail.data(), tail.size())) return false;
    std::size_t eocd = tail.size();
    while (eocd >= 22)
    {
        --eocd;
        if (u32(tail.data() + eocd) == 0x06054b50u
            && eocd + 22u + u16(tail.data() + eocd + 20) == tail.size())
            break;
    }
    if (eocd < 22 || eocd >= tail.size()) return false;
    const auto *end = tail.data() + eocd;
    const std::uint16_t count = u16(end + 10);
    const std::uint32_t central_size = u32(end + 12);
    const std::uint32_t central_offset = u32(end + 16);
    if (u16(end + 4) || u16(end + 6)
        || u16(end + 8) != count || count > k_max_entries
        || count == 0 || count == 0xffffu
        || central_size > k_max_central
        || central_size == 0xffffffffu
        || central_offset == 0xffffffffu
        || static_cast<std::uint64_t>(central_offset) + central_size > tail_at + eocd)
        return false;

    std::vector<std::uint8_t> table(central_size);
    if (!read_at(input, central_offset, table.data(), table.size()))
        return false;
    std::set<std::string> unique;
    std::uint64_t total_size = 0;
    std::size_t at = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        if (at + 46 > table.size() || u32(table.data() + at) != 0x02014b50u)
            return false;
        const auto *p = table.data() + at;
        const std::uint16_t flags = u16(p + 8);
        const std::uint16_t method = u16(p + 10);
        const std::uint32_t compressed = u32(p + 20);
        const std::uint32_t expanded = u32(p + 24);
        const std::uint16_t name_length = u16(p + 28);
        const std::uint16_t extra_length = u16(p + 30);
        const std::uint16_t comment_length = u16(p + 32);
        const std::uint32_t external = u32(p + 38);
        const std::uint32_t offset = u32(p + 42);
        const std::size_t next = at + 46u + name_length + extra_length + comment_length;
        if (next > table.size() || !name_length || name_length > 512
            || flags & ~static_cast<std::uint16_t>(0x0808u)
            || (method != 0 && method != 8)
            || compressed > k_max_file || expanded > k_max_file
            || offset == 0xffffffffu || u16(p + 34)
            || (u16(p + 4) >> 8) == 3 && ((external >> 16) & 0170000u) == 0120000u)
            return false;

        std::string name(reinterpret_cast<const char *>(p + 46), name_length);
        // Directory records are not usable assets; validate their names too.
        const bool directory = name.back() == '/';
        if (directory) name.pop_back();
        if (!safe_name(name)) return false;
        if (!directory)
        {
            if (method == 0 && compressed != expanded) return false;
            if (total_size + expanded > k_max_archive) return false;
            total_size += expanded;
            std::string key = name;
            std::transform(key.begin(), key.end(), key.begin(),
                [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            if (!unique.insert(key).second) return false;
            if (static_cast<std::uint64_t>(offset) + 30 > central_offset)
                return false;
            entries.push_back({name, offset, compressed, expanded,
                u32(p + 16), method});
        }
        at = next;
    }
    if (at != table.size() || entries.empty())
    {
        entries.clear();
        return false;
    }
    return true;
}

bool read(const std::filesystem::path &archive, const s_zip_entry &entry,
    std::vector<std::uint8_t> &output)
{
    output.clear();
    std::uint64_t length = 0;
    if (!archive_size(archive, length)
        || entry.m_size > k_max_file || entry.m_compressed_size > k_max_file
        || !safe_name(entry.m_path)) return false;
    std::ifstream input(archive, std::ios::binary);
    std::array<std::uint8_t, 30> header{};
    if (!input || !read_at(input, entry.m_offset, header.data(), header.size())
        || u32(header.data()) != 0x04034b50u
        || u16(header.data() + 8) != entry.m_method
        || u16(header.data() + 6) & ~static_cast<std::uint16_t>(0x0808u))
        return false;
    const auto name_length = u16(header.data() + 26);
    const auto extra_length = u16(header.data() + 28);
    const std::uint64_t data_at = static_cast<std::uint64_t>(entry.m_offset)
        + header.size() + name_length + extra_length;
    if (!name_length || name_length > 512
        || data_at + entry.m_compressed_size > length) return false;
    std::string local_name(name_length, '\0');
    if (!read_at(input, entry.m_offset + header.size(),
            local_name.data(), name_length) || local_name != entry.m_path)
        return false;
    std::vector<std::uint8_t> compressed(entry.m_compressed_size);
    if (!read_at(input, data_at, compressed.data(), compressed.size()))
        return false;

    std::vector<std::uint8_t> decoded(entry.m_size);
    if (entry.m_method == 0)
    {
        if (entry.m_compressed_size != entry.m_size) return false;
        decoded = std::move(compressed);
    }
    else if (entry.m_method == 8)
    {
        z_stream stream{};
        stream.next_in = compressed.data();
        stream.avail_in = static_cast<uInt>(compressed.size());
        stream.next_out = decoded.data();
        stream.avail_out = static_cast<uInt>(decoded.size());
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
        const int result = inflate(&stream, Z_FINISH);
        const bool ok = result == Z_STREAM_END
            && stream.total_in == compressed.size()
            && stream.total_out == decoded.size();
        inflateEnd(&stream);
        if (!ok) return false;
    }
    else return false;

    const uLong crc = crc32(0L, Z_NULL, 0);
    const auto computed = crc32(crc, decoded.data(),
        static_cast<uInt>(decoded.size()));
    if (static_cast<std::uint32_t>(computed) != entry.m_crc32)
        return false;
    output = std::move(decoded);
    return true;
}
}
