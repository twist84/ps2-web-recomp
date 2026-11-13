#!/usr/bin/env python3
"""Generate the native GS menu fingerprint table from verified CSV evidence.

The generated header is committed, so builds never need Python or the original
game asset. Editing the verified manifest and regenerating replaces constants.
"""
import argparse
import csv
import re
from pathlib import Path

SOURCE = Path("docs/sdbz-native-texture-manifest.csv")
DEST = Path("runtime/src/gs/native_menu_manifest.hpp")


def generate(path):
    with Path(path).open(newline="", encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(stream))
    
    if not rows or len(rows) > 16:
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
        "    const char *game_path;",
        "    std::uint32_t chunk_index;",
        "    std::uint64_t fingerprint;",
        "    std::uint32_t width;",
        "    std::uint32_t height;",
        "    std::uint32_t psm;",
        "};",
        "",
        f"inline constexpr std::array<s_menu_atlas_entry, {len(rows)}> k_menu_atlases = {{{{",
    ]
    seen = set()
    signatures = set()
    for row in rows:
        game_path = row["game_path"]
        chunk = int(row["chunk_index"])
        if (not re.fullmatch(r"[A-Za-z0-9_./-]+", game_path)
                or game_path.startswith("/") or ".." in game_path.split("/")
                or not game_path.lower().endswith(".pix") or not 0 <= chunk <= 999):
            raise ValueError("invalid game path or chunk")
        key = (game_path.lower(), chunk)
        if key in seen:
            raise ValueError("duplicate source atlas")
        seen.add(key)
        value = row["fnv64"].upper()
        if len(value) != 16 or any(c not in "0123456789ABCDEF" for c in value):
            raise ValueError("invalid fingerprint")
        w, h, psm = (int(row["width"]), int(row["height"]),
                     int(row["psm"], 16))
        if w not in (256, 512) or h != w or psm != 0x13:
            raise ValueError("only validated PSMT8 menu atlases are supported")
        signature = (value, w, h, psm)
        if signature in signatures:
            raise ValueError("ambiguous duplicate fingerprint")
        signatures.add(signature)
        output.append(f'    {{"{game_path}", {chunk}u, 0x{value}ull, {w}u, {h}u, 0x{psm:02X}u}},')
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
