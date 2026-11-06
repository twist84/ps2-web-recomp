Running from an extracted disc folder
====================================

When --disc is omitted, ps2run now mounts the folder containing the ELF.
It reads the extracted game files directly; no ISO is opened in that mode.
An explicit --disc image.iso still takes priority. --disc folder is also
accepted, so the disc data folder can differ from the ELF folder.

Important: the original port script extracts only the ELF and selected
support files. You need ALL the disc files to play without the ISO.

One-time extraction (CLANG64, repository root)
-------------------------------------------
python3 scripts/extract_disc_folder.py games/SuperDragonBallZ/disc.iso games/SuperDragonBallZ/disc

Alternatively, use the original ISO path:
python3 scripts/extract_disc_folder.py "C:/Games/Super Dragon Ball Z (Europe) (Beta).iso" games/SuperDragonBallZ/disc

The helper extracts every file, including files over 64 MiB. It uses the
repository's existing scripts/disc.py and Python's standard library.
Keep the .ps2-disc subfolder it creates: layout.tsv records original file
sectors, and metadata/ stores the ISO directory and volume sectors.
The source ISO is only needed during extraction. Extraction rewrites the
extracted files from that ISO, so use the same game revision as your build.

Rebuild (CLANG64)
----------------
cmake -S . -B build/native-SuperDragonBallZ
touch host/main.cpp runtime/src/disc.cpp runtime/src/disc_folder.cpp
cmake --build build/native-SuperDragonBallZ --target ps2run

Run without --disc
------------------
build/native-SuperDragonBallZ/ps2run games/SuperDragonBallZ/disc/SLES_541.61 --window

Or explicitly select the extracted folder:
build/native-SuperDragonBallZ/ps2run games/SuperDragonBallZ/disc/SLES_541.61 --disc games/SuperDragonBallZ/disc --window

play_SuperDragonBallZ.cmd prefers folder mode if its preserved layout is
present. Otherwise it keeps using an existing disc.iso, falling back to
the ELF folder when the ISO is absent. Explicit launch arguments can still
override the launcher's choice.

Startup should report:
[disc] using ELF folder: games/SuperDragonBallZ/disc
[disc] folder preserves original file LBAs and ISO directory metadata

Already-extracted folders
-------------------------
Folders without .ps2-disc/layout.tsv are supported using a virtual ISO
directory and synthetic sector addresses. File lookup, nested directories
and sector reads work, but games with fixed original sector addresses may
fail in that mode. Use the extraction helper for original file LBAs.

The preserved layout verifies that its files exist and have the expected
sizes. Missing or resized files cause a clear mount error. Same-size file
mods remain possible. Sectors outside recorded files/metadata read as zero;
non-file protection data or other unusual raw sectors are not retained.
Actual game behavior still needs verification on your Windows build.

Validation
----------
Sanitized C++ tests covered case-insensitive paths, nested and multi-sector
directories, empty files, partial-sector padding and random sector reads.
An exported test ISO was extracted and mounted again, reproducing the test
ISO byte-for-byte with identical file LBAs. Missing files and invalid layout
paths were rejected. CLI tests covered ELF-folder default, current-directory
ELFs, explicit ISO overrides and explicit folder overrides. The loose archive
expansion regression tests can be run with:

  python -m unittest discover -s tests -p 'test_*.py' -v

Loose INFO.DAT / GAME.DAT archive contents
------------------------------------------
The folder reader can serve the contents of GAME.DAT from loose files under a
host-side GAME directory. Keep the original INFO.DAT at the disc root and
preserve the original .ps2-disc/layout.tsv produced by extract_disc_folder.py.
The GAME directory must mirror the paths stored in INFO.DAT, for example:

  disc/
    INFO.DAT
    GAME/ply/p01/p01a.mdl
    GAME/stg/s01/s01a.stg
    .ps2-disc/layout.tsv

The runtime decodes INFO.DAT on mount, maps each record's sector offset onto
the original GAME.DAT LBA, and overlays the matching file from GAME/. No ISO
or generated-unit changes are required. The ISO directory still exposes the
original GAME.DAT entry to the game.

If GAME.DAT is still present, loose files override matching entries and any
missing entries fall back to GAME.DAT. A present loose file must have the
byte size recorded in INFO.DAT. If GAME.DAT is removed, every INFO.DAT entry
must exist under GAME/ with its recorded byte size; unindexed gaps and unused
sector padding read as zero.

Extract the loose assets with the SDBZ archive tool:

  sdbz_tool extract INFO.DAT GAME.DAT GAME --overwrite

Then launch the ELF from the disc folder as usual. The folder must retain its
original layout metadata; a synthetic folder layout has no original GAME.DAT
LBA from which to place the archive entries.

Size-changing loose GAME replacements
--------------------------------------
Loose replacements may differ from the original INFO.DAT byte size. The
runtime recalculates byte_size, sector_count and sector_offset in a virtual,
re-encoded INFO.DAT overlay. It first uses unindexed sectors inside the original
GAME.DAT extent. If no sufficiently large contiguous extent exists, it appends
new sectors after the original ISO volume and assigns the asset a relative
GAME.DAT sector offset into that appended range.

When appended sectors are needed, the runtime also updates the ISO primary
volume descriptor's volume-space size and the GAME.DAT ISO directory record's
file size (both-endian ISO 9660 fields). The original extracted files and
layout.tsv are not modified. Existing physical disc regions retain their LBAs;
appended data starts at the old volume end, so it cannot overlap any region
recorded by the preserved layout. The virtual GAME.DAT includes the gap between
its original extent and appended data, with original GAME.DAT bytes retained as
fallback wherever they exist and loose assets overlaid at their updated offsets.

This works with GAME.DAT present (the original archive is the fallback) or
removed (all indexed assets must be present under GAME/). Growth is finite, not
unlimited: INFO.DAT sector offsets/counts and ISO volume/file-size fields are
32-bit, and the virtual volume must remain within ISO 9660's 32-bit sector and
file-size limits. Mounting fails rather than wrapping a field or overlapping
an existing region when those limits are exceeded. A contiguous extent is
required for each individual asset.

Keep INFO.DAT unchanged on disk. The modified index and ISO metadata exist only
in memory for the mounted session; they are regenerated from the original
INFO.DAT and loose file sizes each launch. Back up original assets before
editing.


Generated GAME.DAT manifest and independent discovery
------------------------------------------------------
The extractor now creates .ps2-disc/game_manifest.tsv from the decoded
INFO.DAT image. The manifest stores the complete decoded INFO.DAT bytes and
the INFO.DAT/GAME.DAT LBA and size pair from layout.tsv. Runtime mounting uses
this manifest when present and falls back to parsing physical INFO.DAT when
the manifest is absent. The physical INFO.DAT may be removed when the generated
manifest is present; the runtime supplies its virtual contents to the game.
Asset resizing still updates the in-memory INFO.DAT image and virtual overlay.

To generate the manifest for an already-extracted folder:

  python scripts/generate_game_manifest.py games/SuperDragonBallZ/disc

Independent recovery (best effort)
----------------------------------
When the manifest is lost, scripts/discover_game_manifest.py can compare loose
GAME/ files against sector-aligned bytes in the original GAME.DAT and write
.ps2-disc/game_discovery.json. This is a candidate report only: it requires the
original GAME.DAT and unchanged loose files, rejects duplicate matches, and
does not prove that GAME/ contains every archive entry. It cannot recover an
unknown INFO.DAT header template. Use the generated manifest for normal mounts.
