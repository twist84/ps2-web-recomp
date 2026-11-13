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

**The fingerprint is diagnostic, not an asset identity.** GS addresses are
reused and an Arika FPIX/TIM2 source may have been transformed or swizzled
before reaching GS memory. Matching FPIX directory entries to actual
uploads still requires an observed association. Never globally replace
all draws using a particular `TEX0` or base pointer.

```sh
PS2_TRACE_TEXTURE_UPLOADS=1 ./SDBZ 2>gs-textures.log
python3 -m unittest discover -s tests -p 'test_gs_texture_upload_trace.py' -v
```

## Validation

```sh
python3 -m unittest discover -s tests -p 'test_native_assets.py' -v
python3 -m unittest discover -s tests -p 'test_native_png.py' -v
```

The tests cover lookup, case normalization, bounded reads, guest isolation,
missing files, and a file disappearing after mounting.
