# SDBZ runtime-generated disc layout

The extracted disc directory is the only input to the folder-backed runtime. No
`.ps2-disc` directory, layout TSV, manifest TSV, or recovery template is read or
created.

At mount time the runtime scans the extracted files, builds an ISO9660 primary
volume descriptor, path tables, directory records, and file extents in memory.
The loose `GAME/` directory is deliberately excluded from the generated ISO;
it supplies overrides for the files indexed by the physical `INFO.DAT`.

`ARKD_DVD.IRX` calls `sceCdSearchFile` for `\INFO.DAT;1` and `\GAME.DAT;1`.
The generated ISO directory records therefore provide the dynamically assigned
LSNs. The archive entries in `INFO.DAT` contain offsets relative to the LSN
returned for `GAME.DAT`, so the runtime can choose a new GAME.DAT extent without
preserving the original absolute LSN.

`GAME.DAT` is assigned the final file extent in the generated volume. This
allows growth sectors to be appended immediately after it without overlapping
unrelated extracted files. If the physical `GAME.DAT` is absent, a virtual
extent is sized from the largest indexed INFO.DAT entry; this mode requires a
complete loose `GAME/` extraction. `INFO.DAT` itself must remain present because
it provides the original filenames, record count, archive offsets and opaque
record bytes.

Build and run the native tests with a C++20 compiler available as `clang++`,
`g++`, or `c++`. Set `CXX` to an explicit compiler path when it is not on PATH.

```powershell
$env:CXX = 'C:\msys64\clang64\bin\clang++.exe'
python -m unittest discover -s tests -p 'test_game_dat_loose_assets.py' -v
python -m unittest discover -s tests -p 'test_disc_folder_expansion.py' -v
```
