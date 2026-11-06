#!/usr/bin/env python3
"""Tests for rebuilding the runtime manifest without physical INFO.DAT."""

import pathlib
import shutil
import struct
import tempfile
import unittest

from scripts import discover_game_manifest, generate_game_manifest

SECTOR = 2048
KEY = bytes.fromhex("7f 77 77 77 2e 61 72 69 6b 61 2e 63 6f 2e 6a 70")


def encode_info(decoded):
    encoded = bytearray(decoded)
    for offset in range(16, len(decoded)):
        value = (~((decoded[offset] + decoded[offset & 15]) & 0xff)) & 0xff
        encoded[offset] = ((value << 4) | (value >> 4)) & 0xff
    return bytes(encoded)


def create_fixture(root):
    root.mkdir(parents=True)
    (root / ".ps2-disc").mkdir()
    (root / "GAME" / "dir").mkdir(parents=True)

    assets = [
        ("dir/asset.bin", 1, bytes((i * 13 + 7) & 0xff for i in range(3000))),
        ("dir/other.bin", 4, bytes((i * 17 + 3) & 0xff for i in range(800))),
    ]
    game = bytearray(12 * SECTOR)
    for name, sector, payload in assets:
        game[sector * SECTOR:sector * SECTOR + len(payload)] = payload
        (root / "GAME" / name).write_bytes(payload)
    (root / "GAME.DAT").write_bytes(game)

    decoded = bytearray(512)
    decoded[:16] = KEY
    struct.pack_into("<I", decoded, 0x2c, len(assets))
    for index, (name, sector, payload) in enumerate(assets):
        offset = 0x30 + index * 0x30
        name_bytes = name.replace("/", "\\").encode("cp932")
        decoded[offset:offset + len(name_bytes)] = name_bytes
        # Deliberately non-zero opaque bytes; recovery must preserve them.
        decoded[offset + 0x20:offset + 0x24] = bytes((0xa0 + index, 0xb0 + index, 0xc0 + index, 0xd0 + index))
        struct.pack_into("<III", decoded, offset + 0x24,
            sector, (len(payload) + SECTOR - 1) // SECTOR, len(payload))
    (root / "INFO.DAT").write_bytes(encode_info(decoded))

    (root / ".ps2-disc" / "layout.tsv").write_text(
        "PS2_DISC_LAYOUT\t1\t64\n"
        "22\t512\tINFO.DAT\n"
        "23\t%d\tGAME.DAT\n" % len(game),
        encoding="utf-8",
    )
    return bytes(decoded), assets


class GameManifestRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sdbz-manifest-recovery-")
        self.root = pathlib.Path(self.temp.name) / "disc"
        self.decoded, self.assets = create_fixture(self.root)
        generate_game_manifest.generate_manifest(self.root)

    def tearDown(self):
        self.temp.cleanup()

    def remove_primary_indexes(self):
        (self.root / "INFO.DAT").unlink()
        (self.root / ".ps2-disc" / "game_manifest.tsv").unlink()

    def test_recovers_runtime_manifest_without_info_dat(self):
        self.remove_primary_indexes()
        target, count = discover_game_manifest.discover(self.root)
        self.assertEqual(count, len(self.assets))
        self.assertTrue(target.is_file())
        lines = target.read_text(encoding="ascii").splitlines()
        self.assertEqual(lines[0], "PS2_GAME_MANIFEST\t1\t2048\t22\t512\t23\t%d" % (12 * SECTOR))
        rebuilt = bytes.fromhex(lines[1].split("\t", 1)[1])
        self.assertEqual(rebuilt, self.decoded)

    def test_preserves_opaque_record_fields(self):
        self.remove_primary_indexes()
        discover_game_manifest.discover(self.root)
        manifest = (self.root / ".ps2-disc" / "game_manifest.tsv").read_text(encoding="ascii")
        rebuilt = bytes.fromhex(manifest.splitlines()[1].split("\t", 1)[1])
        for index in range(len(self.assets)):
            offset = 0x30 + index * 0x30
            self.assertEqual(rebuilt[offset + 0x20:offset + 0x24],
                self.decoded[offset + 0x20:offset + 0x24])

    def test_rejects_modified_loose_asset(self):
        self.remove_primary_indexes()
        path = self.root / "GAME" / "dir" / "asset.bin"
        data = bytearray(path.read_bytes())
        data[100] ^= 0xff
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "no exact match"):
            discover_game_manifest.discover(self.root)
        self.assertFalse((self.root / ".ps2-disc" / "game_manifest.tsv").exists())

    def test_rejects_ambiguous_exact_match(self):
        self.remove_primary_indexes()
        path = self.root / "GAME" / "dir" / "other.bin"
        data = path.read_bytes()
        with (self.root / "GAME.DAT").open("r+b") as game:
            game.seek(8 * SECTOR)
            game.write(data)
        with self.assertRaisesRegex(ValueError, "multiple exact matches"):
            discover_game_manifest.discover(self.root)
        self.assertFalse((self.root / ".ps2-disc" / "game_manifest.tsv").exists())

    def test_discovery_allows_new_unindexed_assets(self):
        self.remove_primary_indexes()
        (self.root / "GAME" / "new_asset.bin").write_bytes(b"new asset not present in original GAME.DAT")
        target, count = discover_game_manifest.discover(self.root)
        self.assertEqual(count, len(self.assets))
        report = __import__("json").loads((self.root / ".ps2-disc" / "game_discovery.json").read_text(encoding="utf-8"))
        self.assertEqual(report["unindexed_loose_assets"], ["new_asset.bin"])
        lines = target.read_text(encoding="ascii").splitlines()
        rebuilt = bytes.fromhex(lines[1].split("\t", 1)[1])
        self.assertEqual(struct.unpack_from("<I", rebuilt, 0x2c)[0], len(self.assets))

    def test_rejects_incomplete_loose_game_directory(self):
        self.remove_primary_indexes()
        (self.root / "GAME" / "dir" / "other.bin").unlink()
        with self.assertRaisesRegex(ValueError, "missing indexed assets"):
            discover_game_manifest.discover(self.root)
        self.assertFalse((self.root / ".ps2-disc" / "game_manifest.tsv").exists())


if __name__ == "__main__":
    unittest.main()
