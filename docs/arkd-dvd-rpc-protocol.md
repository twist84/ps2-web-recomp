# ARKD_DVD.IRX RPC protocol — SLES_541.61 EU beta

Reverse-engineering notes from the original 71,133-byte `ARKD_DVD.IRX`
(SHA-256 `b61c35c41f71c489b7f07d871e6b5293f0f872dbf3ab66672c487835e3bb460a`).
The EU beta module is byte-identical to the US release. Offsets below
are **module-relative MIPS .text offsets before IOP relocation**. This
file describes observations from the original IRX, not hypothetical
RPC implementations.

The driver registers four SIF RPC servers:

| SIF server | Handler | Role |
|---|---:|---|
| 0x500 | 0x01D4 | Driver lifecycle and control |
| 0x501 | 0x6708 | Sound/stream command input and DMA state |
| 0x502 | 0x9D38 | Sound playback and buffer configuration |
| 0x503 | 0x2700 | Archive query and DVD transfer control |

## Server 0x500 dispatch (jump table at 0xAF38)

| Method | Called routine | Static result |
|---|---:|---|
| 1 | 0x0030 | Initializes driver submodules: 0x2AA8, 0x6920, 0x9EAC, 0x28AC |
| 2 | 0x0088 | Calls a system initializer and stores return value |
| 3 | 0x00D8 | Reinitializes CDVD and stream/control services |
| 0x10 | 0x0110 | Sets global control flag to 1 |
| 0x11 | 0x0138 | Stores first request u32 in global control value |
| 0x12 | 0x017C | Rounds first request u32 to a 128-byte boundary |

The remaining values within 1–18 jump to the default case.

## Server 0x503 dispatch (0x2700)

| Method | Called routine | Static result |
|---|---:|---|
| 1 | 0x16A4 | Queries the CD status and returns a status word |
| 2 | 0x1714 | Copies the global archive-transfer state to the reply |
| 3 | 0x1760 | Filename at request +4; returns actual and aligned length (two u32s) |
| 4 | 0x1A10 | Debug logging of a request-provided string |
| 0x81 | 0x183C | Sets a cancel/stop flag |
| 0x101 | 0x250C | Copies 36-byte request and schedules worker command 1 |
| 0x102 | 0x2410 | Copies 36-byte request and schedules worker command 2 |
| 0x103 | 0x2608 | Reads request u32 (CDVD operation 1–8) and schedules worker command 3 |

Request for method 0x102 (disassembled at 0x2410, worker at
0x21B0/0x1A58):

```cpp
struct s_arkd_full_file_request
{
    std::uint32_t ee_destination; // +0x00, target for SIF DMA writes
    char file_name[32];            // +0x04, INFO record path
};
static_assert(sizeof(s_arkd_full_file_request) == 0x24);
```

The worker uses the filename at global request+4, obtains the
14-bit-indexed INFO record, reads aligned DVD sectors in chunks and
uses SIF DMA to move the data into EE memory. The driver's status
transitions include 0x10000000 (accepted), 0x20000000 (reading),
0x30000000 (completed), and 0x400000XX (error).
The native implementation currently reproduces the basic queue,
status transitions, and bounded copy, but not every hardware error
condition, interrupt or transfer timing.

## Native virtual-CDVD worker 0x503/0x103

The original routine at `0x2608–0x26EC` accepts a 32-bit drive-control
opcode from the request only if its transfer status has the
`0x30000000` (complete) or `0x40000000` (error) high nibble. Otherwise
it returns `-1` without queueing. Accepted calls store the opcode,
set status `0x10000000` and wake the CDVD worker.

Worker routine `0x1EB0–0x219C` dispatches opcodes **1 through 8**.
Each original opcode performs a different physical `cdvdman` sequence;
some have error codes `0x80000190`–`0x80000195`. Native HLE
now reproduces the request format, busy return, queue, and status
transitions. With a mounted **virtual** disc, operations 1–8 complete
successfully as emulated media-control operations; an absent disc gives
`0x40000190`. It does **not** recreate real DVD tray, spindle, seek,
or all the original error-code distinctions. This is intentionally a
virtual-device model, not confirmed instruction-level CDVD parity.

## Server 0x501 dispatch (0x6708)

| Method | Called routine | Static result |
|---|---:|---|
| 1 | 0x4A10 | Copies a 0x2E0-byte command block into alternate buffer |
| 2 | 0x4AC0 | ORs two u32 flag words into alternate buffer |
| 3 | 0x4BB8 | Copies a 0x33C-byte command block into alternate buffer |
| 0x11 | 0x4C84 | Parses two 16-byte sound descriptors, invokes libsd |
| 0x12 | 0x505C | Reads one u16 and updates sound state |
| 0x13 | 0x5108 | Toggles an asynchronous enable flag |
| 0x14 | 0x50A8 | Reads two u16 sound-state fields |
| 0x81 | 0x5194 | Sets a control bit |
| 0x82 | 0x51C8 | Returns a two-u32 buffer status struct |
| 0x101 | 0x6528 | Copies a 0x24-byte request and starts worker command |
| 0x102 | 0x65C8 | Reads request u32 and starts worker command |
| 0x103 | 0x6668 | Reads request u32 and starts worker command |

The 0x501 worker handles double-buffered audio/stream transfer states.
Methods 0x11 and other worker paths call the imported `libsd` library,
so supporting them means reproducing actual SPU2 interactions and
interrupt/completion semantics. A generic zero-valued reply is not
a correct substitute.

