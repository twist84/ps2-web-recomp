# PS2 Recomp Rebuild — Architecture and Migration Plan

Status: **proposal / first audit**, not an assertion that the rewrite is implemented.
Target baseline: `twist84/ps2-web-recomp` branch `SDBZ`, commit
`985c0ac15f2622199f90b206bac3db89268e2f22` (2026-11-15 review).
Working branch: `rewrite/architecture-foundation`.
First compatibility target: **Super Dragon Ball Z, EU beta (`SLES_541.61`)**.
Keep both the native runner and the browser frontend as supported long-term paths.
No game binaries, generated translations, or copyrighted assets belong in this repo.

## 1. What is being rebuilt

The present source tree is a general-purpose PS2 static recompiler with
substantial SDBZ-specific additions:

| Existing location | Responsibility | Migration direction |
| --- | --- | --- |
| `recomp/` | ELF parsing, R5900 analysis and C++ code generation, VU recompilation | Standalone backend(s), with explicit guest ABI |
| `runtime/src/` | EE state, dispatch, hardware, IOP, GS, SPU2, kernel and disc | Core and device libraries with bounded public interfaces |
| `host/`, `web/` | Native/SDL2 runner, direct SDBZ.exe, browser runner | Thin independent frontends; no game logic |
| `runtime/src/disc_folder.cpp` | Folder-backed virtual ISO / loose archive support | Reusable storage service |
| `runtime/src/iop/arika_dvd.cpp` | SDBZ-specific replacement for ARKD_DVD.IRX | Optional SDBZ game adapter, original IRX fallback |
| `runtime/src/gs/native_texture*.`*, `runtime/src/resource_pack*.`* | GS-source tracking and replacement packs | Separate game asset identity, overlay, and renderer adapters |
| `sdbz_scripts/`, `scripts/` | Windows/manual and generic game build workflows | One documented build entry point with host-specific presets |

The SDBZ branch is 459 commits ahead of the current `main` baseline.
It must **not** be reset to `main` or replaced by a fresh import: the
folder-disc logic, native ARKD research, GS experiments, and resource-pack
work contain useful verified behavior and tests.

## 2. Architectural rules

1. **Native-first bring-up, not native-only.** Boot and regression-test EU
   beta on Windows before spending time on browser-specific packaging.
   Keep the browser architecture available behind the same guest/runtime API.
2. **Gameplay fidelity first.** Do not replace guest lifecycle, animations,
   timings, input interpretation or file behavior with guessed host logic.
3. **The EE translation backend is replaceable.** The CPU state layout,
   dispatch convention, memory access callbacks, exceptions, cycles and
   syscall exits are versioned contracts, not private details of one emitter.
4. **PS2 hardware is independent of the title.** EE/IOP/DMAC/VIF/VU/GS/SPU2
   and timing services cannot depend on SDBZ textures, archive paths, menu
   widgets, or `ARKD_DVD.IRX`.
5. **Game adapters are optional.** Arika-specific RPC and asset decoding can
   be selected per title/version, with an original-game behavior fallback.
6. **Original guest assets remain authoritative.** Native PNG/audio packs
   are host presentation overlays, never replacements for bytes returned to
   guest PS2 loaders unless a separate explicit loose-file override is used.
7. **Do not conflate identity with residency.** The original game identifies
   the asset by name; the GS tracks current GPU memory, palette ownership,
   transfers and invalidation. Filename identity must survive valid palette
   changes without leaking into unrelated textures.
8. **A test must precede a risky migration.** Preserve original vs rebuilt
   logs, state digests and frame comparisons; no unverified 'equivalent'
   claims.
9. **No hidden dependencies.** A build must not rely on a sibling `../sdbz`
   worktree, an untracked registry, pre-generated source, or a machine-specific
   SDK installation without explicit diagnostics.

## 3. Target module boundaries

