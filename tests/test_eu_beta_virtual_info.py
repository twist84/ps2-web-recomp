#!/usr/bin/env python3
"""Validate the EU beta's sidecar-free virtual INFO.DAT layout.

No copyrighted archive data is necessary: synthesize a GAME/ directory,
mount the existing folder-backed PS2 DVD reader, and examine its INFO.DAT.
"""

import os
import pathlib
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = r'''#include "disc_folder.hpp"
#include "ps2/disc.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static std::uint32_t read32(const std::uint8_t *p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8)
        | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

int main(int argc, char **argv)
{
    if (argc != 4 || !ps2::disc::open_folder(argv[1])) return 1;
    const std::uint32_t expected_count =
        static_cast<std::uint32_t>(std::stoul(argv[2]));
    const auto info = ps2::disc::find("INFO.DAT");
    const auto game = ps2::disc::find("GAME.DAT");
    if (!info || !game || info->size != 98304) return 2;

    std::vector<std::uint8_t> data(98304);
    if (!ps2::disc::read(info->sector, 48, data.data())) return 3;
    if (data[0] != 0x7F
        || std::memcmp(data.data() + 1, "www.arika.co.jp", 15) != 0) return 4;

    const std::size_t tail = 0x30 + expected_count * 0x30;
    for (std::size_t i = tail; i < data.size(); ++i)
        if (data[i] != 0x40) return 5;

    for (std::size_t i = 16; i < data.size(); ++i)
    {
        const std::uint8_t raw = data[i];
        const std::uint8_t swapped = (raw << 4) | (raw >> 4);
        data[i] = static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(~swapped) - data[i & 15]);
    }

    if (read32(data.data() + 0x2C) != expected_count) return 6;
    std::string previous;
    std::uint32_t next_sector = 0;
    for (std::uint32_t index = 0; index < expected_count; ++index)
    {
        const std::uint8_t *record = data.data() + 0x30 + index * 0x30;
        const auto *end = static_cast<const std::uint8_t *>(
            std::memchr(record, 0, 32));
        const std::size_t length = end ? static_cast<std::size_t>(end - record) : 32;
        if (!length || length > 32) return 7;
        const std::string name(reinterpret_cast<const char *>(record), length);
        if (!previous.empty() && name < previous) return 8;
        if (read32(record + 0x24) != next_sector
            || read32(record + 0x28) != 1
            || read32(record + 0x2C) != 13) return 9;
        previous = name;
        ++next_sector;
    }

    if (game->size != expected_count * 2048) return 10;
    if (argc == 4 && expected_count == 3)
    {
        if (previous != "trc/z_last.bin") return 11;
        std::vector<std::uint8_t> bytes(2048);
        if (!ps2::disc::read(game->sector + 2, 1, bytes.data())) return 12;
        if (std::memcmp(bytes.data(), argv[3], 13) != 0) return 13;
        for (std::size_t i = 13; i < 2048; ++i)
            if (bytes[i] != 0) return 14;
    }
    return 0;
}
'''


class EuBetaVirtualInfoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        env_cxx = os.environ.get("CXX")
        if env_cxx:
            compiler = [part.strip('"') for part in shlex.split(
                env_cxx, posix=(os.name != "nt"))]
        else:
            executable = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
            compiler = [executable] if executable else []
        if not compiler:
            raise unittest.SkipTest("C++ compiler unavailable")

        cls.temp = tempfile.TemporaryDirectory(prefix="sdbz-eu-virtual-")
        cls.root = pathlib.Path(cls.temp.name)
        source = cls.root / "virtual_harness.cpp"
        source.write_text(HARNESS, encoding="utf-8")
        cls.executable = cls.root / ("virtual_harness.exe" if os.name == "nt" else "virtual_harness")
        subprocess.run(compiler + [
            "-std=c++20", "-Wall", "-Wextra",
            "-I", str(ROOT / "runtime/include"),
            "-I", str(ROOT / "runtime/src"),
            str(source), str(ROOT / "runtime/src/disc.cpp"),
            str(ROOT / "runtime/src/disc_folder.cpp"),
            "-o", str(cls.executable),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    def make_assets(self, count):
        root = self.root / f"assets_{count}"
        names = ["dis/a_first.bin", "ply/m_middle.bin", "trc/z_last.bin"]
        if count != 3:
            names = [f"ply/p{i:04d}.bin" for i in range(count)]
        for name in names:
            target = root / "GAME" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"EU-BETA-TEST!" if name == "trc/z_last.bin" else b"A" * 13)
        return root

    def test_eu_beta_three_files_and_spare_slots(self):
        root = self.make_assets(3)
        subprocess.run([
            str(self.executable), str(root), "3", "EU-BETA-TEST!",
        ], check=True)
        self.assertFalse((root / "INFO.DAT").exists())
        self.assertFalse((root / "GAME.DAT").exists())

    def test_full_eu_beta_1934_entry_capacity(self):
        root = self.make_assets(1934)
        subprocess.run([
            str(self.executable), str(root), "1934", "EU-BETA-TEST!",
        ], check=True)

    def test_reaches_suspected_irx_sector_boundary(self):
        root = self.make_assets(2005)
        subprocess.run([
            str(self.executable), str(root), "2005", "EU-BETA-TEST!",
        ], check=True)


if __name__ == "__main__":
    unittest.main()
