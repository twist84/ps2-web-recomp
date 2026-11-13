#!/usr/bin/env python3
"""Build the public-domain, full-red ZIP texture replacement demo.

No original game assets are read or included. The output is a directly
installable ZIP: disc/resourcepacks/Red Menu.zip.
"""
from pathlib import Path
import argparse
import json
import struct
import zlib
import zipfile


def red_png(side=1024):
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", side, side, 8, 6, 0, 0, 0)
    row = b"\x00" + (b"\xff\x00\x00\xff" * side)
    compressed = zlib.compress(row * side, level=9)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", header)
            + chunk(b"IDAT", compressed)
            + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("Red Menu.zip"))
    args = parser.parse_args()
    metadata = {
        "pack": {
            "format": 1,
            "name": "Red Menu",
            "description": "Original solid-red test image; no game assets",
            "game": "sdbz",
            "region": "eu-beta",
            "priority": 100,
        }
    }
    with zipfile.ZipFile(args.output, "w") as archive:
        archive.writestr("pack.json", json.dumps(metadata, separators=(",", ":")),
                         compress_type=zipfile.ZIP_STORED)
        archive.writestr(
            "assets/sdbz/textures/dis/main_menu.pix.001.png",
            red_png(), compress_type=zipfile.ZIP_DEFLATED)
        archive.writestr(
            "README.txt",
            "Place this ZIP in disc/resourcepacks/ beside SDBZ.exe.\n"
            "The 1024x1024 RGBA menu atlas 001 is 100% red and opaque.\n"
            "No copyrighted game pixels are present.\n",
            compress_type=zipfile.ZIP_DEFLATED)
    print(f"Created {args.output}")


if __name__ == "__main__":
    main()
