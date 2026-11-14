#include "texture_dump_job.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <ctime>
#include <cctype>
#include <exception>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <zlib.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sdbz::texture_dump_job
{
namespace
{
namespace fs = std::filesystem;

constexpr std::size_t k_max_asset_bytes = 64u * 1024u * 1024u;
constexpr std::uint32_t k_max_dimension = 4096;
constexpr std::uint64_t k_sector_size = 2048;
constexpr std::uint32_t k_max_chunks = 4096;
std::mutex g_status_mutex;
s_status g_status;

struct s_texture
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    std::uint64_t fingerprint = 0;
    std::uint32_t psm = 0;
};

struct s_result
{
    unsigned exported = 0;
    unsigned failures = 0;
    unsigned candidates = 0;
    std::vector<std::string> entries;
    std::vector<std::string> registry;
};

std::uint16_t read_u16(const std::uint8_t *data)
{
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t read_u32(const std::uint8_t *data)
{
    return static_cast<std::uint32_t>(data[0])
        | (static_cast<std::uint32_t>(data[1]) << 8)
        | (static_cast<std::uint32_t>(data[2]) << 16)
        | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t fingerprint_pixels(const std::uint8_t *data, std::size_t size)
{
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

void append_be32(std::vector<std::uint8_t> &data, std::uint32_t number)
{
    data.push_back(static_cast<std::uint8_t>(number >> 24));
    data.push_back(static_cast<std::uint8_t>(number >> 16));
    data.push_back(static_cast<std::uint8_t>(number >> 8));
    data.push_back(static_cast<std::uint8_t>(number));
}

std::string json_escape(const std::string &value)
{
    std::string out;
    for (unsigned char c : value)
    {
        if (c == '\\' || c == '"')
        {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        }
        else if (c >= 0x20 && c < 0x7f)
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            char buffer[7];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
        }
    }
    return out;
}

std::string normalized_name(std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos)
        return {};
    std::size_t at = 0;
    while (at < name.size())
    {
        const auto end = name.find('/', at);
        const std::string component = name.substr(at, end - at);
        if (component.empty() || component == "." || component == "..")
            return {};
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return name;
}

fs::path path_from_utf8(const std::string &value)
{
#ifdef _WIN32
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), wide.data(), count))
        return {};
    return fs::path(wide);
#else
    return fs::path(value);
#endif
}

fs::path archive_path(const std::string &name)
{
#ifdef _WIN32
    const int count = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
        name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(932, MB_ERR_INVALID_CHARS,
            name.data(), static_cast<int>(name.size()), wide.data(), count))
        return {};
    return fs::path(wide);
#else
    return fs::path(name);
#endif
}

bool read_file(const fs::path &file, std::vector<std::uint8_t> &out)
{
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error || size > k_max_asset_bytes) return false;
    std::ifstream input(file, std::ios::binary);
    if (!input) return false;
    out.resize(static_cast<std::size_t>(size));
    return static_cast<bool>(input.read(
        reinterpret_cast<char *>(out.data()),
        static_cast<std::streamsize>(out.size())));
}

void png_chunk(std::vector<std::uint8_t> &output,
    const char *tag, const std::vector<std::uint8_t> &bytes)
{
    append_be32(output, static_cast<std::uint32_t>(bytes.size()));
    const auto start = output.size();
    output.insert(output.end(), tag, tag + 4);
    output.insert(output.end(), bytes.begin(), bytes.end());
    const auto checksum = crc32(0, output.data() + start,
        static_cast<uInt>(output.size() - start));
    append_be32(output, static_cast<std::uint32_t>(checksum));
}

