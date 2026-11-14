#pragma once

#include <cstdint>

namespace ps2
{
struct Cpu;

// Read-only hook into the EU Beta texture constructors. Uses no byte hashes,
// writes no guest state, and is enabled only for diagnostics.
void sdbz_texture_source_probe(Cpu &cpu, std::uint32_t pc);
}
