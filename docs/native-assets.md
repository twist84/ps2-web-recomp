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

**Important:** the native renderer and native audio playback path have not
yet been connected. Adding PNG/WAV files will not change visible textures or
audible sounds until their respective consumers are implemented. Sending
PNG/WAV bytes to a PS2 `.pix`/`.ams` decoder is not supported.

## Validation

```sh
python3 -m unittest discover -s tests -p 'test_native_assets.py' -v
```

The tests cover lookup, case normalization, bounded reads, guest isolation,
missing files, and a file disappearing after mounting.
