#!/usr/bin/env python3
"""Deterministic host-to-local GS IMAGE provenance tracking fixtures."""
import os
import pathlib
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

HARNESS = r'''
#include "gs/texture_upload_trace.hpp"
#include <cstdint>

int main()
{
    using ps2::gs::c_texture_upload_trace;
    c_texture_upload_trace trace;

    // PSMCT32, 2x2 => 16 bytes. Feed in separate GIF IMAGE pieces.
    const std::uint64_t bitbltbuf = std::uint64_t(0x120) << 32
        | std::uint64_t(1) << 48;
    const std::uint64_t trxreg = 2ull | (2ull << 32);
    const std::uint8_t a[] = {0, 1, 2, 3, 4};
    const std::uint8_t b[] = {5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 99, 100};
    trace.begin(1, bitbltbuf, 0, trxreg);
    if (!trace.current().active || trace.current().expected_bytes != 16) return 1;
    if (trace.append(a, sizeof a)) return 2;
    if (!trace.append(b, sizeof b)) return 3;
    if (trace.current().received_bytes != 16
        || trace.current().active || trace.append(b, sizeof b)) return 4;

    // Hash exactly 16 payload bytes; the extra padded packet bytes are ignored.
    std::uint64_t expected = 0xcbf29ce484222325ull;
    for (std::uint8_t i = 0; i < 16; ++i)
    {
        expected ^= i;
        expected *= 0x100000001b3ull;
    }
    if (trace.current().fnv1a64 != expected) return 5;

    // The same GS base pointer is later reused for unrelated pixels.
    trace.begin(2, bitbltbuf, 0, trxreg);
    const std::uint8_t other[16] = {};
    if (!trace.append(other, sizeof other)
        || trace.current().serial != 2
        || trace.current().fnv1a64 == expected) return 6;

    // PSMT4 transfers count four bits per pixel, rounded to bytes.
    trace.begin(3, (std::uint64_t(0x14) << 56) | bitbltbuf,
        0, 3ull | (1ull << 32));
    if (trace.current().expected_bytes != 2) return 7;
    if (!trace.append(a, sizeof a)
        || trace.current().received_bytes != 2) return 8;

    // A partial transfer is distinguishable from a full upload.
    trace.begin(4, bitbltbuf, 0, trxreg);
    if (trace.append(a, sizeof a)
        || trace.current().received_bytes != 5
        || !trace.current().active) return 9;
    trace.reset();
    if (trace.current().serial != 0 || trace.current().active) return 10;

    // Invalid pixel formats and empty extents are not fingerprinted.
    trace.begin(5, std::uint64_t(0x3f) << 56, 0, trxreg);
    if (trace.current().active) return 11;
    trace.begin(6, bitbltbuf, 0, 0);
    if (trace.current().active) return 12;
    return 0;
}
'''

class TextureUploadTests(unittest.TestCase):
    def test_chunked_fingerprint_and_reuse(self):
        env = os.environ.get("CXX")
        compiler = shlex.split(env, posix=(os.name != "nt")) if env else [
            shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise unittest.SkipTest("no C++ compiler available")
        with tempfile.TemporaryDirectory(prefix="sdbz-gs-upload-") as temporary:
            root = pathlib.Path(temporary)
            source = root / "test.cpp"
            binary = root / ("test.exe" if os.name == "nt" else "test")
            source.write_text(HARNESS, encoding="utf-8")
            subprocess.run(compiler + ["-std=c++20", "-Wall", "-Wextra",
                "-I", str(ROOT / "runtime/src"), str(source), "-o", str(binary)],
                check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    unittest.main()
