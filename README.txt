Optional field-blending diagnostic for the Windows WebGPU build

Extract into C:/dev/ps2-web-recomp, replacing runtime/src/gs/gpu/gpu.cpp.
This file includes the earlier Windows WebGPU surface changes.
Other source files (including the SPU2 fixes) are not included.

In CLANG64, from the repository root:

touch runtime/src/gs/gpu/gpu.cpp
cmake --build build/native-SuperDragonBallZ --target ps2run
unset PS2_NO_DEINTERLACE PS2_TRACE_DISPLAY
export PS2_FORCE_DEINTERLACE=1
build/native-SuperDragonBallZ/ps2run games/SuperDragonBallZ/disc/SLES_541.61 --disc games/SuperDragonBallZ/disc.iso --window

Choose 60 Hz in the game and compare the vertical movement.
The console should say: [gpu] forced field blending enabled

To disable the diagnostic, close the game and run:
unset PS2_FORCE_DEINTERLACE
Then launch it again. No rebuild is required to toggle it.

This averages the current and previous images. It can soften motion or
introduce ghosting. It is a diagnostic workaround, not a verified correction
to GS field alignment. It does not change game timing or framebuffer height.
Full Windows/game validation must be performed on your machine.
