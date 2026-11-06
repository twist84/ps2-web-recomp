#!/usr/bin/env python3
"""Regression tests for the manifest generator and exact discovery utility."""

import pathlib
import struct
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import discover_game_manifest
import generate_game_manifest

SECTOR = 2048
KEY = bytes.fromhex("7f 77 77 77 2e 61 72 69 6b 61 2e 63 6f 2e 6a 70")


def encode_info(decoded):
    encoded = bytearray(decoded)
    for offset in range(16, len(decoded)):
        value = (~((decoded[offset] + decoded[offset & 15]) & 0xFF)) & 0xFF
        encoded[offset] = ((value << 4) | (value >> 4)) & 0xFF
    return bytes(encoded)


def make_fixture(root, duplicate=False):
    (root / ".ps2-disc").mkdir(parents=True)
    (root / "GAME/sub").mkdir(parents=True)

    payload = b"SDBZ-ASSET-SIGNATURE" + bytes(range(80))
    asset_path = root / "GAME/sub/test.bin"
    asset_path.write_bytes(payload)

    game = bytearray(8 * SECTOR)
    game[2 * SECTOR:2 * SECTOR + len(payload)] = payload
    if duplicate:
        game[5 * SECTOR:5 * SECTOR + len(payload)] = payload
    (root / "GAME.DAT").write_bytes(game)

    decoded = bytearray(256)
    decoded[:16] = KEY
    struct.pack_into("<I", decoded, 0x2C, 1)
    name = b"sub\\test.bin"
    decoded[0x30:0x30 + len(name)] = name
    struct.pack_into("<III", decoded, 0x54, 2, 1, len(payload))
    encoded = encode_info(decoded)
    (root / "INFO.DAT").write_bytes(encoded)

    (root / ".ps2-disc/layout.tsv").write_text(
        "PS2_DISC_LAYOUT\t1\t64\n"
        "20\t256\tINFO.DAT\n"
        "24\t%d\tGAME.DAT\n" % len(game),
        encoding="ascii",
    )
    return bytes(decoded), payload


class GameManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="game-manifest-")
        self.root = pathlib.Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_generate_manifest_preserves_decoded_info_image(self):
        expected, _ = make_fixture(self.root)
        target = generate_game_manifest.generate_manifest(self.root)
        lines = target.read_text(encoding="ascii").splitlines()
        self.assertEqual(lines[0], "PS2_GAME_MANIFEST\t1\t2048\t20\t256\t24\t16384")
        self.assertTrue(lines[1].startswith("INFO_HEX\t"))
        self.assertEqual(bytes.fromhex(lines[1].split("\t", 1)[1]), expected)

    def test_discovery_finds_unique_exact_sector_extent(self):
        _, payload = make_fixture(self.root)
        report = discover_game_manifest.discover(self.root)
        self.assertEqual(len(report["assets"]), 1)
        asset = report["assets"][0]
        self.assertEqual(asset["sector_offset"], 2)
        self.assertEqual(asset["byte_size"], len(payload))
        self.assertTrue(report["complete_index_not_proven"])

    def test_discovery_rejects_ambiguous_exact_match(self):
        make_fixture(self.root, duplicate=True)
        with self.assertRaisesRegex(ValueError, "multiple exact matches"):
            discover_game_manifest.discover(self.root)

    def test_discovery_rejects_modified_asset(self):
        _, _ = make_fixture(self.root)
        path = self.root / "GAME/sub/test.bin"
        path.write_bytes(path.read_bytes() + b"edited")
        with self.assertRaisesRegex(ValueError, "no exact match"):
            discover_game_manifest.discover(self.root)


if __name__ == "__main__":
    unittest.main()
