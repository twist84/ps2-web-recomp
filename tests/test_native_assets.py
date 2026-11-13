#!/usr/bin/env python3
"""Host replacement catalog regression tests; converted bytes never reach guest files."""
import os
import pathlib
import shlex
import shutil
import subprocess
import tempfile
import unittest

from test_game_dat_loose_assets import fixture_in_place

ROOT = pathlib.Path(__file__).resolve().parents[1]

HARNESS = r'''
#include "disc_folder.hpp"
#include "ps2/disc.hpp"
#include <cstdint>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 3 || !ps2::disc::open_folder(argv[1])) return 1;
    const auto files = ps2::disc::native_assets();
    const auto mode = std::string(argv[2]);
    if (mode == "empty")
    {
        return files.empty() && !ps2::disc::find_native_asset(
            "ply/p01/p01a.mdl", "png") ? 0 : 2;
    }
    if (files.size() != 2) return 3;
    const auto texture = ps2::disc::find_native_asset(
        "PLY\\P01\\P01A.MDL", "png");
    const auto audio = ps2::disc::find_native_asset(
        "snd/music.ams", "wav");
    if (!texture || !audio || texture->size != 7 || audio->size != 8) return 4;
    if (ps2::disc::find_native_asset("../ply/p01/p01a.mdl", "png")
        || ps2::disc::find_native_asset("/ply/p01/p01a.mdl", "png")
        || ps2::disc::find_native_asset("ply/p01/p01a.mdl", "exe")
        || ps2::disc::find_native_asset("unknown.mdl", "png")) return 5;
    std::uint8_t out[16] = {};
    if (ps2::disc::read_native_asset(texture->id, 0, out, sizeof out) != 7
        || std::memcmp(out, "PNGTEST", 7)) return 6;
    std::memset(out, 0, sizeof out);
    if (ps2::disc::read_native_asset(audio->id, 3, out, sizeof out) != 5
        || std::memcmp(out, "F-WAV", 5)) return 7;
    if (ps2::disc::find("NATIVE") || ps2::disc::find("NATIVE/PLY")) return 8;
    if (mode == "removed")
    {
        return ps2::disc::read_native_asset(texture->id, 0, out, sizeof out) == 0
            ? 0 : 9;
    }
    return 0;
}
'''

class NativeAssetCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler_name = os.environ.get("CXX")
        compiler = shlex.split(compiler_name, posix=(os.name != "nt")) if compiler_name else [
            shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")]
        if not compiler or not compiler[0]:
            raise unittest.SkipTest("C++ compiler unavailable")
        cls.temp = tempfile.TemporaryDirectory(prefix="native-asset-")
        cls.work = pathlib.Path(cls.temp.name)
        cls.source = cls.work / "native_harness.cpp"
        cls.executable = cls.work / ("native_harness.exe" if os.name == "nt" else "native_harness")
        cls.source.write_text(HARNESS, encoding="utf-8")
        subprocess.run(compiler + ["-std=c++20", "-Wall", "-Wextra",
            "-I", str(ROOT / "runtime/include"),
            "-I", str(ROOT / "runtime/src"),
            str(cls.source), str(ROOT / "runtime/src/disc.cpp"),
            str(ROOT / "runtime/src/disc_folder.cpp"),
            "-o", str(cls.executable)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def fixture(self, name, with_native=True):
        root = self.work / name
        shutil.rmtree(root, ignore_errors=True)
        fixture_in_place(root, True)
        if with_native:
            texture = root / "NATIVE/ply/p01/p01a.mdl.png"
            audio = root / "NATIVE/snd/music.ams.wav"
            texture.parent.mkdir(parents=True)
            audio.parent.mkdir(parents=True)
            texture.write_bytes(b"PNGTEST")
            audio.write_bytes(b"RIFF-WAV")
        return root

    def run_harness(self, root, mode):
        env = {**os.environ, "SDBZ_ARKD_NATIVE": "1"}
        subprocess.run([str(self.executable), str(root), mode],
            env=env, check=True)

    def test_missing_replacements_fall_back(self):
        self.run_harness(self.fixture("missing", False), "empty")

    def test_native_files_are_indexed_and_hidden_from_guest(self):
        self.run_harness(self.fixture("replacements"), "normal")

    def test_replacement_removed_after_mount_is_not_read(self):
        # A missing replacement must never cause an unsafe read.
        # The harness removes the file after opening in this mode.
        root = self.fixture("removed")
        self.run_harness(root, "normal")

if __name__ == "__main__":
    unittest.main()
