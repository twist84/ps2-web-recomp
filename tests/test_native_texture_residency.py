#!/usr/bin/env python3
"""Host-only GS replacement residency tests: no real game assets required."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "gs/native_texture_residency.hpp"
#include "ps2/disc.hpp"
#include "ps2/resource_pack.hpp"
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

bool g_replacement_enabled = true;

namespace ps2::disc
{
bool is_open() { return true; }
std::optional<NativeAsset> find_native_asset(
    const std::string &name, const std::string &format)
{
    if (g_replacement_enabled
        && name == "dis/main_menu.pix.000" && format == "png")
        return NativeAsset{0, 1024, name, "PNG"};
    return std::nullopt;
}
}

namespace ps2::gs::native_texture
{
bool load_png(const std::string &name, s_image &out)
{
    if (!g_replacement_enabled || name != "dis/main_menu.pix.000") return false;
    out.width = out.height = 512;
    out.rgba.assign(512 * 512 * 4, 255);
    return true;
}
}

int main()
{
    using namespace ps2::gs;
    using namespace ps2::gs::native_texture;
    const auto root = std::filesystem::temp_directory_path()
        / "sdbz-native-registry-test";
    std::filesystem::create_directories(root / "resourcepacks");
    {
        std::ofstream registry(root / "native_textures.registry");
        registry << "825F7B0566C19109\t19\t256\t256\tdis/main_menu.pix.000\n"
            << "B51AF81B5A15B2A7\t19\t512\t512\tdis/main_menu.pix.001\n"
            << "1111222233334444\t20\t64\t64\tply/character.pix.000\n";
    }
    // Registry entries are discovered from pack assets even when disabled.
    const auto extra = root / "resourcepacks" / "Extra Pack"
        / "assets" / "sdbz";
    std::filesystem::create_directories(extra);
    {
        std::ofstream manifest(root / "resourcepacks" / "Extra Pack" / "pack.json");
        manifest << "{\"pack\":{\"format\":1,\"game\":\"sdbz\"}}";
        std::ofstream registry(extra / "textures.registry");
        registry << "5555666677778888\t19\t128\t128\tply/extra.pix.000\n";
        std::ofstream order(root / "resourcepacks" / "load_order.txt");
        order << "-Extra Pack\n";
    }
    ps2::resource_pack::mount(root, root);
    reset_residency();
    if (!replacements_available() || registered_texture_count() != 4) return 1;
    if (texture_path(3) != "ply/extra.pix.000") return 28;
    if (atlas_scale(0, 256, 256) != 1
        || atlas_scale(0, 512, 512) != 2
        || atlas_scale(1, 512, 512) != 1
        || atlas_scale(1, 1024, 1024) != 2
        || valid_atlas_size(1, 512, 1024)
        || valid_atlas_size(1, 2048, 2048)
        || valid_atlas_size(0, 768, 768)
        || valid_atlas_size(13, 512, 512)) return 15;
    if (!chunk_image(0) || chunk_image(0)->width != 512) return 16;

    s_texture_upload_signature upload{};
    upload.bitbltbuf = (0x3000ull << 32) | (4ull << 48) | (0x13ull << 56);
    upload.trxreg = 256ull | (256ull << 32);
    upload.expected_bytes = upload.received_bytes = 256ull * 256;
    upload.fnv1a64 = 0x825F7B0566C19109ull;
    if (complete_upload(upload) != 0) return 2;
    if (resident_chunk(0x3000, 4, 0x13, 256, 256) != 0) return 3;
    if (resident_chunk(0x3000, 8, 0x13, 256, 256) != -1) return 4;
    if (resident_chunk(0x3000, 4, 0x14, 256, 256) != -1) return 5;
    if (resident_chunk(0x3000, 4, 0x13, 512, 512) != -1) return 6;

    // Duplicate atlas uploaded to another physical GS destination.
    upload.bitbltbuf = (0x3200ull << 32) | (4ull << 48) | (0x13ull << 56);
    if (complete_upload(upload) != 0) return 7;
    if (resident_chunk(0x3000, 4, 0x13, 256, 256) != 0) return 8;
    if (resident_chunk(0x3200, 4, 0x13, 256, 256) != 0) return 9;

    // Overwriting the first resident range must not remove the second.
    const std::uint64_t overwrite = (0x3080ull << 32)
        | (1ull << 48) | (0ull << 56);
    invalidate_destination(overwrite, 8ull | (8ull << 32));
    if (resident_chunk(0x3000, 4, 0x13, 256, 256) != -1) return 10;
    if (resident_chunk(0x3200, 4, 0x13, 256, 256) != 0) return 11;

    // Changing resource packs must not destroy the original upload's identity.
    g_replacement_enabled = false;
    reload_replacements();
    if (!replacements_available() || chunk_image(0)) return 17;
    if (resident_chunk(0x3200, 4, 0x13, 256, 256) != 0) return 18;
    const auto active = resident_chunks();
    if (active.size() != 1 || active[0] != 0) return 19;

    // Even a texture uploaded while packs are disabled retains identity.
    upload.bitbltbuf = (0x3400ull << 32) | (4ull << 48)
        | (0x13ull << 56);
    if (complete_upload(upload) != 0) return 20;
    if (resident_chunk(0x3400, 4, 0x13, 256, 256) != 0) return 21;
    g_replacement_enabled = true;
    reload_replacements();
    if (!replacements_available() || !chunk_image(0)) return 22;
    if (resident_chunk(0x3400, 4, 0x13, 256, 256) != 0) return 23;

    // A non-menu, four-bit source fingerprint is resolved from the same
    // registry. No hardcoded menu offsets, PSM or filename assumptions.
    s_texture_upload_signature character{};
    character.bitbltbuf = (0x3500ull << 32) | (1ull << 48)
        | (0x14ull << 56);
    character.trxreg = 64ull | (64ull << 32);
    character.expected_bytes = character.received_bytes = 64ull * 64 / 2;
    character.fnv1a64 = 0x1111222233334444ull;
    if (complete_upload(character) != 2) return 24;
    if (resident_chunk(0x3500, 1, 0x14, 64, 64) != 2) return 25;
    if (texture_path(2) != "ply/character.pix.000") return 26;
    invalidate_destination((0x3500ull << 32), 64ull | (64ull << 32));
    if (resident_chunk(0x3500, 1, 0x14, 64, 64) != -1) return 27;

    // Wrong fingerprints or incomplete transfers may never establish identity.
    upload.bitbltbuf = (0x3300ull << 32) | (4ull << 48) | (0x13ull << 56);
    upload.fnv1a64 ^= 0x1234;
    if (complete_upload(upload) != -1) return 12;
    upload.fnv1a64 ^= 0x1234;
    --upload.received_bytes;
    if (complete_upload(upload) != -1) return 13;
    // A new upload reusing the same GS base pointer replaces the old
    // mapping; changing dimensions must not leave a stale hit behind.
    upload.received_bytes = upload.expected_bytes;
    upload.bitbltbuf = (0x3200ull << 32) | (4ull << 48)
        | (0x13ull << 56);
    upload.fnv1a64 = 0x825F7B0566C19109ull;
    if (complete_upload(upload) != 0) return 29;
    if (resident_chunk(0x3200, 4, 0x13, 256, 256) != 0) return 30;
    character.bitbltbuf = (0x3200ull << 32) | (1ull << 48)
        | (0x14ull << 56);
    if (complete_upload(character) != 2) return 31;
    if (resident_chunk(0x3200, 4, 0x13, 256, 256) != -1) return 32;
    if (resident_chunk(0x3200, 1, 0x14, 64, 64) != 2) return 33;
    if (resident_chunk(0x4000, 1, 0x14, 64, 64) != -1) return 34;
    reset_residency();
    if (resident_chunk(0x3200, 4, 0x13, 256, 256) != -1) return 14;
    if (resident_chunk(0x3200, 1, 0x14, 64, 64) != -1) return 35;
    return 0;
}
'''

class NativeResidencyTests(unittest.TestCase):
    def test_residency_and_overwrite(self):
        setting = os.environ.get("CXX")
        compiler = shlex.split(setting, posix=(os.name != "nt")) if setting else [
            shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise unittest.SkipTest("no compiler available")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            src = root / "main.cpp"
            exe = root / ("residency.exe" if os.name == "nt" else "residency")
            src.write_text(HARNESS, encoding="utf-8")
            subprocess.run(compiler + ["-std=c++20", "-Wall", "-Wextra",
                "-I", str(ROOT / "runtime/src"),
                "-I", str(ROOT / "runtime/include"),
                str(src), str(ROOT / "runtime/src/gs/native_texture_residency.cpp"),
                str(ROOT / "runtime/src/resource_pack.cpp"),
            str(ROOT / "runtime/src/resource_pack_zip.cpp"),
                "-lz", "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == "__main__":
    unittest.main()
