#!/usr/bin/env python3
# Runtime tests for the sidecar-free ISO layout and loose GAME/ overlays.

import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SECTOR = 2048
KEY = bytes.fromhex("7f 77 77 77 2e 61 72 69 6b 61 2e 63 6f 2e 6a 70")

HARNESS = r'''#include "disc_folder.hpp"
#include "ps2/disc.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static std::uint32_t read_le32(const std::uint8_t *p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8)
        | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
static std::uint32_t read_be32(const std::uint8_t *p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16)
        | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
static void decode_info(std::vector<std::uint8_t> &data, std::size_t size)
{
    for (std::size_t i = 16; i < size; ++i)
    {
        const auto value = data[i];
        const auto swapped = static_cast<std::uint8_t>((value << 4) | (value >> 4));
        data[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(~swapped) - data[i & 15]);
    }
}
static bool has_file_record(const std::vector<std::uint8_t> &root, const char *name,
    std::uint32_t expected_lba, std::uint32_t expected_size)
{
    const std::size_t name_size = std::strlen(name);
    for (std::size_t offset = 0; offset < root.size() && root[offset]; offset += root[offset])
    {
        if (root[offset + 32] == name_size
            && std::memcmp(root.data() + offset + 33, name, name_size) == 0)
            return read_le32(root.data() + offset + 2) == expected_lba
                && read_be32(root.data() + offset + 6) == expected_lba
                && read_le32(root.data() + offset + 10) == expected_size
                && read_be32(root.data() + offset + 14) == expected_size;
    }
    return false;
}
int main(int argc, char **argv)
{
    if (argc != 3 || !ps2::disc::open_folder(argv[1])) return 1;
    const std::string mode = argv[2];
    const auto info = ps2::disc::find("INFO.DAT");
    const auto game = ps2::disc::find("GAME.DAT");
    if (!info || !game) return 2;

    std::uint32_t expected_info_size = mode == "inplace_size" ? 192 : (mode == "relocate" ? 2064 : 512);
    const std::uint32_t new_asset_offset = mode == "relocate" ? 41 : 2;
    const std::uint32_t expected_records = mode == "relocate" ? 42 : 3;
    if (info->size != expected_info_size) return 3;

    std::vector<std::uint8_t> info_data(((info->size + 2047) / 2048) * 2048);
    if (!ps2::disc::read(info->sector, static_cast<std::uint32_t>(info_data.size() / 2048), info_data.data())) return 4;
    decode_info(info_data, info->size);
    if (read_le32(info_data.data() + 0x2c) != expected_records) return 5;
    const std::uint8_t cp932_name[6] = {0x83, 0x5c, '.', 'b', 'i', 'n'};
    const bool cp932 = mode == "cp932";
    const char *wanted = mode == "first" ? "a_first.bin" : "new.bin";
    std::size_t added_record = 0;
    for (std::uint32_t i = 0; i < expected_records; ++i)
    {
        const std::size_t candidate = 0x30 + i * 0x30;
        if ((cp932 && std::memcmp(info_data.data() + candidate, cp932_name, sizeof(cp932_name)) == 0)
            || (!cp932
                && std::memcmp(info_data.data() + candidate, wanted, std::strlen(wanted)) == 0
                && info_data[candidate + std::strlen(wanted)] == 0))
        {
            added_record = candidate;
            break;
        }
    }
    if (!added_record
        || read_le32(info_data.data() + added_record + 0x24) != new_asset_offset
        || read_le32(info_data.data() + added_record + 0x28) != 1
        || read_le32(info_data.data() + added_record + 0x2c) != 100
        || std::memcmp(info_data.data() + added_record + 0x20, info_data.data() + 0x30 + 0x20, 4) != 0) return 6;

    if (mode == "first" && added_record != 0x30) return 13;
    std::vector<std::uint8_t> asset(2048);
    if (!ps2::disc::read(game->sector + new_asset_offset, 1, asset.data())) return 7;
    for (std::size_t i = 0; i < 100; ++i) if (asset[i] != 'N') return 8;

    std::vector<std::uint8_t> pvd(2048);
    if (!ps2::disc::read(16, 1, pvd.data())) return 9;
    const std::uint32_t volume = read_le32(pvd.data() + 80);
    if (read_be32(pvd.data() + 84) != volume || volume != ps2::disc::sector_count()) return 10;
    const std::uint32_t root_lba = read_le32(pvd.data() + 158);
    const std::uint32_t root_size = read_le32(pvd.data() + 166);
    std::vector<std::uint8_t> root(((root_size + 2047) / 2048) * 2048);
    if (!ps2::disc::read(root_lba, static_cast<std::uint32_t>(root.size() / 2048), root.data())
        || !has_file_record(root, "INFO.DAT;1", info->sector, info->size)) return 11;
    if (mode == "relocate" && info->sector < game->sector + (game->size + 2047) / 2048) return 12;
    return 0;
}
'''


def both32(value):
    import struct
    return struct.pack("<I", value) + struct.pack(">I", value)


def both16(value):
    import struct
    return struct.pack("<H", value) + struct.pack(">H", value)


