Windows native quality-of-life update
====================================

Extract this ZIP into C:/dev/ps2-web-recomp and replace the included files.
Keep your existing ps2run.ini if you have customized its settings.
It is based on the earlier Windows WebGPU and successful jitter patches.
The SPU2 audio/stage-completion fixes and generated game code are not
included or replaced. Keep those files from your working installation.

Features
--------
SDBZ.exe        Direct runtime entry point without command-line parsing; see README_GAME_EXE.txt
Omit --disc     Read game files from the ELF's extracted folder
                See README_FOLDER_DISC.txt for full extraction instructions
F5              Cycle GPU render resolution: auto / 1x / 2x / 3x / 4x
F6              Cycle fit / integer aspect / integer pixels / fixed aspect / fixed pixels
Ctrl+2/3/4      Select fixed 2x / 3x / 4x aspect scaling directly
Ctrl+Shift+2/3/4 Select fixed 2x / 3x / 4x pixel scaling directly
F7              Toggle nearest / smooth filtering
F8              Toggle forced field blending / automatic blending (WebGPU)
F9              Toggle performance statistics in the window title
F10             Toggle mute
- / =           Lower / raise volume by 5 percent
Keypad - / +    Lower / raise volume by 5 percent
F11 / Alt+Enter Toggle desktop fullscreen

Keyboard game input is ignored while the window lacks focus. Alt+Enter
does not also send the game's Start button. Controller mappings stay as
before. This update does not add pause or save-state hotkeys.

Default settings: integer aspect scaling, nearest filtering, forced field
blending enabled, full volume. Window size, fullscreen, scaling, filtering,
blending, render resolution, automatic window resizing, statistics, mute
and volume are saved between launches.

All QoL settings are documented in the included ps2run.ini. The launcher
loads this file from the repository root and saves hotkey changes to it.
Close the game before editing it manually. SDBZ.exe automatically uses its
own extracted disc folder and local config; widescreen=0/1 controls its
presentation aspect ratio. The original ps2run utility retains command-line
controls for disc paths and diagnostics.

Config lookup order: the PS2_CONFIG environment path, then ps2run.ini in
the current working directory, then the older per-user settings file at
%APPDATA%/ps2-web-recomp/native/window.ini. The active path is printed at
startup. The bundled launcher sets PS2_CONFIG to the included file.
SDBZ.exe always uses ps2run.ini in its own folder.
Deleting an active file restores defaults on the next start unless the
fallback per-user file exists. To explicitly create a fresh default file,
point PS2_CONFIG to a nonexistent filename in an existing directory.
Scaling values: 0=fit, 1=integer aspect, 2=integer pixels,
3=fixed aspect 2x, 4=fixed aspect 3x, 5=fixed aspect 4x,
6=fixed pixels 2x, 7=fixed pixels 3x, 8=fixed pixels 4x.
render_scale: 0=follow fixed display multiplier (otherwise 1x), 1..4=override.
resize_with_scale: 1=resize the window for fixed modes, 0=keep its size.
The remaining switches use 0 or 1; volume is 0..100.

Scaling details
---------------
Fit: fill the available area at 4:3 (or 16:9 with --widescreen).
Integer aspect: whole multiples of native display height, with the width
corrected to 4:3 or 16:9 and rounded to a whole pixel at the base size.
Integer pixels: equal whole-number multipliers for both native framebuffer
dimensions; the result can be narrower or wider than 4:3.
Fixed 2x / 3x / 4x: use the selected multiplier of the aspect-corrected
native display size. By default these choices BOTH resize the window and
select matching 2x / 3x / 4x internal GPU render resolution. Window resizing
is limited to the current monitor's usable desktop. The display uses the
largest lower integer multiplier that fits if needed, while the internal
GPU resolution can still use the requested higher multiplier. Fullscreen
retains the desktop resolution. If even 1x will not fit, presentation uses
proportional downscaling.
Fixed pixel 2x / 3x / 4x: multiply both framebuffer dimensions equally,
without aspect correction, using the same fit limits. A 512x448 image
becomes 1024x896, 1536x1344 or 2048x1792 respectively if the window fits it.

For example, a 512x448 image on a 1920x1080 output uses:
fit=1440x1080, integer aspect=1194x896, integer pixels=1024x896.

