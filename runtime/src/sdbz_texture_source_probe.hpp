#pragma once

#include <cstdint>

namespace ps2
{
struct Cpu;

// Read-only hook into the EU Beta texture constructors. Uses no byte hashes,
// writes no guest state, and is enabled only for diagnostics.
void sdbz_texture_source_probe(Cpu &cpu, std::uint32_t pc);

// Called from the emulation-thread GIF metadata scan when a TEX0 register
// is written. Resolves the current GS binding from verified guest handles,
// never from upload fingerprints or pixel hashes.
void sdbz_texture_source_tex0(std::uint64_t tex0);
}