bool write_png(const fs::path &file, const s_texture &texture)
{
    if (!texture.width || !texture.height
        || texture.width > k_max_dimension || texture.height > k_max_dimension
        || texture.rgba.size() != static_cast<std::size_t>(texture.width) * texture.height * 4)
        return false;
    const auto stride = static_cast<std::size_t>(texture.width) * 4;
    std::vector<std::uint8_t> scanlines((stride + 1) * texture.height);
    for (std::uint32_t y = 0; y < texture.height; ++y)
    {
        scanlines[static_cast<std::size_t>(y) * (stride + 1)] = 0;
        std::memcpy(scanlines.data() + static_cast<std::size_t>(y) * (stride + 1) + 1,
            texture.rgba.data() + static_cast<std::size_t>(y) * stride, stride);
    }
    uLongf compressed_size = compressBound(static_cast<uLong>(scanlines.size()));
    std::vector<std::uint8_t> compressed(compressed_size);
    if (compress2(compressed.data(), &compressed_size, scanlines.data(),
            static_cast<uLong>(scanlines.size()), Z_BEST_COMPRESSION) != Z_OK)
        return false;
    compressed.resize(compressed_size);
    std::vector<std::uint8_t> png{137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<std::uint8_t> header;
    append_be32(header, texture.width);
    append_be32(header, texture.height);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    png_chunk(png, "IHDR", header);
    png_chunk(png, "IDAT", compressed);
    png_chunk(png, "IEND", {});
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    if (error) return false;
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(png.data()),
        static_cast<std::streamsize>(png.size()));
    return output.good();
}

bool decode_tim2(const std::uint8_t *data, std::size_t size,
    s_texture &texture, std::string &reason)
{
    if (size < 16 || std::memcmp(data, "TIM2", 4) != 0)
    {
        reason = "missing TIM2 header";
        return false;
    }
    const unsigned format = data[5];
    const unsigned pictures = read_u16(data + 6);
    const std::size_t at = format == 1 ? 128 : 16;
    if (format > 1 || pictures != 1 || size < at + 48)
    {
        reason = "unsupported TIM2 layout";
        return false;
    }
    const auto total = read_u32(data + at);
    const auto clut_size = read_u32(data + at + 4);
    const auto image_size = read_u32(data + at + 8);
    const auto header_size = read_u16(data + at + 12);
    const auto colors = read_u16(data + at + 14);
    const auto mipmaps = data[at + 17];
    const auto clut_type = data[at + 18];
    const auto image_type = data[at + 19];
    texture.width = read_u16(data + at + 20);
    texture.height = read_u16(data + at + 22);
    const auto pixels = static_cast<std::uint64_t>(texture.width) * texture.height;
    const auto extent = static_cast<std::uint64_t>(header_size) + image_size + clut_size;
    if (!texture.width || !texture.height
        || texture.width > k_max_dimension || texture.height > k_max_dimension
        || mipmaps != 1 || header_size < 48 || extent > total
        || static_cast<std::uint64_t>(at) + total > size)
    {
        reason = "invalid TIM2 extents or dimensions";
        return false;
    }
    const auto *image = data + at + header_size;
    const auto *palette = image + image_size;
    texture.rgba.resize(static_cast<std::size_t>(pixels) * 4);
    texture.fingerprint = fingerprint_pixels(image, image_size);
    texture.psm = image_type == 3 ? 0x00u
        : image_type == 4 ? 0x14u : image_type == 5 ? 0x13u : 0xffu;
    if (image_type == 3 && image_size == pixels * 4)
    {
        std::memcpy(texture.rgba.data(), image, texture.rgba.size());
        for (std::size_t i = 3; i < texture.rgba.size(); i += 4)
            texture.rgba[i] = static_cast<std::uint8_t>(
                std::min(255u, static_cast<unsigned>(texture.rgba[i]) * 2));
        return true;
    }
    if ((image_type != 4 && image_type != 5)
        || (clut_type & 0x3f) != 3 || colors == 0
        || static_cast<std::uint64_t>(colors) * 4 > clut_size
        || image_size != (image_type == 4 ? (pixels + 1) / 2 : pixels))
    {
        reason = "unsupported TIM2 pixel format or palette";
        return false;
    }
    for (std::size_t i = 0; i < pixels; ++i)
    {
        unsigned index = image_type == 4
            ? (image[i / 2] >> ((i & 1) * 4)) & 15 : image[i];
        if (image_type == 5 && !(clut_type & 0x80))
            index = (index & ~0x18u) | ((index & 8u) << 1) | ((index & 16u) >> 1);
        if (index >= colors)
        {
            reason = "TIM2 palette index outside CLUT";
            return false;
        }
        const std::size_t dst = i * 4;
        const std::size_t src = index * 4;
        std::memcpy(texture.rgba.data() + dst, palette + src, 3);
        texture.rgba[dst + 3] = static_cast<std::uint8_t>(
            std::min(255u, static_cast<unsigned>(palette[src + 3]) * 2));
    }
    return true;
}