```text
tools/
  ee_recompiler/          ELF, discovery, decode, lift, validation
  vu_recompiler/          VU microprogram translation/validation
sdk/
  guest_abi/              Versioned EE CPU state, memory and dispatcher ABI
core/
  machine/                Scheduler, events, guest memory, exceptions
  devices/                IOP, DMA, VIF/VU, GS, SPU2, CDVD, controllers
  storage/                ISO, extracted folder, indexed archive sources
  presentation/           Host-independent frames, audio and input events
games/
  sdbz/                   Version detection, ARKD adapter, asset provenance
  common/                 Game-neutral registry and optional adapters
mods/
  resources/              Named assets, pack manager, safe ZIP, load order
frontends/
  native/                 Windows/native executable and host window backend
  web/                    Emscripten/WebGPU/AudioWorklet frontend
tests/
  unit/
  integration/
  differential/
  fixtures/               Synthetic redistributable cases only
```

This is a **target logical organization**, not an immediate move or mass
rename. Move functionality one isolated slice at a time, keep compatibility
wrappers, and avoid simultaneously changing semantics and layout.

## 4. Recompiler decision: preserve current backend until proven

Today `eerecomp` emits C++ for the R5900 and relies on the existing
`ps2runtime` dispatcher. A separate
[`twist84/MIPS-To-LLVM-IR-To-C`](https://github.com/twist84/MIPS-To-LLVM-IR-To-C)
project has verified and linked 7,901 USA SDBZ guest-function modules.
That result is an important candidate for a future EE LLVM backend, **not**
evidence of complete runtime compatibility. Its own documentation says the
linked library does not boot the game, its in-tree object emitter is
incomplete, and VU0 behavior remains only partially covered.

Before switching to LLVM:

- Define an explicit common guest CPU ABI and a transition/exceptions
  contract, including 128-bit GPRs, HI/LO, FPU, VU0, and delay-slot state.
- Run both backends against identical synthetic instruction and branching
  fixtures, then compare output guest state and cycle/event traces.
- Compare a few actual EU beta functions with the original C++ backend,
  including indirect branches and calls, rather than relying on the USA
  compile/link percentage.
- Make the LLVM emitter an opt-in build alternative. Never remove the
  known-working C++ backend until boot/menu/gameplay parity is measured.

## 5. Migration milestones and acceptance gates

### M0 — Baseline preservation and measurement

- Pin `SDBZ` and record the closed, unmerged experiment in PR #42
  (`feature/sdbz-named-texture-residency`). Its tests and findings must not
  disappear merely because the branch is not merged.
- Document supported EU beta inputs and current build requirements.
- Introduce one always-on, game-free CI job for CMake recompiler tools,
  recompiler synthetic tests, and any core CTest tests that build reliably.
- Preserve current `SDBZ.exe`, native disc, browser, and replacement-pack
  behavior without feature additions.
- **Gate:** reproducible clean build/test results; known gaps recorded.

### M1 — Extract host/runtime contracts

- Make native and browser hosts depend on a narrow `machine` interface.
- Make `eerecomp` / generated code depend on a declared guest ABI.
- Isolate configuration and paths from the emulation core.
- Remove game-specific post-build copy and output path logic from the
  generic runtime target (keep the existing `SDBZ` target working).
- **Gate:** no intended guest-visible change; same smoke traces and tests.

### M2 — Storage and Arika integration

- Separate ISO9660, generated folder ISO, indexed INFO/GAME archives,
  loose `GAME/` overrides, and `NATIVE/` host assets behind typed interfaces.
- Extract `ARKD_DVD.IRX` HLE from the generic IOP module loader into an
  optional title adapter; retain original-IRX differential mode.
- Preserve input validation, cache limits, stable paths, and asynchronous
  guest callbacks.
- **Gate:** existing synthetic tests, actual EU beta menu/stage/audio
  traces, missing-file/overwrite/mount tests.

### M3 — GS and named texture provenance

- Separate guest texture/source-name observation, GS transfer ownership,
  GPU residency, renderer, and replacement-pack selection.
- Incorporate **verified** findings from PR #42 selectively; it is
  closed/unmerged and its last in-game edge cases require revalidation.
- Name-based replacement must not depend on region-specific upload hashes.
  Maintain correct CLUT, TEX0/TEX2, local-copy and partial-upload behavior.
- **Gate:** no original image changes with packs disabled; named PNG pack
  can be enabled/disabled and changed live without stale/incorrect sprites
  across EU beta menu transitions.

### M4 — Frontends and reproducible packaging

- Provide one CMake presets-based developer path on Windows, plus a
  dependency-checked script for generating/linking guest code.
- Keep the direct `SDBZ.exe` extracted-disc experience; make gamepad,
  native audio, rendering and configuration independent of the game adapter.
- Re-enable or retain the browser runner through the common runtime API.
- **Gate:** clean native startup through menu and gameplay, repeatable
  tests, captured diagnostics; browser starts from the same compiled core.

### M5 — Optional LLVM backend

- Integrate the separately validated LLVM backend through the versioned
  guest ABI without replacing runtime/hardware emulation again.
- Run differential state tests and EU beta boot/menu/gameplay checks.
- **Gate:** only switch the default backend after demonstrated parity.

## 6. Validation matrix

| Level | Uses game content? | Check |
| --- | --- | --- |
| Unit | No | ELF decode, branches/delay slots, memory, GS swizzle, archive lookup, pack safety |
| Build | No | Linux Clang/GCC recompiler/runtime, Windows toolchain smoke |
| Differential | Maybe | Existing vs rewritten guest-state traces; PCSX2 trace comparisons when available |
| Integration | Yes, locally | EU beta boot, title/menu, character select, match, stage loading, streaming audio |
| Rendering | Yes, locally | Screenshots/frame digests with packs off/on; GS palette, residency, invalidation |
| Packaging | No proprietary fixtures | Build from documented inputs; no tracked game files or generated copyrighted objects |

CI must not claim game compatibility when it has only built syntax or run
synthetic tests. Differences must be reproducible with captured input/state
and original ELF address references.

## 7. Known baseline issues to resolve

- The single top-level `CMakeLists.txt` currently defines the
  recompiler, entire runtime, optional reconstructed game code, native
  and web hosts, native texture/zlib setup, and SDBZ-specific post-build
  packaging. This coupling is a migration target.
- `sdbz_scripts/sdbz_build.ps1` and its configure script expect a sibling
  `../sdbz` workspace, which is not represented in this repository.
- Some documents are stale: `README_GAME_EXE.txt` still prescribes
  `.ps2-disc` metadata while `docs/sdbz-runtime-disc-layout.md` explicitly
  states the runtime no longer creates or reads it. `docs/resource-packs.md`
  likewise mixes historical limitations with later live-reload features.
- Existing SDBZ GitHub workflows focus on folder-disc and ARKD paths, and
  use path filters; they do not by themselves represent a universal
  end-to-end recompiler/Windows/browser build gate.
- PR #42's strict name-based texture path is unmerged. Its latest experiments
  cover exact GS 256-byte block invalidation, transfer-time name provenance,
  TEX2/CLUT handling, and GIF queue ordering. Do not reintroduce fingerprint
  identity as a workaround for these correctness problems.
- The native ARKD HLE has documented missing audio/timing parity; do not
  remove the original driver fallback until it has been verified.

## 8. Immediate implementation order

1. Add a game-free, non-destructive baseline build/test workflow.
2. Establish machine/guest ABI headers and compatibility shims without
   moving the rest of the implementation.
3. Split `ps2runtime` logically into device/storage/host-facing libraries,
   keeping existing targets and tests green.
4. Build game-adapter interfaces, then migrate ARKD.
5. Migrate asset/GS provenance and pack overlay; verify with real gameplay.
6. Evaluate LLVM backend only after the ABI and runtime tests are stable.

**Non-goals for the initial PR:** deleting old code, changing gameplay
behavior, resetting `SDBZ`, importing unverified LLVM semantics, assuming
unknown native animation timings, or committing proprietary game data.
