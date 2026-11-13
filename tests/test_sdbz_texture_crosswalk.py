#!/usr/bin/env python3
"""Regression tests for aggregate GS upload/TEX0 cross-referencing."""
import csv
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from crosswalk_sdbz_texture_candidates import cross_reference, read_rows, write_csv


class CandidateCrosswalkTests(unittest.TestCase):
    def test_duplicate_hashes_and_ambiguous_layouts(self):
        atlas = [
            dict(bp="2A00", psm="13", width="512", height="512",
                 fnv64="AAAA", completion="complete", count="10"),
            dict(bp="2A00", psm="13", width="512", height="512",
                 fnv64="BBBB", completion="complete", count="3"),
            dict(bp="2A00", psm="13", width="512", height="512",
                 fnv64="BBBB", completion="complete", count="2"),
            dict(bp="2E20", psm="13", width="256", height="256",
                 fnv64="CCCC", completion="complete", count="4"),
            dict(bp="3000", psm="14", width="256", height="256",
                 fnv64="DDDD", completion="partial", count="1"),
        ]
        binds = [
            dict(bp="2A00", psm="13", width="512", height="512",
                 tex0="1111", count="12"),
            dict(bp="2A00", psm="13", width="512", height="512",
                 tex0="2222", count="3"),
            dict(bp="2E20", psm="13", width="256", height="256",
                 tex0="3333", count="7"),
            dict(bp="3000", psm="14", width="256", height="256",
                 tex0="4444", count="2"),
        ]
        groups, candidates = cross_reference(atlas, binds)
        self.assertEqual(len(groups), 2)
        by_bp = {row["bp"]: row for row in groups}
        self.assertEqual(by_bp["2A00"]["uploads"], 15)
        self.assertEqual(by_bp["2A00"]["bindings"], 15)
        self.assertEqual(by_bp["2A00"]["fingerprints"], 2)
        self.assertEqual(by_bp["2A00"]["tex0_variants"], 2)
        self.assertEqual(by_bp["2A00"]["ambiguous"], "yes")
        self.assertEqual(by_bp["2E20"]["ambiguous"], "no")
        self.assertEqual([row["bp"] for row in groups], ["2E20", "2A00"])
        self.assertEqual(len(candidates), 3)
        self.assertEqual(
            {r["fnv64"]: r["fingerprint_uploads"] for r in candidates},
            {"AAAA": 10, "BBBB": 5, "CCCC": 4})

    def test_csv_roundtrip(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.csv"
            rows = [{"bp": "2A00", "width": 512, "height": 512}]
            write_csv(path, ["bp", "width", "height"], rows)
            self.assertEqual(read_rows(path),
                             [{"bp": "2A00", "width": "512", "height": "512"}])


if __name__ == "__main__":
    unittest.main()
