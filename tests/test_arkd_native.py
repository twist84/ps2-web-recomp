#!/usr/bin/env python3
"""Native ARKD HLE catalog and RPC tests using synthetic loose GAME/ files."""

import os
import pathlib
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

HARNESS = r'''#include "iop/arika_dvd.hpp"
#include "iop/kernel.hpp"
#include "iop/sif.hpp"
#include "ps2/disc.hpp"
#include "ps2/memory.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ps2 {
std::uint8_t g_ram[kRamSize] = {};
void watch_span(std::uint32_t, std::uint32_t, const char *) {}
}

namespace ps2::iop::kernel {
HleModuleStart g_start;
void register_hle_module(const std::string &name, HleModuleStart callback)
{
    if (name == "ARKD_DVD") g_start = std::move(callback);
}
}

namespace ps2::sif {
std::map<std::uint32_t, HleServer> g_handlers;
void register_hle_server(std::uint32_t id, HleServer callback)
{
    g_handlers[id] = std::move(callback);
}
}

std::uint32_t word(std::uint32_t at)
{
    std::uint32_t value = 0;
    std::memcpy(&value, ps2::g_ram + at, sizeof(value));
    return value;
}

int main(int argc, char **argv)
{
    using namespace ps2::iop;
    if (argc != 2 || !ps2::disc::open(argv[1])) return 1;

    // The native catalog operates on the synthetic DVD, with no IRX bytes.
    arkd::c_catalog catalog;
    if (!catalog.open() || catalog.entry_count() != 3) return 2;
    const auto entry = catalog.find("PLY/SMALL.BIN");
    if (!entry || entry->file_size != 5000) return 3;
    if (catalog.find("missing.bin")) return 4;
    std::vector<std::uint8_t> data(5000);
    if (catalog.read(*entry, 0, data.data(), data.size()) != data.size()) return 5;
    for (std::uint8_t v : data) if (v != 'S') return 6;
    std::uint8_t fragment[32] = {};
    if (catalog.read(*entry, 2040, fragment, sizeof(fragment)) != sizeof(fragment)) return 7;
    for (std::uint8_t v : fragment) if (v != 'S') return 8;
    if (catalog.read(*entry, 4996, fragment, sizeof(fragment)) != 4) return 9;
    if (catalog.read(*entry, 5000, fragment, sizeof(fragment)) != 0) return 10;

#ifdef _WIN32
    _putenv_s("SDBZ_ARKD_NATIVE", "");
#else
    unsetenv("SDBZ_ARKD_NATIVE");
#endif
    if (arkd::native_requested() || arkd::intercept_module("cdrom0:\\ARKD_DVD.IRX;1"))
        return 11;

#ifdef _WIN32
    _putenv_s("SDBZ_ARKD_NATIVE", "1");
#else
    setenv("SDBZ_ARKD_NATIVE", "1", 1);
#endif
    if (!arkd::native_requested() || !arkd::intercept_module("cdrom0:\\ARKD_DVD.IRX;1")
        || arkd::intercept_module("cdrom0:\\OTHER.IRX;1")) return 12;
    arkd::install();
    if (!kernel::g_start) return 13;
    kernel::g_start();
    if (ps2::sif::g_handlers.size() != 4) return 14;

    // IRX 0x503 method 3: filename at args+4, return real and aligned sizes.
    std::memcpy(ps2::g_ram + 0x104, "ply\\small.bin", 14);
    if (!ps2::sif::g_handlers.at(0x503)(3, 0x100, 0x30, 0x180, 8)) return 15;
    if (word(0x180) != 5000 || word(0x184) != 5008) return 16;

    std::memcpy(ps2::g_ram + 0x104, "missing.bin", 12);
    if (!ps2::sif::g_handlers.at(0x503)(3, 0x100, 0x30, 0x180, 8)) return 17;
    if (word(0x180) != 0x80000064u) return 18;

    // Unsupported methods must report errors, never silently succeed.
    if (!ps2::sif::g_handlers.at(0x502)(0x99, 0x100, 0, 0x180, 4)) return 19;
    if (word(0x180) != 0x80000064u) return 20;
    return 0;
}
'''


class ArkdNativeHleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cxx = os.environ.get("CXX")
        if cxx:
            compiler = shlex.split(cxx, posix=(os.name != "nt"))
        else:
            found = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
            compiler = [found] if found else []
        if not compiler:
            raise unittest.SkipTest("C++ compiler unavailable")
        cls.temp = tempfile.TemporaryDirectory(prefix="arkd-native-")
        cls.root = pathlib.Path(cls.temp.name)
        cls.game = cls.root / "disc" / "GAME"
        for name, content in {
            "dis/a_first.bin": b"A" * 13,
            "ply/small.bin": b"S" * 5000,
            "trc/z_last.bin": b"Z" * 17,
        }.items():
            target = cls.game / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
        source = cls.root / "test.cpp"
        source.write_text(HARNESS, encoding="utf-8")
        cls.executable = cls.root / ("test.exe" if os.name == "nt" else "test")
        subprocess.run(compiler + [
            "-std=c++20", "-Wall", "-Wextra",
            "-I", str(ROOT / "runtime/include"),
            "-I", str(ROOT / "runtime/src"),
            str(source),
            str(ROOT / "runtime/src/disc.cpp"),
            str(ROOT / "runtime/src/disc_folder.cpp"),
            str(ROOT / "runtime/src/iop/arika_dvd.cpp"),
            "-o", str(cls.executable),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    def test_native_catalog_and_file_size_rpc(self):
        subprocess.run([str(self.executable), str(self.root / "disc")], check=True)
        self.assertFalse((self.root / "disc" / "INFO.DAT").exists())
        self.assertFalse((self.root / "disc" / "GAME.DAT").exists())
        self.assertFalse((self.root / "disc" / "ARKD_DVD.IRX").exists())


if __name__ == "__main__":
    unittest.main()
