Extract the contents into C:\dev\ps2-web-recomp, preserving paths and replacing the four source/configuration files.
These files are based on upstream main retrieved on 2026-10-25. Merge changes instead if you have other local edits.

Includes:
- host/window.cpp: Windows process CPU time and working-set statistics; NOMINMAX before SDL headers.
- CMakeLists.txt: link psapi on Windows.
- runtime/src/iop/libs.cpp and runtime/src/gs/gif.cpp: missing <cstdlib> includes.

From MSYS2 CLANG64, with SDL2 installed and the native build already configured:
cmake --build build/native-SuperDragonBallZ --target ps2run

Then:
build/native-SuperDragonBallZ/ps2run games/SuperDragonBallZ/disc/SLES_541.61 --disc games/SuperDragonBallZ/disc.iso --window

This package targets the SDL window with the software renderer. It does not add Windows native WebGPU support.
Source diff reviewed; compilation and execution on Windows have not been verified.
