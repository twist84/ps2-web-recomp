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
#include "ps2/state.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <utility>
#include <string>
#include <vector>

namespace ps2 {
std::uint8_t g_ram[kRamSize] = {};
void watch_span(std::uint32_t, std::uint32_t, const char *) {}
}

namespace ps2::iop {
std::uint8_t g_ram[kRamSize] = {};
}

namespace ps2::iop::kernel {
HleModuleStart g_start;
JobHandler g_read_job = nullptr;
JobHandler g_voice_job = nullptr;
Job g_queued_read;
Job g_queued_voice;
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_voice_writes;
std::vector<std::vector<std::uint8_t>> g_effect_calls;
std::vector<std::uint32_t> g_effect_cores;
bool library_exists(const std::string &name) { return name == "libsd"; }
std::uint32_t alloc(std::uint32_t, int, std::uint32_t) { return 0x1000; }
void free(std::uint32_t) {}
bool call_library_export(const std::string &name, std::uint32_t ordinal,
    std::uint32_t a0, std::uint32_t a1, std::uint32_t,
    std::uint32_t, std::uint32_t &result)
{
    if (name != "libsd") return false;
    if (ordinal == 5)
    {
        g_voice_writes.emplace_back(a0, a1);
        result = 0;
        return true;
    }
    if (ordinal != 23 || a0 > 1 || a1 != 0x1000)
        return false;
    g_effect_cores.push_back(a0);
    const auto *bytes = ps2::iop::ram_ptr(a1);
    g_effect_calls.emplace_back(bytes, bytes + 16);
    result = 0;
    return true;
}
void register_hle_module(const std::string &name, HleModuleStart callback)
{
    if (name == "ARKD_DVD") g_start = std::move(callback);
}
void register_job(std::uint32_t kind, JobHandler callback)
{
    if (kind == kJobArkdRead) g_read_job = callback;
    else if (kind == kJobArkdVoice) g_voice_job = callback;
}
std::uint64_t now() { return 0; }
std::uint64_t usec_to_cycles(std::uint64_t us) { return us; }
void schedule(std::uint64_t, const Job &job)
{
    if (job.kind == kJobArkdVoice) g_queued_voice = job;
    else g_queued_read = job;
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

    // 0x503/0x102 enqueues a real file read to EE RAM. Method 2 exposes
    // the guest-observable pending/completed phase.
    std::memset(ps2::g_ram + 0x100, 0, 0x24);
    const std::uint32_t target = 0x200;
    std::memcpy(ps2::g_ram + 0x100, &target, 4);
    std::memcpy(ps2::g_ram + 0x104, "ply\\small.bin", 14);
    if (!ps2::sif::g_handlers.at(0x503)(0x102, 0x100, 0x24, 0x180, 4))
        return 21;
    if (!kernel::g_read_job || kernel::g_queued_read.kind != kernel::kJobArkdRead)
        return 22;
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x10000000u) return 23;
    kernel::g_read_job(kernel::g_queued_read);
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x30000000u) return 24;
    for (unsigned i = 0; i < 5000; ++i)
        if (ps2::g_ram[0x200 + i] != 'S') return 25;

    // The IRX's 0x501/0x11 method passes two 16-byte effect descriptors
    // through libsd export 23, one invocation for each SPU2 core.
    for (unsigned i = 0; i < 32; ++i)
        ps2::g_ram[0x400 + i] = static_cast<std::uint8_t>(i + 1);
    if (!ps2::sif::g_handlers.at(0x501)(0x11, 0x400, 32, 0x180, 4))
        return 26;
    if (word(0x180) != 0 || kernel::g_effect_calls.size() != 2
        || kernel::g_effect_cores != std::vector<std::uint32_t>({0, 1}))
        return 27;
    for (unsigned core = 0; core < 2; ++core)
        for (unsigned i = 0; i < 16; ++i)
            if (kernel::g_effect_calls[core][i] != core * 16 + i + 1)
                return 28;

    // 0x501 command buffers and stream-state RPCs now have explicit
    // input length requirements and must not accept truncated packets.
    if (!ps2::sif::g_handlers.at(0x501)(1, 0x400, 0x40, 0x180, 4))
        return 29;
    if (word(0x180) != 0x80000064u) return 30;
    if (!ps2::sif::g_handlers.at(0x501)(2, 0x400, 8, 0x180, 4)
        || word(0x180) != 0) return 31;
    if (!ps2::sif::g_handlers.at(0x501)(0x81, 0, 0, 0x180, 4)
        || word(0x180) != 0) return 32;
    if (!ps2::sif::g_handlers.at(0x501)(0x82, 0, 0, 0x180, 8))
        return 33;

    const std::uint16_t channel = 2;
    std::memcpy(ps2::g_ram + 0x400, &channel, 2);
    if (!ps2::sif::g_handlers.at(0x502)(1, 0x400, 2, 0x180, 4)
        || word(0x180) != 0) return 34;
    if (!ps2::sif::g_handlers.at(0x502)(5, 0x400, 2, 0x180, 4)
        || word(0x180) != 0x80000064u) return 35;

    for (unsigned i = 0; i < 0x33C; ++i)
        ps2::g_ram[0x800 + i] = static_cast<std::uint8_t>(i);
    if (!ps2::sif::g_handlers.at(0x501)(3, 0x800, 0x33C, 0x180, 4)
        || word(0x180) != 0) return 36;
    for (unsigned i = 0; i < 0x2E0; ++i)
        ps2::g_ram[0x800 + i] = static_cast<std::uint8_t>(i);
    if (!ps2::sif::g_handlers.at(0x501)(1, 0x800, 0x2E0, 0x180, 4)
        || word(0x180) != 0) return 37;
    for (unsigned i = 0; i < 0x17C; ++i)
        ps2::g_ram[0x800 + i] = static_cast<std::uint8_t>(i);
    if (!ps2::sif::g_handlers.at(0x502)(0x101, 0x800, 0x17C, 0x180, 4)
        || word(0x180) != 0) return 38;

    if (!ps2::sif::g_handlers.at(0x503)(1, 0, 0, 0x180, 4)
        || word(0x180) != 1) return 39;
    // After an ordinary file transfer finishes, the CDVD 0x101 worker
    // may be primed without supplying an asset name.
    std::memset(ps2::g_ram + 0x400, 0, 0x24);
    if (!ps2::sif::g_handlers.at(0x503)(0x101, 0x400, 0x24, 0x180, 4)
        || word(0x180) != 0) return 40;
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x10000000u) return 41;
    kernel::g_read_job(kernel::g_queued_read);
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x30000000u) return 42;

    // IRX's voice worker mutes both registers for flagged slots.
    // On voice 0, libsd #5 parameters 0x000 and 0x100 must be zero.
    const std::uint32_t voice_stop_mask[2] = {1u, 0u};
    std::memcpy(ps2::g_ram + 0x400, voice_stop_mask, sizeof(voice_stop_mask));
    if (!ps2::sif::g_handlers.at(0x501)(2, 0x400, 8, 0x180, 4)
        || word(0x180) != 0) return 50;
    if (!kernel::g_voice_job
        || kernel::g_queued_voice.kind != kernel::kJobArkdVoice) return 51;
    kernel::g_voice_job(kernel::g_queued_voice);
    bool left_muted = false, right_muted = false;
    for (const auto &[reg, value] : kernel::g_voice_writes)
    {
        if (reg == 0 && value == 0) left_muted = true;
        if (reg == 0x100 && value == 0) right_muted = true;
    }
    if (!left_muted || !right_muted) return 52;

    // The IRX splits command masks at 32 voices, but chooses SPU2
    // register IDs using 24 voices per core. Exercise both boundaries.
    kernel::g_voice_writes.clear();
    const std::uint32_t boundary_masks[2] = {1u << 24, 1u << 0};
    std::memcpy(ps2::g_ram + 0x400, boundary_masks, sizeof(boundary_masks));
    if (!ps2::sif::g_handlers.at(0x501)(2, 0x400, 8, 0x180, 4)
        || word(0x180) != 0) return 53;
    kernel::g_voice_job(kernel::g_queued_voice);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> expected_writes
    {
        {0x001u, 0}, {0x101u, 0}, // index 24: core 1, voice 0
        {0x011u, 0}, {0x111u, 0}  // index 32: core 1, voice 8
    };
    if (kernel::g_voice_writes != expected_writes) return 54;

    // Save an unfinished native DMA, cancel it, then restore. The
    // in-flight job must still match the preserved request serial.
    std::memset(ps2::g_ram + 0x100, 0, 0x24);
    std::memcpy(ps2::g_ram + 0x100, &target, 4);
    std::memcpy(ps2::g_ram + 0x104, "ply\\small.bin", 14);
    if (!ps2::sif::g_handlers.at(0x503)(0x102, 0x100, 0x24, 0x180, 4)
        || word(0x180) != 0) return 43;
    const auto saved_job = kernel::g_queued_read;
    ps2::state::Writer writer;
    writer.set_version(7);
    arkd::state(writer);
    if (!ps2::sif::g_handlers.at(0x503)(0x81, 0, 0, 0x180, 4))
        return 44;
    kernel::g_read_job(saved_job); // Canceled transfer should not run.
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x40000065u) return 45;

    ps2::state::Reader reader(writer.out.data(), writer.out.size());
    reader.set_version(7);
    arkd::state(reader);
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x10000000u) return 46;
    kernel::g_read_job(saved_job);
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x30000000u) return 47;

    // Reject restoring a native savestate with the original driver
    // selected; mixing backends would discard the queued native work.
#ifdef _WIN32
    _putenv_s("SDBZ_ARKD_NATIVE", "");
#else
    unsetenv("SDBZ_ARKD_NATIVE");
#endif
    bool rejected = false;
    try
    {
        ps2::state::Reader mismatched(writer.out.data(), writer.out.size());
        mismatched.set_version(7);
        arkd::state(mismatched);
    }
    catch (const std::runtime_error &)
    {
        rejected = true;
    }
    if (!rejected) return 48;

#ifdef _WIN32
    _putenv_s("SDBZ_ARKD_NATIVE", "1");
#else
    setenv("SDBZ_ARKD_NATIVE", "1", 1);
#endif

    // Legacy v6 state files contain no native arkd section.
    const std::uint8_t empty_state = 0;
    ps2::state::Reader old_state(&empty_state, 0);
    old_state.set_version(6);
    arkd::state(old_state);
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x30000000u) return 49;
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
