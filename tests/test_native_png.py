#!/usr/bin/env python3
"""Native PNG decode tests, independent of game files or GPU hardware."""
import os
import pathlib
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
SIGNATURE = b"\x89PNG\r\n\x1a\n"

def chunk(tag, payload):
    return struct.pack(">I", len(payload)) + tag + payload + struct.pack(
        ">I", zlib.crc32(tag + payload))

def png(width, height, color, rows):
    header = struct.pack(">IIBBBBB", width, height, 8, color, 0, 0, 0)
    return (SIGNATURE + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(b"".join(rows)))
            + chunk(b"IEND", b""))

HARNESS = r'''
#include "gs/native_texture.hpp"
#include "ps2/disc.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

std::vector<std::uint8_t> bytes;

namespace ps2::disc
{
std::optional<NativeAsset> find_native_asset(
    const std::string &path, const std::string &format)
{
    if ((path != "menu/title.pix" && path != "menu/title.pix.003")
        || format != "png" || bytes.empty())
        return std::nullopt;
    return NativeAsset{0, static_cast<std::uint32_t>(bytes.size()), path, format};
}
std::size_t read_native_asset(std::uint32_t id, std::uint64_t offset,
    void *destination, std::size_t capacity)
{
    if (id || offset >= bytes.size() || !destination) return 0;
    const std::size_t n = std::min<std::size_t>(capacity,
        bytes.size() - static_cast<std::size_t>(offset));
    std::memcpy(destination, bytes.data() + offset, n);
    return n;
}
}

int main(int argc, char **argv)
{
    if (argc != 3) return 1;
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) return 2;
    bytes.assign(std::istreambuf_iterator<char>(file), {});
    ps2::gs::native_texture::s_image decoded;
    const bool valid = ps2::gs::native_texture::load_png("menu/title.pix", decoded);
    const std::string mode = argv[2];
    if (mode == "invalid")
        return !valid && decoded.rgba.empty() && !decoded.width ? 0 : 3;
    if (!valid || decoded.width != 2 || decoded.height != 2
        || decoded.rgba.size() != 16) return 4;
    const std::uint8_t rgba[] = {
        255, 0, 0, 255, 0, 255, 0, 128,
        0, 0, 255, 255, 255, 255, 255, 255,
    };
    const std::uint8_t rgb[] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255,
    };
    if (std::memcmp(decoded.rgba.data(),
            mode == "rgba" ? rgba : rgb, 16)) return 5;
    if (!ps2::gs::native_texture::load_fpix_chunk_png(
            "menu/title.pix", 3, decoded) || decoded.rgba.size() != 16)
        return 6;
    if (ps2::gs::native_texture::load_fpix_chunk_png(
            "menu/title.pix", 1000, decoded) || !decoded.rgba.empty())
        return 7;
    return ps2::gs::native_texture::load_png("missing.pix", decoded)
        || !decoded.rgba.empty() ? 8 : 0;
}
'''

class PngDecodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler_setting = os.environ.get("CXX")
        compiler = shlex.split(compiler_setting, posix=(os.name != "nt")) if compiler_setting else [
            shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise unittest.SkipTest("C++ compiler unavailable")
        cls.temp = tempfile.TemporaryDirectory(prefix="sdbz-png-")
        cls.root = pathlib.Path(cls.temp.name)
        cls.executable = cls.root / ("test_png.exe" if os.name == "nt" else "test_png")
        source = cls.root / "test_png.cpp"
        source.write_text(HARNESS, encoding="utf-8")
        subprocess.run(compiler + [
            "-std=c++20", "-Wall", "-Wextra",
            "-I", str(ROOT / "runtime/include"),
            "-I", str(ROOT / "runtime/src"),
            str(source), str(ROOT / "runtime/src/gs/native_texture.cpp"),
            "-lz", "-o", str(cls.executable)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_png(self, data, mode):
        path = self.root / "sample.png"
        path.write_bytes(data)
        subprocess.run([str(self.executable), str(path), mode], check=True)

    def test_rgba_with_sub_filter(self):
        self.run_png(png(2, 2, 6, [
            b"\x00" + bytes([255, 0, 0, 255, 0, 255, 0, 128]),
            b"\x01" + bytes([0, 0, 255, 255, 255, 255, 0, 0]),
        ]), "rgba")

    def test_rgb_with_paeth_filter(self):
        # First row red/green; second row blue/white; Paeth uses the row above.
        self.run_png(png(2, 2, 2, [
            b"\x00" + bytes([255, 0, 0, 0, 255, 0]),
            b"\x04" + bytes([1, 0, 255, 255, 0, 0]),
        ]), "rgb")

    def test_invalid_crc(self):
        sample = bytearray(png(2, 2, 6, [b"\x00" + bytes(8)] * 2))
        sample[-5] ^= 1
        self.run_png(sample, "invalid")

    def test_truncated_file(self):
        self.run_png(png(2, 2, 6, [b"\x00" + bytes(8)] * 2)[:-6], "invalid")

    def test_rejects_excessive_dimensions(self):
        self.run_png(png(4097, 1, 6, [b"\x00" + bytes(4)]), "invalid")

    def test_unsupported_indexed_palette(self):
        self.run_png(png(2, 2, 3, [b"\x00" + bytes(2)] * 2), "invalid")

if __name__ == "__main__":
    unittest.main()
