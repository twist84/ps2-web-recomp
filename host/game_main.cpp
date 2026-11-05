#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "game_boot.hpp"
#include "window.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <vector>

void register_generated_code();
void register_generated_vu();
#ifdef PS2_USE_SDBZ_RECONSTRUCTED
namespace ps2 { void register_sdbz_reconstructed(); }
#endif

int main()
{
    try
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) throw std::runtime_error("Cannot locate SDBZ.exe");
        path.resize(length);
        const auto folder = std::filesystem::path(path).parent_path();
        std::filesystem::current_path(folder);
        const auto elf = ps2::game::find_elf(folder);
        const auto settings = folder / L"ps2run.ini";
        if (!SetEnvironmentVariableW(L"PS2_CONFIG", settings.c_str())) throw std::runtime_error("Cannot set game config path");
        SetEnvironmentVariableW(L"PS2_FORCE_DEINTERLACE", nullptr);
        SetEnvironmentVariableW(L"PS2_NO_DEINTERLACE", nullptr);
        ps2::g_options.disc_path = folder.string();
        std::fprintf(stderr, "[game] ELF: %s\n[game] disc folder: %s\n", elf.string().c_str(), folder.string().c_str());

        if (!open_window(false)) return 1;
    register_generated_code();
    register_generated_vu();
#ifdef PS2_USE_SDBZ_RECONSTRUCTED
    ps2::register_sdbz_reconstructed();
#endif
        return run_window([&]() -> int
        {
            std::uint32_t entry = 0;
            if (!ps2::load_elf(elf.string(), entry))
            {
                std::fprintf(stderr, "cannot load %s\n", elf.string().c_str());
                return 1;
            }
            ps2::boot(entry, {elf.string()});
            return ps2::run();
        });
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "[game] %s\n", error.what());
        MessageBoxA(nullptr, error.what(), "SDBZ.exe", MB_OK | MB_ICONERROR);
        return 1;
    }
}
