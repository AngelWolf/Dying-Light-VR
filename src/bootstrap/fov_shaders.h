// Ported from CheekyFoveatedDLSS (https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS), GPL-3.0, src/peripheral_shaders.hpp and
// src/d3d11_composite_shader.hpp, used with the author's permission.
//
// fov_shaders.h - the fixed foveated DLSS / FSR compute shaders (docs/superpowers/plans/2026-11-11-foveated-dlss.md Task 3), compiled
// at runtime through ::dlvr::shaderbin::Compile (namespace fovdlss in dlssflat.cpp; tools/gen_shaderbin.py SITES rows "fov_color",
// "fov_depth", "fov_motion", "fov_composite", decl_file "fov_shaders.h").
//
// WHAT CHANGED FROM CHEEKY'S TEXT (each named; everything else is his):
// - colour downsample: + `EncodeSrgb` (INV-13 "encoded in, encoded out": an sRGB SRV decodes, a UNORM UAV stores raw, so the shader
//   re-encodes - the lens crop's fill rule). 0 on the DLAA path (float in, float out).
// - depth downsample, motion convert: unchanged.
// - composite: centred on the fovea centre (`ShapeCenter`, in his `centered` units = 2 * uv - 1, i.e. the straight-ahead point plus
//   both offsets - foveation_logic.h FoveaCentre) instead of `ShapeOffset * (1 - ShapeSize)` about the image centre; the NR border
//   and next-jump parts are dropped (no DLSS-NR, no gaze); `OutputOrigin`, `InputBase`, `DlssOrigin` are 0 (whole textures, the
//   centre's private output starts at (0, 0) - INV-718) and removed; + `EncodeBase` / `EncodeCentre` (INV-13, as above) and
//   `BorderValue` (the red line's value: 1.0 encoded, 8.0 on the HDR path - the lens crop's tint rule). The weight formula is
//   foveation_logic.h CompositeWeight's, which the unit test pins.
#pragma once

namespace dlvr {
namespace fovhlsl {

// Bilinear colour downsample, render -> working size (his color_downsample_shader_source).
DLVR_HLSL_DECL(kFovColorHlsl) R"(
Texture2D<float4> SourceColor : register(t0);
RWTexture2D<float4> PackedColor : register(u0);
cbuffer Constants : register(b0) {
    uint2 SourceBase;
    uint2 SourceSize;
    uint2 DestSize;
    uint EncodeSrgb;
    uint Pad0;
};
float3 ToSrgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float4 LoadClamped(int2 local) {
    const int2 maximum = int2(SourceSize) - 1;
    return SourceColor.Load(int3(int2(SourceBase) + clamp(local, int2(0, 0), maximum), 0));
}
[numthreads(16, 16, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DestSize)) return;
    const float2 source =
        (float2(id.xy) + 0.5) * float2(SourceSize) / float2(DestSize) - 0.5;
    const int2 base = int2(floor(source));
    const float2 fraction = source - floor(source);
    const float4 p00 = LoadClamped(base);
    const float4 p10 = LoadClamped(base + int2(1, 0));
    const float4 p01 = LoadClamped(base + int2(0, 1));
    const float4 p11 = LoadClamped(base + int2(1, 1));
    float4 c = lerp(
        lerp(p00, p10, fraction.x),
        lerp(p01, p11, fraction.x),
        fraction.y
    );
    if (EncodeSrgb != 0) { c.rgb = ToSrgb(c.rgb); }
    PackedColor[id.xy] = c;
}
)";
DLVR_HLSL_USE(kFovColorHlsl)

// Point depth downsample (his depth_downsample_shader_source, unchanged).
DLVR_HLSL_DECL(kFovDepthHlsl) R"(
Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> PackedDepth : register(u0);
cbuffer Constants : register(b0) {
    uint2 SourceBase;
    uint2 SourceSize;
    uint2 DestSize;
};
[numthreads(16, 16, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DestSize)) return;
    const uint2 numerator = (2U * id.xy + 1U) * SourceSize;
    const uint2 denominator = 2U * DestSize;
    const uint2 local = min(numerator / denominator, SourceSize - 1U);
    PackedDepth[id.xy] = SourceDepth.Load(int3(SourceBase + local, 0));
}
)";
DLVR_HLSL_USE(kFovDepthHlsl)

