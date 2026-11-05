#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace ps2::game
{
inline std::string trim(std::string text)
{
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!text.empty() && space(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
    while (!text.empty() && space(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

inline std::string upper(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

inline bool ee_executable(const std::filesystem::path &path)
{
    std::array<unsigned char, 20> header{};
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char *>(header.data()), header.size());
    return input.good() && header[0] == 0x7F && header[1] == 'E' && header[2] == 'L' && header[3] == 'F' &&
        header[4] == 1 && header[5] == 1 && header[16] == 2 && header[17] == 0 && header[18] == 8 && header[19] == 0;
}

inline std::filesystem::path find_elf(const std::filesystem::path &folder)
{
    namespace fs = std::filesystem;
    fs::path config;
    for (const auto &entry : fs::directory_iterator(folder))
        if (entry.is_regular_file() && upper(entry.path().filename().string()) == "SYSTEM.CNF") config = entry.path();
    std::string boot, boot2;
    if (!config.empty())
    {
        std::ifstream input(config);
        if (!input) throw std::runtime_error("Cannot read SYSTEM.CNF beside SDBZ.exe");
        std::string line;
        while (std::getline(input, line))
        {
            const auto equals = line.find('=');
            if (equals == std::string::npos) continue;
            const auto key = upper(trim(line.substr(0, equals)));
            if (key == "BOOT2") boot2 = trim(line.substr(equals + 1));
            else if (key == "BOOT") boot = trim(line.substr(equals + 1));
        }
    }
    std::string value = boot2.empty() ? boot : boot2;
    if (!value.empty())
    {
        if (value.front() == '"' || value.front() == '\'')
        {
            const auto end = value.find(value.front(), 1);
            if (end == std::string::npos) throw std::runtime_error("Unclosed BOOT2 filename in SYSTEM.CNF");
            value = value.substr(1, end - 1);
        }
        else value = value.substr(0, value.find_first_of(" \t\r\n"));
        if (const auto colon = value.find(':'); colon != std::string::npos) value.erase(0, colon + 1);
        std::replace(value.begin(), value.end(), '\\', '/');
        while (!value.empty() && value.front() == '/') value.erase(value.begin());
        if (const auto version = value.find(';'); version != std::string::npos) value.resize(version);
        const fs::path relative = std::u8string(value.begin(), value.end());
        if (relative.empty() || relative.is_absolute() || relative.has_root_name() || value.find(':') != std::string::npos)
            throw std::runtime_error("Invalid BOOT2 filename in SYSTEM.CNF");
        for (const auto &part : relative) if (part == "..") throw std::runtime_error("BOOT2 must refer to a file in the extracted disc folder");
        const auto elf = folder / relative;
        if (!fs::is_regular_file(elf) || !ee_executable(elf)) throw std::runtime_error("SYSTEM.CNF boot file is missing or is not a PS2 executable: " + value);
        return elf;
    }

    fs::path found;
    for (const auto &entry : fs::directory_iterator(folder))
    {
        if (!entry.is_regular_file() || !ee_executable(entry.path())) continue;
        if (!found.empty()) throw std::runtime_error("Several PS2 executables found; provide SYSTEM.CNF with BOOT2 beside SDBZ.exe");
        found = entry.path();
    }
    if (found.empty()) throw std::runtime_error("Place SDBZ.exe beside the extracted game ELF and SYSTEM.CNF");
    return found;
}
}
