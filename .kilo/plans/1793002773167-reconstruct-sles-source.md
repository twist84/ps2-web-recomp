# Reconstruct SLES_541.61 Source

## Goal
Reconstruct clean, portable C++ source for Super Dragon Ball Z (SLES_541.61) from the PS2 ELF, building on the existing semantic decompilation scaffold.

## Context
- Binary: `games/sdbz_decomp/game/SLES_541.61` (SHA256 verified)
- IDA decompiled reference: `decompiled_cpp/` (16,861 functions)
- Existing scaffold: `src/game.cpp`, `src/face_controller.cpp`, `include/sdbz/`

## Scope
- Port decompiled functions into clean C++ under `src/` and `include/sdbz/`
- Maintain address provenance (comment original address per function)
- Keep host memory bounded, guest pointers 32-bit
- Preserve guest structure offsets with compile-time checks

## Plan
1. Establish function priority list from `config/symbols.json` and entry chain (0x00100008 -> 0x00420160)
2. For each target function: read IDA pseudocode from `decompiled_cpp/`, clean MIPS intrinsics, rewrite as portable C++
3. Add unit tests under `tests/` for each reconstructed function
4. Build with `cmake -S . -B build -DSDBZ_BUILD_CHECKS=ON && cmake --build build`
5. Run GCC strict-warning check (-Wall -Wextra -Werror)
6. Track progress in `config/symbols.json` (mark reconstructed addresses)

## Validation
- `cmake --build build` succeeds
- Tests pass
- No compiler warnings

## Risks
- Pseudocode retains inline MIPS; manual translation required
- Unknown field semantics need research notes
- Matching claim not made (semantic reconstruction only)
