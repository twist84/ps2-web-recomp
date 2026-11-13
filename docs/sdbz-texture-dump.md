# Dumping SDBZ GAME textures into a resource pack

`scripts/dump_sdbz_game_textures.py` exports **source assets**, not textures
observed in the renderer. It reads either every loose texture asset in a
`GAME/` tree or the entries indexed by the original **INFO.DAT** directly
from **GAME.DAT**, without extracting the archive first.

The original `GAME.DAT` has no usable standalone asset-name index: pass its
matching `INFO.DAT` alongside it.

## Directly from the original archive

```powershell
python scripts/dump_sdbz_game_textures.py --info "disc/INFO.DAT" --game-dat "disc/GAME.DAT" --output "disc/resourcepacks/GAME Texture Dump"
```

## From loose GAME/ files

```powershell
python scripts/dump_sdbz_game_textures.py --game-dir "disc/GAME" --output "disc/resourcepacks/GAME Texture Dump"
```

Both inputs produce the same layout:

```text
GAME Texture Dump/
  pack.json
  dump_report.json
  assets/sdbz/textures/dis/main_menu.pix.001.png
  ...
```

The exporter walks all indexed assets (or all loose files) and attempts to
decode texture-bearing `.pix`, `.tim2`, and `.xpix` files. FPIX chunks are
written with **their original zero-based index** as `.NNN.png`. TIFF, other
proprietary texture containers, multi-image or unsupported TIM2 variants,
and corrupted textures are **not** silently counted: unsupported candidate
files appear in `dump_report.json` as failures.

The script currently supports indexed FPIX images and RGBA/indexed
single-image TIM2; extraction of additional texture formats requires decoder
work. `dump_report.json` lists the precise exported PNG names and failures.
An exit status of **2** means some candidate textures failed, whereas **0**
means all supported candidates decoded. This does not mean every unknown
asset type is guaranteed to be a texture.

This generates an editable **folder pack**; the output directory must not
already exist. The exporter does not copy raw `GAME.DAT` entries into a
temporary extracted directory. There are no Python dependencies beyond the
standard library.

**Important:** The resource-pack resolver can find PNG files by logical path,
but the current native texture replacement implementation only recognizes
verified menu texture fingerprints for live GS replacement. Exported PNGs
for other files are usable source material for mod authors; they will become
active replacements only after their original texture identities are mapped.
This is the offline exporter backend; the in-game Dump option is a separate
integration step.
