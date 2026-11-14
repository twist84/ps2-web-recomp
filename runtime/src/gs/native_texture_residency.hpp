#pragma once

#include "native_texture.hpp"
#include "texture_upload_trace.hpp"

#include <cstdint>
#include <vector>
#include <string>
#include <cstddef>

namespace ps2::gs::native_texture
{

// GS-only residency metadata. The guest still receives the original indexed
// texture data and CLUT. Bindings are invalidated on subsequent GS writes.
// Opt-in diagnostics: PS2_TRACE_NATIVE_TEXTURES=1.
bool trace_native_textures();
// Opt-in strict filename replacement mode; skips all fingerprint registries.
bool name_only_mode();
void reset_residency();
// Preserve verified GS upload identities across pack changes. Only decoded
// replacement images and catalog availability are invalidated.
void reload_replacements();
// Registered source identities are loaded from external data files.
std::size_t registered_texture_count();
std::string texture_path(int index);
std::vector<int> resident_chunks();
bool replacements_available();
void invalidate_destination(std::uint64_t bitbltbuf, std::uint64_t trxreg,
    std::uint64_t trxpos = 0);
int complete_upload(const s_texture_upload_signature &upload);
// Register an upload whose identity is established by an asset decoder.
// Must be called on the GS render thread after a complete image upload.
// Unnamed GPU transfers are deliberately not assigned guessed filenames.
bool register_named_upload(const std::string &logical_name,
    std::uint64_t bitbltbuf, std::uint64_t trxreg, std::uint64_t trxpos,
    std::uint32_t clut_bp = 0xffffffffu);
// Retire a named residency when its guest CTexture owner is destroyed.
// Will not touch another texture that has since reused the same GS BP.
bool revoke_named_upload(const std::string &logical_name,
    std::uint32_t bp);

// Returns an index only when both the current residency and TEX0 layout agree.
// A GS base pointer alone is deliberately not sufficient.
int resident_chunk(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm,
    std::uint32_t width, std::uint32_t height,
    std::uint32_t clut_bp = 0xffffffffu);
const s_image *chunk_image(int index);

// High-resolution replacements retain the game's base atlas dimensions.
// Support exact 1x and 2x images; reject nonuniform scales and dimensions
// exceeding the native GPU atlas capacity.
bool valid_atlas_size(std::uint32_t chunk, std::uint32_t width,
    std::uint32_t height);
std::uint32_t atlas_scale(std::uint32_t chunk, std::uint32_t width,
    std::uint32_t height);

}
