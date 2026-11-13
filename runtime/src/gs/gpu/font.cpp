#include "font.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>
#include <algorithm>

namespace ps2::gs::gpu {

namespace {

WGPUDevice g_font_device = nullptr;
WGPUQueue g_font_queue = nullptr;
WGPURenderPipeline g_font_pipeline = nullptr;
WGPURenderPipeline g_panel_pipeline = nullptr;
WGPUBuffer g_panel_uniform_buffer = nullptr;
WGPUBindGroupLayout g_font_bind_group_layout = nullptr;
WGPUBuffer g_font_uniform_buffer = nullptr;
WGPUBuffer g_font_vertex_buffer = nullptr;
WGPUSampler g_font_sampler = nullptr;
WGPUTexture g_font_atlas = nullptr;
WGPUTextureView g_font_atlas_view = nullptr;

inline WGPUStringView wv(const char* s) {
    return WGPUStringView{s, WGPU_STRLEN};
}

struct FontInfo {
    std::uint8_t width = 0;
    std::uint8_t height = 0;
    std::uint8_t bytes_per_row = 0;
    std::uint32_t glyph_count = 0;
    std::vector<std::vector<std::uint8_t>> glyphs;
    std::vector<std::uint16_t> char_map;
};

constexpr int kAtlasCols = 34;
constexpr int kAtlasCell = 24;
constexpr int kFirstChar = 32;
constexpr int kLastChar = 126;
constexpr int kMaxChars = 256;
constexpr int kUniformStride = 256; // aligned to minUniformBufferOffsetAlignment

FontInfo g_font_info;
std::vector<float> g_font_uv_left, g_font_uv_right, g_font_uv_top, g_font_uv_bottom;
std::vector<int> g_font_ink_left, g_font_ink_right;

std::vector<std::uint8_t> read_file(const char* path) {
    std::vector<std::uint8_t> data;
    FILE* f = std::fopen(path, "rb");
    if (!f) return data;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); return data; }
    data.resize(static_cast<std::size_t>(size));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    std::fclose(f);
    return data;
}

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& d, std::size_t off) {
    std::uint32_t v;
    std::memcpy(&v, d.data() + off, 4);
    return v;
}

std::uint16_t read_u16_le(const std::vector<std::uint8_t>& d, std::size_t off) {
    std::uint16_t v;
    std::memcpy(&v, d.data() + off, 2);
    return v;
}

bool parse_ffon(const std::vector<std::uint8_t>& data, FontInfo& font) {
    if (data.size() < 0x40 ||
        std::string_view(reinterpret_cast<const char*>(data.data()), 4) != "FFON")
        return false;

    font.width = data[0x1C];
    font.height = data[0x1D];
    font.bytes_per_row = data[0x1F];
    font.glyph_count = read_u32_le(data, 0x18);

    const auto page_count = read_u32_le(data, 0x10);
    const auto last_column = read_u32_le(data, 0x14);
    const auto char_map_count = page_count * (last_column + 1);
    const auto glyph_data_offset = 0xA0u + char_map_count * 2u;

    if (0xA0 + char_map_count * 2 > data.size()) return false;
    font.char_map.resize(char_map_count);
    for (std::uint32_t i = 0; i < char_map_count; ++i)
        font.char_map[i] = read_u16_le(data, 0xA0 + i * 2);

    font.glyphs.resize(font.glyph_count);
    for (std::uint32_t gi = 0; gi < font.glyph_count; ++gi) {
        const auto offset = glyph_data_offset + gi * font.bytes_per_row * font.height;
        if (offset + font.bytes_per_row * font.height > data.size()) return false;
        font.glyphs[gi].assign(data.begin() + offset,
                               data.begin() + offset + font.bytes_per_row * font.height);
    }
    return true;
}

std::pair<int, int> ink_bounds(const FontInfo& font, const std::vector<std::uint8_t>& data) {
    int left = static_cast<int>(font.width);
    int right = 0;
    for (std::uint32_t y = 0; y < font.height; ++y) {
        for (std::uint32_t x = 0; x < font.width; ++x) {
            const auto packed = data[y * font.bytes_per_row + (x / 2)];
            const auto value = (x & 1) ? (packed >> 4) : (packed & 0x0Fu);
            if (value != 0) {
                left = std::min(left, static_cast<int>(x));
                right = std::max(right, static_cast<int>(x) + 1);
            }
        }
    }
    if (right == 0) return {0, 0};
    return {left, right};
}

} // namespace

