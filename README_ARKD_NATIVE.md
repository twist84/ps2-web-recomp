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
