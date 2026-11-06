#!/usr/bin/env python3
"""Independently locate unchanged loose GAME/ files inside the original GAME.DAT.

This produces a candidate report, not a replacement runtime manifest. Exact
matching can only locate assets whose bytes are still identical to the source
archive. It cannot recover edited/recompressed files or unknown INFO.DAT header
fields. Ambiguous matches are rejected.
"""

import argparse
import json
from pathlib import Path

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
        entries[path.replace("\\", "/").upper()] = (int(lba), int(size), path)
    return volume_sectors, entries


def build_prefix_index(game_path, game_size, signature_size):
    """Index signatures at every possible GAME.DAT sector boundary."""
    index = {}
    with game_path.open("rb") as game:
        sector = 0
        while sector * SECTOR_SIZE + signature_size <= game_size:
            game.seek(sector * SECTOR_SIZE)
            prefix = game.read(signature_size)
            if len(prefix) != signature_size:
                break
            index.setdefault(prefix, []).append(sector)
            sector += 1
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


def discover(disc_folder, signature_size=16):
    root = Path(disc_folder).resolve()
    volume_sectors, layout = read_layout(root)
    game_entry = layout.get("GAME.DAT")
    if game_entry is None:
        raise ValueError("layout.tsv does not contain GAME.DAT")
    game_lba, game_size, game_relative = game_entry
    game_path = root.joinpath(*Path(game_relative).parts)
    if not game_path.is_file() or game_path.stat().st_size != game_size:
        raise ValueError("independent discovery requires the original GAME.DAT with its recorded size")

    loose_root = root / "GAME"
    if not loose_root.is_dir():
        raise ValueError("loose GAME/ directory is missing")

    files = []
    for path in sorted(loose_root.rglob("*")):
        if path.is_symlink():
            raise ValueError("GAME/ contains a symlink: " + str(path))
        if not path.is_file():
            continue
        relative = path.relative_to(loose_root).as_posix()
        encoded_name = relative.replace("/", "\\").encode("cp932")
        if not encoded_name or len(encoded_name) > 32 or b"\0" in encoded_name:
            raise ValueError("asset path does not fit INFO.DAT's 32-byte name field: " + relative)
        size = path.stat().st_size
        if not size or size > 0xFFFFFFFF or size < signature_size:
            raise ValueError("asset has an unsupported size for discovery: " + relative)
        files.append((path, relative, encoded_name, size))
    if not files:
        raise ValueError("GAME/ contains no regular files")

    prefix_index = build_prefix_index(game_path, game_size, signature_size)
    results = []
    for path, relative, encoded_name, size in files:
        with path.open("rb") as asset:
            prefix = asset.read(signature_size)
        candidates = prefix_index.get(prefix, [])
        matches = [sector for sector in candidates if verify_candidate(game_path, path, sector, size)]
        if len(matches) != 1:
            state = "no exact match" if not matches else "multiple exact matches"
            raise ValueError("%s: %s in original GAME.DAT" % (relative, state))
        results.append({
            "name": relative.replace("/", "\\"),
            "path": relative,
            "name_hex": encoded_name.hex(),
            "sector_offset": matches[0],
            "sector_count": (size + SECTOR_SIZE - 1) // SECTOR_SIZE,
            "byte_size": size,
            "match": "exact-byte-match",
        })

    results.sort(key=lambda item: item["sector_offset"])
    previous_end = 0
    for item in results:
        start = item["sector_offset"]
        end = start + item["sector_count"]
        if start < previous_end:
            raise ValueError("candidate extents overlap near " + item["name"])
        if end > (game_size + SECTOR_SIZE - 1) // SECTOR_SIZE:
            raise ValueError("candidate extent exceeds GAME.DAT: " + item["name"])
        previous_end = end

    return {
        "format": "PS2_GAME_DISCOVERY",
        "version": 1,
        "sector_size": SECTOR_SIZE,
        "game_lba": game_lba,
        "game_size": game_size,
        "original_volume_sectors": volume_sectors,
        "complete_index_not_proven": True,
        "requires_info_template": True,
        "assets": results,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disc_folder", type=Path)
    parser.add_argument("--signature-size", type=int, default=16)
    args = parser.parse_args()
    if not 4 <= args.signature_size <= 256:
        parser.error("--signature-size must be between 4 and 256")
    try:
        report = discover(args.disc_folder, args.signature_size)
        target = args.disc_folder / ".ps2-disc" / "game_discovery.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix(".tmp")
        temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        temporary.replace(target)
        print("located %d unchanged GAME/ files: %s" % (len(report["assets"]), target))
        print("Note: this is a candidate index, not a complete INFO.DAT replacement.")
    except (OSError, ValueError, UnicodeError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