bool font_init(WGPUDevice device, WGPUQueue queue, const char* font_path) {
    auto data = read_file(font_path);
    if (data.empty()) {
        std::fprintf(stderr, "[font] cannot load %s\n", font_path);
        return false;
    }
    FontInfo font;
    if (!parse_ffon(data, font)) {
        std::fprintf(stderr, "[font] bad FFON format in %s\n", font_path);
        return false;
    }

    const int atlas_cols = kAtlasCols;
    const int atlas_rows = (static_cast<int>(font.glyph_count) + atlas_cols - 1) / atlas_cols;
    const int atlas_w = atlas_cols * kAtlasCell;
    const int atlas_h = atlas_rows * kAtlasCell;

    // WebGPU requires bytesPerRow to be a multiple of 256 for buffer->texture copies.
    const int atlas_bpr = (atlas_w * 4 + 255) & ~255;
    const int texture_w = atlas_bpr / 4; // padded texture width in pixels
    const std::size_t atlas_bytes = static_cast<std::size_t>(atlas_bpr) * atlas_h;

    std::vector<std::uint8_t> atlas(atlas_bytes, 0);
    for (std::uint32_t gi = 0; gi < font.glyph_count; ++gi) {
        const int gx = static_cast<int>(gi % atlas_cols);
        const int gy = static_cast<int>(gi / atlas_cols);
        const auto& glyph = font.glyphs[gi];
        for (std::uint32_t y = 0; y < font.height; ++y) {
            for (std::uint32_t x = 0; x < font.width; ++x) {
                const auto packed = glyph[y * font.bytes_per_row + (x / 2)];
                const auto value = (x & 1) ? (packed >> 4) : (packed & 0x0Fu);
                const auto intensity = static_cast<std::uint8_t>(value * 17);
                const int px = gx * kAtlasCell + static_cast<int>(x);
                const int py = gy * kAtlasCell + static_cast<int>(y);
                const std::size_t base = (static_cast<std::size_t>(py) * texture_w + px) * 4;
                atlas[base] = 255;
                atlas[base + 1] = 255;
                atlas[base + 2] = 255;
                atlas[base + 3] = intensity;
            }
        }
    }

    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    WGPUExtent3D tsize = WGPU_EXTENT_3D_INIT;
    tsize.width = static_cast<std::uint32_t>(texture_w);
    tsize.height = static_cast<std::uint32_t>(atlas_h);
    tsize.depthOrArrayLayers = 1;
    td.size = tsize;
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    g_font_atlas = wgpuDeviceCreateTexture(device, &td);

    WGPUTextureViewDescriptor tvd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    g_font_atlas_view = wgpuTextureCreateView(g_font_atlas, &tvd);

    WGPUTexelCopyTextureInfo ict = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    ict.texture = g_font_atlas;
    ict.mipLevel = 0;
    WGPUOrigin3D torigin = WGPU_ORIGIN_3D_INIT;
    ict.origin = torigin;
    ict.aspect = WGPUTextureAspect_All;

    WGPUExtent3D write_size = WGPU_EXTENT_3D_INIT;
    write_size.width = static_cast<std::uint32_t>(atlas_w);
    write_size.height = static_cast<std::uint32_t>(atlas_h);
    write_size.depthOrArrayLayers = 1;

    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.offset = 0;
    layout.bytesPerRow = static_cast<std::uint32_t>(atlas_bpr);
    layout.rowsPerImage = static_cast<std::uint32_t>(atlas_h);

    wgpuQueueWriteTexture(wgpuDeviceGetQueue(device),
        &ict,
        atlas.data(),
        atlas_bytes,
        &layout,
        &write_size);

    WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sd.magFilter = WGPUFilterMode_Nearest;
    sd.minFilter = WGPUFilterMode_Nearest;
    sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    g_font_sampler = wgpuDeviceCreateSampler(device, &sd);

    WGPUBufferDescriptor ubo = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubo.size = static_cast<std::uint64_t>(kMaxChars) * kUniformStride;
    ubo.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    g_font_uniform_buffer = wgpuDeviceCreateBuffer(device, &ubo);

    const float quad[12] = {0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1.f};
    WGPUBufferDescriptor vbo = WGPU_BUFFER_DESCRIPTOR_INIT;
    vbo.size = sizeof(quad);
    vbo.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    g_font_vertex_buffer = wgpuDeviceCreateBuffer(device, &vbo);
    wgpuQueueWriteBuffer(queue, g_font_vertex_buffer, 0, quad, sizeof(quad));

    const int ch_count = kLastChar - kFirstChar + 1;
    g_font_uv_left.assign(ch_count, 0.f);
    g_font_uv_right.assign(ch_count, 0.f);
    g_font_uv_top.assign(ch_count, 0.f);
    g_font_uv_bottom.assign(ch_count, 0.f);
    g_font_ink_left.assign(ch_count, 0);
    g_font_ink_right.assign(ch_count, 0);
    for (int ch = kFirstChar; ch <= kLastChar; ++ch) {
        const int idx = ch - kFirstChar;
        const std::size_t slot = static_cast<std::size_t>(idx);
        if (slot >= font.char_map.size()) continue;
        const auto glyph_id = font.char_map[slot];
        if (glyph_id == 0 || static_cast<std::size_t>(glyph_id) > font.glyph_count) continue;
        const std::uint32_t gi = glyph_id - 1;
        const int gx = static_cast<int>(gi % atlas_cols);
        const int gy = static_cast<int>(gi / atlas_cols);
        g_font_uv_left[idx] = static_cast<float>(gx * kAtlasCell) / static_cast<float>(texture_w);
        g_font_uv_right[idx] = static_cast<float>((gx + 1) * kAtlasCell) / static_cast<float>(texture_w);
        g_font_uv_top[idx] = static_cast<float>(gy * kAtlasCell) / static_cast<float>(atlas_h);
        g_font_uv_bottom[idx] = static_cast<float>((gy + 1) * kAtlasCell) / static_cast<float>(atlas_h);
        const auto [il, ir] = ink_bounds(font, font.glyphs[gi]);
        g_font_ink_left[idx] = il;
        g_font_ink_right[idx] = ir;
    }

    g_font_info = font;

    g_font_device = device;
    g_font_queue = queue;
    wgpuDeviceAddRef(device);
    wgpuQueueAddRef(queue);

    return true;
}

