# SDBZ resource packs (folder-pack v1)

Resource packs are an optional host-only overlay. Original game assets
and GS VRAM remain unchanged. A verified GS fingerprint still identifies
the original FPIX chunk; the pack manager only chooses replacement files.

## Installing packs

Since `SDBZ.exe` lives inside `disc/`, put loose packs under
`disc/resourcepacks/` next to that executable. The runtime locates the
executable directory directly, so the working directory does not matter.
For development layouts, the location beside `disc/` is still supported
if there is no executable-adjacent `resourcepacks` directory:

```text
games/SuperDragonBallZ/
  disc/
    SDBZ.exe
    NATIVE/dis/main_menu.pix.001.png
    resourcepacks/
      Red Menu/
        pack.json
        assets/sdbz/textures/dis/main_menu.pix.001.png
      HD Pack/
        pack.json
        assets/sdbz/textures/dis/main_menu.pix.002.png
```

`pack.json`:

```json
{
  "pack": {
    "format": 1,
    "name": "Red Menu",
    "description": "Menu texture replacements",
    "game": "sdbz",
    "region": "eu-beta",
    "priority": 100
  }
}
```

Only `game: "sdbz"`, `format: 1`, and optional `region: "eu-beta"`
are accepted. `name`, `description`, and integer `priority` are
optional; priority defaults to zero. This initial parser accepts simple
unescaped JSON strings and integer fields; escaped strings and nested
extra fields are not yet supported.

The highest numeric priority wins for a particular logical file. Equal
priorities use the folder name as a stable tie-breaker. Packs are
sparse: if a pack lacks a texture, resolution falls through to lower
priority packs. If no pack supplies it, `disc/NATIVE/` remains the
legacy fallback. If neither exists, the original GS texture is sampled.

## Enable, disable and reorder packs

Create `disc/resourcepacks/load_order.txt` alongside the pack folders.
Entries are evaluated **top to bottom**; the first enabled pack takes
precedence for files it contains, regardless of its manifest priority:

```text
# Topmost enabled pack wins
+Red Menu
+HD Pack
-Experimental Pack
```

`+` explicitly enables a pack; `-` disables it without deleting the
folder. Use the **exact folder name** after the prefix. Packs not listed
remain enabled and follow the listed packs, ordered by their numeric
`pack.json` priorities. Duplicate entries and invalid lines are ignored
with a warning. The file is optional: without it, the old manifest-priority
behavior is unchanged. Restart SDBZ after editing this file.

Files in `assets/sdbz/` are addressed by logical keys, currently
`textures/dis/main_menu.pix.001.png`. A pack does not need GS
fingerprints; the runtime's verified mapping establishes texture identity.

Pack paths cannot be absolute, contain traversal components, or escape
their pack directory through symlinks. Resources exceeding 64 MiB are
ignored. `resourcepacks` contains folder packs only in v1; ZIP archives,
a graphical pack manager and live reload are future
features. To apply edits, restart the game. Packs are enumerated at
disc startup, with active packs logged as `[resource-pack] enabled ...`.

A malformed or incompatible pack is skipped with a warning. An
individual replacement that fails PNG decoding is rejected rather
than changing guest-visible resources.

## Next steps

- ZIP/pack archive support
- In-game pack list and order editor
- More verified FPIX/TIM2 mappings
- Shared resource resolver for audio and other asset formats
