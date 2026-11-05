#!/usr/bin/env python3
"""End-to-end tests for loose GAME.DAT growth in preserved disc folders."""

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
#include <cstdio>
#include <cstring>
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

int main(int argc, char **argv)
{
    if (argc != 3 || !ps2::disc::open_folder(argv[1])) return 1;
    const bool has_fallback = std::strcmp(argv[2], "fallback") == 0;
    const auto info = ps2::disc::find("INFO.DAT");
    const auto game = ps2::disc::find("GAME.DAT");
    if (!info || !game || game->size != 20 * 2048 || ps2::disc::sector_count() != 43) return 2;

    std::vector<std::uint8_t> info_data(2048);
    if (!ps2::disc::read(info->sector, 1, info_data.data())) return 3;
    for (std::size_t i = 16; i < info_data.size(); ++i)
    {
        const auto value = info_data[i];
        const auto swapped = static_cast<std::uint8_t>((value << 4) | (value >> 4));
        info_data[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(~swapped) - info_data[i & 15]);
    }
    const auto *record = info_data.data() + 0x30;
    if (read_le32(record + 0x24) != 17 || read_le32(record + 0x28) != 3
        || read_le32(record + 0x2c) != 5000) return 4;

    std::vector<std::uint8_t> asset(3 * 2048);
    if (!ps2::disc::read(game->sector + 17, 3, asset.data())) return 5;
    for (std::size_t i = 0; i < 5000; ++i) if (asset[i] != 'A') return 6;

    std::vector<std::uint8_t> pvd(2048);
    if (!ps2::disc::read(16, 1, pvd.data()) || read_le32(pvd.data() + 80) != 43
        || read_be32(pvd.data() + 84) != 43) return 7;
    std::vector<std::uint8_t> root(2048);
    if (!ps2::disc::read(20, 1, root.data())) return 8;
    bool found_game_record = false;
    for (std::size_t offset = 0; offset < root.size() && root[offset]; offset += root[offset])
    {
        if (root[offset + 32] == 10
            && std::memcmp(root.data() + offset + 33, "GAME.DAT;1", 10) == 0)
        {
            found_game_record = read_le32(root.data() + offset + 10) == 20 * 2048
                && read_be32(root.data() + offset + 14) == 20 * 2048;
            break;
        }
    }
    if (!found_game_record) return 9;

    std::vector<std::uint8_t> original_last_sector(2048);
    if (!ps2::disc::read(39, 1, original_last_sector.data()) || original_last_sector[0] != 'Z') return 10;
    std::vector<std::uint8_t> fallback(2048);
    if (!ps2::disc::read(game->sector, 1, fallback.data())) return 11;
    if (fallback[0] != (has_fallback ? 'F' : 0)) return 12;
    return 0;
}
'''


def both32(value):
    return struct.pack("<I", value) + struct.pack(">I", value)


def both16(value):
    return struct.pack("<H", value) + struct.pack(">H", value)


def iso_record(lba, size, name, is_directory=False):
    name = name if isinstance(name, bytes) else name.encode("ascii")
    length = 33 + len(name) + (len(name) % 2 == 0)
    data = bytearray(length)
    data[0] = length
    data[2:10] = both32(lba)
    data[10:18] = both32(size)
    data[18:21] = bytes((126, 1, 1))
    data[25] = 2 if is_directory else 0
    data[28:32] = both16(1)
    data[32] = len(name)
    data[33:33 + len(name)] = name
    return data


def create_fixture(root, with_fallback):
    (root / ".ps2-disc").mkdir(parents=True)
    (root / "meta").mkdir()
    (root / "GAME/dir").mkdir(parents=True)

    key = bytes.fromhex("7f 77 77 77 2e 61 72 69 6b 61 2e 63 6f 2e 6a 70")
    decoded = bytearray(512)
    decoded[:16] = key
    struct.pack_into("<I", decoded, 0x2c, 2)
    for index, (name, offset, count, size) in enumerate((
        ("dir/asset.bin", 0, 1, 100), ("dir/other.bin", 1, 1, 100)
    )):
        record_offset = 0x30 + index * 0x30
        encoded_name = name.encode("ascii")
        decoded[record_offset:record_offset + len(encoded_name)] = encoded_name
        struct.pack_into("<III", decoded, record_offset + 0x24, offset, count, size)

    encoded = bytearray(decoded)
    for offset in range(16, len(decoded)):
        value = (~((decoded[offset] + decoded[offset & 15]) & 0xff)) & 0xff
        encoded[offset] = ((value << 4) | (value >> 4)) & 0xff
    (root / "INFO.DAT").write_bytes(encoded)
    (root / "GAME/dir/asset.bin").write_bytes(b"A" * 5000)
    (root / "GAME/dir/other.bin").write_bytes(b"B" * 100)
    if with_fallback:
        (root / "GAME.DAT").write_bytes(b"F" * (3 * 2048))

    directory = bytearray(2048)
    records = (
        iso_record(20, 2048, b"\0", True),
        iso_record(20, 2048, b"\1", True),
        iso_record(22, len(encoded), "INFO.DAT;1"),
        iso_record(23, 3 * 2048, "GAME.DAT;1"),
    )
    offset = 0
    for record in records:
        directory[offset:offset + len(record)] = record
        offset += len(record)
    (root / "meta/root.bin").write_bytes(directory)
    (root / "meta/last.bin").write_bytes(b"Z" * 2048)

    pvd = bytearray(2048)
    pvd[:7] = b"\x01CD001\x01"
    pvd[80:88] = both32(40)
    root_record = iso_record(20, 2048, b"\0", True)
    pvd[156:156 + len(root_record)] = root_record
    (root / "meta/pvd.bin").write_bytes(pvd)
    (root / ".ps2-disc/layout.tsv").write_text(
        "PS2_DISC_LAYOUT 1 40\n"
        "16\t2048\tmeta/pvd.bin\n"
        "20\t2048\tmeta/root.bin\n"
        "22\t512\tINFO.DAT\n"
        "23\t6144\tGAME.DAT\n"
        "39\t2048\tmeta/last.bin\n",
        encoding="ascii",
    )


class LooseGameExpansionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="ps2-game-expand-")
        cls.work = pathlib.Path(cls.temp.name)
        cls.harness_source = cls.work / "harness.cpp"
        cls.harness = cls.work / "harness"
        cls.harness_source.write_text(HARNESS, encoding="utf-8")
        subprocess.run([
            "c++", "-std=c++20", "-Wall", "-Wextra",
            "-I", str(ROOT / "runtime/include"), "-I", str(ROOT / "runtime/src"),
            str(cls.harness_source), str(ROOT / "runtime/src/disc.cpp"),
            str(ROOT / "runtime/src/disc_folder.cpp"), "-o", str(cls.harness),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_expands_when_game_dat_is_absent(self):
        fixture = self.work / "without-fallback"
        shutil.rmtree(fixture, ignore_errors=True)
        create_fixture(fixture, False)
        subprocess.run([str(self.harness), str(fixture), "zero"], check=True)

    def test_expands_and_preserves_game_dat_fallback(self):
        fixture = self.work / "with-fallback"
        shutil.rmtree(fixture, ignore_errors=True)
        create_fixture(fixture, True)
        subprocess.run([str(self.harness), str(fixture), "fallback"], check=True)


if __name__ == "__main__":
    unittest.main()
