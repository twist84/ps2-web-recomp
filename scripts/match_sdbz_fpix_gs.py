#!/usr/bin/env python3
"""Match Arika FPIX/TIM2 indexed source pixels to GS upload fingerprints.

Unlike GS BP/TEX0, an exact byte-range fingerprint identifies a source
pixel payload observed by the upload tracer. The mapping remains specific to
the original file and the capture used, and is not an active renderer hook.
"""
import argparse
from collections import defaultdict
import csv
from pathlib import Path
import struct


def fnv1a64(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return f"{value:016X}"


def parse_fpix(data):
    if len(data) < 36 or data[:4] != b"FPIX":
        raise ValueError("expected FPIX header")
    declared_size, count, _version = struct.unpack_from("<III", data, 4)
    if declared_size != len(data) or not 0 < count <= 4096 or 32 + count * 4 > len(data):
        raise ValueError("invalid FPIX directory")
    offsets = struct.unpack_from(f"<{count}I", data, 32)
    if offsets[0] != 32 + count * 4 or list(offsets) != sorted(set(offsets)):
        raise ValueError("invalid FPIX chunk offsets")
    for index, start in enumerate(offsets):
        end = offsets[index + 1] if index + 1 < count else len(data)
        if end > len(data) or start + 208 > end or data[start:start + 4] != b"XPIX":
            raise ValueError(f"invalid XPIX chunk {index}")
        if struct.unpack_from("<I", data, start + 4)[0] != end - start:
            raise ValueError(f"incorrect XPIX extent {index}")
        tim2 = start + 32
        if (data[tim2:tim2 + 4] != b"TIM2" or data[tim2 + 5] != 1
                or struct.unpack_from("<H", data, tim2 + 6)[0] != 1):
            raise ValueError(f"unsupported TIM2 header {index}")
        picture = tim2 + 128
        (total, clut_size, image_size, header_size, colors, _format,
         mipmaps, clut_type, image_type, width, height) = struct.unpack_from(
             "<IIIHHBBBBHH", data, picture)
        if (not width or not height or mipmaps != 1 or header_size < 48
                or picture + total > end
                or total < header_size + image_size + clut_size):
            raise ValueError(f"invalid TIM2 extents {index}")
        if image_type not in (4, 5):
            raise ValueError(f"unsupported indexed image format {index}")
        expected = width * height if image_type == 5 else (width * height + 1) // 2
        if image_size != expected:
            raise ValueError(f"invalid indexed image size {index}")
        pixel_offset = picture + header_size
        pixels = data[pixel_offset:pixel_offset + image_size]
        palette = data[pixel_offset + image_size:pixel_offset + image_size + clut_size]
        yield {
            "index": index, "width": width, "height": height,
            "psm": "13" if image_type == 5 else "14",
            "pixel_offset": pixel_offset, "image_size": image_size,
            "pixels": pixels, "palette": palette, "colors": colors,
            "clut_type": clut_type, "fnv64": fnv1a64(pixels),
        }


def match(pix_path, candidates_csv):
    candidates = defaultdict(list)
    with Path(candidates_csv).open(newline="", encoding="utf-8-sig") as source:
        for row in csv.DictReader(source):
            if row.get("completion", "complete") != "complete":
                continue
            key = (row["fnv64"].upper(), row["psm"].upper(),
                   int(row["width"]), int(row["height"]))
            candidates[key].append(row)
    result = []
    for chunk in parse_fpix(Path(pix_path).read_bytes()):
        key = (chunk["fnv64"], chunk["psm"], chunk["width"], chunk["height"])
        matches = candidates.get(key, [])
        result.append({
            "chunk_index": chunk["index"],
            "native_png": f"{Path(pix_path).name}.{chunk['index']:03d}.png",
            "psm": chunk["psm"], "width": chunk["width"], "height": chunk["height"],
            "image_offset": chunk["pixel_offset"],
            "image_size": chunk["image_size"], "fnv64": chunk["fnv64"],
            "observed_bp": ";".join(sorted({row["bp"] for row in matches})),
            "matching_layouts": len(matches),
            "matched_uploads": sum(int(row["fingerprint_uploads"]) for row in matches),
        })
    return result


def decode_rgba(chunk):
    if ((chunk["clut_type"] & 0x3f) != 3
            or len(chunk["palette"]) < chunk["colors"] * 4):
        raise ValueError("unsupported palette")
    output = bytearray(chunk["width"] * chunk["height"] * 4)
    for pixel in range(chunk["width"] * chunk["height"]):
        if chunk["psm"] == "13":
            index = chunk["pixels"][pixel]
            if not chunk["clut_type"] & 0x80:
                index = (index & ~0x18) | ((index & 8) << 1) | ((index & 16) >> 1)
        else:
            index = (chunk["pixels"][pixel // 2] >> ((pixel & 1) * 4)) & 15
        if index >= chunk["colors"]:
            raise ValueError("palette index out of range")
        source = index * 4
        destination = pixel * 4
        output[destination:destination + 3] = chunk["palette"][source:source + 3]
        output[destination + 3] = min(255, chunk["palette"][source + 3] * 2)
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pix", type=Path)
    parser.add_argument("fingerprints", type=Path)
    parser.add_argument("--output", type=Path, default=Path("fpix_gs_matches.csv"))
    parser.add_argument("--export-png-dir", type=Path)
    args = parser.parse_args()
    rows = match(args.pix, args.fingerprints)
    fields = ["chunk_index", "native_png", "psm", "width", "height",
              "image_offset", "image_size", "fnv64", "observed_bp",
              "matching_layouts", "matched_uploads"]
    with args.output.open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    if args.export_png_dir:
        from PIL import Image
        args.export_png_dir.mkdir(parents=True, exist_ok=True)
        for chunk in parse_fpix(args.pix.read_bytes()):
            Image.frombytes("RGBA", (chunk["width"], chunk["height"]),
                            decode_rgba(chunk)).save(
                                args.export_png_dir /
                                f"{args.pix.name}.{chunk['index']:03d}.png")
    print(f"Matched {sum(row['matching_layouts'] > 0 for row in rows)}/"
          f"{len(rows)} source chunks; report: {args.output}")


if __name__ == "__main__":
    main()
