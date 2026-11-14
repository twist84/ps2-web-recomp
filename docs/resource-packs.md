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
behavior is unchanged. The menu's Start button applies changes live; manual edits to this file
outside the running game are loaded on the next game launch.

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
- D-pad Down past the last pack selects **Dump All Textures to Pack**;
  press A to export textures to a new folder pack without leaving the game

### Cross-version source texture aliases (EU Beta and Japan)

The resource-pack replacement path is **the original logical filename**, not
the source pixel hash. The original texture registry still provides GS upload
recognition: each release may have a different FNV64 fingerprint for the same
filename. To load both releases' known fingerprints, place these optional files
beside `SDBZ.exe`:

- `native_textures.eu-beta.registry` — dumped EU Beta source registry
- `native_textures.japan.registry` — dumped Japan source registry

The legacy `native_textures.registry` is also read. All three use the existing
five-column tab-separated registry format. Resource pack ZIP/folder registries
continue to be read, including those in disabled packs.

The runtime maps every (fingerprint, PSM, width, height) signature to a
canonical (logical filename, PSM, width, height) record. Identical names and
layouts share one decoded PNG and one logical native GPU atlas identity even
when their upload fingerprints differ. Different original layouts are kept
separate to validate replacement image dimensions.

A source signature associated with **more than one different logical filename**
is ambiguous, including when duplicates occur within one release. It will
deliberately fall back to original GS rendering rather than guess based on
registry ordering. Source hashes cannot uniquely recover these filenames:
resolving such cases requires extra archive/asset origin context from the
game's upload pipeline, not simply another filename lookup.

This adds no per-draw filesystem scans: the merged signature and name indices
are constructed when the source registries are loaded.

### Pure name-based texture identity (integration work)

The GS residency layer now exposes
`native_texture::register_named_upload(logical_name, bitbltbuf, trxreg, trxpos)`.
A trusted asset decoder/loader integration can register a fully uploaded
source image using its original logical filename (for example,
`dis/main_menu.pix.001`). Named identities are interned directly without
consulting `textures.registry` and survive resource-pack priority changes.
A named entry is invalidated when the destination GS memory is overwritten.

The experimental filename path uses EE texture-constructor and handle
binding observations to call this API on the GS thread. This avoids guessing
which upload belongs to an ARKD file read. Fingerprint-based matching remains
the default until the new path is tested in-game.

### Name capture from the actual EE texture constructors (research)

Disassembly of the user-provided PS2 executables establishes the following
name-preserving call chain:

| Operation | EU Beta SLES_541.61 | Japan SLPS_256.42 |
|---|---|---|
| CPixTexture::Initialize | 0x0019AA70 | 0x0019A860 |
| CTexture::Initialize | 0x0019A670 | 0x0019A460 |
| Texture-handle allocator | 0x0010E700 | 0x0010E670 |

The CPixTexture function calls the FPIX validator/chunk enumerator, formats a
per-chunk name with `%s%03d`, and creates the individual texture object.
The CTexture function has the inline texture name at `this + 0x10`, its
TIM2 source pointer in its second argument and stores the returned texture
handle at `this + 0x38`.

`PS2_TRACE_SDBZ_TEXTURE_NAMES=1` now enables **read-only** observation of
these constructor entries in the generated EE dispatcher. For a valid FPIX
or TIM2 input it prints source addresses, logical names and dimensions.
The observer does not replace game code, does not compute a pixel hash and
does not assign any GS VRAM address based on timing.

The second guest mapping is now identified: EU Beta `0x0010C3E0` and
Japan `0x0010C350` construct TEX0 from their 48-byte handle records. Their
handle tables are at `0x004FF8F0` and `0x005007F0` respectively.
At this point the game itself provides a base block (BP), pixel format
and original dimensions for the filename-associated handle. The guest TEX0
builder computes texture buffer width (TBW) from original image width
(`max(1, width / 64)`); the handle field at `+0x04` is the **CLUT base
pointer**, not TBW.

