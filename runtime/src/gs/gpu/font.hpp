#pragma once

#include <webgpu/webgpu.h>
#include <cstdint>

namespace ps2::gs::gpu {

// Load the FFON bitmap font from font_path, upload a glyph atlas texture, and
// prepare uniform/sampler resources. Must be called after the wgpu device and
// queue exist (from gpu::init). Returns false if the font cannot be loaded;
// rendering simply skips text overlay in that case.
bool font_init(WGPUDevice device, WGPUQueue queue, const char* font_path);

// Create the text render pipeline for the given surface format. Called from
// attach_surface once the surface format is known.
void font_create_pipeline(WGPUDevice device, WGPUTextureFormat surface_format);

// Record text overlay draws into enc, targeting target. Draws in pixel coords
// with (0,0) at top-left of the surface.
void font_draw_text(WGPUCommandEncoder enc, WGPUTextureView target,
    std::uint32_t surface_width, std::uint32_t surface_height,
    const char* text, float x, float y, float scale,
    std::uint8_t r, std::uint8_t g, std::uint8_t b);

// Record text overlay draw commands into an existing render pass encoder.
// Does not begin/end the render pass. Caller must have already set viewport
// and pipeline state as needed. Used to draw font overlay in the same render
// pass as the game blit so LoadOp_Load is not needed.
void font_draw_text_in_pass(WGPURenderPassEncoder rp, WGPUTextureView target,
    std::uint32_t surface_width, std::uint32_t surface_height,
    const char* text, float x, float y, float scale,
    std::uint8_t r, std::uint8_t g, std::uint8_t b);

// Flat alpha-blended rectangle using the existing font quad and FFON
// presentation pipeline. Used for the pack manager backdrop.
void font_draw_panel_in_pass(WGPURenderPassEncoder rp,
    std::uint32_t surface_width, std::uint32_t surface_height,
    float x, float y, float width, float height,
    float r, float g, float b, float alpha);

void font_shutdown();

}
