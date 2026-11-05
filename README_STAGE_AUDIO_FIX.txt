Extract into C:\dev\ps2-web-recomp, preserving folders, and replace runtime/src/iop/spu2.cpp.
Merge instead if you have additional local SPU2 changes beyond the fixes discussed.

This file includes the earlier loop-address ordering fix, exponential envelope rounding fix,
and 64-bit master-volume multiplication, plus the new deferred ENDX/mute fix.
It uses the original narrow trace filter to avoid excessive diagnostic output.

Cause addressed:
The decoder buffers two ADPCM blocks ahead. Previously decode_block could stop a voice
when it decoded a future non-repeating end block, before render_voice retired the current
end block and set ENDX. A driver polling ENDX could then keep the sound marked as playing.
The patch caches each block's flags and applies ENDX/muting when the block is consumed.
The existing voice save-state layout is unchanged; cached flags are reconstructed on load.

Evidence from the supplied stage_stall.bin:
- CAppFightMain has child app 0x1A, named CAppFightDemoEndCard.
- That app is in update substate 1, and mode 2 skips its end-card animation setup.
- The completion check calls 0x002FCC00 with slot 0x1C, resolving to sound ID 0x11.
- Its playing bit 0x00020000 remains set in the mask at EE address 0x004FCA70.
- The IOP driver clears that playing status only when ENDX is set and the loop address
  points to its low-memory silent terminator region.

Validation:
A focused C++20 regression using the source decoder/voice-render functions and a synthetic
loop-to-silent-terminator sample fails with the original code (ENDX=0, voice off) and passes
with the fixed code (ENDX=1, voice off). Undefined-behavior sanitizer enabled for that test.
Full Windows build and actual game stage progression are not verified here.

Rebuild (Command Prompt, repository root):
  cmake --build build/native-SuperDragonBallZ --target ps2run

Clear diagnostic settings before testing:
  set "PS2_DUMP="
  set "PS2_DUMP_AUDIO="
  set "PS2_TRACE_SPU2="
  set "PS2_VU_INTERPRET="
  set "PS2_VU_CHECK="

Then restart the game and complete the same stage. SDBZ-code regeneration is not required.