### Experimental strict name-only replacement

Set `PS2_SDBZ_NAME_ONLY=1` before starting SDBZ to enable the end-to-end
experimental path. This mode:

- Observes the game's verified texture constructors, preserving each original
  `name -> CTexture object` association
- Resolves the object's final handle and the regional handle table layout
  when the game builds its TEX0
- Resolves the game's compact constructor names through the INFO/GAME archive
  index: `main_menu001` becomes `dis/main_menu.pix.001`, with no hashes.
  Duplicate archive basenames are rejected as ambiguous rather than guessed.
- Observes actual GIF TEX0 writes (PACKED, A+D and REGLIST) as a fallback
  when generated EE code bypasses the original texture-builder entry hook.
  It splits a GIF batch only at resolved name bindings so queued GS drawing
  sees the correct texture identity after each TEX0 write.
- Publishes `name + BP + calculated TBW + PSM + dimensions` to the GS thread,
  in GIF register order without changing the game's original pixel uploads
- Associates that name with GS residency, and loads replacement PNGs from
  `assets/sdbz/textures/<name>.png`
- **Does not load native texture fingerprint registries or compute FNV hashes
  for replacement selection**; unknown or ambiguous names fall back to the
  original GS texture

Set `PS2_TRACE_SDBZ_TEXTURE_NAMES=1` as well to log constructor and
binding records for diagnosis. Indexed GS textures also require the source
CLUT base to match TEX0; changing the palette through TEX2 intentionally
falls back to original GS pixels instead of showing an unrelated PNG.

For malformed/black PNG troubleshooting, set `PS2_TRACE_NATIVE_TEXTURES=1`.
Each attempted PNG logs its canonical name, supplying pack ID, dimensions,
the number of fully transparent pixels, and opaque-black pixels. The entry is
only printed once per texture cache generation. If a pack-supplied PNG is
itself black, re-export its alpha/CLUT correctly instead of changing
fingerprint/name matching.

For an A/B comparison with the same game and resource-pack state, set
`PS2_SDBZ_SKIP_PNG=1`. This keeps named GS tracking and resource packs
enabled but renders original GS textures instead of any replacement PNG.
If the black rectangles disappear, investigate PNG sources or replacement
alpha handling; if they remain, investigate GIF/GS rendering. Unset the
variable for actual texture replacement.

### 2026-11-14 EU Beta/Japan PNG dump audit

Compared the complete user-supplied exported texture archives by canonical
filename. EU Beta has 3,400 PNGs; the Japan pack has 3,362 active PNGs
plus one disabled PNG (`dis/d03/d03t0202.pix.000.png.disabled`).
Of the 3,362 shared active names, 3,254 are byte-identical, 102 have
different pixel data at matching dimensions, and six use different
dimensions. EU Beta has 38 additional active files (37 unrelated to the
disabled Japanese file); 79 of the same-dimension differences modify alpha.

The offending Sofdec/ADX logo image `dis/d03/d03t0202.pix.000.png` is
**byte-for-byte identical** in the EU Beta dump and disabled Japan copy
(SHA-256 `738242b92c15fb33e20c509b4cfdda62297638451f1ac5f5b72a5d7fc39b77df`).
The black background is present in both exported files. Therefore simply
re-exporting Japanese PNGs or rejecting this filename globally cannot fix
the underlying runtime substitution. Source-name ownership must be revoked
when guest texture handles are destroyed or reused, and GS texture/CLUT
residency must be invalidated for uploads touching swizzled pages.

The runtime includes conservative GS page-aware and palette invalidation
and destructor-driven name revocation. Keep the source's `.png.disabled`
extension until real-game testing confirms correct behavior after those
changes. Neither the file contents nor a hardcoded filename special case
is changed by this runtime fix.

