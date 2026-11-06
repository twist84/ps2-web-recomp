#!/usr/bin/env python3
"""Runtime tests for adding unindexed files under GAME/."""

import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import generate_game_manifest

SECTOR = 2048
KEY = bytes.fromhex("7f 77 77 77 2e 61 72 69 6b 61 2e 63 6f 2e 6a 70")

HARNESS = r'''#include "disc_folder.hpp"
#include "ps2/disc.hpp"
#include <cstdint>
#include <cstdio>
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

static bool has_info_record(const std::vector<std::uint8_t> &root,
    std::uint32_t expected_lba, std::uint32_t expected_size)
{
    for (std::size_t offset = 0; offset < root.size() && root[offset]; offset += root[offset])
    {
        if (root[offset + 32] == 10
            && std::memcmp(root.data() + offset + 33, "INFO.DAT;1", 10) == 0)
        {
            return read_le32(root.data() + offset + 2) == expected_lba
                && read_be32(root.data() + offset + 6) == expected_lba
                && read_le32(root.data() + offset + 10) == expected_size
                && read_be32(root.data() + offset + 14) == expected_size;
        }
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

    std::uint32_t expected_info_sector = 22;
    std::uint32_t expected_info_size = 512;
    std::uint32_t expected_volume = 43;
    std::uint32_t new_asset_offset = 2;
    std::size_t info_read_size = 512;
    std::uint32_t expected_records = 3;
    if (mode == "inplace_size")
    {
        expected_info_size = 192;
        info_read_size = 192;
    }
    if (mode == "relocate")
    {
        expected_info_sector = 110;
        expected_info_size = 2064;
        expected_volume = 112;
        new_asset_offset = 41;
        info_read_size = 2064;
        expected_records = 42;
    }
    if (info->sector != expected_info_sector || info->size != expected_info_size
        || ps2::disc::sector_count() != expected_volume) return 3;

    std::vector<std::uint8_t> info_data(((info_read_size + 2047) / 2048) * 2048);
    if (!ps2::disc::read(info->sector, static_cast<std::uint32_t>(info_data.size() / 2048), info_data.data())) return 4;
    decode_info(info_data, info_read_size);
    if (read_le32(info_data.data() + 0x2c) != expected_records) return 5;
    const std::size_t added_record = 0x30 + (expected_records - 1) * 0x30;
    const std::uint8_t cp932_name[6] = {0x83, 0x5c, '.', 'b', 'i', 'n'};
    const bool name_matches = mode == "cp932"
        ? std::memcmp(info_data.data() + added_record, cp932_name, sizeof(cp932_name)) == 0
        : std::memcmp(info_data.data() + added_record, "new.bin", 7) == 0;
    if (!name_matches
        || read_le32(info_data.data() + added_record + 0x24) != new_asset_offset
        || read_le32(info_data.data() + added_record + 0x28) != 1
        || read_le32(info_data.data() + added_record + 0x2c) != 100
        || std::memcmp(info_data.data() + added_record + 0x20, info_data.data() + 0x30 + 0x20, 4) != 0) return 6;

    std::vector<std::uint8_t> asset(2048);
    if (!ps2::disc::read(game->sector + new_asset_offset, 1, asset.data())) return 7;
    for (std::size_t i = 0; i < 100; ++i) if (asset[i] != 'N') return 8;

    std::vector<std::uint8_t> pvd(2048);
    if (!ps2::disc::read(16, 1, pvd.data())
        || read_le32(pvd.data() + 80) != expected_volume
        || read_be32(pvd.data() + 84) != expected_volume) return 9;
    if (mode == "relocate" || mode == "inplace_size")
    {
        std::vector<std::uint8_t> root(2048);
        if (!ps2::disc::read(20, 1, root.data())
            || !has_info_record(root, expected_info_sector, expected_info_size)) return 10;
    }
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


def encode_info(decoded):
    encoded = bytearray(decoded)
    for offset in range(16, len(decoded)):
        value = (~((decoded[offset] + decoded[offset & 15]) & 0xff)) & 0xff
        encoded[offset] = ((value << 4) | (value >> 4)) & 0xff
    return encoded


def write_layout(root, volume_sectors, info_size, game_size, info_lba=22, game_lba=23, last_lba=None):
    (root / "meta").mkdir(exist_ok=True)
    if last_lba is not None:
        (root / "meta/last.bin").write_bytes(b"Z" * SECTOR)
    directory = bytearray(SECTOR)
    records = (
        iso_record(20, SECTOR, b"\0", True),
        iso_record(20, SECTOR, b"\1", True),
        iso_record(info_lba, info_size, "INFO.DAT;1"),
        iso_record(game_lba, game_size, "GAME.DAT;1"),
    )
    offset = 0
    for record in records:
        directory[offset:offset + len(record)] = record
        offset += len(record)
    (root / "meta/root.bin").write_bytes(directory)

    pvd = bytearray(SECTOR)
    pvd[:7] = b"\x01CD001\x01"
    pvd[80:88] = both32(volume_sectors)
    root_record = iso_record(20, SECTOR, b"\0", True)
    pvd[156:156 + len(root_record)] = root_record
    (root / "meta/pvd.bin").write_bytes(pvd)

    rows = [
        "PS2_DISC_LAYOUT\t1\t%d" % volume_sectors,
        "16\t2048\tmeta/pvd.bin",
        "20\t2048\tmeta/root.bin",
        "%d\t%d\tINFO.DAT" % (info_lba, info_size),
        "%d\t%d\tGAME.DAT" % (game_lba, game_size),
    ]
    if last_lba is not None:
        rows.append("%d\t2048\tmeta/last.bin" % last_lba)
    (root / ".ps2-disc/layout.tsv").write_text("\n".join(rows) + "\n", encoding="ascii")


def write_info(root, assets, size):
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
    (root / "INFO.DAT").write_bytes(encode_info(decoded))
    return decoded


def fixture_in_place(root, with_fallback, info_size=512, new_name="new.bin"):
    (root / ".ps2-disc").mkdir(parents=True)
    (root / "GAME/dir").mkdir(parents=True)
    assets = [
        ("dir/asset.bin", 0, b"A" * 100),
        ("dir/other.bin", 1, b"B" * 100),
    ]
    decoded = write_info(root, assets, info_size)
    for name, _, payload in assets:
        (root / "GAME" / name).write_bytes(payload)
    (root / "GAME" / new_name).write_bytes(b"N" * 100)
    if with_fallback:
        (root / "GAME.DAT").write_bytes(b"F" * (3 * SECTOR))
    write_layout(root, 43, len(decoded), 3 * SECTOR, last_lba=39)


def fixture_relocation(root):
    (root / ".ps2-disc").mkdir(parents=True)
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
    decoded = write_info(root, assets, SECTOR)
    (root / "GAME.DAT").write_bytes(game)
    write_layout(root, 110, len(decoded), len(game), last_lba=109)


class NewGameAssetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="sdbz-new-assets-")
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

    def test_adds_new_asset_with_game_dat_fallback(self):
        fixture = self.work / "new-with-fallback"
        shutil.rmtree(fixture, ignore_errors=True)
        fixture_in_place(fixture, True)
        subprocess.run([str(self.harness), str(fixture), "inplace"], check=True)

    def test_adds_new_asset_when_info_dat_is_virtual_from_manifest(self):
        fixture = self.work / "manifest-virtual-info"
        shutil.rmtree(fixture, ignore_errors=True)
        fixture_in_place(fixture, True)
        generate_game_manifest.generate_manifest(fixture)
        (fixture / "INFO.DAT").unlink()
        subprocess.run([str(self.harness), str(fixture), "inplace"], check=True)

    def test_adds_new_asset_without_game_dat_fallback(self):
        fixture = self.work / "new-without-fallback"
        shutil.rmtree(fixture, ignore_errors=True)
        fixture_in_place(fixture, False)
        subprocess.run([str(self.harness), str(fixture), "inplace"], check=True)

    def test_cp932_trail_backslash_is_not_treated_as_a_path_separator(self):
        import os
        fixture = self.work / "cp932-trail-byte"
        shutil.rmtree(fixture, ignore_errors=True)
        name = os.fsdecode(bytes((0x83, 0x5c, 0x2e, 0x62, 0x69, 0x6e)))
        fixture_in_place(fixture, True, new_name=name)
        subprocess.run([str(self.harness), str(fixture), "cp932"], check=True)

    def test_updates_info_dat_size_without_relocating_when_sector_capacity_is_sufficient(self):
        fixture = self.work / "info-size-in-place"
        shutil.rmtree(fixture, ignore_errors=True)
        fixture_in_place(fixture, True, info_size=144)
        subprocess.run([str(self.harness), str(fixture), "inplace_size"], check=True)

    def test_relocates_info_dat_when_record_table_exceeds_original_extent(self):
        fixture = self.work / "info-relocation"
        shutil.rmtree(fixture, ignore_errors=True)
        fixture_relocation(fixture)
        subprocess.run([str(self.harness), str(fixture), "relocate"], check=True)


if __name__ == "__main__":
    unittest.main()