void font_create_pipeline(WGPUDevice device, WGPUTextureFormat surface_format) {
    if (g_font_pipeline) wgpuRenderPipelineRelease(g_font_pipeline);
    if (g_panel_pipeline) wgpuRenderPipelineRelease(g_panel_pipeline);
    g_panel_pipeline = nullptr;
    if (g_font_bind_group_layout) wgpuBindGroupLayoutRelease(g_font_bind_group_layout);
    g_font_pipeline = nullptr;
    g_font_bind_group_layout = nullptr;

    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = wv(R"WGSL(struct Uniforms {
    color: vec4<f32>,
    pos: vec2<f32>,
    size: vec2<f32>,
    uv_rect: vec4<f32>,
    surface: vec2<f32>,
    _pad: vec2<f32>,
}

@group(0) @binding(0) var<uniform> uni: Uniforms;
@group(0) @binding(1) var tex_sampler: sampler;
@group(0) @binding(2) var glyph_tex: texture_2d<f32>;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@vertex
fn vs(@location(0) pos: vec2<f32>) -> VertexOut {
    let xy = pos;
    let ndc_x = (uni.pos.x + xy.x * uni.size.x) / uni.surface.x * 2.0 - 1.0;
    let ndc_y = 1.0 - (uni.pos.y + xy.y * uni.size.y) / uni.surface.y * 2.0;
    var o: VertexOut;
    o.position = vec4<f32>(ndc_x, ndc_y, 0.0, 1.0);
    o.uv = vec2<f32>(
        uni.uv_rect.x + xy.x * (uni.uv_rect.z - uni.uv_rect.x),
        uni.uv_rect.y + xy.y * (uni.uv_rect.w - uni.uv_rect.y));
    return o;
}

@fragment
fn fs(in: VertexOut) -> @location(0) vec4<f32> {
    let a = textureSample(glyph_tex, tex_sampler, in.uv).a;
    return uni.color * a;
}

@fragment
fn fs_panel(in: VertexOut) -> @location(0) vec4<f32> {
    return uni.color;
}
)WGSL");
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &source.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &md);

    WGPUBindGroupLayoutEntry le[3] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                      WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                      WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    le[0].binding = 0;
    le[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    le[0].buffer.type = WGPUBufferBindingType_Uniform;
    le[0].buffer.hasDynamicOffset = false;
    le[1].binding = 1;
    le[1].visibility = WGPUShaderStage_Fragment;
    le[1].sampler.type = WGPUSamplerBindingType_Filtering;
    le[2].binding = 2;
    le[2].visibility = WGPUShaderStage_Fragment;
    le[2].texture.sampleType = WGPUTextureSampleType_Float;
    le[2].texture.viewDimension = WGPUTextureViewDimension_2D;

    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 3;
    ld.entries = le;
    g_font_bind_group_layout = wgpuDeviceCreateBindGroupLayout(device, &ld);

    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &g_font_bind_group_layout;
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(device, &pld);

    WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
    color.format = surface_format;
    color.writeMask = WGPUColorWriteMask_All;

    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    color.blend = &blend;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = wv("fs");
    fragment.targetCount = 1;
    fragment.targets = &color;

    WGPUVertexBufferLayout vb = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vb.arrayStride = 2 * sizeof(float);
    vb.stepMode = WGPUVertexStepMode_Vertex;
    WGPUVertexAttribute va = WGPU_VERTEX_ATTRIBUTE_INIT;
    va.format = WGPUVertexFormat_Float32x2;
    va.offset = 0;
    va.shaderLocation = 0;
    vb.attributeCount = 1;
    vb.attributes = &va;

    WGPUVertexState vertex = WGPU_VERTEX_STATE_INIT;
    vertex.module = module;
    vertex.entryPoint = wv("vs");
    vertex.bufferCount = 1;
    vertex.buffers = &vb;

    WGPUPrimitiveState prim = WGPU_PRIMITIVE_STATE_INIT;
    prim.topology = WGPUPrimitiveTopology_TriangleList;

    WGPURenderPipelineDescriptor rd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rd.layout = pl;
    rd.vertex = vertex;
    rd.fragment = &fragment;
    rd.primitive = prim;

    g_font_pipeline = wgpuDeviceCreateRenderPipeline(device, &rd);
    fragment.entryPoint = wv("fs_panel");
    g_panel_pipeline = wgpuDeviceCreateRenderPipeline(device, &rd);
    if (!g_panel_uniform_buffer)
    {
        WGPUBufferDescriptor uniform_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        uniform_desc.size = 64;
        uniform_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        g_panel_uniform_buffer = wgpuDeviceCreateBuffer(device, &uniform_desc);
    }

    wgpuShaderModuleRelease(module);
    wgpuPipelineLayoutRelease(pl);
}

