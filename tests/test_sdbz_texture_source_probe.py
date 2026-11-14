#!/usr/bin/env python3
"""Host-only original-name constructor probe tests (no game assets)."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "sdbz_texture_source_probe.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace ps2
{
std::uint8_t g_ram[kRamSize]{};
}
namespace ps2::gs
{
int g_named_publishes = 0;
void publish_named_texture(const std::string &logical_name,
    std::uint32_t bp, std::uint32_t bw, std::uint32_t psm,
    std::uint32_t width, std::uint32_t height)
{
    if (logical_name == "dis/main_menu.pix.001" && bp == 0x120u
        && bw == 1 && psm == 19 && width == 4 && height == 4)
    {
        ++g_named_publishes;
    }
}
}

static void put_u16(std::uint8_t *p, std::uint16_t value)
{
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

static void put_u32(std::uint8_t *p, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
    {
        p[i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

int main()
{
    ps2::Cpu cpu{};
    // Verified first words of CPixTexture::Initialize and TEX0 builder.
    put_u32(ps2::g_ram + 0x19AA70u, 0x27BDFF70u);
    put_u32(ps2::g_ram + 0x10C3E0u, 0x27BDFF30u);
    std::uint8_t *name = ps2::g_ram + 0x1010u;
    std::strcpy(reinterpret_cast<char *>(name), "dis/main_menu.pix.001");

    auto *tim2 = ps2::g_ram + 0x2000u;
    std::memcpy(tim2, "TIM2", 4);
    std::uint8_t *picture = tim2 + 128u;
    put_u32(picture, 64);
    put_u32(picture + 8, 16);
    picture[12] = 48; // picture header length
    picture[19] = 5;  // 8-bit indexed source
    picture[20] = 4;  // width
    picture[22] = 4;  // height
    cpu.r[4].w[0] = 0x1000u;
    cpu.r[5].w[0] = 0x2000u;
    ps2::sdbz_texture_source_probe(cpu, 0x0019A670u);

    // Guest CTexture::Initialize writes its handle at object +0x38.
    put_u32(ps2::g_ram + 0x1038u, 1);
    // Both builds have the same 48-byte texture entry layout but
    // different global table addresses.
    for (const std::uint32_t table : {0x004FF8F0u, 0x005007F0u})
    {
        auto *slot = ps2::g_ram + table;
        put_u16(slot + 2, 0x0120u); // GS base block
        put_u16(slot + 4, 4);       // CLUT base pointer, not TBW
        put_u16(slot + 14, 19);     // GS PSMT8
        put_u16(slot + 22, 4);      // original width
        put_u16(slot + 24, 4);      // original height
    }
    cpu.r[4].w[0] = 1;
    ps2::sdbz_texture_source_probe(cpu, 0x0010C3E0u);
    // Invalid handles cannot be assigned a filename.
    cpu.r[4].w[0] = 256;
    ps2::sdbz_texture_source_probe(cpu, 0x0010C3E0u);
    cpu.r[4].w[0] = 0x1000u;

    auto *fpix = ps2::g_ram + 0x3000u;
    std::memcpy(fpix, "FPIX", 4);
    put_u32(fpix + 4, 64);
    put_u32(fpix + 8, 1);
    cpu.r[5].w[0] = 0x3000u;
    ps2::sdbz_texture_source_probe(cpu, 0x0019AA70u);

    // Switch to the Japanese executable. The EU addresses now contain
    // unrelated code and must no longer identify textures or handles.
    put_u32(ps2::g_ram + 0x19AA70u, 0);
    put_u32(ps2::g_ram + 0x10C3E0u, 0);
    put_u32(ps2::g_ram + 0x19A860u, 0x27BDFF70u);
    put_u32(ps2::g_ram + 0x10C350u, 0x27BDFF30u);
    cpu.r[5].w[0] = 0x2000u;
    ps2::sdbz_texture_source_probe(cpu, 0x0019A670u);
    ps2::sdbz_texture_source_probe(cpu, 0x0019A460u);
    cpu.r[4].w[0] = 1;
    ps2::sdbz_texture_source_probe(cpu, 0x0010C3E0u);
    ps2::sdbz_texture_source_probe(cpu, 0x0010C350u);
    cpu.r[4].w[0] = 0x1000u;
    cpu.r[5].w[0] = 0x3000u;
    ps2::sdbz_texture_source_probe(cpu, 0x0019A860u);

    // An invalid TIM2 source must not generate a named identity.
    tim2[0] = 0;
    cpu.r[5].w[0] = 0x2000u;
    ps2::sdbz_texture_source_probe(cpu, 0x0019A460u);
    return ps2::gs::g_named_publishes == 2 ? 0 : 1;
}
'''

class NamedTextureProbeTests(unittest.TestCase):
    def test_guest_constructor_names(self):
        setting = os.environ.get("CXX")
        compiler = shlex.split(setting, posix=(os.name != "nt")) if setting else [
            shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise unittest.SkipTest("no C++ compiler")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main = root / "main.cpp"
            executable = root / ("probe.exe" if os.name == "nt" else "probe")
            main.write_text(HARNESS, encoding="utf-8")
            subprocess.run(compiler + [
                "-std=c++20", "-Wall", "-Wextra",
                "-I", str(ROOT / "runtime/include"),
                "-I", str(ROOT / "runtime/src"),
                str(main),
                str(ROOT / "runtime/src/sdbz_texture_source_probe.cpp"),
                "-o", str(executable)
            ], check=True)
            env = dict(os.environ)
            env["PS2_SDBZ_NAME_ONLY"] = "1"
            env["PS2_TRACE_SDBZ_TEXTURE_NAMES"] = "1"
            done = subprocess.run([str(executable)], env=env,
                                  text=True, capture_output=True, check=True)
            self.assertEqual(done.stderr.count("[sdbz-name] texture="), 2)
            self.assertEqual(done.stderr.count("[sdbz-name] FPIX"), 2)
            self.assertEqual(done.stderr.count("[sdbz-name] bind"), 2)
            self.assertIn("bp=0120 bw=1 psm=19 size=4x4", done.stderr)
            self.assertIn("dis/main_menu.pix.001", done.stderr)
            self.assertIn("pixels=000020B0", done.stderr)
            self.assertIn("PSM=19 size=4x4", done.stderr)

if __name__ == "__main__":
    unittest.main()