For finer isolation without disabling other PNG replacements, set
`PS2_SDBZ_SKIP_PNG_NAMES` to a comma-separated list of canonical names.
For example:

```bat
set PS2_SDBZ_SKIP_PNG=
set PS2_SDBZ_SKIP_PNG_NAMES=dis/main_menu.pix.002,dis/main_menu.pix.003
SDBZ.exe
```

The named entries use the original GS pixels; all other enabled pack PNGs
still load normally. Reset `PS2_SDBZ_SKIP_PNG_NAMES=` after testing. This
is intentionally an exact-path diagnostic, not a runtime hash or heuristic
black-pixel filter.

This mode is intentionally **opt-in and unmerged** pending in-game tests.
Because GS uploads are asynchronous and guest handles can be recycled, validate
rendered replacements (including reloads/VRAM overwrites) before choosing it
as the default. The older fingerprint path remains the default while this
draft is under review.

### Data-driven texture identities

The C++ dumper now writes `assets/sdbz/textures.registry` alongside exported PNGs.
Each tab-separated registry record contains:

```text
FNV64(hex)    PSM(decimal)    width    height    logical texture name
```

(Fields are separated by literal tab characters.) Registry fingerprints
describe **original PS2 source pixel payloads**, not edited PNG contents.
At runtime, completed GS texture uploads are matched by fingerprint,
pixel format and dimensions, then resolved to their original logical
asset name (without the final `.png` suffix). All installed pack registries are read, including disabled packs,
so enabling a pack later does not depend on another texture upload.

The formerly compiled-in 13-entry main-menu hash table is removed.
`runtime/data/native_textures.registry` is copied beside `SDBZ.exe` as a
compatibility dataset for older packs that do not contain a registry.
Changing pack order or disabling a pack retains verified GS residency.

WebGPU maintains an adapter-limited pool of physical native texture slots;
logical registry entries are uploaded on demand and old slots are evicted
only after pending draws have been submitted. Supported native images must
fit within 1024x1024 and be 1x or uniformly 2x the game's original
dimensions; unsupported images fall back to the original GS texture.
Identical signatures assigned to different asset names are treated as
ambiguous and not substituted.

The dump action uses native C++ to read both loose `GAME/` and indexed
`INFO.DAT` + `GAME.DAT` directly (preferring the archive when both are
available). It writes an editable `GAME Texture Dump YYYY-MM-DD HH-MM-SS`
folder under the executable's `resourcepacks/`, using the machine's local
clock. The generated `pack.json` **name** field uses exactly the same
folder name, so the timestamp also appears as the pack's display name.
If two dumps share a timestamp, a numbered suffix avoids overwriting
previous dumps. Supported FPIX/XPIX/TIM2 images become PNGs; the
`dump_report.json` records which assets could not be decoded. Extraction
runs on a worker thread and the FFON menu displays a completion summary.
No Python installation, script process, or other executable is required.

The manager intentionally has **no mouse controls or keyboard navigation**;
F6 remains an open/close shortcut. While
it is open, gamepad inputs are consumed and do not reach gameplay.
Keyboard input is also blocked from the game while the modal menu is
open. Existing window-level shortcuts outside the manager are unchanged.

Pressing Start saves the pack selection and **applies native PNG changes
during the next GS render update**, without restarting SDBZ. The render
thread finishes pending GPU draws, updates the resource resolver, refreshes
the native PNG cache, and re-uploads replacement images for already-known
GS texture identities. The game keeps its original GS VRAM contents and
scene state. Disabling all packs restores the game's original GS texture
sampling. Previously matched source textures remain eligible when packs
are re-enabled, even without a new upload by the game.

The overlay requires WebGPU and the game's FFON font; pack selection is
otherwise independent of any SVG or third-party UI library. ZIP and folder
pack selections both use this same live reload path.

## Next steps

- In-game pack list and order editor
- More verified FPIX/TIM2 mappings
- Shared resource resolver for audio and other asset formats