## Server 0x502 dispatch (0x9D38)

| Method | Called routine | Static result |
|---|---:|---|
| 1 | 0x96C4 | Reads u16, adds one, updates voice/stream state |
| 2 | 0x9754 | Reads u16 stream configuration |
| 3 | 0x9878 | Toggles sound-state flag |
| 4 | 0x991C | Returns current sound-state word |
| 5 | 0x97DC | Reads two u16 stream configuration values |
| 0x101 | 0x9548 | Copies 0x17C-byte sound-control buffer |
| 0x102 | 0x95C0 | Reads u32 and updates control bits |
| 0x103 | 0x9628 | Reads u32 and updates control bits |

## Sound worker details confirmed from original MIPS instructions

- IRX `0x6920-0x6970`: the three sound-volume multipliers
  (`master`, `channel left`, `channel right`) default to **`0x3FFF`**.
  The native module now preserves these defaults on startup and old-state
  restoration instead of zero-initialising them.
- IRX `0x4A10` copies `0x2E0` bytes into the alternate command buffer.
  **`0x2E0 = 46 * 16`**, so this is a 46-voice command array with
  16 bytes per entry.
- IRX `0x4BB8` copies `0x33C` bytes into another alternate buffer.
  **`0x33C = 46 * 18`**, so this is a second 46-entry command array
  with 18 bytes per entry.
- IRX `0x3020`–`0x4618` consumes those arrays. Its loop iterates 46
  voice slots, uses per-core voice bitmasks, and calls `libsd` ordinal
  **5** (`sceSdSetParam`) for individual SPU2 register updates. The
  16-byte voice record has two volume fields at offsets `+8` and
  `+10`. The 18-byte command record has flags at offset `+0`.
- IRX `0x3194-0x31E8` indexes its per-voice **command masks** by
  `index >> 5` and `index & 31` (32 slots per word). By contrast,
  `0x3110-0x316C` constructs each `libsd` voice register ID from
  `index / 24` and `index % 24`. These are distinct: voice indices
  24 and 32 cross different boundaries. The native queued mute handler
  now uses the original mapping and has boundary regression tests.
- IRX `0x3234-0x3278` calls `libsd` ordinal 5 twice with zero volume,
  first for register `(index / 24) | ((index % 24) << 1)`, then that
  register OR `0x100`. The new native queued worker performs precisely
  these mute writes when its command bit or record bit 3 is set. Other
  voice-start/voice-parameter operations are **not** implemented yet.
- IRX `0x4DBC` re-applies volumes for *active* voices after `0x501/0x12`
  or `0x501/0x14`. It scales first by the master coefficient and then
  by the corresponding channel coefficient (14-bit fixed point,
  with a shift by 14 after each multiplication).
- IRX `0x6528`, `0x65C8`, and `0x6668` queue three distinct
  audio-worker opcodes. The dispatch at `0x62C8` calls `0x5894`,
  `0x5228`, or `0x5F08`. These perform file/bank reads and sound
  hardware operations. Merely copying the 46-voice command buffers does
  **not** execute those operations.
- IRX `0x5894` copies the first 32-bit request word into its working
  bank index and calls archive lookup `0x0A60` with request offset `+4`,
  then reads sectors through `0x0CCC`. This proves `0x501/0x101`
  includes a real archive-backed audio-file load, not just an acknowledgement.

The next implementation work is to reproduce the 46-voice worker's
`libsd` calls and bank storage, followed by the other audio worker
commands. No parity should be claimed until those workers consume the
queued command buffers and perform the equivalent SPU2 effects.

## Savestate and build coverage

Native state is serialized in a new `arkd` section of **savestate version
7**, including the pending transfer job ID, file extent, destination,
volume/control buffers, and sound state. Version 2–6 savestates remain
loadable with native state reset to defaults. Loading a version 7 state
with the wrong `SDBZ_ARKD_NATIVE` setting is explicitly rejected.

The GitHub Actions workflow now compiles the entire `ps2runtime` target
using Clang and runs the ARKD unit harness under Clang and GCC.
The complete runtime cannot be built with GCC without changes to the
unrelated `vu_ops.hpp` Clang-specific vector intrinsics.

## Implementation notes

- `SDBZ_ARKD_NATIVE=1` substitutes native HLE for the ARKD_DVD module.
  Without it, the original IRX runs unchanged.
- Native `0x503/3` and `0x503/0x102` are implemented. `0x503/2`
  exposes native transfer state; `0x503/0x81` cancels queued reads.
- Some 0x500 control methods are implemented, but the sound/stream
  handlers are **not** yet at parity. Keep the PR draft and HLE opt-in.
- Queue state is currently process-global and needs savestate integration
  before native mode becomes the default.
- A successful C++ harness is not a substitute for differential tests
  of DMA completion, IRX return status, audio playback, and EE callbacks.

## Next low-level research tasks

1. Trace exact 0x501/0x502 `libsd` imported calls and their buffer
   layout to the SPU2 device registers.
2. Map EE-side RPC call-sites for the EU beta so we know which sound
   commands are exercised during boot and gameplay.
3. Capture original-IRX SIF RPC arguments and replies, plus DMA and
   status transitions. Implement protocol-specific native equivalents.
4. Compare cancellation, concurrent requests, failure paths, and
   savestate restoration at each completion phase.

No original IRX executable, game assets, or lifted copyrighted
instructions are included in this repository.
