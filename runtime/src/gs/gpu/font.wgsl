struct Uniforms {
    color: vec4<f32>,
    pos: vec2<f32>,
    size: vec2<f32>,
    uv_rect: vec4<f32>,
    surface: vec2<f32>,
    _pad: vec2<f32>,
}

@group(0) @binding(0) var<uniform> uni: Uniforms;
@group(0) @binding(1) var sampler: sampler;
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
    let a = textureSample(glyph_tex, sampler, in.uv).a;
    return uni.color * a;
}
