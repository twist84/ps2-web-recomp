#!/usr/bin/env python3
"""Cross-reference GS upload candidates with TEX0 binds.

These aggregate CSV reports contain no event timestamps or original asset
names. A layout join proves that a TEX0 configuration was observed, not
that a particular payload was sampled by that draw.
"""
import argparse
from collections import Counter, defaultdict
import csv
from pathlib import Path


def read_rows(path):
    with Path(path).open(newline="", encoding="utf-8-sig") as source:
        return list(csv.DictReader(source))


def layout(row):
    return (row["bp"].upper(), row["psm"].upper(),
            int(row["width"]), int(row["height"]))


def cross_reference(atlas_rows, bind_rows):
    layouts = defaultdict(lambda: {
        "uploads": 0, "hashes": set(), "bindings": 0, "tex0": set(),
        "samples": Counter(),
    })
    for row in atlas_rows:
        if row["completion"].lower() != "complete":
            continue
        item = layouts[layout(row)]
        count = int(row["count"])
        fingerprint = row["fnv64"].upper()
        item["uploads"] += count
        item["hashes"].add(fingerprint)
        item["samples"][fingerprint] += count

    for row in bind_rows:
        key = layout(row)
        if key not in layouts:
            continue
        item = layouts[key]
        item["bindings"] += int(row["count"])
        item["tex0"].add(row["tex0"].upper())

    groups = []
    candidates = []
    for key, item in layouts.items():
        bp, psm, width, height = key
        fingerprints = len(item["hashes"])
        tex0_count = len(item["tex0"])
        # A layout may have many distinct uploads and many TEX0 values.
        # Do not claim hash-to-draw association from aggregate counts.
        common = {
            "bp": bp, "psm": psm, "width": width, "height": height,
            "uploads": item["uploads"], "fingerprints": fingerprints,
            "bindings": item["bindings"], "tex0_variants": tex0_count,
            "ambiguous": "yes" if fingerprints != 1 or tex0_count != 1 else "no",
        }
        groups.append(common)
        for fingerprint, count in item["samples"].items():
            candidates.append({
                **common, "fnv64": fingerprint, "fingerprint_uploads": count,
            })

    groups.sort(key=lambda r: (
        r["ambiguous"] != "no", -r["bindings"], -r["uploads"],
        r["bp"], r["psm"], r["width"], r["height"],
    ))
    candidates.sort(key=lambda r: (
        r["ambiguous"] != "no", -r["fingerprint_uploads"],
        -r["bindings"], r["bp"], r["psm"], r["fnv64"],
    ))
    return groups, candidates


def write_csv(path, fields, rows):
    with Path(path).open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("atlas_candidates", type=Path)
    parser.add_argument("bindings", type=Path)
    parser.add_argument("--output", type=Path, default=Path("texture-crosswalk"))
    args = parser.parse_args()

    groups, candidates = cross_reference(
        read_rows(args.atlas_candidates), read_rows(args.bindings))
    args.output.mkdir(parents=True, exist_ok=True)
    base = ["bp", "psm", "width", "height", "uploads",
            "fingerprints", "bindings", "tex0_variants", "ambiguous"]
    write_csv(args.output / "layout_groups.csv", base, groups)
    write_csv(args.output / "fingerprint_candidates.csv",
              base + ["fnv64", "fingerprint_uploads"], candidates)
    print("groups:", len(groups), "fingerprints:", len(candidates))
    print("groups with matching TEX0 layout:",
          sum(group["bindings"] > 0 for group in groups))
    print("unambiguous observed layouts:",
          sum(group["ambiguous"] == "no" and group["bindings"] > 0
              for group in groups))
    print("Output:", args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