Integer scaling can leave large black borders. If even 1x will not fit,
the image scales down proportionally instead of being cropped. Scaling
adapts to PAL/NTSC display dimensions and window resizing. It controls
presentation size. GPU resolution now follows fixed modes by default;
set render_scale=1..4 or use F5 to override it independently. Set
resize_with_scale=0 to retain a manually chosen window size. The legacy
PS2_SCALE environment variable is still a startup override (1..4).
Exact texel-to-screen integer ratios are obtained with integer pixels at
the native GPU render scale. Aspect correction necessarily changes the
horizontal pixel shape.

The performance title reports actual internal render dimensions and scale.
GPU scale changes are applied between frames without restarting the game;
VRAM samples are remapped and dependent GPU resources/shaders rebuilt.
A scale change can briefly pause rendering. 4x requires more GPU work than
1x; unsupported buffer sizes are rejected while the current scale is kept.
Higher internal resolution requires the WebGPU renderer.

Nearest filtering is crisp. Smooth filtering uses the existing WebGPU
sharp-bilinear filter; SDL fallback uses linear filtering.
The field-blending toggle applies to direct WebGPU presentation. The
earlier PS2_NO_DEINTERLACE switch still disables blending if present.

Build in CLANG64 from the repository root
---------------------------------------
1. Build the tools with static runtime linking:

cmake -S . -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=C:/msys64/clang64/bin/clang++.exe -DPS2_STATIC_CPP_RUNTIME=ON
cmake --build build/tools --target eerecomp vurecomp

2. Reconfigure the existing native game build and rebuild it:

cmake -S . -B build/native-SuperDragonBallZ -DPS2_STATIC_CPP_RUNTIME=ON
touch host/window.cpp runtime/src/gs/gpu/gpu.cpp runtime/src/gs/gpu/blit.wgsl
cmake --build build/native-SuperDragonBallZ --target ps2run

The existing native build cache retains the generated-code and SDL2 paths.
These commands assume the working build directories from our earlier steps.
You do not need to regenerate translated EE/VU code for this update.

Folder disc support adds runtime/src/disc_folder.cpp to the runtime target.
Reconfigure/build as above so the new source is compiled. Explicit ISO
arguments continue to work; see README_FOLDER_DISC.txt for details.

Static runtime linking is now ON by default under MinGW/CLANG64. It applies
to eerecomp, vurecomp, vujit_dump and ps2run. It can be disabled with
-DPS2_STATIC_CPP_RUNTIME=OFF. Other toolchains are unaffected.
SDL2.dll and wgpu_native.dll remain separate dependencies of the runner.

Run
---
Double-click play_SuperDragonBallZ.cmd in the repository root. It uses
saved preferences and clears the old field-blending environment overrides
within that launcher only.

Or run in CLANG64:

unset PS2_FORCE_DEINTERLACE PS2_NO_DEINTERLACE PS2_TRACE_DISPLAY
build/native-SuperDragonBallZ/ps2run games/SuperDragonBallZ/disc/SLES_541.61 --disc games/SuperDragonBallZ/disc.iso --window

PS2_FORCE_DEINTERLACE=0 or =1 is still accepted as a startup override.
Leave it unset for the saved F8 preference to control subsequent launches.
Mute continues to queue silent audio, preserving the audio pacing clock.

Validation
----------
Viewport bounds/integer-multiple tests and settings round-trip/invalid-input
tests passed under AddressSanitizer and UndefinedBehaviorSanitizer.
Window and GPU C++ syntax checks passed against official SDL2 and
wgpu-native v29.0.1.1 headers. Offscreen shader/render tests passed for
nearest/smooth filtering, field blending and black letterboxing.
Live GPU scale transition tests covered 1x/2x/3x/4x, preserved VRAM/CLUT
contents, rebuilt presentation bindings, and verified output dimensions.
The standalone GPU test used a software OpenGL adapter on the test host;
the production backend selection is unchanged. An SDL dummy-display test
also verified actual window resizing and shortcut/config overrides.
CMake's generated build graph includes -static for all four executables.
eerecomp was also successfully linked statically on the Linux test host.
vurecomp uses Clang-specific intrinsics unavailable in that host's GCC.
The complete Windows build and this game still need validation locally.

Optional developer checks (not needed to play):
g++ -std=c++20 -Iruntime/include -Ihost tests/presentation_tests.cpp -o presentation_tests
./presentation_tests
python tests/presentation_gpu_test.py
The GPU test requires Python's wgpu package and a working adapter.

qol_changes.patch contains only the changes relative to the earlier
Windows GPU package plus the successful jitter test. Full source files
are included so applying the patch manually is not required.
