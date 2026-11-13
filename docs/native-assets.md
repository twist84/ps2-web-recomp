## Native PNG build dependency

The Windows build uses the existing zlib PNG decoder. CMake first tries
`find_package(ZLIB)` and automatically retrieves pinned zlib 1.3.1 into
`<build>/_deps/ps2_zlib-src` when the library is not installed.
The downloaded dependency is built as a static library and does not
require a system-wide zlib installation.

On first configuration with an empty build directory, an internet
connection is needed to populate `_deps`. Subsequent builds reuse the
download. To force-test the fallback:

```powershell
cmake -S . -B build-game -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE
cmake --build build-game --config Release --target SDBZ
```

The configure output must contain `GS-native PNG replacements: enabled`.
A missing dependency is now a configuration error rather than silently
disabling PNG replacement. Setting `-DPS2_FETCH_ZLIB=OFF` requires an
installed zlib.

# SDBZ native replacement asset catalog

Native replacement files live beside the extracted disc tree, in `NATIVE/`.
Unlike `GAME/`, this directory is **host-only**: it is excluded from the
synthetic ISO and never returned as guest PS2 file data.

## Naming

Keep the original logical game asset path, then append the native format:

```text
GAME/ply/p01/p01a.mdl          original game asset
NATIVE/ply/p01/p01a.mdl.png   future native texture replacement

GAME/snd/music.ams             original game asset
NATIVE/snd/music.ams.wav       future native PCM audio replacement
```

Lookup is case-insensitive and accepts PS2 backslash separators. Individual
files are limited to 64 MiB and reads are bounded. Symlinks are not indexed;
duplicate names after case-folding are rejected. Missing replacements return
no match and should use the original game asset.

