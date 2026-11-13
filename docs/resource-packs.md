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

## ZIP packs

Place a ZIP file directly in `disc/resourcepacks/`, next to `SDBZ.exe`.
Like folder packs, the archive must contain a `pack.json` at its root
and replacement files under `assets/sdbz/`. The archive name (without
`.zip`) is its pack ID for `load_order.txt`, so these entries work:

```text
+Red Menu
-HD Pack
```

ZIP and folder packs share the same priority and load-order rules. Files
are read from ZIP on demand; extraction is not required. Stored and
deflated entries are supported. Encrypted, ZIP64, multi-volume, unsafe
paths, duplicate case-insensitive entries and oversized files are
rejected. A ZIP is capped at 512 MiB and individual entries at 64 MiB.

Create a freely redistributable, pure-red example with:

```sh
python3 scripts/build_red_menu_pack.py --output "Red Menu.zip"
```

The generated archive contains only a solid `#FF0000` 1024×1024 RGBA
image, `pack.json`, and README; it contains no original game imagery.
Its full menu atlas replacement is deliberately a diagnostic, not a
preservation of alpha transparency or game artwork.

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
ignored. `resourcepacks` supports both folder and ZIP packs; a graphical pack manager
and live reload are future features. To apply edits, restart the game. Packs are enumerated at
disc startup, with active packs logged as `[resource-pack] enabled ...`.

A malformed or incompatible pack is skipped with a warning. An
individual replacement that fails PNG decoding is rejected rather
than changing guest-visible resources.

## In-game manager (experimental)

Press **F6** or **Select + Start** on the first game controller to open the
resource-pack manager; use the same shortcut again to cancel.
The simple, resolution-scaled WebGPU overlay uses the original
`5-24-2.fon` game font and a flat translucent background.

Controller-only controls:

- D-pad Up / Down selects a pack
- D-pad Left moves an enabled pack into Available (disabled)
- D-pad Right moves a disabled pack into Enabled
- A toggles the selected pack between lists
- LB / RB moves the selected enabled pack up / down in priority (requires at least two enabled packs; no wrapping at the ends)
- Start saves `disc/resourcepacks/load_order.txt` and closes
- B or Select + Start cancels unsaved changes

The manager intentionally has **no mouse controls or keyboard navigation**;
F6 remains an open/close shortcut. While
it is open, gamepad inputs are consumed and do not reach gameplay.
Keyboard input is also blocked from the game while the modal menu is
open. Existing window-level shortcuts outside the manager are unchanged.

Selections take effect after restarting SDBZ, since native PNG atlases
are cached for the duration of the game session. The overlay requires
WebGPU and the game's FFON font; pack selection is otherwise independent
of any SVG or third-party UI library.

## Next steps

- In-game pack list and order editor
- More verified FPIX/TIM2 mappings
- Shared resource resolver for audio and other asset formats
