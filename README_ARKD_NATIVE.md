# Native ARKD_DVD HLE — EU beta (work in progress)

This branch targets **SLES_541.61**, the European beta of Super Dragon
Ball Z. The game's original IOP module, `ARKD_DVD.IRX`, implements
custom file and sound services through SIF RPC servers `0x500–0x503`.

**Status: native by default for this game, with an original-driver fallback.**
Real-game runs have reached gameplay and played streaming music, but exact
audio and timing parity remains under investigation.

## Selection

The native IOP module is selected by default. To run the original driver
for comparison, use PowerShell:

```powershell
$env:SDBZ_ARKD_NATIVE = "0"
# Launch the game as usual.
Remove-Item Env:SDBZ_ARKD_NATIVE  # restore native mode
```

On Linux: `SDBZ_ARKD_NATIVE=0 ./ps2run ...` selects the original IRX.

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

Queued IOP events, bank transfers, multi-bank source caches, and
per-voice source gains and pending silent-loop redirects are saved in
**version 16** savestates. Older
versions retain their original layouts; native-only fields introduced
later are reset safely during restoration. A savestate captured with one backend cannot
be loaded with the other backend selected.

## Remaining native-mode validation

The driver is **not complete** and full-game equivalence has **not**
been demonstrated:

1. Implement the complete 46-voice start/update, key-on/key-off,
   pitch/volume and sample-address timing against the original worker.
2. Complete `0x501` and `0x502` streaming/audio behaviour, hardware
   callbacks, interleaving and underrun/error handling. The current
   subset is not proof of correct gameplay audio.
3. Compare SIF RPC return packets, DMA timing and completion ordering
   against the original IOP driver using the **real EU beta game**.
4. Run and inspect boot, title/menu, character select, stage load,
   fight, streaming audio, and repeated sound-bank load/unload cycles.
5. Resolve any differences and ensure save/load and missing-file
   behaviour preserve the game's expectations.

The original driver remains selectable for differential comparison while
these checks are completed.

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
- RPC `0x501/0x12`, `0x13`, and `0x14` now recalculate
  live SPU2 voice gains, matching the original IRX's two-stage
  14-bit scaling and per-core register mapping. The `0x14` handler
  rejects truncated four-byte stereo control packets.
  Per-voice source gains are persisted in version 13 savestates.
  The native new-sample path now records source gains before key-on.
  End monitoring now reads libsd ENDX/loop addresses, performs the original
  one-time silent-loop redirect and reports active voices through EE SREGs
  14/15. Version 14 preserves pending redirects. Callback timing and full
  playback equivalence still need verification.
  Live pitch/gain updates now use the verified flag-prefix/record layout,
  and starts update both effect masks while preserving other voices.
- `SDBZ_TRACE_ARKD_RPC=1` enables diagnostics for the first 48
  request bytes of RPC servers `0x500`–`0x503` under either the
  original IRX or native implementation.
- Native pitch interpolation now preserves the original integer divisions
  and halfword wrapping. Synthetic boundary tests run without game data;
  setting `ARKD_REFERENCE_IRX` to the original EU beta module enables an
  exhaustive comparison against its MIPS routine (262,144 cases).
  The queued new-sample path now uses this helper after resolving the
  selected cached bank and sample entry to its allocated SPU2 address.
  It writes address, pitch, envelope defaults and gains before issuing
  per-core key-on masks. Synthetic tests check core-1 selection, pitch,
  address, invalid-bank rejection and one-time command consumption.
  This subset does not establish complete playback or gameplay parity.

Native mode is now the default. Stream interrupts, original DMA/callback
timing, and EU beta gameplay/audio parity still need comparison with the
original IRX. Passing synthetic C++ CI does not prove those guest-visible
behaviours match.

The native lifecycle now invokes guest `sceSdInit` and reproduces the
original default master volumes, routing, voice envelopes and effect setup.
Effect RPC descriptors are expanded from 16 bytes to the 20-byte libsd
structure with the core word prepended. A game-backed 6,000-frame capture
now contains mixed audio and reaches combat; it is not a listening-based
fidelity comparison. Fresh-process loading of a native version-14 menu
savestate also resumes, with RPC callbacks rebound without resetting SPU2.
A version-15 real-game stream savestate resumed for 1,200 frames in a fresh
process, loading more banks and starting subsequent streams. A separate
300-frame run resumed that snapshot with no selection variable set.

The native sound initializer also uploads the original four silent ADPCM
blocks at SPU address `0x5010`. One-shot voices redirected there now stop at
the flagged silence block. Without the upload, they ran into sound-bank data
and caused repeated, overlapping effects. A fresh 6,200-frame game run
confirmed those voices end at `0x5010` while music streaming continues.
One-shot starts install that loop address before key-on. This matters because
SPU2 decodes ahead: waiting for the next vblank could already queue data past
the sample end, most noticeably during level-intro speech and win audio.
Bank and AMS uploads wait for that initial silence DMA to finish. Stream
startup also tolerates a short interval before SPU2 reports a valid playback
address. The desktop launcher now reports the selected SDL audio driver.
Loading an older native savestate repairs the same reserved SPU silence
region after SPU2 restoration. Games that never start `ARKD_DVD` save an
inactive ARKD state, so enabling native mode by default does not make their
savestates depend on this game's asset catalog.
Version 16 can submit the two AMS stereo DMA channels concurrently, then
waits for both before reusing their IOP source buffer. Version-15 in-flight
stream states are translated when loaded.

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
