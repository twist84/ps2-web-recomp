#!/usr/bin/env python3
# End-to-end tests for GAME.DAT growth with a runtime-generated ISO.

import os
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

HARNESS = r'''#include "disc_folder.hpp"
#include "ps2/disc.hpp"
#include <cstdint>
#include <cstring>
#include <vector>
static std::uint32_t le32(const std::uint8_t *p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24); }
static std::uint32_t be32(const std::uint8_t *p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]); }
int main(int argc, char **argv)
{
    if (argc != 3 || !ps2::disc::open_folder(argv[1])) return 1;
    const bool has_fallback = std::strcmp(argv[2], "fallback") == 0;
    const auto info = ps2::disc::find("INFO.DAT");
    const auto game = ps2::disc::find("GAME.DAT");
    if (!info || !game) return 2;
    std::vector<std::uint8_t> info_data(((info->size + 2047) / 2048) * 2048);
    if (!ps2::disc::read(info->sector, static_cast<std::uint32_t>(info_data.size() / 2048), info_data.data())) return 3;
    for (std::size_t i = 16; i < info->size; ++i) { const auto v = info_data[i]; const auto s = static_cast<std::uint8_t>((v << 4) | (v >> 4)); info_data[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(~s) - info_data[i & 15]); }
    const auto *record = info_data.data() + 0x30;
    const std::uint32_t asset_offset = le32(record + 0x24);
    if (le32(record + 0x28) != 3 || le32(record + 0x2c) != 5000 || asset_offset < 2) return 4;
    const std::uint32_t expected_game_size = (asset_offset + 3) * 2048;
    if (game->size != expected_game_size) return 5;
    std::vector<std::uint8_t> asset(3 * 2048);
    if (!ps2::disc::read(game->sector + asset_offset, 3, asset.data())) return 6;
    for (std::size_t i = 0; i < 5000; ++i) if (asset[i] != 'A') return 7;
    std::vector<std::uint8_t> pvd(2048);
    if (!ps2::disc::read(16, 1, pvd.data())) return 8;
    const auto volume = le32(pvd.data() + 80);
    if (be32(pvd.data() + 84) != volume || volume != ps2::disc::sector_count()) return 9;
    std::vector<std::uint8_t> fallback(2048);
    if (!ps2::disc::read(game->sector, 1, fallback.data()) || fallback[0] != (has_fallback ? 'F' : 0)) return 10;
    const auto root_lba = le32(pvd.data() + 158);
    const auto root_size = le32(pvd.data() + 166);
    std::vector<std::uint8_t> root(((root_size + 2047) / 2048) * 2048);
    if (!ps2::disc::read(root_lba, static_cast<std::uint32_t>(root.size() / 2048), root.data())) return 11;
    bool found = false;
    for (std::size_t offset = 0; offset < root.size() && root[offset]; offset += root[offset])
        if (root[offset + 32] == 10 && std::memcmp(root.data() + offset + 33, "GAME.DAT;1", 10) == 0)
            found = le32(root.data() + offset + 2) == game->sector && be32(root.data() + offset + 6) == game->sector
                && le32(root.data() + offset + 10) == game->size && be32(root.data() + offset + 14) == game->size;
    if (!found) return 12;
    return 0;
}
'''


def make_fixture(root, with_fallback):
    root.mkdir(parents=True, exist_ok=True)
    (root / "GAME/dir").mkdir(parents=True)
    key = bytes.fromhex("7f 77 77 77 2e 61 72 69 6b 61 2e 63 6f 2e 6a 70")
    decoded = bytearray(512)
    decoded[:16] = key
    struct.pack_into("<I", decoded, 0x2c, 2)
    for index, (name, offset, size) in enumerate((("dir/asset.bin", 0, 100), ("dir/other.bin", 1, 100))):
        at = 0x30 + index * 0x30
        encoded_name = name.encode("ascii")
        decoded[at:at + len(encoded_name)] = encoded_name
        struct.pack_into("<III", decoded, at + 0x24, offset, 1, size)
    encoded = bytearray(decoded)
    for offset in range(16, len(decoded)):
        v = (~((decoded[offset] + decoded[offset & 15]) & 0xff)) & 0xff
        encoded[offset] = ((v << 4) | (v >> 4)) & 0xff
    (root / "INFO.DAT").write_bytes(encoded)
    (root / "GAME/dir/asset.bin").write_bytes(b"A" * 5000)
    (root / "GAME/dir/other.bin").write_bytes(b"B" * 100)
    if with_fallback:
        (root / "GAME.DAT").write_bytes(b"F" * (3 * 2048))


class LooseGameExpansionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        env_cxx = os.environ.get("CXX")
        if env_cxx:
            import shlex
            compiler = [part.strip('"') for part in shlex.split(env_cxx, posix=(os.name != "nt"))]
        else:
            import shutil
            compiler_path = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
            compiler = [compiler_path] if compiler_path else []
        if not compiler:
            raise unittest.SkipTest("no C++ compiler found; set CXX to clang++ or g++")
        cls.temp = tempfile.TemporaryDirectory(prefix="ps2-game-expand-")
        cls.work = pathlib.Path(cls.temp.name)
        cls.harness_source = cls.work / "harness.cpp"
        cls.harness = cls.work / ("harness.exe" if os.name == "nt" else "harness")
        cls.harness_source.write_text(HARNESS, encoding="utf-8")
        subprocess.run(compiler + ["-std=c++20", "-Wall", "-Wextra", "-I", str(ROOT / "runtime/include"),
            "-I", str(ROOT / "runtime/src"), str(cls.harness_source), str(ROOT / "runtime/src/disc.cpp"),
            str(ROOT / "runtime/src/disc_folder.cpp"), "-o", str(cls.harness)], check=True)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    def run_fixture(self, fixture, mode):
        # Legacy GAME.DAT extent expansion is used by the original IRX path.\n        env = os.environ.copy()\n        env["SDBZ_ARKD_NATIVE"] = "0"\n        subprocess.run([str(self.harness), str(fixture), mode], check=True, env=env)
        self.assertFalse((fixture / ".ps2-disc").exists())

    def test_expands_when_game_dat_is_absent(self):
        fixture = self.work / "without-fallback"
        shutil.rmtree(fixture, ignore_errors=True)
        make_fixture(fixture, False)
        self.run_fixture(fixture, "zero")

    def test_expands_and_preserves_game_dat_fallback(self):
        fixture = self.work / "with-fallback"
        shutil.rmtree(fixture, ignore_errors=True)
        make_fixture(fixture, True)
        self.run_fixture(fixture, "fallback")


if __name__ == "__main__":
    unittest.main()