The first implementation (Issue #9) provides the **host catalog and its API**:
`ps2::disc::native_assets()`, `find_native_asset()`, and
`read_native_asset()`. Indexing is enabled when native ARKD is selected
(default for SDBZ) on folder-backed discs. IDs are stable within a mount and
cleared when opening another disc.

A host-side PNG decoder is also available when zlib is found at build time:
`ps2::gs::native_texture::load_png(original_game_path, image)`. It supports
8-bit RGB/RGBA PNGs, PNG's standard row filters, CRC checking, and dimensions
up to 4096 x 4096. It returns RGBA8 pixels; malformed files and missing
replacements return false. Indexed-color and interlaced PNGs are not yet
supported. This is a **decoder API only**, not a working GS texture override.

### Multi-texture FPIX containers

Arika `.pix` assets can contain multiple XPIX/TIM2 chunks. In particular,
`dis/main_menu.pix` contains 13 atlases (indices 0–12). Native replacements
therefore use the FPIX **directory index**, not the changing GS texture
base-pointer address:

```text
GAME/dis/main_menu.pix
NATIVE/dis/main_menu.pix.000.png
NATIVE/dis/main_menu.pix.001.png
...
NATIVE/dis/main_menu.pix.012.png
```

`load_fpix_chunk_png("dis/main_menu.pix", index, image)` resolves these names.
This mapping is not yet wired into the guest FPIX loader. Atlas dimensions
and ordering must match the original resource being replaced; the PNG decoder
does not infer or alter the game's texture-coordinate mappings.

**Important:** the native renderer and native audio playback path have not
yet been connected. Adding PNG/WAV files will not change visible textures or
audible sounds until their respective consumers are implemented. Sending
PNG/WAV bytes to a PS2 `.pix`/`.ams` decoder is not supported.

## GS texture provenance diagnostics

Set `PS2_TRACE_TEXTURE_UPLOADS=1` to log host-to-local GS IMAGE uploads
alongside GS `TEX0` texture bindings. This is enabled only when explicitly
requested. It has no effect on guest VRAM, texture data, or renderer output.

Each `[gs-texture] upload` record contains an upload serial, destination
`BITBLTBUF` base pointer and format, transfer position, dimensions,
byte count, and a 64-bit FNV-1a fingerprint of the uploaded payload.
Transfers split across multiple GIF IMAGE packets are fingerprinted as one
upload. Padded packet bytes past the declared transfer extent are ignored.
Interrupted uploads are labelled `partial`. `[gs-texture] bind` records
show the `TEX0` value, buffer pointer, format and dimensions chosen for
texture sampling.

Use `scripts/analyze_sdbz_texture_log.py` to summarize a raw capture,
including Windows PowerShell UTF-16 stderr redirects:

```sh
python3 scripts/analyze_sdbz_texture_log.py gs-textures.log --output texture-report
```

The tool creates `uploads.csv` (deduplicated upload layout and content hash),
`bindings.csv` (deduplicated TEX0 bindings), `reused_destinations.csv`
(GS addresses with different payloads/layouts), and
`atlas_candidates.csv` (complete 256x256 and 512x512 uploads, suitable
for further FPIX investigation). These are observations, **not automatic
FPIX chunk mappings**.

After exporting the CSV reports, correlate complete atlas-sized uploads with
observed TEX0 configurations:

```sh
python3 scripts/crosswalk_sdbz_texture_candidates.py \
    texture-report/atlas_candidates.csv \
    texture-report/bindings.csv \
    --output texture-crosswalk
```

This writes `layout_groups.csv` and `fingerprint_candidates.csv`. The first
groups by destination/PSM/dimensions and flags layouts with multiple observed
payload fingerprints or TEX0 variants. The second preserves each content hash
and occurrence count. The crosswalk is **aggregate**: it cannot determine
which particular upload was active for a particular draw, nor identify the
source `.pix` file. An "unambiguous" layout means only one fingerprint and
one TEX0 variant were observed, not that a matching game asset is verified.

### Verified main-menu source matches (EU beta)

The provided original `dis/main_menu.pix` is a 13-entry FPIX/XPIX
container. Its picture headers put the 8-bit indexed image data at
offset `0x120` from each XPIX start (rather than the last 64 KiB/256 KiB of
the chunk). Matching the **complete raw indexed pixel data** against the
captured GS upload payload hashes found exact matches for **all 13 chunks**:
one 256×256 atlas and twelve 512×512 atlases, all PSMT8 (`0x13`).
The reference hashes are in
`docs/sdbz-main-menu-verified-fingerprints.csv` and are tied to the
original EU-beta file supplied for verification.

```sh
python3 scripts/match_sdbz_fpix_gs.py \
    GAME/dis/main_menu.pix \
    texture-crosswalk/fingerprint_candidates.csv \
    --output fpix_gs_matches.csv \
    --export-png-dir exported-menu
```

The optional PNG export uses Pillow and writes files named
`main_menu.pix.000.png` through `main_menu.pix.012.png`. Copy these to
`NATIVE/dis/` to make them discoverable by the existing host catalog.
The converter follows Arika's indexed TIM2 palette ordering and expands
PS2 alpha values 0–128 into 0–255. The exact hash comparison is on original
indexed image bytes *before* palette expansion. This validates source
payload identity, but does not yet activate native sampling: the GS renderer
still uses the game's own VRAM textures until a replacement sampling path
is implemented.

**The fingerprint is diagnostic, not an asset identity.** GS addresses are
reused and an Arika FPIX/TIM2 source may have been transformed or swizzled
before reaching GS memory. Matching FPIX directory entries to actual
uploads still requires an observed association. Never globally replace
all draws using a particular `TEX0` or base pointer.

```sh
PS2_TRACE_TEXTURE_UPLOADS=1 ./SDBZ 2>gs-textures.log
python3 -m unittest discover -s tests -p 'test_gs_texture_upload_trace.py' -v
```

## Debugging a PNG that does not appear

Native PNGs are loaded once per game run and cached. **Exit and restart
the game** after editing a PNG, and ensure the edited file—not merely a
separate export—is installed at
`disc/NATIVE/dis/main_menu.pix.001.png`. Rebuild the latest SDBZ branch.

For a diagnostic capture on Windows PowerShell:

```powershell
$env:PS2_TRACE_NATIVE_TEXTURES = '1'
.\SDBZ.exe 2> native-textures.log
```

The `[gs-native]` diagnostics identify which stage is failing:

- `no menu PNG in native asset catalog`: the edited file is absent from
  the mounted disc's NATIVE directory
- `catalog found`: an atlas was indexed
- `png chunk=001 status=loaded` or `rejected`: the replacement was
  decoded and dimension-validated or rejected
- `matched` / `resident chunk=001`: original TIM2 upload fingerprint
  established residency at the expected GS base pointer
- `gpu draw BP=2A00 ... selector=1`: the WebGPU draw chose native atlas
  001; `selector=-1` means fallback to original GS VRAM

These logs report **selection decisions**, not a guarantee of visible
output. Save the log alongside a screenshot if the draw selects a native
image but the original still appears. The GPU shader is not executed by CI.

## Native menu texture rendering (experimental)

The runtime now recognizes the **verified 13 EU-beta `dis/main_menu.pix` TIM2
indexed-image fingerprints** at the GS host-to-local upload boundary. When a
replacement exists under `NATIVE/dis/main_menu.pix.NNN.png`, the renderer
associates the PNG with the observed physical GS resident range. Subsequent
texture draws with matching BP, TBW, PSM and dimensions sample RGBA pixels
from the host replacement instead of the guest CLUT. GS VRAM, guest reads
and DMA transfers remain untouched.

Overlapping GS transfers invalidate resident mappings, including repeated
uploads into the same TBP. Missing, corrupt or dimension-mismatched PNGs use
the original texture. PNG alpha (0–255) is translated to GS alpha (0–128).
The native RGB/RGBA PNG decoder currently requires **exact dimensions**
(256×256 for chunk 000, 512×512 for chunks 001–012); higher resolution
replacement scaling remains future work.

The software renderer and WebGPU compute shader both include replacement
sampling, but **in-game fidelity and shader runtime validation still need
an EU-beta comparison**. For targeted comparison, use `PS2_GS=cpu` and
`PS2_GS=gpu` respectively, checking a recoloured test atlas in each mode.
Original game textures remain the fallback if native assets are absent.

## Validation

```sh
python3 -m unittest discover -s tests -p 'test_native_assets.py' -v
python3 -m unittest discover -s tests -p 'test_native_png.py' -v
```

The tests cover lookup, case normalization, bounded reads, guest isolation,
missing files, and a file disappearing after mounting.
