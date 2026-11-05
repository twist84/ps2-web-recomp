Direct SDBZ.exe entry point
===========================

SDBZ.exe is the same compiled runtime and translated EE/VU code as ps2run,
linked to host/game_main.cpp instead of the command-line host/main.cpp.
There is no wrapper, child process, archive, unpacking or runtime cache.

Build (CLANG64, repository root)
-------------------------------
cmake -S . -B build/native-SuperDragonBallZ
cmake --build build/native-SuperDragonBallZ --target SDBZ

For the standard port layout, the output is written directly to:
games/SuperDragonBallZ/disc/SDBZ.exe

CMake copies SDL2.dll and wgpu_native.dll there when it locates them. These
are the same shared libraries needed by the existing native ps2run build.
Keep them beside SDBZ.exe. Static C++ runtime linking also applies to SDBZ.

If the standard games/<generated-folder-name>/disc folder does not exist,
the executable stays in the build directory. You can explicitly select its
destination using -DPS2_GAME_OUTPUT_DIR=C:/path/to/extracted/disc.
SDL2 must be enabled for the SDBZ target to be available.

Launch
------
Put SDBZ.exe beside the extracted ELF, SYSTEM.CNF, all disc files and the
.ps2-disc metadata directory, then double-click it. No arguments are used.

The new main reads BOOT2 from SYSTEM.CNF to find the game ELF. If there is
no boot entry, it selects a single nearby ELF32 MIPS executable (ET_EXEC),
ignoring relocatable IRX modules. Missing/ambiguous boot files produce an
error message. It uses SDBZ.exe's own folder regardless of the terminal or
shortcut working directory, and mounts that folder as the disc source.

Settings and runtime behavior
-----------------------------
ps2run.ini is read/written beside SDBZ.exe. Keep your existing customized
file there. Fullscreen, scaling, filtering, field blending, statistics and
volume continue to use the existing config and hotkeys. The widescreen key
selects the presentation aspect ratio: 0=4:3, 1=16:9.

SDBZ.exe starts windowed presentation automatically, with fullscreen taken
from the saved config. Console logging remains available for diagnostics.
The original ps2run target remains the command-line utility used by the
port/exploration scripts. Both targets share the same game/runtime code.

Validation
----------
Tests covered BOOT2/BOOT precedence, CRLF/quoted paths, nested boot paths,
missing files, ambiguous executables and rejection of IRX boot modules.
The Windows x64 game_main.cpp compilation was checked against the actual
runtime headers. Config round-trip tests cover the widescreen setting.
The full game still needs to be built and launched on your Windows machine.
