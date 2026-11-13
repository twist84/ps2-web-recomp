#!/usr/bin/env python3
"""Source payload fingerprint verification without the proprietary game file."""
import csv
import pathlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
from match_sdbz_fpix_gs import decode_rgba, fnv1a64, match, parse_fpix


def make_fpix(pixels=b"\x00\x01\x02\x03"):
    palette = bytearray(1024)
    for index in range(4):
        palette[index * 4:index * 4 + 4] = bytes([index * 20, index * 30,
                                                    index * 40, 64])
    picture = struct.pack("<IIIHHBBBBHH", 128 + len(pixels) + len(palette),
                          len(palette), len(pixels), 128, 256, 0, 1,
                          0x83, 5, 2, 2)
    picture += bytes(128 - len(picture))
    tim2 = b"TIM2" + bytes([4, 1]) + struct.pack("<H", 1) + bytes(120)
    xpix = b"XPIX" + bytes(28) + tim2 + picture + pixels + palette
    xpix = xpix[:4] + struct.pack("<I", len(xpix)) + xpix[8:]
    size = 36 + len(xpix)
    return (b"FPIX" + struct.pack("<III", size, 1, 256)
            + bytes(16) + struct.pack("<I", 36) + xpix)


class FpixGsMatchTests(unittest.TestCase):
    def test_exact_payload_and_rgba(self):
        raw = make_fpix()
        chunk, = list(parse_fpix(raw))
        self.assertEqual(chunk["pixel_offset"], 36 + 32 + 128 + 128)
        self.assertEqual(chunk["pixels"], bytes([0, 1, 2, 3]))
        self.assertEqual(chunk["fnv64"], fnv1a64(bytes([0, 1, 2, 3])))
        rgba = decode_rgba(chunk)
        self.assertEqual(len(rgba), 16)
        self.assertEqual(rgba[:4], bytes([0, 0, 0, 128]))
        self.assertEqual(rgba[12:], bytes([60, 90, 120, 128]))

    def test_match_only_identical_source_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            pix = root / "main_menu.pix"
            pix.write_bytes(make_fpix())
            candidate = root / "fingerprint_candidates.csv"
            with candidate.open("w", newline="") as file:
                writer = csv.writer(file)
                writer.writerow(["fnv64", "psm", "width", "height",
                                 "completion", "bp", "fingerprint_uploads"])
                writer.writerow([fnv1a64(b"\x00\x01\x02\x03"),
                                 "13", 2, 2, "complete", "2A00", 5])
                writer.writerow(["0000000000000000",
                                 "13", 2, 2, "complete", "2B00", 9])
            result, = match(pix, candidate)
            self.assertEqual(result["observed_bp"], "2A00")
            self.assertEqual(result["matching_layouts"], 1)
            self.assertEqual(result["matched_uploads"], 5)
            self.assertEqual(result["native_png"], "main_menu.pix.000.png")

    def test_corrupt_header_rejected(self):
        blob = bytearray(make_fpix())
        blob[0:4] = b"FAIL"
        with self.assertRaises(ValueError):
            list(parse_fpix(blob))
        blob = bytearray(make_fpix())
        blob[36 + 32 + 5] = 0
        with self.assertRaises(ValueError):
            list(parse_fpix(blob))


if __name__ == "__main__":
    unittest.main()
