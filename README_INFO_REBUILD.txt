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

EU beta archive compatibility (SLES_541.61)
-------------------------------------------
The SDBZ branch targets the European beta, not the US release.

The EU beta INFO.DAT uses codec key 0x7F and is 98,304 (0x18000) bytes:
one 48-byte header plus 2,047 physically allocated 48-byte record slots.
The original EU beta has 1,934 active entries and 113 encoded spare slots.
When INFO.DAT is absent, the virtual builder retains 0x7F and pads its
encoded INFO.DAT to 98,304 bytes using 0x40 for the unused slots, just
like the original archive. If more than 2,047 records are generated, the
virtual INFO grows rather than silently truncating.

When existing INFO.DAT is present and loose GAME/ files are added, the
runtime now sorts the resulting INFO records by normalized filename. The
original record fields, allocated sector offsets and GAME.DAT data remain
unchanged; only their INFO table order is modified. This is required
because ARKD_DVD.IRX builds first-letter lookup buckets from ordered
records. The EU beta's ARKD_DVD.IRX is byte-identical to the US driver's
binary, so its lookup implementation is shared.

Both EU and US executables pass file=2047 to that IRX. Static analysis
indicates a strict entry_count < 2047 validation, and a possible earlier
2,004-record boundary due to a 47-sector table read. That read boundary
has not been experimentally confirmed; the runtime logs a warning when
the table reaches 2,005 records instead of rejecting it.

Verification:
    python -m unittest discover -s tests -p "test_game_dat_loose_assets.py" -v
    python -m unittest discover -s tests -p "test_disc_folder_expansion.py" -v
    python -m unittest discover -s tests -p "test_eu_beta_virtual_info.py" -v

The runtime still does not require .ps2-disc, INFO.DAT or GAME.DAT host
files when a complete GAME/ extraction is available.
