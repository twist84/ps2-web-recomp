#include "../host/texture_dump_job.hpp"

#include <array>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <zlib.h>

namespace fs = std::filesystem;

void put16(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint16_t value)
{
    bytes.at(at) = static_cast<std::uint8_t>(value);
    bytes.at(at + 1) = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        bytes.at(at + i) = static_cast<std::uint8_t>(value >> (i * 8));
}

void write_file(const fs::path &path, const std::vector<std::uint8_t> &bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> tim2()
{
    std::vector<std::uint8_t> image(16 + 48 + 8);
    image[0] = 'T';
    image[1] = 'I';
    image[2] = 'M';
    image[3] = '2';
    image[4] = 4;
    put16(image, 6, 1);
    put32(image, 16, 56);
    put32(image, 24, 8);
    put16(image, 28, 48);
    image[33] = 1;
    image[35] = 3;
    put16(image, 36, 2);
    put16(image, 38, 1);
    const std::array<std::uint8_t, 8> pixels = {255, 0, 0, 128, 0, 255, 0, 64};
    std::copy(pixels.begin(), pixels.end(), image.begin() + 64);
    return image;
}

void make_archive(const fs::path &folder)
{
    const auto image = tim2();
    std::vector<std::uint8_t> game(2048);
    std::copy(image.begin(), image.end(), game.begin());
    write_file(folder / "GAME.DAT", game);
    std::vector<std::uint8_t> info(96);
    const std::array<std::uint8_t, 16> key = {
        0x7f,'w','w','w','.','a','r','i','k','a','.','c','o','.','j','p'
    };
    std::copy(key.begin(), key.end(), info.begin());
    put32(info, 44, 1);
    const std::string name = "dis/sample.tim2";
    std::copy(name.begin(), name.end(), info.begin() + 48);
    put32(info, 48 + 40, 1);
    put32(info, 48 + 44, static_cast<std::uint32_t>(image.size()));
    for (std::size_t i = 16; i < info.size(); ++i)
    {
        auto value = static_cast<std::uint8_t>(~static_cast<std::uint8_t>(
            info[i] + key[(i - 16) & 15]));
        info[i] = static_cast<std::uint8_t>((value << 4) | (value >> 4));
    }
    write_file(folder / "INFO.DAT", info);
}

bool wait_done()
{
    for (int i = 0; i < 500; ++i)
    {
        if (!sdbz::texture_dump_job::status().running) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool check_result(const fs::path &output)
{
    const auto png = output / "assets/sdbz/textures/dis/sample.tim2.000.png";
    if (!fs::is_regular_file(png)) return false;
    std::ifstream stream(png, std::ios::binary);
    const std::vector<std::uint8_t> data{
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()
    };
    if (data.size() < 60 || data[0] != 137 || data[1] != 'P') return false;
    if (data[24] != 8 || data[25] != 6) return false; // RGBA8 IHDR
    std::ifstream report(output / "dump_report.json");
    std::string text(std::istreambuf_iterator<char>{report}, {});
    return text.find("\"textures_exported\": 1") != std::string::npos
        && text.find("\"assets_failed\": 0") != std::string::npos;
}

int main()
{
    const auto root = fs::temp_directory_path() /
        ("sdbz-native-dump-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "resourcepacks");
    const auto loose = root / "loose";
    write_file(loose / "GAME/dis/sample.tim2", tim2());
    if (!sdbz::texture_dump_job::start(loose.string(), root.string())
        || !wait_done() || sdbz::texture_dump_job::status().exit_code != 0)
    {
        std::cerr << "Loose GAME export failed: "
                  << sdbz::texture_dump_job::status().message << "\n";
        return 1;
    }
    if (!check_result(root / "resourcepacks/GAME Texture Dump")) return 2;
    const auto archived = root / "archived";
    make_archive(archived);
    if (!sdbz::texture_dump_job::start(archived.string(), root.string())
        || !wait_done() || sdbz::texture_dump_job::status().exit_code != 0)
    {
        std::cerr << "GAME.DAT export failed: "
                  << sdbz::texture_dump_job::status().message << "\n";
        return 3;
    }
    if (!check_result(root / "resourcepacks/GAME Texture Dump 2")) return 4;
    if (!fs::exists(root / "resourcepacks/GAME Texture Dump/pack.json")) return 5;
    fs::remove_all(root);
    return 0;
}