// Point motion-vector downsample (his motion_convert_shader_source, unchanged). The vectors keep their values; the periphery eval
// is told MV scale -workW, -workH, so their UV meaning is kept (Cheeky d3d11_peripheral_dlaa.cpp:1098-1103 scales by the same ratio).
DLVR_HLSL_DECL(kFovMotionHlsl) R"(
Texture2D<float2> SourceMotion : register(t0);
RWTexture2D<float2> PackedMotion : register(u0);
cbuffer Constants : register(b0) {
    uint2 SourceBase;
    uint2 SourceSize;
    uint2 DestSize;
};
[numthreads(16, 16, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DestSize)) return;
    const uint2 numerator = (2U * id.xy + 1U) * SourceSize;
    const uint2 denominator = 2U * DestSize;
    const uint2 local = min(numerator / denominator, SourceSize - 1U);
    PackedMotion[id.xy] = SourceMotion.Load(int3(SourceBase + local, 0));
}
)";
DLVR_HLSL_USE(kFovMotionHlsl)

// The composite (his d3d11_composite_shader_source, adapted as the header says). t0 = the periphery base (the periphery feature's
// output, or the engine input itself with fov_periphery off), upsampled bilinear over its whole size; t1 = the centre feature's
// private output, placed at RectBase and area-resampled when supersampled; u0 = the eye's output texture, written WHOLE.
DLVR_HLSL_DECL(kFovCompositeHlsl) R"(
Texture2D<float4> LowResolutionColor : register(t0);
Texture2D<float4> DlssColor : register(t1);
RWTexture2D<float4> GameOutput : register(u0);

cbuffer Constants : register(b0) {
    uint2 OutputSize;
    uint2 InputSize;
    uint2 RectBase;
    uint2 RectSize;
    float ShapeWidth;
    float ShapeHeight;
    float ShapeCenterX;
    float ShapeCenterY;
    float ShapeRoundness;
    float Feather;
    uint ShowAlignmentBorder;
    float BorderValue;
    uint EncodeBase;
    uint EncodeCentre;
    uint Pad0;
    uint Pad1;
};

float3 ToSrgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float ShapeDistance(float2 centered) {
    const float2 shape_size = max(
        float2(ShapeWidth, ShapeHeight),
        float2(0.0001, 0.0001)
    );
    const float2 scaled = abs(centered) / shape_size;
    return lerp(
        max(scaled.x, scaled.y),
        length(scaled),
        saturate(ShapeRoundness)
    );
}

float4 LoadInputBilinear(float2 position) {
    const float2 base = floor(position);
    const float2 fraction = position - base;
    const int2 minimum = int2(0, 0);
    const int2 maximum = int2(InputSize) - 1;
    const int2 p00 = clamp(int2(base), minimum, maximum);
    const int2 p10 = clamp(p00 + int2(1, 0), minimum, maximum);
    const int2 p01 = clamp(p00 + int2(0, 1), minimum, maximum);
    const int2 p11 = clamp(p00 + int2(1, 1), minimum, maximum);
    return lerp(
        lerp(LowResolutionColor.Load(int3(p00, 0)),
             LowResolutionColor.Load(int3(p10, 0)), fraction.x),
        lerp(LowResolutionColor.Load(int3(p01, 0)),
             LowResolutionColor.Load(int3(p11, 0)), fraction.x),
        fraction.y
    );
}

float2 InputPosition(uint2 local_pixel) {
    return (float2(local_pixel) + 0.5) * float2(InputSize) /
        float2(OutputSize) - 0.5;
}

