#!/usr/bin/env python3
"""Export SDBZ GAME/ or INFO.DAT + GAME.DAT texture assets to a folder pack.

This is an offline exporter, not a GS capture. Unsupported images are reported,
never silently counted as successes. No third-party Python packages required.
"""
import argparse
import json
from pathlib import Path, PurePosixPath
import struct
import sys
import zlib

from match_sdbz_fpix_gs import parse_fpix, decode_rgba

SECTOR = 2048
ENTRY_SIZE = 48
MAX_ASSET_SIZE = 64 * 1024 * 1024
MAX_IMAGES = 4096


def safe_name(name):
    name = name.replace("\\", "/")
    path = PurePosixPath(name)
    if (not name or name.startswith("/") or ":" in name
            or any(part in ("", ".", "..") for part in name.split("/"))):
        raise ValueError("unsafe archive asset path")
    if not path.parts:
        raise ValueError("empty asset path")
    return path


def indexed_assets(info_path, game_path):
    encoded = info_path.read_bytes()
    if len(encoded) < 48 or encoded[1:16] != b"www.arika.co.jp":
        raise ValueError("invalid INFO.DAT signature")
    key = encoded[:16]
    decoded = key + bytes(
        ((~((value << 4 & 0xf0) | (value >> 4))) - key[i & 15]) & 0xff
        for i, value in enumerate(encoded[16:])
    )
    count = struct.unpack_from("<I", decoded, 0x2c)[0]
    if count > (len(decoded) - 48) // ENTRY_SIZE:
        raise ValueError("invalid INFO.DAT entry count")
    size = game_path.stat().st_size
    records = []
    seen = set()
    for i in range(count):
        off = 48 + i * ENTRY_SIZE
        raw = decoded[off:off + 32].split(b"\0", 1)[0]
        name = raw.decode("cp932", errors="strict")
        path = safe_name(name)
        key_name = str(path).lower()
        if key_name in seen:
            raise ValueError(f"duplicate asset: {name}")
        seen.add(key_name)
        _, sector, sectors, length = struct.unpack_from("<IIII", decoded, off + 32)
        offset = sector * SECTOR
        if (length > sectors * SECTOR or offset + sectors * SECTOR > size):
            raise ValueError(f"invalid extent: {name}")
        records.append((path, offset, length, sectors * SECTOR))
    intervals = sorted((offset, offset + allocation) for _, offset, _, allocation in records)
    if any(intervals[i][0] < intervals[i - 1][1] for i in range(1, len(intervals))):
        raise ValueError("overlapping GAME.DAT extents")
    with game_path.open("rb") as source:
        for path, offset, length, _ in records:
            if path.suffix.lower() not in (".pix", ".tim2", ".xpix"):
                continue
            if length > MAX_ASSET_SIZE:
                yield path, None, "asset exceeds 64 MiB safety limit"
                continue
            source.seek(offset)
            data = source.read(length)
            if len(data) != length:
                yield path, None, "truncated GAME.DAT asset"
            else:
                yield path, data, None


def loose_assets(root):
    root = root.resolve(strict=True)
    for file in sorted(root.rglob("*")):
        if file.is_symlink() or not file.is_file():
            continue
        if file.suffix.lower() not in (".pix", ".tim2", ".xpix"):
            continue
        name = safe_name(file.relative_to(root).as_posix())
        if file.stat().st_size > MAX_ASSET_SIZE:
            yield name, None, "asset exceeds 64 MiB safety limit"
            continue
        yield name, file.read_bytes(), None


def decode_tim2(data):
    if len(data) < 16 or data[:4] != b"TIM2":
        raise ValueError("missing TIM2 header")
    fmt, pictures = data[5], struct.unpack_from("<H", data, 6)[0]
    if fmt not in (0, 1) or pictures != 1:
        raise ValueError(f"unsupported TIM2 format/pictures {fmt}/{pictures}")
    at = 128 if fmt == 1 else 16
    if len(data) < at + 48:
        raise ValueError("short TIM2 picture")
    total, clut_size, image_size, header_size, colors, _, mipmaps, clut_type, image_type, width, height = struct.unpack_from(
        "<IIIHHBBBBHH", data, at)
    if (not width or not height or width > 4096 or height > 4096
            or mipmaps != 1 or header_size < 48
            or total < header_size + image_size + clut_size
            or at + total > len(data)):
        raise ValueError("invalid or unsupported TIM2 picture")
    offset = at + header_size
    pixels = data[offset:offset + image_size]
    palette = data[offset + image_size:offset + image_size + clut_size]
    if image_type in (4, 5):
        expected = (width * height + 1) // 2 if image_type == 4 else width * height
        if len(pixels) != expected:
            raise ValueError("invalid indexed TIM2 pixel count")
        chunk = {
            "width": width, "height": height,
            "pixels": pixels, "palette": palette,
            "colors": colors, "clut_type": clut_type,
            "psm": "14" if image_type == 4 else "13",
        }
        return width, height, decode_rgba(chunk)
    if image_type == 3:
        if len(pixels) != width * height * 4:
            raise ValueError("invalid RGBA32 pixel count")
        rgba = bytearray(pixels)
        for i in range(3, len(rgba), 4):
            rgba[i] = min(255, rgba[i] * 2)
        return width, height, bytes(rgba)
    raise ValueError(f"unsupported TIM2 image type {image_type}")


