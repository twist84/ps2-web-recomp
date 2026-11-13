#!/usr/bin/env python3
"""GAME/ and indexed INFO.DAT + GAME.DAT texture-pack export regressions."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/dump_sdbz_game_textures.py"


def make_tim2():
    width, height = 2, 1
    pixels = bytes((255, 0, 0, 128, 0, 255, 0, 64))
    header = struct.pack("<IIIHHBBBBHH", 48 + len(pixels), 0, len(pixels),
                         48, 0, 0, 1, 0, 3, width, height)
    return b"TIM2" + bytes((4, 0)) + struct.pack("<H", 1) + bytes(8) + header + bytes(48 - len(header)) + pixels


def encode_info(plain):
    key = plain[:16]
    output = bytearray(key)
    for i, value in enumerate(plain[16:]):
        inverted = (~((value + key[i % 16]) & 0xff)) & 0xff
        output.append(((inverted & 15) << 4) | (inverted >> 4))
    return bytes(output)


def create_archive(root, name="dis/example.tim2"):
    tim2 = make_tim2()
    game = root / "GAME.DAT"
    info = root / "INFO.DAT"
    game.write_bytes(tim2 + bytes(2048 - len(tim2)))
    raw = bytearray(96)
    raw[:16] = b"\x7fwww.arika.co.jp"
    struct.pack_into("<I", raw, 0x2c, 1)
    raw[48:48 + len(name.encode("cp932"))] = name.encode("cp932")
    struct.pack_into("<IIII", raw, 48 + 32, 0, 0, 1, len(tim2))
    info.write_bytes(encode_info(raw))
    return info, game


def read_rgba(png):
    data = png.read_bytes()
    assert data.startswith(b"\x89PNG\r\n\x1a\n")
    offset = 8
    compressed = bytearray()
    while offset < len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + size]
        if kind == b"IDAT":
            compressed.extend(body)
        offset += 12 + size
        if kind == b"IEND":
            break
    return zlib.decompress(compressed)


class TextureDumpTests(unittest.TestCase):
    def test_loose_game_tree(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            folder = root / "GAME/dis"
            folder.mkdir(parents=True)
            (folder / "example.tim2").write_bytes(make_tim2())
            output = root / "resourcepacks" / "Texture Dump"
            result = subprocess.run([sys.executable, str(SCRIPT), "--game-dir",
                                     str(root / "GAME"), "--output", str(output)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.check_pack(output, "GAME/")

    def test_game_dat_direct_without_extraction(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            info, game = create_archive(root)
            output = root / "resourcepacks" / "Texture Dump"
            result = subprocess.run([sys.executable, str(SCRIPT), "--info", str(info),
                                     "--game-dat", str(game), "--output", str(output)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.check_pack(output, "INFO.DAT+GAME.DAT")
            self.assertFalse((root / "GAME").exists())

    def test_reject_unsafe_archive_entry(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            info, game = create_archive(root, "../outside.tim2")
            output = root / "resourcepacks" / "Texture Dump"
            result = subprocess.run([sys.executable, str(SCRIPT), "--info", str(info),
                                     "--game-dat", str(game), "--output", str(output)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((root / "outside.tim2").exists())

    def check_pack(self, output, source):
        manifest = json.loads((output / "pack.json").read_text())
        self.assertEqual(manifest["pack"]["game"], "sdbz")
        report = json.loads((output / "dump_report.json").read_text())
        self.assertEqual(report["source"], source)
        self.assertEqual(report["textures_exported"], 1)
        self.assertEqual(report["assets_failed"], 0)
        png = output / "assets/sdbz/textures/dis/example.tim2.000.png"
        self.assertTrue(png.is_file())
        self.assertEqual(read_rgba(png),
                         b"\x00\xff\x00\x00\xff\x00\x00\xff\xff\x00\x40\x00\xff\x00\xff\x80")


if __name__ == "__main__":
    unittest.main()