void register_image(s_result &result, const std::string &filename,
    const s_texture &texture)
{
    std::ostringstream line;
    line << std::uppercase << std::hex << std::setfill('0')
        << std::setw(16) << texture.fingerprint
        << std::dec << '\t' << texture.psm
        << '\t' << texture.width << '\t' << texture.height
        << '\t' << filename;
    result.registry.push_back(line.str());
}

void note_result(s_result &result, const std::string &name,
    const std::string &error, unsigned images)
{
    if (!error.empty()) ++result.failures;
    result.entries.push_back("    {\"asset\": \"" + json_escape(name)
        + "\", \"textures\": " + std::to_string(images)
        + ", \"error\": \"" + json_escape(error) + "\"}");
}

void export_asset(const std::string &name, const std::vector<std::uint8_t> &data,
    const fs::path &output, s_result &result)
{
    ++result.candidates;
    unsigned count = 0;
    std::string reason;
    const auto base = output / "assets" / "sdbz" / "textures" / archive_path(name);
    if (base.empty() || data.size() < 4)
    {
        note_result(result, name, "invalid or empty texture asset", 0);
        return;
    }
    if (std::memcmp(data.data(), "FPIX", 4) == 0)
    {
        if (data.size() < 36)
            reason = "truncated FPIX header";
        else
        {
            const auto declared = read_u32(data.data() + 4);
            const auto chunks = read_u32(data.data() + 8);
            if (declared != data.size() || !chunks || chunks > k_max_chunks
                || 32ull + static_cast<std::uint64_t>(chunks) * 4 > data.size())
                reason = "invalid FPIX directory";
            else
            {
                std::uint32_t previous = 0;
                for (std::uint32_t i = 0; i < chunks; ++i)
                {
                    const auto start = read_u32(data.data() + 32 + i * 4);
                    const auto end = i + 1 < chunks
                        ? read_u32(data.data() + 32 + (i + 1) * 4)
                        : static_cast<std::uint32_t>(data.size());
                    if (start < 32 + chunks * 4 || start <= previous
                        || end <= start || end > data.size()
                        || end - start < 36
                        || std::memcmp(data.data() + start, "XPIX", 4) != 0
                        || read_u32(data.data() + start + 4) != end - start)
                    {
                        reason = "invalid FPIX chunk";
                        break;
                    }
                    previous = start;
                    s_texture image;
                    if (!decode_tim2(data.data() + start + 32,
                            end - start - 32, image, reason))
                        break;
                    char number[16];
                    std::snprintf(number, sizeof(number), ".%03u.png", i);
                    if (!write_png(fs::path(base.string() + number), image))
                    {
                        reason = "cannot write PNG";
                        break;
                    }
                    register_image(result, name
                        + std::string(number).substr(0, std::strlen(number) - 4),
                        image);
                    ++count;
                    ++result.exported;
                }
            }
        }
    }
    else
    {
        s_texture image;
        const bool wrapped = std::memcmp(data.data(), "XPIX", 4) == 0;
        const bool valid_wrapper = !wrapped || data.size() >= 36;
        const auto *tim2 = data.data() + (wrapped && valid_wrapper ? 32 : 0);
        const auto length = wrapped && valid_wrapper ? data.size() - 32 : data.size();
        if (!valid_wrapper
            || !decode_tim2(tim2, length, image, reason))
        {
            if (reason.empty()) reason = "unsupported asset";
        }
        else if (!write_png(fs::path(base.string() + ".000.png"), image))
            reason = "cannot write PNG";
        else
        {
            register_image(result, name + ".000", image);
            ++count;
            ++result.exported;
        }
    }
    note_result(result, name, reason, count);
}

bool is_texture(const fs::path &path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".pix" || ext == ".tim2" || ext == ".xpix";
}

