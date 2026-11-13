#!/usr/bin/env python3
"""Regressions for native PowerShell UTF-16 GS texture logs."""
import csv
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from analyze_sdbz_texture_log import analyze, export_summary, read_text


class TextureLogAnalysisTests(unittest.TestCase):
    def test_wrapped_hash_reuse_and_bind(self):
        text = (
            "[gs-texture] upload serial=1 bp=2A00 bw=8 psm=13 size=512x512 "
            "pos=0000000000000000 bytes=262144/262144\n"
            "fnv64=1111111111111111 complete\n"
            "[gs-texture] bind ctx=0 tex0=4005C00665322A00 "
            "bp=2A00 psm=13 size=512x512\n"
            "[gs-texture] upload serial=2 bp=2A00 bw=8 psm=13 size=512x512 "
            "pos=0000000000000000 bytes=262144/262144 "
            "fnv64=2222222222222222 complete\n"
            "[gs-texture] upload serial=3 bp=3000 bw=4 psm=14 size=256x256 "
            "pos=0000000000000000 bytes=2/32768 "
            "fnv64=3333333333333333 partial\n"
        )
        report = analyze(text)
        self.assertEqual(report["upload_count"], 3)
        self.assertEqual(report["bind_count"], 1)
        self.assertEqual(report["partial_count"], 1)
        self.assertEqual(report["unique_uploads"], 3)
        self.assertEqual(report["unique_fingerprints"], 3)
        self.assertEqual(report["reused_destinations"], {"2A00": 2})
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "report"
            export_summary(report, output)
            with (output / "atlas_candidates.csv").open(newline="") as file:
                rows = list(csv.DictReader(file))
            self.assertEqual(len(rows), 2)
            self.assertTrue(all(row["width"] == "512" for row in rows))
            self.assertTrue((output / "bindings.csv").exists())

    def test_powershell_encoding_detection(self):
        with tempfile.TemporaryDirectory() as directory:
            sample = Path(directory) / "gs.log"
            content = "[gs-texture] bind ctx=0 tex0=ABCD bp=1000 psm=13 size=256x256\n"
            for encoding in ("utf-16", "utf-8-sig", "utf-8"):
                with self.subTest(encoding=encoding):
                    sample.write_text(content, encoding=encoding)
                    self.assertEqual(analyze(read_text(sample))["bind_count"], 1)


if __name__ == "__main__":
    unittest.main()