void font_shutdown() {
    if (g_panel_pipeline) wgpuRenderPipelineRelease(g_panel_pipeline);
    if (g_panel_uniform_buffer) wgpuBufferRelease(g_panel_uniform_buffer);
    g_panel_pipeline = nullptr;
    g_panel_uniform_buffer = nullptr;
    if (g_font_pipeline) wgpuRenderPipelineRelease(g_font_pipeline);
    if (g_font_bind_group_layout) wgpuBindGroupLayoutRelease(g_font_bind_group_layout);
    if (g_font_uniform_buffer) wgpuBufferRelease(g_font_uniform_buffer);
    if (g_font_vertex_buffer) wgpuBufferRelease(g_font_vertex_buffer);
    if (g_font_sampler) wgpuSamplerRelease(g_font_sampler);
    if (g_font_atlas_view) wgpuTextureViewRelease(g_font_atlas_view);
    if (g_font_atlas) wgpuTextureRelease(g_font_atlas);
    if (g_font_queue) wgpuQueueRelease(g_font_queue);
    if (g_font_device) wgpuDeviceRelease(g_font_device);
    g_font_pipeline = nullptr;
    g_font_bind_group_layout = nullptr;
    g_font_uniform_buffer = nullptr;
    g_font_vertex_buffer = nullptr;
    g_font_sampler = nullptr;
    g_font_atlas_view = nullptr;
    g_font_atlas = nullptr;
    g_font_queue = nullptr;
    g_font_device = nullptr;
}


