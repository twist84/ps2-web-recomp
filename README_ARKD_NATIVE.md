# Experimental native ARKD_DVD replacement (EU beta)

The `SDBZ` branch targets **SLES_541.61**, the European beta build of
Super Dragon Ball Z. The stock module `ARKD_DVD.IRX` is a PS2 IOP driver
which implements custom Arika file services above INFO.DAT and GAME.DAT.

The runtime has a new **opt-in** C++ high-level-emulation (HLE) backend.
It is **incomplete**. Do not enable it for regular gameplay yet.

## Enable / disable

On Windows (PowerShell):

```powershell
$env:SDBZ_ARKD_NATIVE = "1"
# Start SDBZ / ps2run using your usual command.
# To restore the original driver:
Remove-Item Env:SDBZ_ARKD_NATIVE
```

On Linux:

```bash
SDBZ_ARKD_NATIVE=1 ./ps2run ...    # experimental
./ps2run ...                       # normal original IRX
```

With `SDBZ_ARKD_NATIVE=1`, the IOP module loader substitutes native code
for `cdrom0:\\ARKD_DVD.IRX;1` *before reading the IRX bytes*. With the
variable unset, existing loading and emulation are unchanged.

## Implemented

- Native `ARKD_DVD` module substitution and SIF RPC registrations
  `0x500`, `0x501`, `0x502`, `0x503`.
- Uses the runtime's *virtual disc*, so a complete loose `GAME/` extraction
  works without the original physical INFO.DAT or GAME.DAT. The synthetic
  INFO.DAT is still used **internally to index assets**. Thus this is an
  IRX bypass, *not* yet a direct filesystem bypass of the archive index.
- Parses EU INFO header (`0x7F`), 48-byte records, and sector offsets;
  normalizes slash separators and ASCII case while preserving CP932
  multibyte trail bytes.
- Native bounded asset reads, including partial reads spanning DVD sectors.
- File-size RPC `0x503 / method 3`: name at request + 4, returns actual
  size and 16-byte-aligned size; returns `0x80000064` for missing files.
- Preliminary `0x500 / method 1` initialization acknowledgement.

## Not implemented — native mode can fail to boot

The original IRX also implements many service methods dealing with
asynchronous reads, buffer state, streaming and audio. Those are **not yet
emulated**. For unimplemented methods the HLE logs the server/method and
returns error `0x80000064` rather than faking a successful operation.
The installed replacement is therefore **not a complete driver** and is
not intended to run a full game session yet.

The original IRX and its RPC handler addresses (module-relative) are:

| RPC server | Handler | Purpose / current state |
| --- | --- | --- |
| `0x500` | `0x01D4` | Startup/control; only method 1 acknowledged |
| `0x501` | `0x6708` | File read / streaming commands: TODO |
| `0x502` | `0x9D38` | Auxiliary/audio streaming: TODO |
| `0x503` | `0x2700` | File size method 3 implemented; others TODO |

The EU beta IRX is byte-identical to the US IRX. The EE game passes
`file=2047`, but the native catalog does not use this artificial table
capacity bound.

## Protocol implementation progress

The latest draft includes the following **statically recovered** handlers:

| Server | Native methods | Important limitation |
| --- | --- | --- |
| `0x500` | `1`, `3`, `0x10`, `0x11`, `0x12` | Some initialization side effects remain unmapped |
| `0x501` | `1`, `2`, `3`, `0x11`, `0x12`, `0x13`, `0x14`, `0x81`, `0x82` | Added queued voice-stop `libsd #5` volume writes; voice start, bank upload and streaming still incomplete |
| `0x502` | `1`–`5`, `0x101`–`0x103` | Stream control flags and packet storage, not full SPU2 playback |
| `0x503` | `1`–`4`, `0x81`, `0x101`, `0x102`, `0x103` | File copy, CDVD initialization and virtual-disc control use approximate native timing; physical-drive errors aren't emulated |

The `0x501/0x11` handler forwards both real 16-byte `sceSdEffectAttr`
structures to **the game's own `LIBSD.IRX` export ordinal 23** via the
existing IOP guest-library call bridge. There is no need to duplicate
the SPU2 device implementation.

The `0x503/0x102` request queues a bounded file read into EE RAM and
exposes the 0x10000000/0x20000000/0x30000000 transfer phases. The
`0x503/0x101` request prepares the CDVD worker and completes a separate
queued job. The actual original-IRX timing and interrupts still require
comparison before declaring equivalence.

**Still unimplemented:** `0x501/0x101`–`0x103` sound/stream bank
workers, voice-start and complete parameter updates, exact SIF DMA/interrupt
callbacks, and physical-disc error parity. The `0x503/0x103` native
worker now handles the command queue and virtual-media completion for
opcodes 1–8, but does not reproduce mechanical CD/DVD operations.
Native pending state is serialized with version 7 savestates. The driver replacement is not yet
ready for end-to-end gameplay validation. The native module is deliberately
opt-in and PR #2 remains a draft.

The original EU executable's client bindings are:

```text
SLES_541.61 EE address 0x1BCD90:
server 0x500 -> client 0x005A55B0
server 0x501 -> client 0x005A5600
server 0x502 -> client 0x005A5628
server 0x503 -> client 0x005A55D8

EE SifCallRpc wrapper: 0x1789E8
0x503/0x101 call site: 0x1C0778 (input 0x30, reply 0x10)
0x503/0x102 call site: 0x1C0BA8 (input 0x30, reply 0x10)
```

## Additional worker progress

The original 46-voice worker checks stop requests through two **32-bit**
command masks but addresses SPU2 voices using **24 voices per core**.
Native HLE now queues the verified `libsd` ordinal 5 volume-clearing
operations and tests the distinct mask/register boundaries at voice
indices 24 and 32. Voice startup and sample data transfer are still missing.

The original `0x503/0x103` accepts a single u32 CDVD control opcode in
the range 1–8 when the transfer state is complete or errored. Native HLE
now returns a busy status when appropriate, schedules an emulated drive
operation, and exposes its completion through `0x503/2`. These are
**virtual-disc** semantics, not mechanical command parity.

## Validation

Run the synthetic native catalog and RPC test:

```bash
python3 -m unittest discover -s tests -p 'test_arkd_native.py' -v
```

Existing folder-disc regressions:

```bash
python3 -m unittest discover -s tests -p 'test_game_dat_loose_assets.py' -v
python3 -m unittest discover -s tests -p 'test_disc_folder_expansion.py' -v
```

The `arkd-native.yml` GitHub Actions workflow runs those test suites under
GCC and Clang, as well as compilation of the integrated IOP units. This is
**not a substitute** for an EU beta boot/differential run.

## Next compatibility milestone

1. Run **original** IRX with the existing `trace_iop_modules` option
   enabled and record RPC server IDs, methods, request sizes/words,
   corresponding replies and SIF DMA completions.
2. Map those commands against the four dispatchers above, with
   accurate structures, error codes, and completion timing.
3. Implement native methods incrementally; compare the same menu, character,
   stage and audio paths against the original driver before enabling by
   default.

Do not remove the original IRX path before these comparisons pass.
