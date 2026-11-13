# ARKD_DVD.IRX RPC protocol â€” SLES_541.61 EU beta

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

The remaining values within 1â€“18 jump to the default case.

## Server 0x503 dispatch (0x2700)

| Method | Called routine | Static result |
|---|---:|---|
| 1 | 0x16A4 | Queries the CD status and returns a status word |
| 2 | 0x1714 | Copies the global archive-transfer state to the reply |
| 3 | 0x1760 | Filename at request +4; returns actual and aligned length (two u32s) |
| 4 | 0x1A10 | Debug logging of a request-provided string |
| 0x81 | 0x183C | Sets a cancel/stop flag |
| 0x101 | 0x250C | Copies 36-byte request and schedules full-file worker command 2 |
| 0x102 | 0x2410 | Copies 36-byte request and schedules decoded-index DMA command 1 |
| 0x103 | 0x2608 | Reads request u32 (CDVD operation 1â€“8) and schedules worker command 3 |

Request for method 0x101 (disassembled at 0x2410, worker at
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

The original routine at `0x2608â€“0x26EC` accepts a 32-bit drive-control
opcode from the request only if its transfer status has the
`0x30000000` (complete) or `0x40000000` (error) high nibble. Otherwise
it returns `-1` without queueing. Accepted calls store the opcode,
set status `0x10000000` and wake the CDVD worker.

Worker routine `0x1EB0â€“0x219C` dispatches opcodes **1 through 8**.
Each original opcode performs a different physical `cdvdman` sequence;
some have error codes `0x80000190`â€“`0x80000195`. Native HLE
now reproduces the request format, busy return, queue, and status
transitions. With a mounted **virtual** disc, operations 1â€“8 complete
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

## 0x501/0x102 and 0x501/0x103 audio worker admission

The EU IRX handlers at `0x65C8` and `0x6668` share the same
control sequence: copy the 32-bit request selector to `0x3334` or
`0x3338`, clear a worker-ready bit, set command opcode 1 or 2,
set the worker state to `0x10000000`, and wake its thread.
The thread dispatcher at `0x62C8` calls `0x5228` (opcode 1) or
`0x5F08` (opcode 2).

Native HLE now accepts and queues both RPC commands, reporting busy
while another audio-bank operation is pending. For `0x102` it releases
the selected SPU2 bank and makes that address available for a future
allocation; it intentionally does **not** compact other live banks.
For `0x103` it re-uploads the most recently cached matching bank
using the native SPU2 DMA worker, reporting a failure if no such bank
is cached. Full equivalence to `0x5228` and `0x5F08` is **not
proven**: original compaction, playback state and callbacks still
require differential validation. Audio request state is serialized
by the latest v11 savestate format.

## Verified bank-header layout (original 0x5894â€“0x5C04)

The original loader first reads one 0x800-byte sector from the audio bank.
It then reads `u32` at `+0x04` as an entry count and `u32` at
`+0x08` as its SPU2 allocation span. The subsequent per-entry table
starts at `+0x0C` with an eight-byte stride:

```cpp
struct s_arkd_bank_entry
{
    std::uint32_t parameter;  // +0: used in the bank's parameter scaler
    std::uint32_t spu_offset; // +4: added to the allocated SPU2 base
};
```

The per-entry base calculation at `0x5B20â€“0x5B68` reads the second
field and adds the bank's SPU2 memory base. The parameter calculation
at `0x5BACâ€“0x5BEC` reads the first field and scales it by a global
coefficient. The native `inspect_sound_bank` function now parses
these fields without assuming they are PCM samples. It limits the table
to the first 2048 bytes, checks the declared SPU2 span against 2 MiB,
and rejects relative entry offsets outside that span.

Header validation precedes native sample uploads from byte 0x800
onwards via `sceSdVoiceTrans`. The original also synthesizes
per-entry voice parameters and maintains additional bank tables;
these are **not** reconstructed in full. A valid header alone does
not prove that the complete sample-bank format is supported.

## Native 0x501/0x101 sound-bank preload (partial)

The original `0x6528` method copies 36 bytes to its worker request
storage. In `0x5894`, the worker calls archive lookup at `0x0A60`
using the filename at request offset `+4`. The original then performs
a more involved bank allocation and read sequence, followed by SPU2
transfers and additional state changes.

Native HLE reads the archive file into a bounded 64 MiB bank cache.
On a loose `GAME/` folder mount it reads directly from host files
without rebuilding INFO.DAT; for indexed ISO mounts it uses the
existing virtual GAME.DAT backend. The first request word selects
the bank slot and the following 32 bytes contain its filename.

For a validated bank header, the native SPU2 worker allocates sample
memory, copies bytes after the 0x800-byte header into a persistent
32 KiB IOP staging buffer, and issues `libsd` ordinal 17
(`sceSdVoiceTrans`) with an optional fifth MIPS argument. It polls
ordinal 19 to avoid changing or freeing the source during DMA.
The actual game's per-voice setup, DMA callbacks and timing still
need original-IRX differential validation.

Pending bank IDs, direct host-file IDs, cache contents and SPU2
uploads are serialized in **version 11 savestates**; cache sizes
are checked before allocation on restore.

## Alternating voice-command buffer generation (0x4A10â€“0x4BB8)

Original `0x501` methods `1` (`0x4A10`), `2` (`0x4AC0`),
and `3` (`0x4BB8`) **each** increment the same generation word
at IRX global `0xB370`; the new low bit selects the destination
command buffer. Native HLE previously failed to increment generation
for method `2`, allowing later flags and sound command records to be
paired with the wrong buffer.

Method `3` increments the pending-work word at `0xB36C` only if
it was zero; methods `1` and `2` increment it unconditionally.
The native queue now models those admission rules, while voice
processing itself remains a partial reconstruction. The harness
checks method order, parity, and rejected packets.

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

## IRX import table and the five-argument libsd transfer

The ELF's actual IOP import stubs were extracted from the EU
`ARKD_DVD.IRX` `.text` section (`0x41E00000` import headers,
`jr ra; addiu v0, zero, ordinal` stubs). The `libsd` table starts
at offset `0xAC00`; exported ordinals referenced are:

| Stub | libsd ordinal |
| --- | ---: |
| `0xAC14` | 4 |
| `0xAC1C` | 5 |
| `0xAC24` | 7 |
| `0xAC2C` | 8 |
| `0xAC34` | 9 |
| `0xAC3C` | 10 |
| `0xAC44` | 11 |
| `0xAC4C` | 17 |
| `0xAC54` | 23 |
| `0xAC5C` | 26 |
| `0xAC64` | 27 |

The original bank transfer worker invokes **ordinal 17**
(`sceSdVoiceTrans`) at `0x5420`, `0x54F4`, `0x5DF4`, and `0x6250`.
Its argument setup is **five arguments**, matching the MIPS o32 ABI:
`a0`â€“`a3` in registers and the fifth at `sp+0x10`.
At `0x5420`, the IRX sends the low bit of a local core selector
as argument 0, constant `1` as argument 1, an IOP data buffer as
argument 2, a transfer destination as argument 3, and the transfer
size as the **fifth stack argument**. The other sites use
different modes/destinations but the same five-argument structure.

The shared native IOP guest-library bridge now optionally writes this
fifth argument to the guest stack and restores the overwritten word
after the guest call. Existing four-argument calls remain unchanged.

The native implementation now stages sample payloads from its bank
cache in persistent IOP RAM, invokes ordinal 17 in chunks and polls
ordinal 19 before changing or freeing the buffer. This sequence is
covered by synthetic DMA tests, **not** by differential validation of
the original IRQ, completion callback ordering, or gameplay playback.

## Original 46-voice start command, 16-byte records

The IRX's voice thread at `0x3020â€“0x4618` reads one of two
alternating `0x2E0` command arrays (`46 * 16`). At `0x3330` it
loads the first u32 from each record and compares it with
`0xFFFFFFFF`, its no-sample sentinel. For a non-sentinel entry,
`0x3374` reads a control u16 at `+4`, `0x33BC` reads a signed
halfword at `+6`, and the subsequent voice logic inspects the
remaining fields. The record's `+8` and `+10` source words
are used in the live-gain path.

The native `inspect_voice_command()` now decodes the full 16-byte
record without unaligned host loads or out-of-bounds access:

```cpp
struct s_voice_command
{
    std::uint32_t sample_selector;  // +0, -1 = no new sample
    std::uint16_t control;          // +4
    std::int16_t pitch_request;     // +6, signed branch input
    std::uint16_t volume_0;        // +8
    std::uint16_t volume_1;        // +10
    std::uint32_t remaining;       // +12, semantics pending
};
```

Signed `+6` controls the IRX's integer pitch interpolation, described below.
This decoder **does not key on a voice**. The 46-voice startup branch
requires bank-index resolution, sample address, pitch, ADSR and
key-on validation against the original binary before the IRX can
be disabled by default.

## Pitch request interval arithmetic and start-control words

The original voice-start path has two branches at `0x33C4` and
`0x35D4`: strictly positive signed pitch-control values follow the
first path, while zero/negative values follow the second. Both paths
use the signed MIPS multiply-high constant `0x2AAAAAAB` to perform
division by 12 on the **magnitude** and calculate two values:

- interval `(magnitude + 12) / 12` (truncated division)
- remainder `magnitude % 12`

The second branch retains that the original request was non-positive.
These intermediate values feed the pitch-table accesses at `0x34B4`
and `0x36C8`, followed by interpolation. Native
`decode_pitch_interval()` reproduces only this verified integer
decomposition, with tests for values around 0, 12 and both signed
16-bit extremes. `calculate_voice_pitch()` now reproduces the subsequent
interpolation through `0x37F8`, given the bank's base-pitch halfword.

The lookups at `0x34B4` and `0x36C8` address the same per-sample
halfword at `0x106C0 + bank * 500 + sample * 2`. This is not a
semitone lookup table. Bank loading at `0x5BBC-0x5BEC` initializes it
as `u16(u32(entry.parameter * pitch_scale) >> 12)`, including low-word
multiplication wrap. `pitch_scale` is the global at `0xB4A8`, initially
`0x1000`, which server `0x500/0x11` can change.

For base halfword `B`, interval `N = magnitude / 12 + 1` and
remainder `R = magnitude % 12`, the final pitch halfword is:

- Positive request: `u16(u16(B * N) + (B * R) / 12)`.
- Zero/negative request: `U - ((U - L) * R) / 12`, where
  `U = B / N` and `L = B / (N + 1)` are truncated integer quotients.

The positive branch's intermediate difference is stored and reloaded as
a halfword; it is exactly `B` modulo 65536. The result wraps to a
halfword rather than saturating to the SPU2 pitch range. The negative
branch interpolates already truncated quotients. Replacing either path
with floating-point frequency conversion changes guest-visible values.
`0x44E0-0x4508` sends this halfword to `libsd` export 5 with
`SD_VP_PITCH` (`0x200 | core | (voice << 1)`).

An optional differential regression executes the original routine from a
locally supplied, SHA-256-verified IRX using a bounded MIPS test interpreter.
It compares all 65,536 signed requests for each of four base pitches
(`0`, `4096`, `32769`, `65535`), including overflow and quotient boundaries.
No original instructions or tables are embedded in the test sources.
Set `ARKD_REFERENCE_IRX` to the module path before running
`test_arkd_native.py` to enable it. Ordinary synthetic tests need no IRX.

The calculation is ready for the voice worker; bank resolution, register
sequencing, and actual key-on remain unimplemented.

The original `0x39C0` and `0x3A2C` instructions separately read
two halfwords at offsets `+0x0E` and `+0x0C` of each 16-byte
voice command and update distinct per-core masks. The decoder
therefore also exposes `start_control` and `restart_control`
rather than treating their combined u32 as an uninterpreted field.

## Live voice gain recalculation (0x501/0x12, 0x13, 0x14)

Original `ARKD_DVD.IRX` routines `0x505C`, `0x5108`, and
`0x50A8` all call `0x4DBC` after changing master volume, mono-mode,
or channel volume. Previously native HLE only remembered these values;
existing SPU2 voices would retain their old gain.

The original `0x4DBC` walks **46 voice slots** and checks active
masks indexed as `voice_index / 32`. Active slots have two retained
source volume words (IRX globals `0x13258` and `0x1325A`) with 4
bytes per voice. `sceSdSetParam` (libsd export 5) receives:

- `reg = (voice_index / 24) | ((voice_index % 24) << 1)`:
  source `+10` scaled by the master and *first* channel coefficient.
- `reg | 0x100`: source `+8` scaled by the master and *second*
  channel coefficient.
- If the IRX mixing-control flag `0x10000` is set, the source pair
  is bitwise ORed for the first register and the second source is zero.

Each gain stage is a 32-bit multiplication followed by a **logical
right shift of 14** (`(source * master >> 14) * channel >> 14`).
Native HLE now performs these writes immediately for active voices
upon all three RPC control calls and checks the entire four-byte
channel request before reading either `u16` (the previous handler
could read past a truncated request).

Native HLE now retains the source gain pair independently of the two
alternating command buffers. That per-voice bookkeeping is serialized
in savestate **version 13**, with older native savestates preserving
their previous sections. `record_voice_volume_state()` provides a
bookkeeping entry point for the unported 46-voice startup worker.

**Remaining dependency:** full voice startup must call that
bookkeeping entry after a real libsd start. This change closes the
gain-control path for *tracked active voices*; it does not by itself
make the incomplete native voice-start path functional.

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
- IRX `0x3020`â€“`0x4618` consumes those arrays. Its loop iterates 46
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

Native state is serialized in a new `arkd` section, currently
**savestate version 11**, including pending file request IDs, direct
GAME asset IDs, sound buffers, loaded bank data, and in-flight SPU2
transfer state. Versions 2â€“10 remain readable with missing newer
fields initialized. Loading a native state with the original driver
selected, or vice versa, is explicitly rejected.

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
- Native queue and transfer state has versioned savestate integration.
  Accurate original-driver timing, callbacks and gameplay parity
  remain prerequisites for removing the opt-in.
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

## Game-backed startup correction

The subsequent black-screen loading stall was caused by a missing EE SIF
status notification. Original worker `0x2C90` sends the audio-worker result
to EE SREG 12 at `0x2CBC-0x2CCC` via SIF command `0x80000001`.
The load result at `0x6480-0x649C` is `0x30000000 | (result & 0xFFFF)`;
the first local sound bank produces `0x3000C270` (declared span 49,776).
Native upload completion now sends this packet after DMA completes;
upload failure also sends its error status. Completing the bank RPC alone
did not satisfy the EE's separate status check.

After this correction, a native 1,200-frame game-backed run loads menu
textures and displays the 50Hz/60Hz/TEST/To Game menu, as visually checked
against the original driver. This establishes startup/menu progress only;
voice startup, gameplay and audio parity remain unverified.

Native testing with the local EU beta initially stopped at frame 11 with a
null EE call (return address `0x001ABB24`), while the original IRX reached
the video-mode menu. The dispatch above corrects an earlier reversal of
`0x101` and `0x102`: `0x2410` writes command 1, consumed at `0x2234`
by `0x1DA4`, which DMAs decoded 48-byte index records to the EE destination.
`0x250C` writes command 2, consumed at `0x228C` by the full-file reader.
Native startup now exports index records from the merged catalog, without
requiring INFO.DAT on loose mounts. A game run reaches 1,800 frames after
these corrections; this alone does not establish menu/audio parity.