def textures(name, data):
    if data.startswith(b"FPIX"):
        for chunk in parse_fpix(data):
            yield name.as_posix() + f".{chunk['index']:03d}.png", (
                chunk["width"], chunk["height"], decode_rgba(chunk))
    elif data.startswith(b"XPIX") and len(data) >= 36:
        yield name.as_posix() + ".000.png", decode_tim2(data[32:])
    elif data.startswith(b"TIM2"):
        yield name.as_posix() + ".000.png", decode_tim2(data)
    else:
        raise ValueError("unrecognized PIX/TIM2 asset")


def png_bytes(width, height, rgba):
    if len(rgba) != width * height * 4:
        raise ValueError("RGBA dimensions mismatch")
    raw = b"".join(
        b"\x00" + rgba[y * width * 4:(y + 1) * width * 4]
        for y in range(height)
    )
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))
    return (b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b""))


def export(source, output, mode):
    output = output.resolve()
    if output.exists():
        raise ValueError(f"output pack already exists: {output}")
    results = []
    images = []
    exported = 0
    failures = 0
    output.mkdir(parents=True)
    (output / "pack.json").write_text(json.dumps({
        "pack": {"format": 1, "game": "sdbz", "region": "eu-beta", "priority": 0}
    }, indent=2) + "\n", encoding="utf-8")
    for name, data, error in source:
        report = {"asset": name.as_posix()}
        if error:
            report["error"] = error
            failures += 1
        else:
            try:
                converted = list(textures(name, data))
                if not converted or len(converted) > MAX_IMAGES:
                    raise ValueError("invalid number of texture chunks")
                for file_name, (width, height, rgba) in converted:
                    relative = safe_name(file_name)
                    file_path = output / "assets" / "sdbz" / "textures" / relative
                    if file_path.exists():
                        raise ValueError(f"duplicate output: {relative}")
                    file_path.parent.mkdir(parents=True, exist_ok=True)
                    file_path.write_bytes(png_bytes(width, height, rgba))
                    images.append({"name": relative.as_posix(), "width": width, "height": height})
                    exported += 1
                report["textures"] = len(converted)
            except (ValueError, struct.error, IndexError, OverflowError) as exc:
                report["error"] = str(exc)
                failures += 1
        results.append(report)
    (output / "dump_report.json").write_text(json.dumps({
        "source": mode,
        "assets_considered": len(results),
        "textures_exported": exported,
        "assets_failed": failures,
        "assets": results,
        "images": images
    }, indent=2) + "\n", encoding="utf-8")
    print(f"Exported {exported} textures; {failures} assets failed: {output}")
    return 0 if failures == 0 else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sources = parser.add_mutually_exclusive_group(required=True)
    sources.add_argument("--game-dir", type=Path, help="Loose GAME/ directory")
    sources.add_argument("--game-dat", type=Path, help="Direct GAME.DAT archive")
    parser.add_argument("--info", type=Path, help="Required alongside --game-dat")
    parser.add_argument("--output", type=Path, required=True, help="New folder pack")
    args = parser.parse_args()
    if args.game_dat:
        if args.info is None:
            parser.error("--info is required with --game-dat")
        source = indexed_assets(args.info, args.game_dat)
        mode = "INFO.DAT+GAME.DAT"
    else:
        if args.info:
            parser.error("--info is only used with --game-dat")
        source = loose_assets(args.game_dir)
        mode = "GAME/"
    try:
        return export(source, args.output, mode)
    except (OSError, ValueError, UnicodeError) as exc:
        print(f"Texture dump failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
