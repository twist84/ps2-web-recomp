#!/usr/bin/env python3
"""Summarize SDBZ PS2_TRACE_TEXTURE_UPLOADS=1 output.

PowerShell redirects native stderr as UTF-16 LE on some Windows versions.
The output is a list of observed GS uploads, NOT a proven FPIX asset mapping.
"""
import argparse
from collections import Counter, defaultdict
import csv
from pathlib import Path
import re

UPLOAD = re.compile(
    r"\[gs-texture\] upload serial=(\d+) bp=([0-9a-f]+) bw=(\d+)"
    r" psm=([0-9a-f]+) size=(\d+)x(\d+) pos=([0-9a-f]+)"
    r" bytes=(\d+)/(\d+)\s+fnv64=([0-9a-f]+)\s+(complete|partial)",
    re.I,
)
BIND = re.compile(
    r"\[gs-texture\] bind ctx=(\d+) tex0=([0-9a-f]+)"
    r" bp=([0-9a-f]+) psm=([0-9a-f]+) size=(\d+)x(\d+)",
    re.I,
)

def read_text(path):
    with open(path, "rb") as source:
        prefix = source.read(4)
    if prefix.startswith(b"\xff\xfe") or prefix.startswith(b"\xfe\xff"):
        encoding = "utf-16"
    elif prefix.startswith(b"\xef\xbb\xbf"):
        encoding = "utf-8-sig"
    else:
        encoding = "utf-8"
    return Path(path).read_text(encoding=encoding, errors="replace")

def records(text):
    # PowerShell wraps some native-error lines between the byte count and
    # fnv64 field; permit arbitrary whitespace there.
    for _, kind, match in sorted(
        ([(m.start(), "upload", m) for m in UPLOAD.finditer(text)]
         + [(m.start(), "bind", m) for m in BIND.finditer(text)]),
        key=lambda item: item[0],
    ):
        if kind == "upload":
            yield {
                "kind": "upload",
                "serial": int(match[1]),
                "bp": match[2].upper(),
                "bw": int(match[3]),
                "psm": match[4].upper(),
                "width": int(match[5]),
                "height": int(match[6]),
                "pos": match[7].upper(),
                "received": int(match[8]),
                "expected": int(match[9]),
                "fingerprint": match[10].upper(),
                "completion": match[11].lower(),
            }
        else:
            yield {
                "kind": "bind",
                "ctx": int(match[1]),
                "tex0": match[2].upper(),
                "bp": match[3].upper(),
                "psm": match[4].upper(),
                "width": int(match[5]),
                "height": int(match[6]),
            }

def analyze(text):
    uploads = Counter()
    binds = Counter()
    destinations = defaultdict(set)
    distinct_hashes = set()
    total_uploads = total_binds = partial = 0
    for event in records(text):
        if event["kind"] == "upload":
            total_uploads += 1
            partial += event["completion"] == "partial"
            key = (event["bp"], event["bw"], event["psm"],
                   event["width"], event["height"], event["pos"],
                   event["fingerprint"], event["completion"])
            uploads[key] += 1
            distinct_hashes.add(event["fingerprint"])
            destinations[event["bp"]].add(
                (event["psm"], event["width"], event["height"],
                 event["fingerprint"]))
        else:
            total_binds += 1
            key = (event["ctx"], event["tex0"], event["bp"],
                   event["psm"], event["width"], event["height"])
            binds[key] += 1

    return {
        "upload_count": total_uploads, "bind_count": total_binds,
        "partial_count": partial, "unique_uploads": len(uploads),
        "unique_fingerprints": len(distinct_hashes),
        "uploads": uploads, "binds": binds,
        "reused_destinations": {bp: len(values)
                                for bp, values in destinations.items()
                                if len(values) > 1},
    }

def write_csv(path, header, rows):
    with open(path, "w", newline="", encoding="utf-8") as target:
        writer = csv.writer(target)
        writer.writerow(header)
        writer.writerows(rows)

def export_summary(summary, output):
    output.mkdir(parents=True, exist_ok=True)
    write_csv(output / "uploads.csv",
              ["bp", "bw", "psm", "width", "height", "pos",
               "fnv64", "completion", "count"],
              ([*key, count] for key, count in sorted(
                  summary["uploads"].items(), key=lambda item: (-item[1], item[0]))))
    write_csv(output / "bindings.csv",
              ["ctx", "tex0", "bp", "psm", "width", "height", "count"],
              ([*key, count] for key, count in sorted(
                  summary["binds"].items(), key=lambda item: (-item[1], item[0]))))
    write_csv(output / "reused_destinations.csv",
              ["bp", "different_payload_or_layout_count"],
              sorted(summary["reused_destinations"].items(),
                     key=lambda item: (-item[1], item[0])))
    # FPIX menu atlas candidates: observed 256x256 and 512x512 uploads.
    # Dimensions alone cannot establish the asset name/chunk ordinal.
    write_csv(output / "atlas_candidates.csv",
              ["bp", "bw", "psm", "width", "height", "pos",
               "fnv64", "completion", "count"],
              ([*key, count] for key, count in sorted(
                  summary["uploads"].items(), key=lambda item: (-item[1], item[0]))
               if key[3:5] in ((256, 256), (512, 512))
               and key[7] == "complete"))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path, default=Path("texture-report"))
    args = parser.parse_args()
    report = analyze(read_text(args.log))
    export_summary(report, args.output)
    print("uploads:", report["upload_count"],
          "bindings:", report["bind_count"],
          "partial:", report["partial_count"])
    print("distinct upload signatures:", report["unique_uploads"],
          "distinct payload fingerprints:", report["unique_fingerprints"])
    print("reused GS base pointers:", len(report["reused_destinations"]))
    print("CSV output:", args.output)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
