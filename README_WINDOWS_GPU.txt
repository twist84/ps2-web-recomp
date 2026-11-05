PS2 WEB RECOMP: WINDOWS CLANG64 GPU CHANGES

INSTALL
Extract this ZIP directly into C:\dev\ps2-web-recomp, preserving folders.
Replace the included source files. Back up or merge any additional local changes first.
This package contains full replacement files based on upstream main fetched on 2026-10-25.
It includes the earlier Windows fixes; the previous ZIP is not required.

BUILD (MSYS2 CLANG64)
Install SDL2 if needed:
  pacman -S --needed mingw-w64-clang-x86_64-SDL2

From the repository root:
  bash scripts/build_windows_gpu.sh SuperDragonBallZ

This downloads wgpu-native v29.0.1.1 Windows x86-64 GNU if its files are absent,
configures the existing native build and copies wgpu_native.dll beside ps2run.exe.
Keep the MSYS2 CLANG64 terminal open when launching; its PATH provides toolchain and SDL DLLs.
Generated game sources must already exist. No ISO or game code is included.
The third-party binary download requires network access and is not bundled in this ZIP.

PLAY (MSYS2 CLANG64)
  unset PS2_ONE_THREAD PS2_NO_VU_THREAD PS2_SDL_RENDERER PS2_GS PS2_GS_COMPARE PS2_GPU_FALLBACK PS2_GPU_LOW_POWER
  build/native-SuperDragonBallZ/ps2run games/SuperDragonBallZ/disc/SLES_541.61 --disc games/SuperDragonBallZ/disc.iso --window

Successful direct GPU presentation prints:
  [window] presenting through WebGPU

CONTENTS
- CMakeLists.txt: Windows GNU DLL import linking, psapi, automatic runtime DLL copy.
- host/window.cpp: Windows process statistics, NOMINMAX, SDL HWND/HINSTANCE forwarding.
- runtime/include/ps2/system.hpp: Win32 window target.
- runtime/src/gs/gpu/gpu.hpp: matching Win32 surface target.
- runtime/src/gs/gpu/gpu.cpp: WebGPU Windows HWND surface creation.
- runtime/src/iop/libs.cpp and runtime/src/gs/gif.cpp: missing cstdlib headers.
- scripts/cxx.sh: resolve the actual Windows compiler filename, including .exe.
- scripts/build_windows_gpu.sh: dependency download and native GPU build.
- windows_gpu.patch: alternative unified patch for the eight upstream files.

PATCH ALTERNATIVE
Use the full replacement files OR windows_gpu.patch, not both.
The patch is based on unmodified upstream files; it also contains the earlier Windows changes.
The new build_windows_gpu.sh is provided separately and is not included in that patch.

VALIDATION
Source diff and shell syntax checked; ZIP integrity checked.
Windows compilation, DLL loading, GPU initialization and game performance have not been verified.
This adds Windows GPU plumbing; game/runtime issues may still require further work.
