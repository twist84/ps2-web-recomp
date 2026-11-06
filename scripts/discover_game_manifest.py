#!/usr/bin/env python3
"""Rebuild a runtime GAME.DAT manifest using independent sector discovery.

Requirements:
- original GAME.DAT with the exact size recorded in .ps2-disc/layout.tsv;
- a complete unchanged loose GAME/ extraction;
- game_manifest_template.bin, created by generate_game_manifest.py.

Every originally indexed asset is matched byte-for-byte at a sector boundary.
All template entries must be recovered exactly once. Additional files under
GAME/ are accepted and listed as unindexed; the runtime allocates and adds
those files when it mounts the disc.
"""

import argparse
import json
from pathlib import Path
import struct

SECTOR_SIZE = 2048


def read_layout(root):
    lines = (root / ".ps2-disc" / "layout.tsv").read_text(encoding="utf-8").splitlines()
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
        lba, size, path = line.split("\t", 2)
        key = path.replace("\\", "/").upper()
        if key in entries:
            raise ValueError("duplicate path in layout.tsv: " + path)
        entries[key] = (int(lba), int(size), path)
    return volume_sectors, entries


def build_prefix_index(game_path, game_size, signature_size):
    index = {}
    chunk_size = (1 << 20) // SECTOR_SIZE * SECTOR_SIZE
    with game_path.open("rb") as game:
        base = 0
        while base + signature_size <= game_size:
            game.seek(base)
            chunk = game.read(min(chunk_size, game_size - base))
            if not chunk:
                break
            for offset in range(0, len(chunk) - signature_size + 1, SECTOR_SIZE):
                prefix = chunk[offset:offset + signature_size]
                index.setdefault(prefix, []).append((base + offset) // SECTOR_SIZE)
            base += len(chunk)
    return index


def verify_candidate(game_path, asset_path, sector, asset_size):
    with game_path.open("rb") as game, asset_path.open("rb") as asset:
        game.seek(sector * SECTOR_SIZE)
        remaining = asset_size
        while remaining:
            amount = min(1 << 20, remaining)
            left = game.read(amount)
            right = asset.read(amount)
            if len(left) != amount or left != right:
                return False
            remaining -= amount
    return True


def normalized_name(name):
    return name.replace("\\", "/").casefold()


def write_runtime_manifest(root, decoded, info_lba, info_size, game_lba, game_size):
    header = (
        "PS2_GAME_MANIFEST\t1\t%d\t%d\t%d\t%d\t%d\n"
        % (SECTOR_SIZE, info_lba, info_size, game_lba, game_size)
    )
    payload = header + "INFO_HEX\t" + decoded.hex() + "\n"
    target = root / ".ps2-disc" / "game_manifest.tsv"
    temporary = target.with_suffix(".tmp")
    temporary.write_text(payload, encoding="ascii", newline="\n")
    temporary.replace(target)
    return target


def discover(disc_folder, signature_size=16):
    root = Path(disc_folder).resolve()
    volume_sectors, layout = read_layout(root)
    info_entry = layout.get("INFO.DAT")
    game_entry = layout.get("GAME.DAT")
    if info_entry is None or game_entry is None:
        raise ValueError("layout.tsv must contain INFO.DAT and GAME.DAT")
    info_lba, info_size, _ = info_entry
    game_lba, game_size, game_relative = game_entry

    game_path = root.joinpath(*Path(game_relative.replace("\\", "/")).parts)
    if not game_path.is_file() or game_path.stat().st_size != game_size:
        raise ValueError("independent discovery requires the original GAME.DAT with its recorded size")
    loose_root = root / "GAME"
    if not loose_root.is_dir():
        raise ValueError("loose GAME/ directory is missing")

    template_path = root / ".ps2-disc" / "game_manifest_template.bin"
    if not template_path.is_file() or template_path.stat().st_size != info_size:
        raise ValueError("recovery template missing or size mismatch; run generate_game_manifest.py before deleting INFO.DAT")
    template = bytearray(template_path.read_bytes())
    if len(template) < 0x30 or template[1:16] != b"www.arika.co.jp":
        raise ValueError("invalid decoded INFO.DAT recovery template")
    count = struct.unpack_from("<I", template, 0x2C)[0]
    table_end = 0x30 + count * 0x30
    if not count or table_end > len(template):
        raise ValueError("recovery template has an invalid record count")

    records = []
    expected = {}
    for index in range(count):
        offset = 0x30 + index * 0x30
        record = template[offset:offset + 0x30]
        raw_name = bytes(record[:0x20]).split(b"\0", 1)[0]
        if not raw_name:
            raise ValueError("recovery template has an empty entry name")
        try:
            name = raw_name.decode("cp932")
        except UnicodeDecodeError as error:
            raise ValueError("recovery template contains an invalid CP932 name") from error
        relative = name.replace("\\", "/")
        parts = relative.split("/")
        if relative.startswith("/") or ":" in relative or any(part in ("", ".", "..") for part in parts):
            raise ValueError("unsafe path in recovery template: " + name)
        key = normalized_name(relative)
        if key in expected:
            raise ValueError("duplicate path in recovery template: " + name)
        expected[key] = (index, name, relative)
        records.append((index, name, relative))

    loose_files = {}
    for path in sorted(loose_root.rglob("*")):
        if path.is_symlink():
            raise ValueError("GAME/ contains a symlink: " + str(path))
        if not path.is_file():
            continue
        relative = path.relative_to(loose_root).as_posix()
        key = normalized_name(relative)
        if key in loose_files:
            raise ValueError("case-insensitive path collision in GAME/: " + relative)
        loose_files[key] = path

    missing = sorted(set(expected) - set(loose_files))
    extra = sorted(set(loose_files) - set(expected))
    if missing:
        raise ValueError("GAME/ is missing indexed assets: "
            + ", ".join(expected[key][1] for key in missing[:8]))

    prefix_index = build_prefix_index(game_path, game_size, signature_size)
    discovered = []
    occupied = []
    for index, name, relative in records:
        path = loose_files[normalized_name(relative)]
        size = path.stat().st_size
        if not size or size > 0xFFFFFFFF or size < signature_size:
            raise ValueError("unsupported asset size for discovery: " + name)
        with path.open("rb") as asset:
            prefix = asset.read(signature_size)
        candidates = prefix_index.get(prefix, [])
        matches = [sector for sector in candidates if verify_candidate(game_path, path, sector, size)]
        if len(matches) != 1:
            reason = "no exact match" if not matches else "multiple exact matches"
            raise ValueError("cannot recover %s: %s in original GAME.DAT" % (name, reason))
        sector_count = (size + SECTOR_SIZE - 1) // SECTOR_SIZE
        sector_offset = matches[0]
        end = sector_offset + sector_count
        if end > (game_size + SECTOR_SIZE - 1) // SECTOR_SIZE:
            raise ValueError("discovered extent exceeds GAME.DAT: " + name)
        occupied.append((sector_offset, end, name))
        record_offset = 0x30 + index * 0x30
        struct.pack_into("<III", template, record_offset + 0x24,
            sector_offset, sector_count, size)
        discovered.append({
            "name": name,
            "path": relative,
            "sector_offset": sector_offset,
            "sector_count": sector_count,
            "byte_size": size,
        })

    occupied.sort()
    for previous, current in zip(occupied, occupied[1:]):
        if current[0] < previous[1]:
            raise ValueError("discovered extents overlap: %s and %s" % (previous[2], current[2]))

    manifest_path = write_runtime_manifest(root, template, info_lba, info_size, game_lba, game_size)
    report = {
        "format": "PS2_GAME_DISCOVERY",
        "version": 1,
        "sector_size": SECTOR_SIZE,
        "game_lba": game_lba,
        "game_size": game_size,
        "info_lba": info_lba,
        "info_size": info_size,
        "original_volume_sectors": volume_sectors,
        "complete": True,
        "match_method": "sector-aligned signature plus full byte comparison",
        "manifest": str(manifest_path.relative_to(root)),
        "assets": discovered,
        "unindexed_loose_assets": [
            loose_files[key].relative_to(loose_root).as_posix() for key in extra
        ],
    }
    report_path = root / ".ps2-disc" / "game_discovery.json"
    temporary = report_path.with_suffix(".tmp")
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(report_path)
    return manifest_path, len(discovered)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disc_folder", type=Path)
    parser.add_argument("--signature-size", type=int, default=16)
    args = parser.parse_args()
    if not 4 <= args.signature_size <= 256:
        parser.error("--signature-size must be between 4 and 256")
    try:
        target, count = discover(args.disc_folder, args.signature_size)
        print("recovered %d GAME.DAT entries into runtime manifest: %s" % (count, target))
    except (OSError, ValueError, UnicodeError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
