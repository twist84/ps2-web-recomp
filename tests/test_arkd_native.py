#!/usr/bin/env python3
"""Native ARKD HLE catalog and RPC tests using synthetic loose GAME/ files."""

import os
import pathlib
import shlex
import struct
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

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
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
JobHandler g_bank_job = nullptr;
JobHandler g_audio_job = nullptr;
JobHandler g_upload_job = nullptr;
Job g_queued_read;
Job g_queued_voice;
Job g_queued_bank;
Job g_queued_audio;
Job g_queued_upload;

struct s_capture
{
    std::uint32_t channel = 0;
    std::uint32_t mode = 0;
    std::uint32_t source = 0;
    std::uint32_t destination = 0;
    std::uint32_t size = 0;
    std::vector<std::uint8_t> contents;
};
std::vector<s_capture> g_captures;
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_voice_writes;
std::vector<std::vector<std::uint8_t>> g_effect_calls;
std::vector<std::uint32_t> g_effect_cores;
bool library_exists(const std::string &name) { return name == "libsd"; }
std::uint32_t alloc(std::uint32_t, int, std::uint32_t) { return 0x1000; }
void free(std::uint32_t) {}
bool call_library_export(const std::string &name, std::uint32_t ordinal,
    std::uint32_t a0, std::uint32_t a1, std::uint32_t a2,
    std::uint32_t a3, std::uint32_t &result, std::optional<std::uint32_t> a4)
{
    if (name != "libsd") return false;
    if (ordinal == 17)
    {
        if (!a4 || *a4 > 0x8000 || a2 != 0x1000
            || a0 > 1 || a1 != 0) return false;
        s_capture capture;
        capture.channel = a0;
        capture.mode = a1;
        capture.source = a2;
        capture.destination = a3;
        capture.size = *a4;
        const auto *iop_bytes = ps2::iop::ram_ptr(a2);
        capture.contents.assign(iop_bytes, iop_bytes + *a4);
        g_captures.push_back(std::move(capture));
        result = *a4;
        return true;
    }
    if (ordinal == 19)
    {
        result = 1;
        return true;
    }
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
    else if (kind == kJobArkdBank) g_bank_job = callback;
    else if (kind == kJobArkdAudio) g_audio_job = callback;
    else if (kind == kJobArkdSpuUpload) g_upload_job = callback;
}
std::uint64_t now() { return 0; }
std::uint64_t usec_to_cycles(std::uint64_t us) { return us; }
void schedule(std::uint64_t, const Job &job)
{
    if (job.kind == kJobArkdVoice) g_queued_voice = job;
    else if (job.kind == kJobArkdBank) g_queued_bank = job;
    else if (job.kind == kJobArkdAudio) g_queued_audio = job;
    else if (job.kind == kJobArkdSpuUpload) g_queued_upload = job;
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
    if ((argc != 2 && argc != 3) || !ps2::disc::open(argv[1]))
        return 1;

    // The native catalog operates on the synthetic DVD, with no IRX bytes.
    arkd::c_catalog catalog;
    if (!catalog.open()) return 2;

    if (argc == 3)
    {
        const auto expected = static_cast<std::size_t>(
            std::strtoul(argv[2], nullptr, 10));
        if (catalog.entry_count() != expected
            || ps2::disc::find("INFO.DAT")
            || ps2::disc::find("GAME.DAT"))
            return 100;

        const auto asset = catalog.find("MOD/A2099.BIN");
        if (!asset || asset->file_size != 1) return 101;
        std::uint8_t value = 0;
        if (catalog.read(*asset, 0, &value, 1) != 1
            || value != 0x5A) return 102;
        const auto direct = ps2::disc::loose_game_assets();
        if (direct.size() != expected) return 103;
        // Native savestates store these IDs; they must be assigned in
        // deterministic name order, not filesystem enumeration order.
        if (direct.front().name != "mod\\a0000.bin"
            || direct.back().name != "mod\\a2099.bin"
            || asset->loose_id != expected - 1)
            return 113;
        return 0;
    }

    if (catalog.entry_count() != 5) return 2;
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

    // Reset the synthetic test's earlier voice-command bytes. Their
    // pseudo-random flags otherwise request unrelated sound writes.
    if (!ps2::sif::g_handlers.at(0x500)(1, 0, 0, 0x180, 4)
        || word(0x180) != 0) return 55;

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

    // 0x503/0x103 accepts CDVD worker commands 1..8 after a
    // terminal state, reports busy while queued, then completes.
    const std::uint32_t first_drive_command = 1;
    std::memcpy(ps2::g_ram + 0x400, &first_drive_command, 4);
    if (!ps2::sif::g_handlers.at(0x503)(0x103, 0x400, 4, 0x180, 4)
        || word(0x180) != 0) return 56;
    if (!ps2::sif::g_handlers.at(0x503)(0x103, 0x400, 4, 0x180, 4)
        || word(0x180) != 0xFFFFFFFFu) return 57;
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x10000000u) return 58;
    kernel::g_read_job(kernel::g_queued_read);
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x30000000u) return 59;

    const std::uint32_t last_drive_command = 8;
    std::memcpy(ps2::g_ram + 0x400, &last_drive_command, 4);
    if (!ps2::sif::g_handlers.at(0x503)(0x103, 0x400, 4, 0x180, 4)
        || word(0x180) != 0) return 60;
    kernel::g_read_job(kernel::g_queued_read);
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x30000000u) return 61;

    const std::uint32_t invalid_drive_command = 9;
    std::memcpy(ps2::g_ram + 0x400, &invalid_drive_command, 4);
    if (!ps2::sif::g_handlers.at(0x503)(0x103, 0x400, 4, 0x180, 4)
        || word(0x180) != 0x80000064u) return 62;

    // 0x501/0x101 accepts a 36-byte named audio bank and stages the
    // bytes asynchronously. It does not yet upload the bank to SPU2.
    std::memset(ps2::g_ram + 0x100, 0, 0x24);
    const std::uint32_t bank_id = 7;
    std::memcpy(ps2::g_ram + 0x100, &bank_id, 4);
    std::memcpy(ps2::g_ram + 0x104, "ply\\small.bin", 14);
    if (!ps2::sif::g_handlers.at(0x501)(0x101, 0x100, 0x24, 0x180, 4)
        || word(0x180) != 0) return 63;
    if (arkd::sound_bank_ready() || !kernel::g_bank_job
        || kernel::g_queued_bank.kind != kernel::kJobArkdBank)
        return 64;
    if (!ps2::sif::g_handlers.at(0x501)(0x101, 0x100, 0x24, 0x180, 4)
        || word(0x180) != 0x80000065u) return 65;
    // The bank reader and both control opcodes wake one shared worker.
    const std::uint32_t blocked_selector = 1;
    std::memcpy(ps2::g_ram + 0x400, &blocked_selector, 4);
    if (!ps2::sif::g_handlers.at(0x501)(0x102, 0x400, 4, 0x180, 4)
        || word(0x180) != 0x80000065u) return 81;
    if (!ps2::sif::g_handlers.at(0x501)(0x103, 0x400, 4, 0x180, 4)
        || word(0x180) != 0x80000065u) return 82;

    // Native v8 state retains pending bank request and job serial.
    const auto pending_bank_job = kernel::g_queued_bank;
    ps2::state::Writer bank_writer;
    bank_writer.set_version(8);
    arkd::state(bank_writer);
    kernel::g_bank_job(pending_bank_job);
    if (!arkd::sound_bank_ready()) return 66;

    // Reload the pending state; completion must remain deterministic.
    ps2::state::Reader bank_reader(
        bank_writer.out.data(), bank_writer.out.size());
    bank_reader.set_version(8);
    arkd::state(bank_reader);
    if (arkd::sound_bank_ready()) return 67;
    kernel::g_bank_job(pending_bank_job);
    if (!arkd::sound_bank_ready() || arkd::sound_bank_id() != bank_id)
        return 68;
    std::vector<std::uint8_t> bank(5000);
    if (arkd::copy_sound_bank(bank.data(), bank.size()) != 5000)
        return 69;
    for (const std::uint8_t value : bank)
        if (value != 'S') return 70;

    // Test a completed-bank round trip as well.
    ps2::state::Writer loaded_writer;
    loaded_writer.set_version(8);
    arkd::state(loaded_writer);
    ps2::state::Reader loaded_reader(
        loaded_writer.out.data(), loaded_writer.out.size());
    loaded_reader.set_version(8);
    arkd::state(loaded_reader);
    if (!arkd::sound_bank_ready()) return 71;

    // IRX 0x501/0x102 and /0x103 queue a single word selector for
    // the audio worker. Until the SPU2 stage is ported, completion
    // must report an explicit failure, never false success.
    const std::uint32_t selector = 0x12345678;
    std::memcpy(ps2::g_ram + 0x400, &selector, 4);
    if (!ps2::sif::g_handlers.at(0x501)(0x102, 0x400, 4, 0x180, 4)
        || word(0x180) != 0) return 72;
    if (!kernel::g_audio_job || kernel::g_queued_audio.kind != kernel::kJobArkdAudio
        || arkd::audio_worker_status() != 0x10000000u) return 73;
    if (!ps2::sif::g_handlers.at(0x501)(0x103, 0x400, 4, 0x180, 4)
        || word(0x180) != 0x80000065u) return 74;
    if (!ps2::sif::g_handlers.at(0x501)(0x101, 0x100, 0x24, 0x180, 4)
        || word(0x180) != 0x80000065u) return 83;

    const auto pending_audio_job = kernel::g_queued_audio;
    ps2::state::Writer audio_writer;
    audio_writer.set_version(9);
    arkd::state(audio_writer);
    kernel::g_audio_job(pending_audio_job);
    if (arkd::audio_worker_status() != 0x4000012Cu) return 75;

    ps2::state::Reader audio_reader(audio_writer.out.data(), audio_writer.out.size());
    audio_reader.set_version(9);
    arkd::state(audio_reader);
    if (arkd::audio_worker_status() != 0x10000000u) return 76;
    kernel::g_audio_job(pending_audio_job);
    if (arkd::audio_worker_status() != 0x4000012Cu) return 77;
    if (!ps2::sif::g_handlers.at(0x501)(0x103, 0x400, 4, 0x180, 4)
        || word(0x180) != 0) return 78;
    kernel::g_audio_job(kernel::g_queued_audio);
    if (arkd::audio_worker_status() != 0x40000064u) return 79;

    // Loading an older version discards the new worker state.
    ps2::state::Reader legacy_audio(loaded_writer.out.data(),
        loaded_writer.out.size());
    legacy_audio.set_version(8);
    arkd::state(legacy_audio);
    if (arkd::audio_worker_status() != 0x30000000u) return 80;

    // The bank's first sector is a metadata table, not raw sample
    // bytes. Verify sizes and reject malformed metadata before future
    // SPU2 upload code consumes these fields.
    std::array<std::uint8_t, 2048> header{};
    const std::uint32_t two_entries = 2;
    const std::uint32_t spu_span = 0x18000;
    std::memcpy(header.data() + 4, &two_entries, 4);
    std::memcpy(header.data() + 8, &spu_span, 4);
    const std::uint32_t first_parameter = 0x1111;
    const std::uint32_t first_offset = 0;
    const std::uint32_t second_parameter = 0x2222;
    const std::uint32_t second_offset = 0x10000;
    std::memcpy(header.data() + 0x0C, &first_parameter, 4);
    std::memcpy(header.data() + 0x10, &first_offset, 4);
    std::memcpy(header.data() + 0x14, &second_parameter, 4);
    std::memcpy(header.data() + 0x18, &second_offset, 4);
    auto info = arkd::inspect_sound_bank(header.data(), header.size());
    if (!info || info->entry_count != 2 ||
        info->spu_memory_bytes != spu_span
        || info->entries.size() != 2
        || info->entries[0].parameter != first_parameter
        || info->entries[0].spu_offset != first_offset
        || info->entries[1].parameter != second_parameter
        || info->entries[1].spu_offset != second_offset) return 84;
    if (arkd::inspect_sound_bank(header.data(), 11)) return 85;
    const std::uint32_t excessive_entries = 255;
    std::memcpy(header.data() + 4, &excessive_entries, 4);
    if (arkd::inspect_sound_bank(header.data(), header.size())) return 86;
    const std::uint32_t zero = 0;
    std::memcpy(header.data() + 4, &two_entries, 4);
    std::memcpy(header.data() + 8, &zero, 4);
    if (arkd::inspect_sound_bank(header.data(), header.size())) return 87;
    const std::uint32_t excessive_spu_span = 0x200001;
    std::memcpy(header.data() + 8, &excessive_spu_span, 4);
    if (arkd::inspect_sound_bank(header.data(), header.size())) return 88;
    std::memcpy(header.data() + 8, &spu_span, 4);
    const std::uint32_t invalid_offset = spu_span;
    std::memcpy(header.data() + 0x18, &invalid_offset, 4);
    if (arkd::inspect_sound_bank(header.data(), header.size())) return 89;

    // A well-formed audio bank now performs real libsd #17 uploads from
    // persistent IOP staging RAM, with #19 completion between chunks.
    std::memset(ps2::g_ram + 0x100, 0, 0x24);
    const std::uint32_t upload_bank_id = 1;
    std::memcpy(ps2::g_ram + 0x100, &upload_bank_id, 4);
    std::memcpy(ps2::g_ram + 0x104, "snd\\test.bnk", 13);
    if (!ps2::sif::g_handlers.at(0x501)(0x101, 0x100, 0x24, 0x180, 4)
        || word(0x180) != 0) return 90;
    kernel::g_bank_job(kernel::g_queued_bank);
    if (!arkd::sound_bank_ready()
        || arkd::sound_upload_status() != 0x10000000u
        || !kernel::g_upload_job
        || kernel::g_queued_upload.kind != kernel::kJobArkdSpuUpload)
        return 91;
    if (!ps2::sif::g_handlers.at(0x501)(0x102, 0x400, 4, 0x180, 4)
        || word(0x180) != 0x80000065u) return 92;

    // Execute first upload, then serialize during in-flight DMA.
    kernel::g_upload_job(kernel::g_queued_upload);
    if (arkd::sound_upload_status() != 0x20000000u
        || kernel::g_captures.size() != 1) return 93;

    ps2::state::Writer upload_writer;
    upload_writer.set_version(10);
    arkd::state(upload_writer);
    ps2::state::Reader upload_reader(
        upload_writer.out.data(), upload_writer.out.size());
    upload_reader.set_version(10);
    arkd::state(upload_reader);
    if (arkd::sound_upload_status() != 0x20000000u) return 94;
    for (unsigned step = 0; step < 8 &&
         arkd::sound_upload_status() != 0x30000000u; ++step)
    {
        kernel::g_upload_job(kernel::g_queued_upload);
    }
    if (arkd::sound_upload_status() != 0x30000000u
        || kernel::g_captures.size() != 2) return 95;

    const auto &first_upload = kernel::g_captures[0];
    const auto &second_upload = kernel::g_captures[1];
    if (first_upload.channel != 0 || second_upload.channel != 1
        || first_upload.mode != 0 || second_upload.mode != 0
        || first_upload.destination != 0x5050
        || second_upload.destination != 0xD050
        || first_upload.size != 0x8000
        || second_upload.size != 0x40) return 96;

    for (std::uint32_t index = 0; index < 0x8000; ++index)
        if (first_upload.contents[index] != (index & 0xFFu)) return 97;
    for (std::uint32_t index = 0; index < 0x40; ++index)
        if (second_upload.contents[index] != (index & 0xFFu)) return 98;

    // The original 0x501/0x102 command releases a selected SPU bank.
    // Native mode retains other banks at stable addresses and can then
    // reuse the freed interval on a later allocation.
    const auto bank_address = arkd::sound_bank_spu_address(upload_bank_id);
    if (!bank_address || *bank_address != 0x5050) return 104;
    std::memcpy(ps2::g_ram + 0x400, &upload_bank_id, 4);
    if (!ps2::sif::g_handlers.at(0x501)(0x102, 0x400, 4, 0x180, 4)
        || word(0x180) != 0) return 105;
    kernel::g_audio_job(kernel::g_queued_audio);
    if (arkd::audio_worker_status() != 0x30000000u
        || arkd::sound_bank_spu_address(upload_bank_id))
        return 106;

    if (!ps2::sif::g_handlers.at(0x501)(0x101, 0x100, 0x24, 0x180, 4)
        || word(0x180) != 0) return 107;
    kernel::g_bank_job(kernel::g_queued_bank);
    const auto reused = arkd::sound_bank_spu_address(upload_bank_id);
    if (!reused || *reused != *bank_address) return 108;
    for (unsigned step = 0; step < 8 &&
         arkd::sound_upload_status() != 0x30000000u; ++step)
        kernel::g_upload_job(kernel::g_queued_upload);
    if (arkd::sound_upload_status() != 0x30000000u) return 109;

    // The original 0x501/0x103 worker reuses the last cached IOP
    // bank payload, releases the current SPU allocation and transfers
    // it again. Confirm it issues libsd DMA rather than a false no-op.
    kernel::g_captures.clear();
    if (!ps2::sif::g_handlers.at(0x501)(0x103, 0x400, 4, 0x180, 4)
        || word(0x180) != 0) return 110;
    kernel::g_audio_job(kernel::g_queued_audio);
    if (arkd::audio_worker_status() != 0x10000000u
        || arkd::sound_upload_status() != 0x10000000u)
        return 111;
    for (unsigned step = 0; step < 8 &&
        arkd::sound_upload_status() != 0x30000000u; ++step)
        kernel::g_upload_job(kernel::g_queued_upload);
    if (arkd::audio_worker_status() != 0x30000000u
        || arkd::sound_upload_status() != 0x30000000u
        || kernel::g_captures.size() != 2)
        return 112;

    // Growing a live bank must not silently relocate it: voice start
    // addresses may still refer to the old allocation.
    const auto stable_address = arkd::sound_bank_spu_address(upload_bank_id);
    if (!stable_address) return 113;
    std::memset(ps2::g_ram + 0x100, 0, 0x24);
    std::memcpy(ps2::g_ram + 0x100, &upload_bank_id, 4);
    std::memcpy(ps2::g_ram + 0x104, "snd\\big.bnk", 12);
    if (!ps2::sif::g_handlers.at(0x501)(0x101, 0x100, 0x24, 0x180, 4)
        || word(0x180) != 0) return 114;
    kernel::g_bank_job(kernel::g_queued_bank);
    if (arkd::sound_upload_status() != 0x40000064u
        || arkd::sound_bank_spu_address(upload_bank_id) != stable_address)
        return 115;
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
        bank = bytearray(0x800 + 0x8040)
        struct.pack_into("<IIIIII", bank, 4,
                         2, 0x9000, 0x1111, 0, 0x2222, 0x100)
        for index in range(0x8040):
            bank[0x800 + index] = index & 0xFF
        big_bank = bytearray(0x800 + 0x10040)
        struct.pack_into("<IIIIII", big_bank, 4,
                         2, 0x11000, 0x1111, 0, 0x2222, 0x100)
        for index in range(0x10040):
            big_bank[0x800 + index] = index & 0xFF
        for name, content in {
            "dis/a_first.bin": b"A" * 13,
            "ply/small.bin": b"S" * 5000,
            "snd/test.bnk": bytes(bank),
            "snd/big.bnk": bytes(big_bank),
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

    def test_direct_catalog_beyond_legacy_info_limit(self):
        root = self.root / "native_direct"
        assets = root / "GAME" / "mod"
        assets.mkdir(parents=True, exist_ok=True)
        for index in range(2100):
            (assets / f"a{index:04d}.bin").write_bytes(b"Z")
        environment = os.environ.copy()
        environment["SDBZ_ARKD_NATIVE"] = "1"
        subprocess.run([str(self.executable), str(root), "2100"],
                       env=environment, check=True)
        self.assertFalse((root / "INFO.DAT").exists())
        self.assertFalse((root / "GAME.DAT").exists())


if __name__ == "__main__":
    unittest.main()
