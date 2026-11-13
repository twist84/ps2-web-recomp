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
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ps2::disc
{
bool is_open() { return true; }
std::optional<NativeAsset> find_native_asset(
    const std::string &name, const std::string &format)
{
    if (name == "dis/main_menu.pix.000" && format == "png")
        return NativeAsset{0, 1024, name, "PNG"};
    return std::nullopt;
}
}

namespace ps2::gs::native_texture
{
bool load_fpix_chunk_png(const std::string &, std::uint32_t chunk, s_image &out)
{
    if (chunk != 0) return false;
    out.width = out.height = 512;
    out.rgba.assign(512 * 512 * 4, 255);
    return true;
}
}

int main()
{
    using namespace ps2::gs;
    using namespace ps2::gs::native_texture;
    reset_residency();
    if (!replacements_available()) return 1;
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

    // Wrong fingerprints or incomplete transfers may never establish identity.
    upload.bitbltbuf = (0x3300ull << 32) | (4ull << 48) | (0x13ull << 56);
    upload.fnv1a64 ^= 0x1234;
    if (complete_upload(upload) != -1) return 12;
    upload.fnv1a64 ^= 0x1234;
    --upload.received_bytes;
    if (complete_upload(upload) != -1) return 13;
    reset_residency();
    if (resident_chunk(0x3200, 4, 0x13, 256, 256) != -1) return 14;
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
