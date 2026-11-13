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
#include "iop/spu2.hpp"
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

namespace ps2::iop::spu2 {
std::array<std::uint16_t, 0x100000> g_test_spu_ram{};
std::uint16_t *ram() { return g_test_spu_ram.data(); }
}

namespace ps2::iop::kernel {
HleModuleStart g_start;
JobHandler g_read_job = nullptr;
JobHandler g_voice_job = nullptr;
JobHandler g_bank_job = nullptr;
JobHandler g_audio_job = nullptr;
JobHandler g_upload_job = nullptr;
JobHandler g_stream_job = nullptr;
Job g_queued_read;
Job g_queued_voice;
Job g_queued_bank;
Job g_queued_audio;
Job g_queued_upload;
Job g_queued_stream;
std::array<std::uint32_t, 2> g_dma_done{1, 1};

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
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_address_writes;
std::map<std::uint32_t, std::uint32_t> g_loop_addresses;
std::array<std::uint32_t, 2> g_end_bits{};
unsigned g_sound_init_calls = 0;
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_core_attributes;
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_voice_writes;
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_switch_writes;
std::vector<std::vector<std::uint8_t>> g_effect_calls;
std::vector<std::uint32_t> g_effect_cores;
std::map<int, Module> g_modules;
const std::map<int, Module> &modules() { return g_modules; }
bool library_exists(const std::string &name) { return name == "libsd"; }
std::uint32_t alloc(std::uint32_t size, int, std::uint32_t) { return size >= 0x10000 ? 0x10000 : 0x1000; }
void free(std::uint32_t) {}
bool call_library_export(const std::string &name, std::uint32_t ordinal,
    std::uint32_t a0, std::uint32_t a1, std::uint32_t a2,
    std::uint32_t a3, std::uint32_t &result, std::optional<std::uint32_t> a4)
{
    if (name != "libsd") return false;
    if (ordinal == 4 || ordinal == 11)
    {
        if (ordinal == 4) ++g_sound_init_calls;
        else g_core_attributes.emplace_back(a0, a1);
        result = 0;
        return true;
    }
    if (ordinal == 9)
    {
        g_address_writes.emplace_back(a0, a1);
        g_loop_addresses[a0] = a1;
        result = 0;
        return true;
    }
    if (ordinal == 8)
    {
        result = ((a0 & ~1u) == 0x1900u || (a0 & ~1u) == 0x1B00u)
            ? 0x800000u : ((a0 & ~1u) == 0x1700u ? g_end_bits[a0 & 1u] : 0u);
        return true;
    }
    if (ordinal == 10)
    {
        result = g_loop_addresses[a0];
        return true;
    }
    if (ordinal == 17)
    {
        if (!a4 || *a4 > 0x8000 || (a2 != 0x1000 && a2 != 0x10000 && a2 != 0x18000)
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
        result = g_dma_done[a0 & 1u];
        return true;
    }
    if (ordinal == 7)
    {
        g_switch_writes.emplace_back(a0, a1);
        result = 0;
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
    g_effect_calls.emplace_back(bytes, bytes + 20);
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
    else if (kind == kJobArkdStream) g_stream_job = callback;
}
std::uint64_t now() { return 0; }
std::uint64_t usec_to_cycles(std::uint64_t us) { return us; }
void schedule(std::uint64_t, const Job &job)
{
    if (job.kind == kJobArkdVoice) g_queued_voice = job;
    else if (job.kind == kJobArkdBank) g_queued_bank = job;
    else if (job.kind == kJobArkdAudio) g_queued_audio = job;
    else if (job.kind == kJobArkdSpuUpload) g_queued_upload = job;
    else if (job.kind == kJobArkdStream) g_queued_stream = job;
    else g_queued_read = job;
}
}

namespace ps2::sif {
std::map<std::uint32_t, HleServer> g_handlers;
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_ee_sregs;
void publish_ee_sreg(std::uint32_t index, std::uint32_t value)
{
    g_ee_sregs.emplace_back(index, value);
}
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

int test_streams()
{
    using namespace ps2::iop;
    arkd::install();
    kernel::g_start();
    auto status = [&]() {
        ps2::sif::g_handlers.at(0x502)(4, 0, 0, 0x180, 4);
        return word(0x180);
    };
    auto step = [&]() { kernel::g_stream_job(kernel::g_queued_stream); };
    auto streams = [&]() {
        std::vector<kernel::s_capture> result;
        for (const auto &capture : kernel::g_captures)
            if (capture.destination >= 0x180000) result.push_back(capture);
        return result;
    };
    std::array<std::uint32_t, 0x17C / 4> packet{};
    packet[1] = 1; // One playlist segment.
    packet[4] = 0x3FFF;
    packet[5] = 1; // Play the segment once.
    std::memcpy(reinterpret_cast<std::uint8_t *>(packet.data()) + 24, "sqm/test.ams", 13);
    const auto submit = [&]() {
        std::memcpy(ps2::g_ram + 0x800, packet.data(), sizeof(packet));
        ps2::sif::g_handlers.at(0x502)(0x101, 0x800, sizeof(packet), 0x180, 4);
    };
    kernel::g_loop_addresses[0x226F] = 0x180010;
    kernel::g_dma_done[0] = 0;
    submit();
    if (!kernel::g_stream_job || status() != 0x20000000u) return 180;
    step(); // First channel is now in flight.
    if (streams().size() != 1 || streams()[0].contents[2] != 0x11) return 181;
    const auto pending = kernel::g_queued_stream;
    ps2::state::Writer writer;
    writer.set_version(16);
    arkd::state(writer);
    const std::uint32_t immediate = 0;
    std::memcpy(ps2::g_ram + 0x400, &immediate, 4);
    ps2::sif::g_handlers.at(0x502)(0x102, 0x400, 4, 0x180, 4);
    step();
    if (streams().size() != 1 || ps2::iop::g_ram[0x10002] != 0x11) return 182;
    ps2::state::Reader reader(writer.out.data(), writer.out.size());
    reader.set_version(16);
    arkd::state(reader);
    kernel::g_stream_job(pending);
    if (streams().size() != 2 || streams()[1].destination != 0x190000)
        return 201; // Right DMA starts while the left channel is still busy.
    kernel::g_dma_done[0] = 1;
    for (unsigned n = 0; n < 12 && status() != 0x30000000u; ++n) step();
    auto uploaded = streams();
    if (uploaded.size() != 4 || status() != 0x30000000u
        || uploaded[0].destination != 0x180000 || uploaded[1].destination != 0x190000
        || uploaded[2].destination != 0x188000 || uploaded[3].destination != 0x198000
        || uploaded[2].contents[2] != 0x21 || uploaded[3].contents[2] != 0x22)
        return 183;

    // Right after key-on, libsd may still report the previous NAX. It
    // must not tear down the newly started stream before SPU2 advances.
    kernel::g_loop_addresses[0x226F] = 0;
    step();
    if (status() != 0x30000000u || streams().size() != 4) return 196;
    kernel::g_loop_addresses[0x226F] = 0x180010;
    step();
    if (status() != 0x30000000u) return 197;

    // Pause freezes the hardware pitch, retaining both prepared halves.
    ps2::sif::g_handlers.at(0x502)(0x103, 0x400, 4, 0x180, 4);
    step();
    if (ps2::sif::g_ee_sregs.back()
        != std::pair<std::uint32_t, std::uint32_t>{13, 3}) return 184;
    const std::uint32_t resume = 1;
    std::memcpy(ps2::g_ram + 0x400, &resume, 4);
    ps2::sif::g_handlers.at(0x502)(0x103, 0x400, 4, 0x180, 4);
    step();
    if (streams().size() != 4 || ps2::sif::g_ee_sregs.back().second != 1) return 185;

    // Cross to half 1; the freed half must use the folded third block.
    kernel::g_loop_addresses[0x226F] = 0x188010;
    kernel::g_dma_done[0] = 0;
    step();
    if (streams().size() != 5 || streams().back().contents[2] != 0x31) return 186;
    std::memset(ps2::g_ram + 0x400, 0, 0x24);
    const std::uint32_t bank_id = 7;
    std::memcpy(ps2::g_ram + 0x400, &bank_id, 4);
    std::memcpy(ps2::g_ram + 0x404, "snd/test.bnk", 13);
    ps2::sif::g_handlers.at(0x501)(0x101, 0x400, 0x24, 0x180, 4);
    kernel::g_bank_job(kernel::g_queued_bank);
    const auto before_bank = kernel::g_captures.size();
    kernel::g_upload_job(kernel::g_queued_upload);
    if (kernel::g_captures.size() != before_bank) return 187;
    kernel::g_dma_done[0] = 1;
    for (unsigned n = 0; n < 8; ++n)
    {
        step();
        kernel::g_upload_job(kernel::g_queued_upload);
    }
    if (streams().size() != 6 || arkd::sound_upload_status() != 0x30000000u) return 188;

    // Fourth logical block is the second physical half of block 0.
    kernel::g_loop_addresses[0x226F] = 0x180010;
    for (unsigned n = 0; n < 4; ++n) step();
    uploaded = streams();
    if (uploaded.size() != 8 || uploaded[6].contents[2] != 0x41
        || uploaded[7].contents[2] != 0x42
        || (uploaded[6].contents[0xF1] & 3) != 3 || uploaded[6].contents[0x100] != 0)
        return 189;
    kernel::g_loop_addresses[0x226F] = 0x188010;
    for (unsigned n = 0; n < 4; ++n) step();
    kernel::g_loop_addresses[0x226F] = 0x180010;
    step();
    if (status() != 0x30000000u || kernel::g_switch_writes.back()
        != std::pair<std::uint32_t, std::uint32_t>{0x1601, 0xC00000}) return 190;

    // A looping playlist reopens the selected segment instead of ending.
    packet[2] = 1;
    submit();
    for (unsigned n = 0; n < 8; ++n) step();
    for (unsigned half : {1u, 0u, 1u})
    {
        kernel::g_loop_addresses[0x226F] = 0x180010 + half * 0x8000;
        for (unsigned n = 0; n < 4; ++n) step();
    }
    if (streams().back().contents[2] != 0x12 || status() != 0x30000000u) return 191;
    const std::uint32_t fade = 2;
    std::memcpy(ps2::g_ram + 0x400, &fade, 4);
    ps2::sif::g_handlers.at(0x502)(0x102, 0x400, 4, 0x180, 4);
    step();
    auto tick = kernel::g_queued_stream;
    tick.b = 1;
    kernel::g_stream_job(tick);
    kernel::g_stream_job(tick);
    step();
    if (status() != 0x30000000u) return 192;
    packet[1] = 11;
    submit();
    step();
    if (status() != 0x40000064u) return 193;
    return 0;
}

int main(int argc, char **argv)
{
    using namespace ps2::iop;
    if (argc == 3 && std::strcmp(argv[1], "--pitch-vectors") == 0)
    {
        const auto base = static_cast<std::uint16_t>(
            std::strtoul(argv[2], nullptr, 10));
        for (std::int32_t request = -32768; request <= 32767; ++request)
            std::printf("%u\n", static_cast<unsigned>(arkd::calculate_voice_pitch(
                base, static_cast<std::int16_t>(request))));
        return 0;
    }
    if ((argc != 2 && argc != 3) || !ps2::disc::open(argv[1]))
        return 1;
    if (argc == 3 && std::strcmp(argv[2], "streams") == 0) return test_streams();

    // The native catalog operates on the synthetic DVD, with no IRX bytes.
    arkd::c_catalog catalog;
    if (!catalog.open()) return 2;

    if (argc == 3 && std::strcmp(argv[2], "mixed") == 0)
    {
        // One original archived file, a loose override of a second
        // archived file, and one brand-new loose asset must coexist.
        if (catalog.entry_count() != 3) return 120;
        const auto original = catalog.find("archived.bin");
        const auto override = catalog.find("override.bin");
        const auto added = catalog.find("new.bin");
        if (!original || !override || !added) return 121;
        std::array<std::uint8_t, 100> bytes{};
        if (catalog.read(*original, 0, bytes.data(), bytes.size()) != 100
            || bytes[0] != 'A') return 122;
        if (catalog.read(*override, 0, bytes.data(), bytes.size()) != 100
            || bytes[0] != 'N') return 123;
        if (catalog.read(*added, 0, bytes.data(), bytes.size()) != 100
            || bytes[0] != 'Z') return 124;
        return 0;
    }

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

    if (catalog.entry_count() != 7) return 2;
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
    if (!arkd::native_requested() || !arkd::intercept_module("cdrom0:\\ARKD_DVD.IRX;1"))
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

    // 0x503/0x101 enqueues a real file read to EE RAM. Method 2 exposes
    // the guest-observable pending/completed phase.
    std::memset(ps2::g_ram + 0x100, 0, 0x24);
    const std::uint32_t target = 0x200;
    std::memcpy(ps2::g_ram + 0x100, &target, 4);
    std::memcpy(ps2::g_ram + 0x104, "ply\\small.bin", 14);
    if (!ps2::sif::g_handlers.at(0x503)(0x101, 0x100, 0x24, 0x180, 4))
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
            if (kernel::g_effect_calls[core][i + 4] != core * 16 + i + 1)
                return 28;
    if (kernel::g_effect_calls[0][0] != 0 || kernel::g_effect_calls[1][0] != 1)
        return 170;

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
    // After an ordinary file transfer finishes, the CDVD 0x102 worker
    // may be primed without supplying an asset name.
    std::memset(ps2::g_ram + 0x400, 0, 0x24);
    const std::uint32_t index_destination = 0x9000;
    std::memcpy(ps2::g_ram + 0x400, &index_destination, 4);
    if (!ps2::sif::g_handlers.at(0x503)(0x102, 0x400, 0x24, 0x180, 4)
        || word(0x180) != 0) return 40;
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x10000000u) return 41;
    kernel::g_read_job(kernel::g_queued_read);
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x30000000u) return 42;
    const auto ee_index = catalog.ee_index();
    if (ee_index.size() != catalog.entry_count() * 48
        || std::memcmp(ee_index.data(), "dis/a_first.bin", 15) != 0
        || std::memcmp(ps2::g_ram + index_destination,
            ee_index.data(), ee_index.size()) != 0) return 154;

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
    bool key_off_voice_zero = false;
    for (const auto &[reg, mask] : kernel::g_switch_writes)
        if (reg == 0x1600u && (mask & 1u))
            key_off_voice_zero = true;
    if (!key_off_voice_zero) return 125;

    // Reset the synthetic test's earlier voice-command bytes. Their
    // pseudo-random flags otherwise request unrelated sound writes.
    if (!ps2::sif::g_handlers.at(0x500)(1, 0, 0, 0x180, 4)
        || word(0x180) != 0) return 55;
    bool core0_master = false, core1_master = false, last_envelope = false;
    for (const auto &[reg, value] : kernel::g_voice_writes)
    {
        if (reg == 0x980 && value == 0x3FFF) core0_master = true;
        if (reg == 0x981 && value == 0x3FFF) core1_master = true;
        if (reg == 0x42F && value == 0x1FC0) last_envelope = true;
    }
    if (!kernel::g_sound_init_calls || !core0_master || !core1_master || !last_envelope
        || kernel::g_core_attributes.back()
            != std::pair<std::uint32_t, std::uint32_t>{3, 1}
        || kernel::g_effect_calls.back().size() != 20
        || kernel::g_effect_calls.back()[0] != 1
        || kernel::g_effect_calls.back()[4] != 5
        || kernel::g_effect_calls.back()[5] != 1) return 172;
    if (kernel::g_captures.size() != 1
        || kernel::g_captures[0].destination != 0x5010
        || kernel::g_captures[0].size != 64) return 194;
    for (std::size_t i = 0; i < 64; ++i)
        if (kernel::g_captures[0].contents[i]
            != (i == 1 ? 1 : (i == 17 || i == 33 || i == 49 ? 7 : 0)))
            return 195;
    kernel::g_captures.clear();

    // The IRX splits command masks at 32 voices, but chooses SPU2
    // register IDs using 24 voices per core. Exercise both boundaries.
    kernel::g_voice_writes.clear();
    kernel::g_switch_writes.clear();
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
    if (kernel::g_switch_writes.size() != 1
        || kernel::g_switch_writes[0]
            != std::pair<std::uint32_t, std::uint32_t>(
                0x1601u, (1u << 0) | (1u << 8)))
        return 126;

    // Save an unfinished native DMA, cancel it, then restore. The
    // in-flight job must still match the preserved request serial.
    std::memset(ps2::g_ram + 0x100, 0, 0x24);
    std::memcpy(ps2::g_ram + 0x100, &target, 4);
    std::memcpy(ps2::g_ram + 0x104, "ply\\small.bin", 14);
    if (!ps2::sif::g_handlers.at(0x503)(0x101, 0x100, 0x24, 0x180, 4)
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
    auto *silent_spu = reinterpret_cast<std::uint8_t *>(ps2::iop::spu2::ram()) + 0x5010;
    std::memset(silent_spu, 0x5A, 64);
    arkd::state(reader);
    for (std::size_t i = 0; i < 64; ++i)
        if (silent_spu[i]
            != (i == 1 ? 1 : (i == 17 || i == 33 || i == 49 ? 7 : 0)))
            return 198;
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x10000000u) return 46;
    kernel::g_read_job(saved_job);
    ps2::sif::g_handlers.at(0x503)(2, 0, 0, 0x180, 4);
    if (word(0x180) != 0x30000000u) return 47;

    // Reject restoring a native savestate with the original driver
    // selected; mixing backends would discard the queued native work.
#ifdef _WIN32
    _putenv_s("SDBZ_ARKD_NATIVE", "0");
#else
    setenv("SDBZ_ARKD_NATIVE", "0", 1);
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

    // Current state retains the direct asset ID for a pending bank.
    const auto pending_bank_job = kernel::g_queued_bank;
    ps2::state::Writer bank_writer;
    bank_writer.set_version(15);
    arkd::state(bank_writer);
    kernel::g_bank_job(pending_bank_job);
    if (!arkd::sound_bank_ready()) return 66;

    // Reload the pending state; completion must remain deterministic.
    ps2::state::Reader bank_reader(
        bank_writer.out.data(), bank_writer.out.size());
    bank_reader.set_version(15);
    arkd::state(bank_reader);
    if (arkd::sound_bank_ready()) return 67;
    kernel::g_bank_job(pending_bank_job);
    if (!arkd::sound_bank_ready() || arkd::sound_bank_id() != bank_id)
        return 68;
    std::vector<std::uint8_t> bank(5000);
    if (arkd::copy_sound_bank(bank.data(), bank.size()) != 5000)
        return 69;
    for (std::size_t i = 0; i < bank.size(); ++i)
        if (bank[i] != 'S')
        {
            std::fprintf(stderr, "bank mismatch at %zu: %02x\n", i, bank[i]);
            return 70;
        }

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
    if (ps2::sif::g_ee_sregs.back()
        != std::pair<std::uint32_t, std::uint32_t>{12, 0x10000000u}) return 163;
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
    if (ps2::sif::g_ee_sregs.back()
        != std::pair<std::uint32_t, std::uint32_t>{12, 0x4000012Cu}) return 164;
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

    // A valid header can still request space reserved for streaming.
    // Reject it before any libsd transfer or live-bank relocation.
    std::memset(ps2::g_ram + 0x100, 0, 0x24);
    const std::uint32_t reserved_bank_id = 15;
    std::memcpy(ps2::g_ram + 0x100, &reserved_bank_id, 4);
    std::memcpy(ps2::g_ram + 0x104, "snd/reserved.bnk", 17);
    const auto uploads_before_reserved = kernel::g_captures.size();
    ps2::sif::g_handlers.at(0x501)(0x101, 0x100, 0x24, 0x180, 4);
    kernel::g_bank_job(kernel::g_queued_bank);
    if (arkd::sound_bank_spu_address(reserved_bank_id)
        || kernel::g_captures.size() != uploads_before_reserved
        || ps2::sif::g_ee_sregs.back()
            != std::pair<std::uint32_t, std::uint32_t>{12, 0x40000064u}) return 173;

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
    // The EE waits on the audio-worker result delivered via SIF command
    // 0x80000001, SREG 12. RPC completion alone cannot release that wait.
    if (ps2::sif::g_ee_sregs.empty()
        || ps2::sif::g_ee_sregs.back().first != 12
        || ps2::sif::g_ee_sregs.back().second != 0x30009000u) return 155;

    // A real queued start on core 1 resolves bank 1, sample 1; a
    // second worker pass must not restart an already consumed command.
    ps2::sif::g_handlers.at(0x500)(0x10, 0, 0, 0x180, 4);
    std::array<std::uint8_t, 46 * 16> starts{};
    for (unsigned voice = 0; voice < 46; ++voice)
        std::memset(starts.data() + voice * 16, 0xFF, 4);
    const std::uint32_t start_bank = 1;
    const std::uint16_t start_sample = 1;
    const std::uint16_t enable_effect = 1;
    std::memcpy(starts.data() + 24 * 16, &start_bank, 4);
    std::memcpy(starts.data() + 24 * 16 + 4, &start_sample, 2);
    std::memcpy(starts.data() + 24 * 16 + 12, &enable_effect, 2);
    std::memcpy(ps2::g_ram + 0x800, starts.data(), starts.size());
    kernel::g_address_writes.clear();
    kernel::g_voice_writes.clear();
    kernel::g_switch_writes.clear();
    ps2::sif::g_handlers.at(0x501)(1, 0x800, starts.size(), 0x180, 4);
    kernel::g_voice_job(kernel::g_queued_voice);
    if (kernel::g_address_writes.size() != 2
        || kernel::g_address_writes[0] != std::pair<std::uint32_t, std::uint32_t>{0x2041, 0x5150}
        || kernel::g_address_writes[1] != std::pair<std::uint32_t, std::uint32_t>{0x2141, 0x5010}
        || kernel::g_switch_writes.size() != 3
        || kernel::g_switch_writes[0] != std::pair<std::uint32_t, std::uint32_t>{0x1501, 1}
        || kernel::g_switch_writes[1] != std::pair<std::uint32_t, std::uint32_t>{0x1901, 0x800001}
        || kernel::g_switch_writes[2] != std::pair<std::uint32_t, std::uint32_t>{0x1B01, 0x800001})
        return 156;
    bool found_pitch = false;
    for (const auto &[reg, value] : kernel::g_voice_writes)
        if (reg == 0x201 && value == 0x2222) found_pitch = true;
    if (!found_pitch) return 158;
    const auto started_switches = kernel::g_switch_writes.size();
    kernel::g_voice_job(kernel::g_queued_voice);
    if (kernel::g_switch_writes.size() != started_switches) return 157;

    // The once-only end redirect must survive a new savestate, while
    // active voices report in EE groups of 32, not hardware cores of 24.
    arkd::record_voice_volume_state(32, 0, 0);
    ps2::state::Writer legacy_end_writer;
    legacy_end_writer.set_version(13);
    arkd::state(legacy_end_writer);
    ps2::state::Writer end_writer;
    end_writer.set_version(14);
    arkd::state(end_writer);
    arkd::on_vblank();
    kernel::g_voice_job(kernel::g_queued_voice);
    if (kernel::g_address_writes.back()
        != std::pair<std::uint32_t, std::uint32_t>{0x2141, 0x5010}
        || ps2::sif::g_ee_sregs[ps2::sif::g_ee_sregs.size() - 2]
            != std::pair<std::uint32_t, std::uint32_t>{14, 1u << 24}
        || ps2::sif::g_ee_sregs.back()
            != std::pair<std::uint32_t, std::uint32_t>{15, 1}) return 166;
    kernel::g_end_bits[1] = 1;
    arkd::on_vblank();
    kernel::g_voice_job(kernel::g_queued_voice);
    if (ps2::sif::g_ee_sregs[ps2::sif::g_ee_sregs.size() - 2]
        != std::pair<std::uint32_t, std::uint32_t>{14, 0}) return 167;
    ps2::state::Reader end_reader(end_writer.out.data(), end_writer.out.size());
    end_reader.set_version(14);
    const auto effects_before_restore = kernel::g_effect_calls.size();
    ps2::sif::g_handlers.clear(); // Simulate fresh-process SIF bindings.
    arkd::state(end_reader);
    if (ps2::sif::g_handlers.size() != 4
        || kernel::g_effect_calls.size() != effects_before_restore) return 171;
    kernel::g_end_bits[1] = 0;
    const auto redirected_count = kernel::g_address_writes.size();
    arkd::on_vblank();
    kernel::g_voice_job(kernel::g_queued_voice);
    if (kernel::g_address_writes.size() != redirected_count + 1) return 168;
    ps2::state::Reader legacy_end_reader(legacy_end_writer.out.data(), legacy_end_writer.out.size());
    legacy_end_reader.set_version(13);
    arkd::state(legacy_end_reader);
    const auto legacy_redirected_count = kernel::g_address_writes.size();
    arkd::on_vblank();
    kernel::g_voice_job(kernel::g_queued_voice);
    if (kernel::g_address_writes.size() != legacy_redirected_count) return 169;
    const auto address_count = kernel::g_address_writes.size();
    const std::uint32_t invalid_bank = 16;
    std::memcpy(starts.data() + 24 * 16, &invalid_bank, 4);
    std::memcpy(ps2::g_ram + 0x800, starts.data(), starts.size());
    ps2::sif::g_handlers.at(0x501)(1, 0x800, starts.size(), 0x180, 4);
    kernel::g_voice_job(kernel::g_queued_voice);
    if (kernel::g_address_writes.size() != address_count
        || kernel::g_switch_writes.size() != started_switches) return 159;

    // Update flags occupy a contiguous 92-byte prefix. Voice 24's
    // record starts at +0x5C + 24*16, not +24*18.
    std::array<std::uint8_t, 0x33C> updates{};
    const std::uint16_t pitch_and_gain = 6;
    const std::int16_t raised_pitch = 12;
    const std::uint16_t gain0 = 0x1000, gain1 = 0x2000;
    std::memcpy(updates.data() + 24 * 2, &pitch_and_gain, 2);
    std::memcpy(updates.data() + 0x5C + 24 * 16, &start_bank, 4);
    std::memcpy(updates.data() + 0x60 + 24 * 16, &start_sample, 2);
    std::memcpy(updates.data() + 0x62 + 24 * 16, &raised_pitch, 2);
    std::memcpy(updates.data() + 0x64 + 24 * 16, &gain0, 2);
    std::memcpy(updates.data() + 0x66 + 24 * 16, &gain1, 2);
    std::memcpy(ps2::g_ram + 0x800, updates.data(), updates.size());
    kernel::g_voice_writes.clear();
    ps2::sif::g_handlers.at(0x501)(3, 0x800, updates.size(), 0x180, 4);
    kernel::g_voice_job(kernel::g_queued_voice);
    if (kernel::g_voice_writes.size() != 3
        || kernel::g_voice_writes[0] != std::pair<std::uint32_t, std::uint32_t>{0x201, 0x4444}
        || kernel::g_voice_writes[1] != std::pair<std::uint32_t, std::uint32_t>{1, 8190}
        || kernel::g_voice_writes[2] != std::pair<std::uint32_t, std::uint32_t>{0x101, 4094}
        || kernel::g_switch_writes.size() != started_switches) return 160;
    kernel::g_voice_job(kernel::g_queued_voice);
    if (kernel::g_voice_writes.size() != 3) return 161;
    updates.fill(0);
    updates[32 * 2] = 8;
    std::memcpy(ps2::g_ram + 0x800, updates.data(), updates.size());
    ps2::sif::g_handlers.at(0x501)(3, 0x800, updates.size(), 0x180, 4);
    kernel::g_voice_job(kernel::g_queued_voice);
    if (kernel::g_switch_writes.back()
        != std::pair<std::uint32_t, std::uint32_t>{0x1601, 1u << 8}) return 162;
    ps2::sif::g_handlers.at(0x500)(1, 0, 0, 0x180, 4);

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
    if (ps2::sif::g_ee_sregs.back()
        != std::pair<std::uint32_t, std::uint32_t>{12, 0x30000000u}) return 165;

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

    // A second bank must coexist with bank 1. Re-upload bank 1 after
    // loading bank 2 to prove the native cache is not latest-only.
    const std::uint32_t second_bank_id = 2;
    std::memcpy(ps2::g_ram + 0x100, &second_bank_id, 4);
    if (!ps2::sif::g_handlers.at(0x501)(0x101, 0x100, 0x24, 0x180, 4)
        || word(0x180) != 0) return 116;
    kernel::g_bank_job(kernel::g_queued_bank);
    for (unsigned step = 0; step < 12 &&
         arkd::sound_upload_status() != 0x30000000u; ++step)
        kernel::g_upload_job(kernel::g_queued_upload);
    const auto second_address = arkd::sound_bank_spu_address(second_bank_id);
    if (arkd::sound_upload_status() != 0x30000000u
        || !second_address || *second_address <= *stable_address)
        return 117;

    ps2::state::Writer multi_writer;
    multi_writer.set_version(12);
    arkd::state(multi_writer);
    ps2::state::Reader multi_reader(
        multi_writer.out.data(), multi_writer.out.size());
    multi_reader.set_version(12);
    arkd::state(multi_reader);

    kernel::g_captures.clear();
    std::memcpy(ps2::g_ram + 0x400, &upload_bank_id, 4);
    if (!ps2::sif::g_handlers.at(0x501)(0x103, 0x400, 4, 0x180, 4)
        || word(0x180) != 0) return 118;
    kernel::g_audio_job(kernel::g_queued_audio);
    for (unsigned step = 0; step < 12 &&
         arkd::sound_upload_status() != 0x30000000u; ++step)
        kernel::g_upload_job(kernel::g_queued_upload);
    if (arkd::sound_upload_status() != 0x30000000u
        || kernel::g_captures.size() != 2
        || kernel::g_captures[0].size != 0x8000
        || kernel::g_captures[1].size != 0x40)
        return 119;
    if (arkd::sound_bank_spu_address(second_bank_id) != second_address)
        return 120;

    // The second bank is also cached and can be reloaded after first.
    kernel::g_captures.clear();
    std::memcpy(ps2::g_ram + 0x400, &second_bank_id, 4);
    if (!ps2::sif::g_handlers.at(0x501)(0x103, 0x400, 4, 0x180, 4)
        || word(0x180) != 0) return 121;
    kernel::g_audio_job(kernel::g_queued_audio);
    for (unsigned step = 0; step < 12 &&
         arkd::sound_upload_status() != 0x30000000u; ++step)
        kernel::g_upload_job(kernel::g_queued_upload);
    if (arkd::sound_upload_status() != 0x30000000u
        || kernel::g_captures.size() != 3
        || kernel::g_captures[0].size != 0x8000)
        return 122;

    // IRX 0x4DBC: changing master gain, per-channel gain, or mono mode
    // immediately re-applies 14-bit fixed-point gain to active voices.
    // Source +10 maps to the first libsd register; source +8 to +0x100.
    if (!arkd::record_voice_volume_state(0, 0x2000, 0x1000)
        || !arkd::record_voice_volume_state(24, 0x3000, 0x1000)
        || arkd::record_voice_volume_state(46, 0x4000, 0x4000))
        return 130;
    const auto check_voice_writes =
        [&](const std::vector<std::pair<std::uint32_t, std::uint32_t>> &expected)
    {
        return kernel::g_voice_writes == expected;
    };

    const std::uint16_t master_gain = 0x2000;
    std::memcpy(ps2::g_ram + 0x400, &master_gain, 2);
    kernel::g_voice_writes.clear();
    if (!ps2::sif::g_handlers.at(0x501)(0x12, 0x400, 2, 0x180, 4)
        || word(0x180) != 0) return 131;
    if (!check_voice_writes({
        {0, 0x7FF}, {0x100, 0xFFF},
        {1, 0x7FF}, {0x101, 0x17FF}
    })) return 132;

    const std::uint16_t channel_gains[2] = {0x1000, 0x3000};
    std::memcpy(ps2::g_ram + 0x400, channel_gains, 4);
    kernel::g_voice_writes.clear();
    if (!ps2::sif::g_handlers.at(0x501)(0x14, 0x400, 4, 0x180, 4)
        || word(0x180) != 0) return 133;
    if (!check_voice_writes({
        {0, 0x200}, {0x100, 0xC00},
        {1, 0x200}, {0x101, 0x1200}
    })) return 134;

    kernel::g_voice_writes.clear();
    if (!ps2::sif::g_handlers.at(0x501)(0x14, 0x400, 2, 0x180, 4)
        || word(0x180) != 0x80000064u
        || !kernel::g_voice_writes.empty()) return 135;

    // 0x501/0x13: mono/mix-disabled bit ORs both sources into the
    // first register, and silences the second one.
    const std::uint32_t mono = 0;
    std::memcpy(ps2::g_ram + 0x400, &mono, 4);
    kernel::g_voice_writes.clear();
    if (!ps2::sif::g_handlers.at(0x501)(0x13, 0x400, 4, 0x180, 4)
        || word(0x180) != 0) return 136;
    if (!check_voice_writes({
        {0, 0x600}, {0x100, 0},
        {1, 0x600}, {0x101, 0}
    })) return 137;

    const std::uint32_t stereo = 1;
    std::memcpy(ps2::g_ram + 0x400, &stereo, 4);
    kernel::g_voice_writes.clear();
    if (!ps2::sif::g_handlers.at(0x501)(0x13, 0x400, 4, 0x180, 4)
        || word(0x180) != 0) return 138;
    if (!check_voice_writes({
        {0, 0x200}, {0x100, 0xC00},
        {1, 0x200}, {0x101, 0x1200}
    })) return 139;

    // V13 stores base source gain separately from the original sound
    // command buffers. Replacing the live gain and restoring the
    // snapshot must restore the original register values.
    ps2::state::Writer gain_writer;
    gain_writer.set_version(13);
    arkd::state(gain_writer);
    if (!arkd::record_voice_volume_state(0, 0x100, 0x200)) return 140;
    ps2::state::Reader gain_reader(
        gain_writer.out.data(), gain_writer.out.size());
    gain_reader.set_version(13);
    arkd::state(gain_reader);
    // The preceding 0x13 RPC left the stereo-mode value at +0x400.
    // Restore the intended master gain before exercising 0x12 again.
    std::memcpy(ps2::g_ram + 0x400, &master_gain, 2);
    kernel::g_voice_writes.clear();
    if (!ps2::sif::g_handlers.at(0x501)(0x12, 0x400, 2, 0x180, 4)
        || !check_voice_writes({
            {0, 0x200}, {0x100, 0xC00},
            {1, 0x200}, {0x101, 0x1200}
        })) return 141;

    // A legacy v12 state may contain an active-voice mask but has no
    // original source-gain words. Clear those stale masks on load,
    // rather than emitting register writes from invented zero gains.
    ps2::state::Writer legacy_writer;
    legacy_writer.set_version(12);
    arkd::state(legacy_writer);
    ps2::state::Reader legacy_reader(
        legacy_writer.out.data(), legacy_writer.out.size());
    legacy_reader.set_version(12);
    arkd::state(legacy_reader);
    kernel::g_voice_writes.clear();
    if (!ps2::sif::g_handlers.at(0x501)(0x12, 0x400, 2, 0x180, 4)
        || !kernel::g_voice_writes.empty()) return 142;

    // The original 0x4A10, 0x4AC0 and 0x4BB8 handlers EACH advance
    // the alternating command generation. Verify 0x501/2 chooses the
    // next parity, including after a malformed packet.
    if (!ps2::sif::g_handlers.at(0x500)(1, 0, 0, 0x180, 4)
        || word(0x180) != 0) return 143;
    std::memset(ps2::g_ram + 0x800, 0, 0x33C);
    if (!ps2::sif::g_handlers.at(0x501)(2, 0x800, 4, 0x180, 4)
        || word(0x180) != 0x80000064u) return 144;
    if (!ps2::sif::g_handlers.at(0x501)(1, 0x800, 0x2E0, 0x180, 4)
        || word(0x180) != 0) return 145;
    if (!ps2::sif::g_handlers.at(0x501)(2, 0x800, 8, 0x180, 4)
        || word(0x180) != 0
        || kernel::g_queued_voice.a != 0) return 146;
    if (!ps2::sif::g_handlers.at(0x501)(3, 0x800, 0x33C, 0x180, 4)
        || word(0x180) != 0
        || kernel::g_queued_voice.a != 1) return 147;
    if (!ps2::sif::g_handlers.at(0x501)(2, 0x800, 8, 0x180, 4)
        || word(0x180) != 0
        || kernel::g_queued_voice.a != 0) return 148;

    // IRX 0x3330 reads the 16-byte start command for each of 46
    // voices, including a signed pitch-control halfword at +6.
    std::array<std::uint8_t, 46 * 16> voice_packet{};
    auto write_field = [&](std::size_t offset, const auto &value)
    {
        std::memcpy(voice_packet.data() + offset, &value, sizeof(value));
    };
    const std::uint32_t sample_id = 0x12345678u;
    const std::uint16_t command_flags = 0x40u;
    const std::int16_t signed_pitch = -42;
    const std::uint16_t left_source = 0x1000u;
    const std::uint16_t right_source = 0x2000u;
    const std::uint32_t tail_value = 0xDEADBEEFu;
    constexpr std::size_t last_voice = 45;
    constexpr std::size_t base = last_voice * 16;
    write_field(base + 0, sample_id);
    write_field(base + 4, command_flags);
    write_field(base + 6, signed_pitch);
    write_field(base + 8, left_source);
    write_field(base + 10, right_source);
    write_field(base + 12, tail_value);
    const auto parsed = arkd::inspect_voice_command(
        voice_packet.data(), voice_packet.size(), last_voice);
    if (!parsed || parsed->sample_selector != sample_id
        || parsed->control != command_flags
        || parsed->pitch_request != signed_pitch
        || parsed->volume_0 != left_source
        || parsed->volume_1 != right_source
        || parsed->remaining != tail_value
        || parsed->start_control != 0xBEEFu
        || parsed->restart_control != 0xDEADu) return 149;
    if (arkd::inspect_voice_command(
            voice_packet.data(), voice_packet.size() - 1, last_voice)
        || arkd::inspect_voice_command(
            voice_packet.data(), voice_packet.size(), 46)
        || arkd::inspect_voice_command(nullptr, 46 * 16, 0))
        return 150;

    // -1 is the original 'no sample selected' sentinel at 0x3354.
    const std::uint32_t sentinel = 0xFFFFFFFFu;
    write_field(base, sentinel);
    const auto empty = arkd::inspect_voice_command(
        voice_packet.data(), voice_packet.size(), last_voice);
    if (!empty || empty->sample_selector != sentinel) return 151;

    // IRX 0x33C4/0x35D4 selects positive vs. nonpositive signed
    // pitch, then divides the magnitude into 12-step intervals.
    // Include octave boundaries and int16 endpoints.
    struct s_pitch_case
    {
        std::int16_t request;
        std::uint32_t octaves;
        std::uint32_t remainder;
        bool non_positive;
    };
    const s_pitch_case pitch_cases[]
    {
        {0, 1, 0, true},
        {1, 1, 1, false},
        {11, 1, 11, false},
        {12, 2, 0, false},
        {13, 2, 1, false},
        {-1, 1, 1, true},
        {-11, 1, 11, true},
        {-12, 2, 0, true},
        {-13, 2, 1, true},
        {32767, 2731, 7, false},
        {-32768, 2731, 8, true}
    };
    for (const auto &test : pitch_cases)
    {
        const auto pitch = arkd::decode_pitch_interval(test.request);
        if (pitch.octave_count != test.octaves
            || pitch.semitone_remainder != test.remainder
            || pitch.non_positive != test.non_positive)
            return 152;
    }
    struct s_voice_pitch_case
    {
        std::uint16_t base;
        std::int16_t request;
        std::uint16_t expected;
    };
    const s_voice_pitch_case voice_pitch_cases[]
    {
        {4096, 0, 4096}, {4096, 1, 4437}, {4096, 11, 7850},
        {4096, 12, 8192}, {4096, 13, 8533}, {4096, 24, 12288},
        {4096, -1, 3926}, {4096, -11, 2219}, {4096, -12, 2048},
        {4096, -13, 1992}, {4096, -24, 1365},
        // Adjacent truncated quotients differ from one rational division.
        {17, -13, 8},
        // Intermediate halfword stores wrap, rather than saturating.
        {65535, 1, 5460}, {65535, 12, 65534}, {65535, 13, 5459},
        {65535, 32767, 35497}, {65535, -32768, 23},
        {0, -32768, 0}, {0, 32767, 0},
    };
    for (const auto &test : voice_pitch_cases)
        if (arkd::calculate_voice_pitch(test.base, test.request) != test.expected)
            return 153;

    // A different game never started ARKD_DVD. Its state must remain
    // inactive even though native ARKD is the default selection.
    arkd::install();
    ps2::state::Writer inactive_writer;
    inactive_writer.set_version(15);
    arkd::state(inactive_writer);
#ifdef _WIN32
    _putenv_s("SDBZ_ARKD_NATIVE", "0");
#else
    setenv("SDBZ_ARKD_NATIVE", "0", 1);
#endif
    ps2::state::Reader inactive_reader(
        inactive_writer.out.data(), inactive_writer.out.size());
    inactive_reader.set_version(15);
    try { arkd::state(inactive_reader); }
    catch (const std::runtime_error &) { return 199; }

    // An inactive state from SDBZ with the original IRX loaded still
    // rejects a change of backend on restore.
    kernel::g_modules[1] = {1, "ARKD_DVD", 0, 0, 0, 0};
#ifdef _WIN32
    _putenv_s("SDBZ_ARKD_NATIVE", "1");
#else
    setenv("SDBZ_ARKD_NATIVE", "1", 1);
#endif
    ps2::state::Reader original_reader(
        inactive_writer.out.data(), inactive_writer.out.size());
    original_reader.set_version(15);
    bool rejected_original = false;
    try { arkd::state(original_reader); }
    catch (const std::runtime_error &) { rejected_original = true; }
    if (!rejected_original) return 200;
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
        reserved_bank = bytearray(bank)
        struct.pack_into("<I", reserved_bank, 8, 0x190000)
        ams = bytearray(0x40800)
        struct.pack_into("<IIII", ams, 0, 120, 0x1000, 3 * 0x8000 + 0x100, 2)
        for offset, marker in ((0x800, 0x11), (0x20800, 0x21), (0x30800, 0x31), (0x10800, 0x41)):
            for channel in range(2):
                start = offset + channel * 0x8000
                ams[start:start + 0x8000] = bytes([marker + channel]) * 0x8000
                for block in range(start, start + 0x8000, 16):
                    ams[block:block + 2] = bytes([12, 0])
        for name, content in {
            "dis/a_first.bin": b"A" * 13,
            "ply/small.bin": b"S" * 5000,
            "snd/test.bnk": bytes(bank),
            "snd/reserved.bnk": bytes(reserved_bank),
            "snd/big.bnk": bytes(big_bank),
            "sqm/test.ams": bytes(ams),
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

    def test_ams_stream_buffers_and_controls(self):
        environment = os.environ.copy()
        environment["SDBZ_ARKD_NATIVE"] = "1"
        subprocess.run([str(self.executable), str(self.root / "disc"), "streams"],
                       env=environment, check=True)

    def test_pitch_against_original_mips(self):
        reference = os.environ.get("ARKD_REFERENCE_IRX")
        if not reference:
            self.skipTest("set ARKD_REFERENCE_IRX to the original EU beta IRX")
        from arkd_pitch_reference import PitchReference

        original = PitchReference(pathlib.Path(reference).read_bytes())
        for base in (0, 4096, 32769, 65535):
            result = subprocess.run(
                [str(self.executable), "--pitch-vectors", str(base)],
                check=True, capture_output=True, text=True)
            values = [int(line) for line in result.stdout.splitlines()]
            self.assertEqual(len(values), 65536)
            for request, native in zip(range(-32768, 32768), values):
                expected = original.run(base, request)
                if native != expected:
                    self.fail(f"base={base} request={request}: "
                              f"native={native} original={expected}")

    def test_original_archive_and_partial_loose_overrides(self):
        # The native index must not lose untouched GAME.DAT entries just
        # because one or more assets are present in a loose GAME/ tree.
        from test_game_dat_loose_assets import SECTOR, write_info

        root = self.root / "mixed_overlay"
        root.mkdir()
        (root / "GAME").mkdir()
        assets = [
            ("archived.bin", 0, b"A" * 100),
            ("override.bin", 1, b"O" * 100),
        ]
        write_info(root, assets, 512)
        archive = bytearray(2 * SECTOR)
        archive[:100] = assets[0][2]
        archive[SECTOR:SECTOR + 100] = assets[1][2]
        (root / "GAME.DAT").write_bytes(archive)
        (root / "GAME" / "override.bin").write_bytes(b"N" * 100)
        (root / "GAME" / "new.bin").write_bytes(b"Z" * 100)

        environment = os.environ.copy()
        environment["SDBZ_ARKD_NATIVE"] = "1"
        subprocess.run(
            [str(self.executable), str(root), "mixed"],
            env=environment, check=True)

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
