#!/usr/bin/env python3
"""Generate the native GS menu fingerprint table from verified CSV evidence.

The generated header is committed, so builds never need Python or the original
game asset. Editing the verified manifest and regenerating replaces constants.
"""
import argparse
import csv
from pathlib import Path

SOURCE = Path("docs/sdbz-main-menu-verified-fingerprints.csv")
DEST = Path("runtime/src/gs/native_menu_manifest.hpp")


def generate(path):
    with Path(path).open(newline="", encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(stream))
    if [int(row["chunk_index"]) for row in rows] != list(range(len(rows))):
        raise ValueError("chunk indices must start at zero and be contiguous")
    if not rows or len(rows) > 64:
        raise ValueError("unsupported number of atlases")
    output = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace ps2::gs::native_texture",
        "{",
        "",
        "struct s_menu_atlas_entry",
        "{",
        "    std::uint64_t fingerprint;",
        "    std::uint32_t width;",
        "    std::uint32_t height;",
        "    std::uint32_t psm;",
        "};",
        "",
        f"inline constexpr std::array<s_menu_atlas_entry, {len(rows)}> k_menu_atlases = {{{{",
    ]
    for row in rows:
        value = row["fnv64"].upper()
        if len(value) != 16 or any(c not in "0123456789ABCDEF" for c in value):
            raise ValueError("invalid fingerprint")
        w, h, psm = (int(row["width"]), int(row["height"]),
                     int(row["psm"], 16))
        if w not in (256, 512) or h != w or psm != 0x13:
            raise ValueError("only validated PSMT8 menu atlases are supported")
        output.append(f"    {{0x{value}ull, {w}u, {h}u, 0x{psm:02X}u}},")
    output += ["}};", "", "}", ""]
    return "\n".join(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--output", type=Path, default=DEST)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    content = generate(args.source)
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding="utf-8") != content:
            parser.error("native menu header is stale; regenerate it")
    else:
        args.output.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
