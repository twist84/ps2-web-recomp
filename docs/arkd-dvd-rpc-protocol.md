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
| 0x103 | 0x2608 | Reads request u32 and schedules CDVD worker command 3 |

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