// The private texture exactly matches the DLSS reconstruction extent.
// Integrate pixel coverage, including fractional boundaries, without negative
// filter lobes or an extra intermediate pass. 1x retains the original load.
float4 LoadDlssResampled(uint2 local_pixel) {
    uint width, height;
    DlssColor.GetDimensions(width, height);
    const uint2 source_size = uint2(width, height);
    if (all(source_size == RectSize)) {
        return DlssColor.Load(int3(int2(local_pixel), 0));
    }
    const float2 ratio = float2(source_size) / float2(RectSize);
    if (any(source_size < RectSize)) {
        const float2 position = (float2(local_pixel) + 0.5) * ratio - 0.5;
        const int2 base = int2(floor(position));
        const float2 fraction = frac(position);
        const int2 maximum = int2(source_size) - 1;
        const int2 p00 = clamp(base, int2(0, 0), maximum);
        const int2 p10 = clamp(base + int2(1, 0), int2(0, 0), maximum);
        const int2 p01 = clamp(base + int2(0, 1), int2(0, 0), maximum);
        const int2 p11 = clamp(base + int2(1, 1), int2(0, 0), maximum);
        return lerp(lerp(DlssColor.Load(int3(p00, 0)), DlssColor.Load(int3(p10, 0)), fraction.x),
            lerp(DlssColor.Load(int3(p01, 0)), DlssColor.Load(int3(p11, 0)), fraction.x), fraction.y);
    }

    const float2 begin = float2(local_pixel) * ratio;
    const float2 end = min(float2(local_pixel + 1U) * ratio, float2(source_size));
    float4 sum = 0.0;
    float total = 0.0;
    [loop] for (int y = int(floor(begin.y)); y < int(ceil(end.y)); ++y) {
        const float wy = max(0.0, min(end.y, float(y + 1)) - max(begin.y, float(y)));
        [loop] for (int x = int(floor(begin.x)); x < int(ceil(end.x)); ++x) {
            const float wx = max(0.0, min(end.x, float(x + 1)) - max(begin.x, float(x)));
            const int2 pixel = clamp(int2(x, y), int2(0, 0), int2(source_size) - 1);
            sum += DlssColor.Load(int3(pixel, 0)) * (wx * wy);
            total += wx * wy;
        }
    }
    return sum / max(total, 0.000001);
}

[numthreads(16, 16, 1)]
void CompositeMain(uint3 dispatch_id : SV_DispatchThreadID) {
    if (any(dispatch_id.xy >= OutputSize)) return;
    const uint2 output_pixel = dispatch_id.xy;
    float4 bilinear = LoadInputBilinear(InputPosition(output_pixel));
    if (EncodeBase != 0U) { bilinear.rgb = ToSrgb(bilinear.rgb); }
    float2 centered =
        (float2(output_pixel) + 0.5) / (0.5 * float2(OutputSize)) - 1.0;
    centered -= float2(ShapeCenterX, ShapeCenterY);
    const float distance_from_center = ShapeDistance(centered);
    const float2 pixel_size = 2.0 / float2(OutputSize);
    const float distance_per_pixel = max(
        abs(ShapeDistance(centered + float2(pixel_size.x, 0.0)) -
            distance_from_center),
        abs(ShapeDistance(centered + float2(0.0, pixel_size.y)) -
            distance_from_center)
    );
    const bool alignment_border = ShowAlignmentBorder != 0U &&
        distance_from_center <= 1.0 &&
        distance_from_center >= 1.0 - 5.0 * distance_per_pixel;
    if (alignment_border) {
        GameOutput[output_pixel] = float4(BorderValue, 0.0, 0.0, 1.0);
        return;
    }
    const float normalized_feather = Feather /
        max(0.0001, min(ShapeWidth, ShapeHeight));
    const float weight = Feather <= 0.0
        ? (distance_from_center <= 1.0 ? 1.0 : 0.0)
        : 1.0 - smoothstep(
            max(0.0, 1.0 - normalized_feather),
            1.0,
            distance_from_center
        );
    const bool inside_rect = all(output_pixel >= RectBase) &&
        all(output_pixel < RectBase + RectSize);
    if (!inside_rect || weight <= 0.0) {
        GameOutput[output_pixel] = bilinear;
        return;
    }

    // The private DX11 DLSS feature writes a packed crop at scratch (0, 0).
    // RectBase is where that crop belongs in the game's full-resolution output.
    float4 dlss = LoadDlssResampled(output_pixel - RectBase);
    if (EncodeCentre != 0U) { dlss.rgb = ToSrgb(dlss.rgb); }
    GameOutput[output_pixel] = lerp(bilinear, dlss, weight);
}
)";
DLVR_HLSL_USE(kFovCompositeHlsl)

}  // namespace fovhlsl
}  // namespace dlvr