def write_info(root, assets, size):
    import struct
    decoded = bytearray(size)
    decoded[:16] = KEY
    struct.pack_into("<I", decoded, 0x2c, len(assets))
    for index, (name, sector, payload) in enumerate(assets):
        record = 0x30 + index * 0x30
        encoded_name = name.replace("/", "\\").encode("ascii")
        decoded[record:record + len(encoded_name)] = encoded_name
        decoded[record + 0x20:record + 0x24] = b"\x11\x22\x33\x44"
        struct.pack_into("<III", decoded, record + 0x24,
            sector, (len(payload) + SECTOR - 1) // SECTOR, len(payload))
    encoded = bytearray(decoded)
    for offset in range(16, len(decoded)):
        value = (~((decoded[offset] + decoded[offset & 15]) & 0xff)) & 0xff
        encoded[offset] = ((value << 4) | (value >> 4)) & 0xff
    (root / "INFO.DAT").write_bytes(encoded)
    return bytes(decoded)


def fixture_in_place(root, with_fallback, info_size=512, new_name="new.bin"):
    root.mkdir(parents=True, exist_ok=True)
    (root / "GAME/dir").mkdir(parents=True)
    assets = [("dir/asset.bin", 0, b"A" * 100), ("dir/other.bin", 1, b"B" * 100)]
    write_info(root, assets, info_size)
    for name, _, payload in assets:
        (root / "GAME" / name).write_bytes(payload)
    (root / "GAME" / new_name).write_bytes(b"N" * 100)
    if with_fallback:
        (root / "GAME.DAT").write_bytes(b"F" * (3 * SECTOR))


def fixture_relocation(root):
    root.mkdir(parents=True, exist_ok=True)
    (root / "GAME").mkdir()
    assets = []
    game = bytearray(50 * SECTOR)
    for index in range(41):
        name = "asset%02d.bin" % index
        payload = bytes([index + 1]) * 100
        assets.append((name, index, payload))
        game[index * SECTOR:index * SECTOR + len(payload)] = payload
        (root / "GAME" / name).write_bytes(payload)
    (root / "GAME" / "new.bin").write_bytes(b"N" * 100)
    write_info(root, assets, SECTOR)
    (root / "GAME.DAT").write_bytes(game)


class NewGameAssetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        env_cxx = os.environ.get("CXX")
        if env_cxx:
            compiler = [part.strip('"') for part in shlex.split(env_cxx, posix=(os.name != "nt"))]
        else:
            compiler_path = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
            compiler = [compiler_path] if compiler_path else []
        if not compiler:
            raise unittest.SkipTest("no C++ compiler found; set CXX to clang++ or g++")
        cls.temp = tempfile.TemporaryDirectory(prefix="sdbz-no-sidecar-")
        cls.work = pathlib.Path(cls.temp.name)
        cls.harness_source = cls.work / "harness.cpp"
        cls.harness = cls.work / ("harness.exe" if os.name == "nt" else "harness")
        cls.harness_source.write_text(HARNESS, encoding="utf-8")
        subprocess.run(compiler + [
            "-std=c++20", "-Wall", "-Wextra",
            "-I", str(ROOT / "runtime/include"), "-I", str(ROOT / "runtime/src"),
            str(cls.harness_source), str(ROOT / "runtime/src/disc.cpp"),
            str(ROOT / "runtime/src/disc_folder.cpp"),
            str(ROOT / "runtime/src/resource_pack_zip.cpp"), "-lz", "-o", str(cls.harness),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    def run_fixture(self, fixture, mode):
        # This suite validates the original IRX-compatible ISO overlay. Native\n        # ARKD reads GAME/ directly and intentionally skips this rebuild path.\n        env = os.environ.copy()\n        env["SDBZ_ARKD_NATIVE"] = "0"\n        subprocess.run([str(self.harness), str(fixture), mode], check=True, env=env)
        self.assertFalse((fixture / ".ps2-disc").exists())

    def test_builds_runtime_iso_without_sidecar(self):
        fixture = self.work / "no-sidecar"
        shutil.rmtree(fixture, ignore_errors=True)
        fixture_in_place(fixture, True)
        self.run_fixture(fixture, "inplace")

    def test_new_asset_sorts_before_existing_first_letter_group(self):
        fixture = self.work / "new-first"
        shutil.rmtree(fixture, ignore_errors=True)
        fixture_in_place(fixture, True, new_name="a_first.bin")
        self.run_fixture(fixture, "first")

    def test_adds_new_asset_without_game_dat_fallback(self):
        fixture = self.work / "new-without-fallback"
        shutil.rmtree(fixture, ignore_errors=True)
        fixture_in_place(fixture, False)
        self.run_fixture(fixture, "inplace")

    def test_cp932_trail_backslash_is_not_treated_as_a_path_separator(self):
        name = os.fsdecode(bytes((0x83, 0x5c, 0x2e, 0x62, 0x69, 0x6e)))
        fixture = self.work / "cp932-trail-byte"
        shutil.rmtree(fixture, ignore_errors=True)
        fixture_in_place(fixture, True, new_name=name)
        self.run_fixture(fixture, "cp932")

    def test_updates_info_dat_size_without_relocating_when_sector_capacity_is_sufficient(self):
        fixture = self.work / "info-size-in-place"
        shutil.rmtree(fixture, ignore_errors=True)
        fixture_in_place(fixture, True, info_size=144)
        self.run_fixture(fixture, "inplace_size")

    def test_relocates_info_dat_when_record_table_exceeds_original_extent(self):
        fixture = self.work / "info-relocation"
        shutil.rmtree(fixture, ignore_errors=True)
        fixture_relocation(fixture)
        self.run_fixture(fixture, "relocate")


if __name__ == "__main__":
    unittest.main()
