Runtime INFO.DAT rebuild (from arika_tooling_sdbz)
=================================================

When INFO.DAT is absent but GAME/ exists, the runtime rebuilds INFO.DAT in
memory using the same Arika format as arika-py / arika-rs:

  key: 0x7F "www.arika.co.jp"
  header 0x30, records 0x30
  encode_info nibble-swap codec

Pipeline
--------
1. Enumerate disc files (skip GAME/, .ps2-disc/)
2. If no INFO.DAT: recursive walk of GAME/ → generate INFO + virtual GAME.DAT
3. Synthetic ISO9660
4. Overlay each GAME/ file onto GAME.DAT sectors
5. Optional host INFO.DAT path still supported for classic overlay

Host INFO.DAT is no longer required when GAME/ is a complete extraction.
