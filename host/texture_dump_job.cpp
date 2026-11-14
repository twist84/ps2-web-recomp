#include "texture_dump_job.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sdbz::texture_dump_job
{
namespace
{
namespace fs = std::filesystem;

std::mutex g_mutex;
s_status g_status;

fs::path find_script(const fs::path &executable)
{
    std::vector<fs::path> candidates = {
        executable / "scripts" / "dump_sdbz_game_textures.py",
        executable / "dump_sdbz_game_textures.py",
        fs::current_path() / "scripts" / "dump_sdbz_game_textures.py",
        executable.parent_path() / "scripts" / "dump_sdbz_game_textures.py",
        executable.parent_path().parent_path() / "scripts" / "dump_sdbz_game_textures.py"
    };
    for (const auto &candidate : candidates)
    {
        std::error_code error;
        if (fs::is_regular_file(candidate, error) && !error)
            return fs::absolute(candidate);
    }
    return {};
}

fs::path find_source_root(const fs::path &disc)
{
    std::error_code error;
    if (fs::is_directory(disc, error) && !error)
        return disc;
    if (fs::is_regular_file(disc, error) && !error)
        return disc.parent_path();
    return {};
}

std::string report_count(const fs::path &file, const char *key)
{
    std::ifstream input(file, std::ios::binary);
    if (!input) return "?";
    std::string buffer(8192, '\0');
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<std::size_t>(input.gcount()));
    const std::string token = std::string("\"") + key + "\"";
    auto pos = buffer.find(token);
    if (pos == std::string::npos) return "?";
    pos = buffer.find(':', pos + token.size());
    if (pos == std::string::npos) return "?";
    pos = buffer.find_first_of("0123456789", pos + 1);
    if (pos == std::string::npos) return "?";
    const auto end = buffer.find_first_not_of("0123456789", pos);
    return buffer.substr(pos, end - pos);
}

#ifdef _WIN32
std::wstring quote_arg(const std::wstring &arg)
{
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : arg)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'"')
        {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(character);
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}
#endif

int run_process(const std::vector<std::string> &args)
{
#ifdef _WIN32
    // CreateProcessW accepts a mutable command line and avoids shell injection.
    std::wstring command;
    for (const auto &arg : args)
    {
        if (!command.empty()) command += L" ";
        command += quote_arg(fs::u8path(arg).wstring());
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return -1;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
#else
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0)
    {
        std::vector<char *> argv;
        for (const auto &arg : args)
            argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int result = 0;
    if (waitpid(pid, &result, 0) != pid) return -1;
    return WIFEXITED(result) ? WEXITSTATUS(result) : -1;
#endif
}

void finish(const int exit_code, const fs::path &output)
{
    std::string summary = "Texture dump failed";
    if (exit_code == 0 || exit_code == 2)
    {
        summary = "Dump: " + report_count(output / "dump_report.json",
                "textures_exported") + " textures, "
            + report_count(output / "dump_report.json",
                "assets_failed") + " failed";
        if (exit_code == 2) summary += " - see dump_report.json";
    }
    else if (exit_code == 127 || exit_code == -1)
    {
        summary = "Texture dump failed: Python not available";
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.running = false;
    g_status.complete = true;
    g_status.exit_code = exit_code;
    g_status.message = std::move(summary);
}
}

bool start(const std::string &disc_path, const std::string &executable_directory)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_status.running) return false;
    namespace fs = std::filesystem;
    const fs::path executable = fs::u8path(executable_directory);
    const fs::path disc = find_source_root(fs::u8path(disc_path));
    const fs::path script = find_script(executable);
    if (script.empty())
    {
        g_status = {false, true, 1, "Exporter script not found beside SDBZ.exe"};
        return false;
    }
    if (disc.empty())
    {
        g_status = {false, true, 1, "No disc folder found for texture export"};
        return false;
    }
    const fs::path info = disc / "INFO.DAT";
    const fs::path game_dat = disc / "GAME.DAT";
    const fs::path loose = disc / "GAME";
    std::error_code error;
    const bool archive = fs::is_regular_file(info, error) && !error
        && fs::is_regular_file(game_dat, error) && !error;
    if (!archive && !(fs::is_directory(loose, error) && !error))
    {
        g_status = {false, true, 1, "No GAME/ or INFO.DAT + GAME.DAT found"};
        return false;
    }
    const fs::path pack_root = executable / "resourcepacks";
    fs::create_directories(pack_root, error);
    if (error)
    {
        g_status = {false, true, 1, "Cannot create resourcepacks directory"};
        return false;
    }
    fs::path output;
    for (unsigned index = 1; index <= 9999; ++index)
    {
        output = pack_root / (index == 1
            ? "GAME Texture Dump"
            : "GAME Texture Dump " + std::to_string(index));
        error.clear();
        if (!fs::exists(output, error) && !error) break;
        if (index == 9999 || error)
        {
            g_status = {false, true, 1, "No free texture dump folder"};
            return false;
        }
    }
    // All file arguments come from trusted paths; no shell is involved.
    std::vector<std::string> arguments = {
#ifdef _WIN32
        "python",
#else
        "python3",
#endif
        script.string(),
    };
    if (archive)
    {
        arguments.insert(arguments.end(), {
            "--info", info.string(), "--game-dat", game_dat.string()
        });
    }
    else
    {
        arguments.insert(arguments.end(), {"--game-dir", loose.string()});
    }
    arguments.insert(arguments.end(), {"--output", output.string()});
    g_status = {true, false, 0,
        std::string("Dumping ") + (archive ? "GAME.DAT" : "GAME/") + " textures..."};
    std::thread([arguments = std::move(arguments), output]()
    {
        const int code = run_process(arguments);
        finish(code, output);
    }).detach();
    return true;
}

s_status status()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}
}