void font_draw_panel_in_pass(WGPURenderPassEncoder rp,
    std::uint32_t surface_width, std::uint32_t surface_height,
    float x, float y, float width, float height,
    float r, float g, float b, float alpha)
{
    if (!g_panel_pipeline || !g_panel_uniform_buffer
        || !g_font_bind_group_layout || !g_font_vertex_buffer)
        return;
    const float uniform[16] = {
        r, g, b, alpha,
        x, y, width, height,
        0.f, 0.f, 1.f, 1.f,
        static_cast<float>(surface_width),
        static_cast<float>(surface_height), 0.f, 0.f
    };
    wgpuQueueWriteBuffer(g_font_queue, g_panel_uniform_buffer,
        0, uniform, sizeof uniform);
    WGPUBindGroupEntry entries[3] = {
        WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT
    };
    entries[0].binding = 0;
    entries[0].buffer = g_panel_uniform_buffer;
    entries[0].size = sizeof uniform;
    entries[1].binding = 1;
    entries[1].sampler = g_font_sampler;
    entries[2].binding = 2;
    entries[2].textureView = g_font_atlas_view;
    WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    descriptor.layout = g_font_bind_group_layout;
    descriptor.entryCount = 3;
    descriptor.entries = entries;
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(g_font_device, &descriptor);
    wgpuRenderPassEncoderSetViewport(rp, 0.f, 0.f,
        static_cast<float>(surface_width),
        static_cast<float>(surface_height), 0.f, 1.f);
    wgpuRenderPassEncoderSetPipeline(rp, g_panel_pipeline);
    wgpuRenderPassEncoderSetVertexBuffer(rp, 0,
        g_font_vertex_buffer, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderSetBindGroup(rp, 0, group, 0, nullptr);
    wgpuRenderPassEncoderDraw(rp, 6, 1, 0, 0);
    wgpuBindGroupRelease(group);
}

void font_draw_text_in_pass(WGPURenderPassEncoder rp, WGPUTextureView target,
    std::uint32_t surface_width, std::uint32_t surface_height,
    const char* text, float x, float y, float scale,
    std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    if (!g_font_pipeline || !g_font_atlas_view || !g_font_sampler ||
        !g_font_bind_group_layout) return;
    if (g_font_info.char_map.empty()) return;

    const float f_width = static_cast<float>(g_font_info.width);
    const float f_height = static_cast<float>(g_font_info.height);
    const float space_w = std::max(8.f, f_width / 3.f);
    const float k_spacing = 3.f;
    const float k_shadow_offset = 1.f * scale;

    // Collect per-character uniform data (64 bytes each, stride kUniformStride).
    // Each glyph emits two entries: black drop shadow (offset down-right),
    // then the main color on top — matching the game's draw_text().
    struct CharData {
        float uniforms[16];
    };
    CharData chars[kMaxChars];
    int char_count = 0;
    float cursor_x = x;
    float top = y;
    const float line_h = f_height * scale;

    for (const char* p = text; *p; ++p) {
        if (char_count + 2 > kMaxChars) break;
        unsigned char ch = static_cast<unsigned char>(*p);
        if (ch == '\n') {
            cursor_x = x;
            top += line_h;
            continue;
        }
        if (ch < kFirstChar || ch > kLastChar) ch = kFirstChar;
        const int idx = ch - kFirstChar;

        if (ch == ' ') {
            cursor_x += (space_w + k_spacing) * scale;
            continue;
        }

        if (idx < 0 || idx >= static_cast<int>(g_font_uv_left.size())) continue;
        if (g_font_ink_right[idx] <= g_font_ink_left[idx]) continue;

        const float glyph_w = f_width;
        const float glyph_x = cursor_x - static_cast<float>(g_font_ink_left[idx]) * scale;

        // Drop shadow: black, offset down-right by 1px.
        CharData& shadow = chars[char_count++];
        shadow.uniforms[0] = 0.f;
        shadow.uniforms[1] = 0.f;
        shadow.uniforms[2] = 0.f;
        shadow.uniforms[3] = 1.f;
        shadow.uniforms[4] = glyph_x + k_shadow_offset;
        shadow.uniforms[5] = top + k_shadow_offset;
        shadow.uniforms[6] = glyph_w * scale;
        shadow.uniforms[7] = f_height * scale;
        shadow.uniforms[8] = g_font_uv_left[idx];
        shadow.uniforms[9] = g_font_uv_top[idx];
        shadow.uniforms[10] = g_font_uv_right[idx];
        shadow.uniforms[11] = g_font_uv_bottom[idx];
        shadow.uniforms[12] = static_cast<float>(surface_width);
        shadow.uniforms[13] = static_cast<float>(surface_height);
        shadow.uniforms[14] = 0.f;
        shadow.uniforms[15] = 0.f;

        // Main glyph in requested color.
        CharData& main = chars[char_count++];
        main.uniforms[0] = r / 255.f;
        main.uniforms[1] = g / 255.f;
        main.uniforms[2] = b / 255.f;
        main.uniforms[3] = 1.f;
        main.uniforms[4] = glyph_x;
        main.uniforms[5] = top;
        main.uniforms[6] = glyph_w * scale;
        main.uniforms[7] = f_height * scale;
        main.uniforms[8] = g_font_uv_left[idx];
        main.uniforms[9] = g_font_uv_top[idx];
        main.uniforms[10] = g_font_uv_right[idx];
        main.uniforms[11] = g_font_uv_bottom[idx];
        main.uniforms[12] = static_cast<float>(surface_width);
        main.uniforms[13] = static_cast<float>(surface_height);
        main.uniforms[14] = 0.f;
        main.uniforms[15] = 0.f;

        cursor_x += ((static_cast<float>(g_font_ink_right[idx] - g_font_ink_left[idx]) + k_spacing) * scale);
    }

    if (char_count == 0) return;

    // Write all character uniforms at strided positions (each 64 bytes at stride 256)
    std::vector<float> padded(char_count * kUniformStride / 4, 0.f);
    for (int i = 0; i < char_count; i++)
        std::memcpy(&padded[i * kUniformStride / 4], chars[i].uniforms, 64);
    wgpuQueueWriteBuffer(g_font_queue, g_font_uniform_buffer, 0, padded.data(),
        static_cast<std::uint64_t>(padded.size()) * sizeof(float));

    wgpuRenderPassEncoderSetPipeline(rp, g_font_pipeline);
    wgpuRenderPassEncoderSetViewport(rp, 0.f, 0.f, static_cast<float>(surface_width),
        static_cast<float>(surface_height), 0.0f, 1.0f);
    wgpuRenderPassEncoderSetVertexBuffer(rp, 0, g_font_vertex_buffer, 0, WGPU_WHOLE_SIZE);

    for (int i = 0; i < char_count; i++) {
        uint32_t dynamic_offset = static_cast<uint32_t>(i * kUniformStride);
        WGPUBindGroupEntry bge[3] = {WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT};
        bge[0].binding = 0;
        bge[0].buffer = g_font_uniform_buffer;
        bge[0].offset = dynamic_offset;
        bge[0].size = kUniformStride;
        bge[1].binding = 1;
        bge[1].sampler = g_font_sampler;
        bge[2].binding = 2;
        bge[2].textureView = g_font_atlas_view;
        WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgd.layout = g_font_bind_group_layout;
        bgd.entryCount = 3;
        bgd.entries = bge;
        WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g_font_device, &bgd);
        wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 0, nullptr);
        wgpuRenderPassEncoderDraw(rp, 6, 1, 0, 0);
        wgpuBindGroupRelease(bg);
    }
}

void font_draw_text(WGPUCommandEncoder enc, WGPUTextureView target,
    std::uint32_t surface_width, std::uint32_t surface_height,
    const char* text, float x, float y, float scale,
    std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    WGPURenderPassColorAttachment att = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    att.view = target;
    att.resolveTarget = nullptr;
    att.loadOp = WGPULoadOp_Clear;
    att.storeOp = WGPUStoreOp_Store;
    att.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};

    WGPURenderPassDescriptor rpd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rpd.colorAttachmentCount = 1;
    rpd.colorAttachments = &att;

    WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &rpd);
    font_draw_text_in_pass(rp, target, surface_width, surface_height,
        text, x, y, scale, r, g, b);
    wgpuRenderPassEncoderEnd(rp);
    wgpuRenderPassEncoderRelease(rp);
}

} // namespace ps2::gs::gpu
