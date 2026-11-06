#!/usr/bin/env python3
"""Generate the runtime GAME.DAT manifest from an extracted disc's INFO.DAT.

The manifest stores the complete decoded INFO.DAT image, not merely a list of
asset rows. This preserves header bytes that the runtime/game may also read.
"""

import argparse
from pathlib import Path
import struct

SECTOR_SIZE = 2048
SIGNATURE = b"www.arika.co.jp"


def read_layout(root):
    layout_path = root / ".ps2-disc" / "layout.tsv"
    lines = layout_path.read_text(encoding="utf-8").splitlines()
    if not lines:
        raise ValueError("empty .ps2-disc/layout.tsv")
    header = lines[0].split("\t")
    if len(header) != 3 or header[:2] != ["PS2_DISC_LAYOUT", "1"]:
        raise ValueError("unsupported .ps2-disc/layout.tsv header")

    volume_sectors = int(header[2])
    entries = {}
    for line in lines[1:]:
        if not line:
            continue
        fields = line.split("\t", 2)
        if len(fields) != 3:
            raise ValueError("invalid .ps2-disc/layout.tsv entry")
        lba, size = int(fields[0]), int(fields[1])
        key = fields[2].replace("\\", "/").upper()
        if key in entries:
            raise ValueError("duplicate path in .ps2-disc/layout.tsv: " + fields[2])
        entries[key] = (lba, size, fields[2])
    return volume_sectors, entries


def decode_info(encoded):
    if len(encoded) < 0x30:
        raise ValueError("INFO.DAT is smaller than its 0x30-byte header")
    if encoded[1:1 + len(SIGNATURE)] != SIGNATURE:
        raise ValueError("INFO.DAT signature is not www.arika.co.jp")

    decoded = bytearray(encoded)
    for offset in range(0x10, len(decoded)):
        value = encoded[offset]
        swapped = ((value << 4) | (value >> 4)) & 0xFF
        decoded[offset] = ((~swapped) - encoded[offset & 15]) & 0xFF

    count = struct.unpack_from("<I", decoded, 0x2C)[0]
    table_end = 0x30 + count * 0x30
    if not count or table_end > len(decoded):
        raise ValueError("INFO.DAT contains an invalid or truncated record table")

    names = set()
    extents = []
    for index in range(count):
        offset = 0x30 + index * 0x30
        record = decoded[offset:offset + 0x30]
        raw_name = bytes(record[:0x20]).split(b"\0", 1)[0]
        if not raw_name:
            raise ValueError("INFO.DAT contains an empty asset name")
        try:
            name = raw_name.decode("cp932")
        except UnicodeDecodeError as error:
            raise ValueError("INFO.DAT contains an invalid CP932 asset name") from error
        normalized = name.replace("\\", "/").upper()
        parts = normalized.split("/")
        if normalized.startswith("/") or ":" in normalized or any(part in ("", ".", "..") for part in parts):
            raise ValueError("INFO.DAT contains an unsafe asset path: " + name)
        if normalized in names:
            raise ValueError("INFO.DAT contains a duplicate asset path: " + name)
        names.add(normalized)

        sector_offset, sector_count, byte_size = struct.unpack_from("<III", record, 0x24)
        if not sector_count or not byte_size or byte_size > sector_count * SECTOR_SIZE:
            raise ValueError("INFO.DAT contains an invalid extent for " + name)
        extents.append((sector_offset, sector_offset + sector_count, name))

    extents.sort()
    previous_end = 0
    for start, end, name in extents:
        if start < previous_end:
            raise ValueError("INFO.DAT contains overlapping extents near " + name)
        previous_end = end
    return bytes(decoded), count


def generate_manifest(disc_folder):
    root = Path(disc_folder).resolve()
    volume_sectors, entries = read_layout(root)
    info = entries.get("INFO.DAT")
    game = entries.get("GAME.DAT")
    if info is None or game is None:
        raise ValueError("layout.tsv must contain INFO.DAT and GAME.DAT")

    info_lba, info_size, info_relative = info
    game_lba, game_size, _ = game
    info_path = root.joinpath(*Path(info_relative).parts)
    if not info_path.is_file() or info_path.stat().st_size != info_size:
        raise ValueError("INFO.DAT is missing or does not match layout.tsv")
    decoded, count = decode_info(info_path.read_bytes())
    if len(decoded) != info_size:
        raise ValueError("decoded INFO.DAT size differs from its layout extent")

    header = (
        "PS2_GAME_MANIFEST\t1\t%d\t%d\t%d\t%d\t%d\n"
        % (SECTOR_SIZE, info_lba, info_size, game_lba, game_size)
    )
    payload = header + "INFO_HEX\t" + decoded.hex() + "\n"
    target = root / ".ps2-disc" / "game_manifest.tsv"
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(".tmp")
    temporary.write_text(payload, encoding="ascii", newline="\n")
    temporary.replace(target)
    print("generated %d-entry GAME.DAT manifest: %s (volume %d sectors)" % (count, target, volume_sectors))
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disc_folder", type=Path)
    args = parser.parse_args()
    try:
        generate_manifest(args.disc_folder)
    except (OSError, ValueError, UnicodeError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