bool read_archive(const fs::path &info, const fs::path &game,
    const fs::path &output, s_result &result, std::string &reason)
{
    std::vector<std::uint8_t> bytes;
    if (!read_file(info, bytes) || bytes.size() < 48
        || std::memcmp(bytes.data() + 1, "www.arika.co.jp", 15) != 0)
    {
        reason = "Cannot read valid INFO.DAT";
        return false;
    }
    const std::array<std::uint8_t, 16> key = {
        bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],
        bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]
    };
    for (std::size_t i = 16; i < bytes.size(); ++i)
    {
        const auto c = bytes[i];
        const auto reversed = static_cast<std::uint8_t>((c << 4) | (c >> 4));
        bytes[i] = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(~reversed) - key[(i - 16) & 15]);
    }
    const auto count = read_u32(bytes.data() + 0x2c);
    if (count > (bytes.size() - 48) / 48)
    {
        reason = "Invalid INFO.DAT record count";
        return false;
    }
    struct s_entry
    {
        std::string name;
        std::uint64_t offset;
        std::uint64_t length;
        std::uint64_t allocated;
    };
    std::vector<s_entry> records;
    std::ifstream archive(game, std::ios::binary | std::ios::ate);
    if (!archive)
    {
        reason = "Cannot read GAME.DAT";
        return false;
    }
    const auto size = static_cast<std::uint64_t>(archive.tellg());
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto *entry = bytes.data() + 48 + i * 48;
        auto nul = std::find(entry, entry + 32, static_cast<std::uint8_t>(0));
        const std::string name(reinterpret_cast<const char *>(entry),
            static_cast<std::size_t>(nul - entry));
        const auto normalized = normalized_name(name);
        const std::uint64_t offset = static_cast<std::uint64_t>(read_u32(entry + 36)) * k_sector_size;
        const std::uint64_t allocated = static_cast<std::uint64_t>(read_u32(entry + 40)) * k_sector_size;
        const std::uint64_t length = read_u32(entry + 44);
        if (normalized.empty() || archive_path(normalized).empty()
            || length > allocated || offset > size || allocated > size - offset)
        {
            reason = "Invalid GAME.DAT entry path or extent";
            return false;
        }
        records.push_back({normalized, offset, length, allocated});
    }
    auto ordered = records;
    std::sort(ordered.begin(), ordered.end(),
        [](const s_entry &a, const s_entry &b) { return a.offset < b.offset; });
    std::uint64_t previous_end = 0;
    for (const auto &entry : ordered)
    {
        if (entry.offset < previous_end)
        {
            reason = "Overlapping GAME.DAT records";
            return false;
        }
        previous_end = entry.offset + entry.allocated;
    }
    for (const auto &entry : records)
    {
        if (!is_texture(fs::path(entry.name))) continue;
        if (entry.length > k_max_asset_bytes)
        {
            note_result(result, entry.name, "texture exceeds 64 MiB", 0);
            ++result.candidates;
            continue;
        }
        std::vector<std::uint8_t> contents(static_cast<std::size_t>(entry.length));
        archive.seekg(static_cast<std::streamoff>(entry.offset));
        if (!archive.read(reinterpret_cast<char *>(contents.data()),
                static_cast<std::streamsize>(contents.size())))
        {
            note_result(result, entry.name, "cannot read archive asset", 0);
            ++result.candidates;
            archive.clear();
            continue;
        }
        export_asset(entry.name, contents, output, result);
    }
    return true;
}

bool read_loose(const fs::path &root, const fs::path &output,
    s_result &result, std::string &reason)
{
    std::error_code error;
    const auto canonical = fs::canonical(root, error);
    if (error)
    {
        reason = "Cannot open GAME directory";
        return false;
    }
    for (fs::recursive_directory_iterator it(root,
            fs::directory_options::skip_permission_denied, error), end;
        !error && it != end; it.increment(error))
    {
        if (it->is_symlink(error) || error || !it->is_regular_file(error)
            || error || !is_texture(it->path()))
            continue;
        const auto path = fs::relative(it->path(), canonical, error);
        if (error) break;
        const auto name = normalized_name(path.generic_string());
        std::vector<std::uint8_t> contents;
        if (name.empty() || !read_file(it->path(), contents))
        {
            ++result.candidates;
            note_result(result, name, "cannot read loose texture", 0);
            continue;
        }
        export_asset(name, contents, output, result);
    }
    if (error)
    {
        reason = "Error walking GAME directory";
        return false;
    }
    return true;
}

void write_registry(const fs::path &output, const s_result &result)
{
    const fs::path name = output / "assets" / "sdbz" / "textures.registry";
    std::error_code error;
    fs::create_directories(name.parent_path(), error);
    if (error) return;
    std::ofstream stream(name, std::ios::binary | std::ios::trunc);
    stream << "# SDBZ source texture registry v1\n";
    stream << "# FNV64 hex\\tPSM decimal\\twidth\\theight\\tlogical PNG basename\n";
    for (const auto &entry : result.registry)
        stream << entry << '\n';
}

