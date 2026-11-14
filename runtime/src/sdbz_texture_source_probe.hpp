#pragma once

#include <cstdint>
#include <string>

namespace ps2
{
struct Cpu;

// Read-only hook into the EU Beta texture constructors. Uses no byte hashes,
// writes no guest state, and is enabled only for diagnostics.
void sdbz_texture_source_probe(Cpu &cpu, std::uint32_t pc);

// Called from the emulation-thread GIF metadata scan when a TEX0 register
// is written. Resolves the current GS binding from verified guest handles,
// never from upload fingerprints or pixel hashes.
struct s_named_texture_binding
{
    std::string name;
    std::uint32_t bp = 0;
    std::uint32_t bw = 0;
    std::uint32_t psm = 0;
    std::uint32_t clut_bp = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// With result != nullptr, only resolve the name; the GIF scanner must
// publish the binding after the corresponding TEX0 qword reaches the GS.
bool sdbz_texture_source_tex0(std::uint64_t tex0,
    s_named_texture_binding *result = nullptr);

// A valid full-image GS upload can overwrite a named resident after the
// game's last TEX0 register write. Recover its canonical identity only when
// a unique, currently live CTexture handle owns the exact destination and
// source image layout. Called on the emulation thread at TRXDIR=host->local.
bool sdbz_texture_source_transfer(std::uint64_t bitbltbuf,
    std::uint64_t trxreg, std::uint64_t trxpos,
    s_named_texture_binding &result);
}
