#pragma once

#include "native_texture.hpp"
#include "texture_upload_trace.hpp"

#include <cstdint>

namespace ps2::gs::native_texture
{

// GS-only residency metadata. The guest still receives the original indexed
// texture data and CLUT. Bindings are invalidated on subsequent GS writes.
void reset_residency();
bool replacements_available();
void invalidate_destination(std::uint64_t bitbltbuf, std::uint64_t trxreg);
int complete_upload(const s_texture_upload_signature &upload);

// Returns an index only when both the current residency and TEX0 layout agree.
// A GS base pointer alone is deliberately not sufficient.
int resident_chunk(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm,
    std::uint32_t width, std::uint32_t height);
const s_image *chunk_image(int index);

}