void write_report(const fs::path &output, const s_result &result,
    const std::string &source)
{
    std::ofstream report(output / "dump_report.json", std::ios::binary);
    report << "{\n  \"source\": \"" << json_escape(source) << "\",\n"
        << "  \"assets_considered\": " << result.candidates << ",\n"
        << "  \"textures_exported\": " << result.exported << ",\n"
        << "  \"assets_failed\": " << result.failures << ",\n  \"assets\": [\n";
    for (std::size_t i = 0; i < result.entries.size(); ++i)
        report << result.entries[i] << (i + 1 == result.entries.size() ? "\n" : ",\n");
    report << "  ]\n}\n";
}

void run_dump(const fs::path &disc, const fs::path &output, bool archive)
{
    s_result result;
    std::string reason;
    bool success = false;
    try
    {
        success = archive
            ? read_archive(disc / "INFO.DAT", disc / "GAME.DAT", output, result, reason)
            : read_loose(disc / "GAME", output, result, reason);
        write_registry(output, result);
        write_report(output, result, archive ? "INFO.DAT+GAME.DAT" : "GAME/");
    }
    catch (const std::exception &error)
    {
        reason = error.what();
    }
    std::string message;
    if (!success)
        message = "Texture dump failed: " + reason;
    else
        message = "Dumped " + std::to_string(result.exported)
            + " textures; " + std::to_string(result.failures) + " failed";
    std::lock_guard<std::mutex> lock(g_status_mutex);
    g_status = {false, true, success ? (result.failures ? 2 : 0) : 1, message};
}
}

bool start(const std::string &disc_path, const std::string &executable_directory)
{
    std::lock_guard<std::mutex> lock(g_status_mutex);
    if (g_status.running) return false;
    std::error_code error;
    const fs::path disc_root = path_from_utf8(disc_path);
    const fs::path disc = fs::is_directory(disc_root, error)
        ? disc_root : disc_root.parent_path();
    const fs::path exe = path_from_utf8(executable_directory);
    const bool archive = fs::is_regular_file(disc / "INFO.DAT", error)
        && fs::is_regular_file(disc / "GAME.DAT", error);
    if (!archive && !fs::is_directory(disc / "GAME", error))
    {
        g_status = {false, true, 1, "No GAME/ or INFO.DAT + GAME.DAT found"};
        return false;
    }
    const fs::path pack_root = exe / "resourcepacks";
    fs::create_directories(pack_root, error);
    if (error)
    {
        g_status = {false, true, 1, "Cannot create resourcepacks directory"};
        return false;
    }
    // Local wall-clock time, using a filename-safe format on Windows.
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm local_time{};
#ifdef _WIN32
    if (localtime_s(&local_time, &now) != 0)
#else
    if (localtime_r(&now, &local_time) == nullptr)
#endif
    {
        g_status = {false, true, 1, "Cannot determine texture dump timestamp"};
        return false;
    }
    char timestamp[32]{};
    if (!std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H-%M-%S",
            &local_time))
    {
        g_status = {false, true, 1, "Cannot format texture dump timestamp"};
        return false;
    }
    const std::string name = std::string("GAME Texture Dump ") + timestamp;
    fs::path output;
    for (unsigned index = 1; index <= 9999; ++index)
    {
        output = pack_root / (index == 1 ? name
            : name + " (" + std::to_string(index) + ")");
        error.clear();
        if (fs::create_directory(output, error)) break;
        if (error || index == 9999)
        {
            g_status = {false, true, 1, "Cannot reserve a dump output folder"};
            return false;
        }
    }
    std::ofstream manifest(output / "pack.json", std::ios::binary);
    manifest << "{\n  \"pack\": {\"format\": 1, \"game\": \"sdbz\","
        << " \"name\": \"" << output.filename().string() << "\","
        << " \"region\": \"eu-beta\", \"priority\": 0}\n}\n";
    manifest.close();
    if (!manifest)
    {
        g_status = {false, true, 1, "Cannot create pack.json"};
        return false;
    }
    g_status = {true, false, 0, archive
        ? "Dumping textures directly from GAME.DAT..."
        : "Dumping textures from GAME/..."};
    std::thread([disc, output, archive]()
    {
        run_dump(disc, output, archive);
    }).detach();
    return true;
}

s_status status()
{
    std::lock_guard<std::mutex> lock(g_status_mutex);
    return g_status;
}
}
