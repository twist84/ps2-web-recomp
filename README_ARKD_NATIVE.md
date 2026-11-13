# Native ARKD_DVD HLE — EU beta (work in progress)

This branch targets **SLES_541.61**, the European beta of Super Dragon
Ball Z. The game's original IOP module, `ARKD_DVD.IRX`, implements
custom file and sound services through SIF RPC servers `0x500–0x503`.

**Status: draft, opt-in, not ready to replace the original IRX.**
Passing synthetic unit tests and compiling the runtime do not establish
successful boot or gameplay parity with the EU beta game.

## Selection

By default, the original `ARKD_DVD.IRX` is still loaded. To exercise
the experimental native IOP module, run from PowerShell:

```powershell
$env:SDBZ_ARKD_NATIVE = "1"
# Launch the game as usual.
Remove-Item Env:SDBZ_ARKD_NATIVE  # restore original IRX
```

On Linux: `SDBZ_ARKD_NATIVE=1 ./ps2run ...`

The IOP loader substitutes the HLE before reading the original IRX file.
It registers the original four server IDs. Unsupported methods are logged
and return explicit failures rather than fabricated successful results.

## File service and disc backend

The native HLE reads directly from extracted `GAME/` on folder-backed
discs, **without requiring synthetic INFO.DAT or GAME.DAT**. Direct
assets are canonicalized, case-normalized with CP932-aware separator
handling, sorted into stable host-file IDs, and accessed through bounded
host-file reads. Synthetic regression coverage includes **2,100 assets**
without an INFO index, beyond the original IRX's entry limit.

For ISO-backed discs without a loose `GAME/` directory, the native file
catalog can still parse the encoded `INFO.DAT` index and read GAME.DAT
sectors through the existing virtual disc layer. The original IRX path
remains available for differential comparison.

Native `0x503` handlers include file sizes, queued full-file reads
to EE memory, polling and cancellation, virtual CDVD initialization
and control, and diagnostic handling. CDVD mechanical operations and
some exact error codes/timing are modeled approximately.

## Sound service — implemented portions

- `0x501/0x101`: request bank by ID and filename, asynchronously
  preload its bytes from `GAME/` or indexed GAME.DAT.
- Read and validate the first 2,048-byte bank header: entry count at
  `+0x04`, declared SPU memory span at `+0x08`, and 8-byte entry
  records starting at `+0x0C`.
- Bounded native bank cache (64 MiB maximum), with saved pending and
  completed state.
- Allocate SPU2 space beginning at `0x5050`, using 64-byte alignment
  and reusable gaps. An individual bank never extends past 2 MiB.
- Copy payload into persistent 32 KiB IOP staging memory and invoke
  the *guest game's* `libsd` export #17 (`sceSdVoiceTrans`) with
  the fifth MIPS o32 stack argument. Alternate DMA channels by chunk,
  poll `libsd` #19 for completion, and retain source memory until
  it finishes.
- `0x501/0x102`: selected bank release. Native implementation leaves
  other banks at stable SPU2 addresses rather than replicating the
  original driver's compaction.
- `0x501/0x103`: re-upload the cached selected bank through the same
  SPU2 DMA path.
- `0x501` command buffers and the verified subset of the 46-voice
  worker, including mute writes through `libsd` #5.
- `0x501/0x11`: effect-attribute forwarding through `libsd` #23.
- `0x502`: selected stream-state and command-buffer operations.

Queued IOP events and native bank/transfer state are saved in the
**version 11** savestate format. Loading older states clears newer
native-only fields. A savestate captured with one backend cannot
be loaded with the other backend selected.

## What still blocks making native mode the default

The driver is **not complete** and full-game equivalence has **not**
been demonstrated:

1. Implement the complete 46-voice start/update, key-on/key-off,
   pitch/volume and sample-address state machine, not just mutes.
2. Complete `0x501` and `0x502` streaming/audio behaviour, hardware
   callbacks, interleaving and underrun/error handling. The current
   subset is not proof of correct gameplay audio.
3. Compare SIF RPC return packets, DMA timing and completion ordering
   against the original IOP driver using the **real EU beta game**.
4. Run and inspect boot, title/menu, character select, stage load,
   fight, streaming audio, and repeated sound-bank load/unload cycles.
5. Resolve any differences and ensure save/load and missing-file
   behaviour preserve the game's expectations.

Do not remove the original driver or change the default until these
tests pass. The optional environment toggle is intentional protection
for unverified behaviours, not a permanent design requirement.

## Latest compatibility fixes

- The native catalog merges original `INFO.DAT/GAME.DAT` entries
  with loose `GAME/` replacements, preserving untouched archive files.
  Pure loose mounts still work without INFO or the old entry limit.
- Live SPU2 allocations cannot be silently moved when a larger bank is
  loaded into the same slot: that would invalidate active voice addresses.
- The native 46-voice mute worker now also calls `libsd` export #7
  (`sceSdSetSwitch`) with `SD_S_KOFF` (`0x1600 | core`) and a
  per-core voice mask. Muting volume alone does not stop playback.
- A bounded multi-bank source cache supports reuploading older bank
  IDs after newer loads. Only admitted uploads update the cache, so
  failed growth leaves the previous source intact. Version **12**
  savestates preserve the bank cache, with a 128 MiB total limit.
- `SDBZ_TRACE_ARKD_RPC=1` enables diagnostics for the first 48
  request bytes of RPC servers `0x500`–`0x503` under either the
  original IRX or native implementation.

**Native mode is still opt-in.** Full voice start, stream interrupts,
original DMA/callback timing, and EU beta gameplay/audio parity have
not been established. Passing synthetic C++ CI does not prove those
guest-visible behaviours match the original IRX.

## Tests

```sh
python3 -m unittest discover -s tests -p 'test_arkd_native.py' -v
python3 -m unittest discover -s tests -p 'test_game_dat_loose_assets.py' -v
python3 -m unittest discover -s tests -p 'test_disc_folder_expansion.py' -v
```

GitHub Actions workflow `.github/workflows/arkd-native.yml` runs
native regression tests with GCC and Clang, and builds the full
`ps2runtime` library with Clang (the existing VU implementation uses
Clang-only intrinsics).

See [docs/arkd-dvd-rpc-protocol.md](docs/arkd-dvd-rpc-protocol.md)
for original module offsets and ongoing differential research.
