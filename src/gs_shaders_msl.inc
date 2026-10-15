R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Uniforms
{
    float targetWidth, targetHeight;
    uint tme, tfx, tcc, fge;
    uint abe, blendA, blendB, blendC, blendD, fix;
    uint atst, aref, alphaMode;
    uint writeMode;
    uint fbmask, fba, colclamp, destinationHasAlpha;
    float fogR, fogG, fogB;
    float texScaleU, texScaleV;
    uint texAlphaFixed, ta0;
    uint texIndexed, texLinear;
    float texWidth, texHeight;
    uint wrapU, wrapV;
    float minU, maxU, minV, maxV;
    uint maxLevel, lodFixed;
    float lodScale, lodK;
    float pointSize;
    uint paletteScale;
    uint texBox;
    uint texSelf;
    uint texReplaced;
    uint nativeTexel;
};

// GS_SPECIALIZED: the fields that pick a code path come from function constants, one compiled variant per
// combination in use (the generic build reads them from the uniforms and decides per pixel).
#if defined(GS_SPECIALIZED)
constant uint k_tme [[function_constant(0)]];
constant uint k_tfx [[function_constant(1)]];
constant uint k_tcc [[function_constant(2)]];
constant uint k_fge [[function_constant(3)]];
constant uint k_abe [[function_constant(4)]];
constant uint k_blendA [[function_constant(5)]];
constant uint k_blendB [[function_constant(6)]];
constant uint k_blendC [[function_constant(7)]];
constant uint k_blendD [[function_constant(8)]];
constant uint k_atst [[function_constant(9)]];
constant uint k_alphaMode [[function_constant(10)]];
constant uint k_writeMode [[function_constant(11)]];
constant uint k_fba [[function_constant(12)]];
constant uint k_colclamp [[function_constant(13)]];
constant uint k_destinationHasAlpha [[function_constant(14)]];
constant uint k_texAlphaFixed [[function_constant(15)]];
constant uint k_texIndexed [[function_constant(16)]];
constant uint k_texLinear [[function_constant(17)]];
constant uint k_wrapU [[function_constant(18)]];
constant uint k_wrapV [[function_constant(19)]];
constant uint k_texBox [[function_constant(20)]];
constant uint k_texSelf [[function_constant(21)]];
constant uint k_hasLevels [[function_constant(22)]];
constant uint k_lodFixed [[function_constant(23)]];
constant uint k_hasMask [[function_constant(24)]];
#define S(field) k_##field
#define HAS_LEVELS (k_hasLevels != 0)
#define LOD_FIXED (k_lodFixed != 0)
#define HAS_MASK (k_hasMask != 0)
#else
#define S(field) u.field
#define HAS_LEVELS (u.maxLevel != 0)
#define LOD_FIXED (u.lodFixed != 0)
#define HAS_MASK (u.fbmask != 0)
#endif

struct ShuffleUniforms
{
    uint sourceHalf, destinationHalf, mask16, sourceIsDepth;
};

struct VertexIn
{
    float3 position [[attribute(0)]];
    float4 color [[attribute(1)]];
    float3 stq [[attribute(2)]];
    float fog [[attribute(3)]];
};

struct VertexOut
{
    float4 position [[position]];
    float4 color [[center_no_perspective]];
    float3 stq [[center_no_perspective]];
    float fog [[center_no_perspective]];
    float pointSize [[point_size]];
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]], constant Uniforms &u [[buffer(1)]])
{
    VertexOut out;
    // The GS samples a pixel at its integer coordinate, the GPU at the centre: half a pixel of shift makes
    // both cover the same pixels.
    out.position = float4((in.position.x + 0.5) / u.targetWidth * 2.0 - 1.0, 1.0 - (in.position.y + 0.5) / u.targetHeight * 2.0, in.position.z, 1.0);
    out.color = in.color;
    out.stq = in.stq;
    out.fog = in.fog;
    out.pointSize = u.pointSize;
    return out;
}

// A render target's alpha channel used as an 8-bit texture: palette lookup per texel, filtered afterwards as the GS does.
static float4 indexedTexel(texture2d<float> tex, texture2d<float> palette, float2 at, float2 size, uint paletteScale)
{
    uint2 where = uint2(clamp(at, float2(0.0), size - 1.0));
    uint index = uint(round(tex.read(where).a * 255.0));
    // The palette texture keeps the layout it has in VRAM: 16x16, with the two middle index bits swapped.
    uint entry = (index & 0xE7u) | ((index & 0x08u) << 1) | ((index & 0x10u) >> 1);
    return round(palette.read(uint2(entry & 15u, entry >> 4) * paletteScale + paletteScale / 2) * 255.0);
}

struct QuadOut
{
    float4 position [[position]];
};

vertex QuadOut quadVertex(uint id [[vertex_id]])
{
    QuadOut out;
    out.position = float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0.0, 1.0);
    return out;
}

// A target's alpha copied into an indexed texture: through a PSMT4HH view into a 4-bit one (the top four bits,
// origin.z = 16) or through a PSMT8H view into an 8-bit one (the whole byte, origin.z = 1). The index is kept in
// alpha, where the indexed path reads it.
fragment float4 alphaNibbleFragment(QuadOut in [[stage_in]], texture2d<float> source [[texture(0)]], constant uint4 &origin [[buffer(1)]])
{
    const float a = round(source.read(uint2(in.position.xy) + origin.xy).a * 255.0);
    return float4(0.0, 0.0, 0.0, floor(a / float(origin.z)) / 255.0);
}

// A depth buffer read as a colour texture: the 24-bit Z as the GS memory holds it (low byte in red), alpha 0.
fragment float4 depthAsColorFragment(QuadOut in [[stage_in]], depth2d<float> depth [[texture(0)]])
{
    const float d = depth.read(uint2(in.position.xy));
    const uint z = uint(clamp(round(d * 16777216.0), 0.0, 16777215.0));
    return float4(float(z & 255u), float((z >> 8) & 255u), float(z >> 16), 0.0) / 255.0;
}

// The target seen as 16-bit pixels: each 32-bit pixel is two of them (red+green, blue+alpha). A full-screen copy
// in that view moves one half of the source word into one half of the destination word, under a write mask.
fragment float4 shuffleFragment(QuadOut in [[stage_in]], float4 destination [[color(0)]], depth2d<float> depth [[texture(0)]], constant ShuffleUniforms &u [[buffer(1)]])
{
    uint4 d = uint4(round(destination * 255.0));
    uint word = d.r | (d.g << 8) | (d.b << 16) | (d.a << 24);
    uint source = word;
    if (u.sourceIsDepth != 0) source = uint(depth.read(uint2(in.position.xy)) * 16777216.0 + 0.5);
    uint from = (source >> (16 * u.sourceHalf)) & 0xFFFFu;
    uint shift = 16 * u.destinationHalf;
    uint old = (word >> shift) & 0xFFFFu;
    uint merged = (old & u.mask16) | (from & ~u.mask16 & 0xFFFFu);
    word = (word & ~(0xFFFFu << shift)) | (merged << shift);
    return float4(float(word & 255u), float((word >> 8) & 255u), float((word >> 16) & 255u), float(word >> 24)) / 255.0;
}

struct PresentUniforms
{
    float originX, originY, scaleX, scaleY;
    float fxaa; // 1: smooth polygon edges (luminance-contrast edge filter), 0: plain sampling
    float gamma, brightness; // display adjustment: out = (c ^ (1 / gamma)) * brightness
};

float4 presentAdjust(float3 c, constant PresentUniforms &u)
{
    return float4(saturate(pow(max(c, 0.0), float3(1.0 / u.gamma)) * u.brightness), 1.0);
}

float presentLuma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

// The displayed part of a target, drawn at the window's own pixel size. With fxaa on, an edge (a strong luminance step
// against the four neighbours) is blended along its direction, which smooths the stair steps of polygon edges.
fragment float4 presentFragment(QuadOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]], constant PresentUniforms &u [[buffer(1)]])
{
    const float2 uv = float2(u.originX, u.originY) + in.position.xy * float2(u.scaleX, u.scaleY);
    const float3 c = tex.sample(smp, uv).rgb;
    if (u.fxaa == 0.0)
        return presentAdjust(c, u);
    const float2 texel = 1.0 / float2(tex.get_width(), tex.get_height());
    const float lC = presentLuma(c);
    const float lN = presentLuma(tex.sample(smp, uv + float2(0.0, -texel.y)).rgb);
    const float lS = presentLuma(tex.sample(smp, uv + float2(0.0, texel.y)).rgb);
    const float lW = presentLuma(tex.sample(smp, uv + float2(-texel.x, 0.0)).rgb);
    const float lE = presentLuma(tex.sample(smp, uv + float2(texel.x, 0.0)).rgb);
    const float lMin = min(lC, min(min(lN, lS), min(lW, lE)));
    const float lMax = max(lC, max(max(lN, lS), max(lW, lE)));
    const float range = lMax - lMin;
    if (range < max(0.0625, lMax * 0.125))
        return presentAdjust(c, u); // flat area: no smoothing
    // Edge direction: across the larger luminance gradient; blend two samples along the edge.
    const float2 gradient = float2((lW + lE) - 2.0 * lC, (lN + lS) - 2.0 * lC);
    const float2 along = abs(gradient.x) > abs(gradient.y) ? float2(0.0, texel.y) : float2(texel.x, 0.0);
    const float3 a = tex.sample(smp, uv + along * 0.5).rgb;
    const float3 b = tex.sample(smp, uv - along * 0.5).rgb;
    const float3 across1 = tex.sample(smp, uv + (along.x > 0.0 ? float2(0.0, texel.y) : float2(texel.x, 0.0)) * 0.5).rgb;
    const float3 across2 = tex.sample(smp, uv - (along.x > 0.0 ? float2(0.0, texel.y) : float2(texel.x, 0.0)) * 0.5).rgb;
    const float strength = saturate(range / max(lMax, 1e-4) * 2.0);
    const float3 smoothed = (a + b) * 0.375 + (across1 + across2) * 0.125;
    return presentAdjust(mix(c, smoothed, strength * 0.75), u);
}

struct DepthOut
{
    float depth [[depth(any)]];
};

// A depth buffer drawn to as if it were colour, textured with another depth buffer: a depth-to-depth copy.
fragment DepthOut depthCopyFragment(VertexOut in [[stage_in]], depth2d<float> source [[texture(0)]], sampler smp [[sampler(0)]], constant Uniforms &u [[buffer(1)]])
{
    DepthOut out;
    out.depth = source.sample(smp, in.stq.xy / in.stq.z * float2(u.texScaleU, u.texScaleV));
    return out;
}

struct DepthAndColorOut
{
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};

// The same copy while ZBUF points at a colour target: the sprite's own Z lands there as pixel bytes (a clear in disguise).
fragment DepthAndColorOut depthCopyClearFragment(VertexOut in [[stage_in]], depth2d<float> source [[texture(0)]], sampler smp [[sampler(0)]], constant Uniforms &u [[buffer(1)]])
{
    DepthAndColorOut out;
    out.depth = source.sample(smp, in.stq.xy / in.stq.z * float2(u.texScaleU, u.texScaleV));
    uint word = uint(in.position.z * 16777216.0 + 0.5);
    out.color = float4(float(word & 255u), float((word >> 8) & 255u), float((word >> 16) & 255u), float(word >> 24)) / 255.0;
    return out;
}

static float4 shade(VertexOut in, float4 destination, texture2d<float> tex, texture2d<float> palette, sampler smp, constant Uniforms &u)
{
#if defined(GS_TRIVIAL)
    return float4(in.color.rgb / 255.0, 1.0); // PS2X_GS_NATIVE_TRIVIAL=1: what rasterizing alone costs (diagnostic)
#endif
    // Everything below is in GS units: 0..255 per channel, alpha 128 meaning 1.0.
    float4 c = floor(in.color);
    if (S(tme) != 0)
    {
        float2 coordinate = in.stq.xy / in.stq.z;
        // How far the texture coordinate moves per pixel of ours (before the region modes bend it).
        const float2 stepX = dfdx(coordinate) * float2(u.texScaleU, u.texScaleV), stepY = dfdy(coordinate) * float2(u.texScaleU, u.texScaleV);
        if (S(wrapU) >= 2 || S(wrapV) >= 2)
        {
            // Region modes work on texel coordinates: clamp to [MIN, MAX], or (texel & MIN) | MAX (unfiltered).
            float2 texel = coordinate * float2(u.texWidth, u.texHeight);
            if (S(wrapU) == 2) texel.x = clamp(texel.x, u.minU + 0.5, u.maxU + 0.5);
            else if (S(wrapU) == 3) texel.x = float((int(floor(texel.x)) & int(u.minU)) | int(u.maxU)) + 0.5;
            else if (S(wrapU) == 4) texel.x = fract(coordinate.x) * u.texWidth;
            if (S(wrapV) == 2) texel.y = clamp(texel.y, u.minV + 0.5, u.maxV + 0.5);
            else if (S(wrapV) == 3) texel.y = float((int(floor(texel.y)) & int(u.minV)) | int(u.maxV)) + 0.5;
            else if (S(wrapV) == 4) texel.y = fract(coordinate.y) * u.texHeight;
            coordinate = texel / float2(u.texWidth, u.texHeight);
        }
        coordinate *= float2(u.texScaleU, u.texScaleV);
        float4 t;
        if (S(texSelf) != 0)
        {
            // The game re-reads the frame in place (each pixel reads itself): that is the destination pixel, with no
            // copy of the target and no break in the pass.
            if (S(texIndexed) != 0)
            {
                uint index = uint(round(destination.a * 255.0));
                uint entry = (index & 0xE7u) | ((index & 0x08u) << 1) | ((index & 0x10u) >> 1);
                t = round(palette.read(uint2(entry & 15u, entry >> 4) * u.paletteScale + u.paletteScale / 2) * 255.0);
            }
            else
                t = round(destination * 255.0);
        }
        else if (S(texIndexed) == 2)
        {
            // A 32-bit target read as 16-bit texels (PSMCT16S/16 view of the same memory): the palette slot holds a
            // 64x64 table giving, for each 16-bit texel of a page, the 32-bit pixel of that page holding it and
            // which half. paletteScale carries the buffer width, texBox the target's scale.
            const uint tbw = max(u.paletteScale, 1u), scale = max(u.texBox, 1u);
            const uint2 t16 = uint2(clamp(floor(coordinate * float2(u.texWidth, u.texHeight)), float2(0.0), float2(u.texWidth, u.texHeight) - 1.0));
            const uint page = (t16.x >> 6) + (t16.y >> 6) * tbw;
            const uint4 map = uint4(round(palette.read(uint2(t16.x & 63u, t16.y & 63u)) * 255.0));
            const uint2 p32 = uint2((page % tbw) * 64u + map.x, (page / tbw) * 32u + map.y);
            const uint2 at = min(p32 * scale + scale / 2u, uint2(tex.get_width() - 1, tex.get_height() - 1));
            const uint4 b = uint4(round(tex.read(at) * 255.0));
            const uint h = map.z != 0u ? (b.b | (b.a << 8)) : (b.r | (b.g << 8));
            t = float4(float((h & 31u) << 3), float(((h >> 5) & 31u) << 3), float(((h >> 10) & 31u) << 3), (h >> 15) != 0u ? 128.0 : float(u.ta0));
        }
        else if (S(texIndexed) != 0)
        {
            float2 size = float2(tex.get_width(), tex.get_height());
            float2 at = coordinate * size;
            if (S(texLinear) != 0)
            {
                at -= 0.5;
                float2 base = floor(at), f = at - base;
                float4 top = mix(indexedTexel(tex, palette, base, size, u.paletteScale), indexedTexel(tex, palette, base + float2(1.0, 0.0), size, u.paletteScale), f.x);
                float4 bottom = mix(indexedTexel(tex, palette, base + float2(0.0, 1.0), size, u.paletteScale), indexedTexel(tex, palette, base + float2(1.0, 1.0), size, u.paletteScale), f.x);
                t = floor(mix(top, bottom, f.y));
            }
            else
                t = indexedTexel(tex, palette, floor(at), size, u.paletteScale);
        }
        else
        {
            // The GS picks the mip level from Q, not from screen-space derivatives: (log2(1/Q) << L) + K, rounded.
            float lod = 0.0;
            if (HAS_LEVELS)
            {
                lod = LOD_FIXED ? u.lodK : log2(1.0 / abs(in.stq.z)) * u.lodScale + u.lodK;
                lod = clamp(floor(lod + 0.5), 0.0, float(u.maxLevel));
            }
            if (u.texReplaced != 0)
            {
                // A replacement from a texture pack: its own alpha is 0..255 (opaque = 255), the GS works in 0..128.
                t = round(tex.sample(smp, coordinate) * 255.0);
                t.a = floor(t.a * 128.0 / 255.0 + 0.5);
            }
            else if (S(texBox) > 1)
            {
                // A render target drawn larger, read with the GS's bilinear filter: the game counts on that filter
                // mixing neighbouring texels of its own size (it blurs by reading half a texel off). One of its
                // texels is texBox of ours across, so the same mix is the mean over that many of our samples.
                // Four filtered samples, a quarter of the game's texel away from the centre on each axis: together they
                // weigh the texBox x texBox texels of ours about evenly, at any scale (texBox squared samples would be
                // nine at 3x and sixteen at 4x, on passes that cover the whole screen).
                float2 reach = float(S(texBox)) * 0.25 / float2(tex.get_width(), tex.get_height());
                float4 sum = tex.sample(smp, coordinate + float2(-reach.x, -reach.y), level(0.0)) + tex.sample(smp, coordinate + float2(reach.x, -reach.y), level(0.0)) +
                             tex.sample(smp, coordinate + float2(-reach.x, reach.y), level(0.0)) + tex.sample(smp, coordinate + float2(reach.x, reach.y), level(0.0));
                t = round(sum * 0.25 * 255.0);
            }
#if !defined(GS_HW_BILINEAR)
            else if (S(texLinear) != 0 && lod == 0.0)
            {
                // The GS's bilinear filter: weights in sixteenths of a texel, the result truncated. The hardware filter
                // (finer weights, rounded) comes out a little higher, which Silent Hill Origins' flashlight turns into
                // shadow stripes: it compares a filtered depth ramp against its shadow map by subtracting them.
                const float2 size = float2(tex.get_width(), tex.get_height());
                const float2 at = coordinate * size - 0.5;
                const float2 snapped = (floor(at) + floor(fract(at) * 16.0) / 16.0 + 0.5) / size;
                t = floor(tex.sample(smp, snapped, level(0.0)) * 255.0 + 1.0 / 64.0);
            }
#endif
#if !defined(GS_NO_DOWNFILTER)
            else if (u.nativeTexel != 0u && lod == 0.0 && max(length(stepX * float2(tex.get_width(), tex.get_height())), length(stepY * float2(tex.get_width(), tex.get_height()))) * u.pointSize > 1.05)
            {
                // A texture drawn smaller than it is, unfiltered: at the game's resolution each pixel shows one texel out
                // of several and the fine detail drops out; drawn larger, every texel shows. Silent Hill Origins' map is
                // built from 16x16 tiles drawn about half size, and showed their dotted edges as broken lines. Here a
                // pixel averages the texels one pixel of the game's covers (spread a little less, so the edge of a tile
                // does not reach into its neighbour in the atlas).
                // The texel the game's own pixel would show: the coordinate at the centre of the game pixel this pixel
                // belongs to, so detail the GS skips stays skipped (measured: 4x then sampled = the 1x picture).
                const float scale = u.pointSize;
                const float2 offset = (floor(in.position.xy / scale) + 0.5) * scale - in.position.xy;
                t = round(tex.sample(smp, coordinate + stepX * offset.x + stepY * offset.y, level(0.0)) * 255.0);
            }
#endif
            else
                t = round(tex.sample(smp, coordinate, level(lod)) * 255.0);
        }
        if (S(texAlphaFixed) != 0) t.a = float(u.ta0);
        float3 modulated = min(floor(t.rgb * c.rgb / 128.0), 255.0);
        if (S(tfx) == 0)
        {
            c.rgb = modulated;
            if (S(tcc) != 0) c.a = min(floor(t.a * c.a / 128.0), 255.0);
        }
        else if (S(tfx) == 1)
        {
            c.rgb = t.rgb;
            if (S(tcc) != 0) c.a = t.a;
        }
        else
        {
            c.rgb = min(modulated + c.a, 255.0);
            if (S(tcc) != 0) c.a = S(tfx) == 2 ? min(t.a + c.a, 255.0) : t.a;
        }
    }
    if (S(fge) != 0)
    {
        float f = floor(in.fog);
        c.rgb = floor((c.rgb * f + float3(u.fogR, u.fogG, u.fogB) * (256.0 - f)) / 256.0);
    }
    if (S(alphaMode) != 0)
    {
        float a = c.a, r = float(u.aref);
        bool pass = S(atst) == 0 ? false : S(atst) == 1 ? true : S(atst) == 2 ? a < r : S(atst) == 3 ? a <= r : S(atst) == 4 ? a == r : S(atst) == 5 ? a >= r : S(atst) == 6 ? a > r : a != r;
        if (pass != (S(alphaMode) == 1)) discard_fragment();
    }
    float4 d = round(destination * 255.0);
    if (S(writeMode) == 1) return destination;
    float3 rgb = c.rgb;
    if (S(abe) != 0)
    {
        float3 A = S(blendA) == 0 ? c.rgb : S(blendA) == 1 ? d.rgb : float3(0.0);
        float3 B = S(blendB) == 0 ? c.rgb : S(blendB) == 1 ? d.rgb : float3(0.0);
        float C = S(blendC) == 0 ? c.a : S(blendC) == 1 ? (S(destinationHasAlpha) != 0 ? d.a : 128.0) : float(u.fix);
        float3 D = S(blendD) == 0 ? c.rgb : S(blendD) == 1 ? d.rgb : float3(0.0);
        rgb = floor((A - B) * C / 128.0) + D;
    }
    rgb = S(colclamp) != 0 ? clamp(rgb, 0.0, 255.0) : rgb - 256.0 * floor(rgb / 256.0);
    float alpha = S(fba) != 0 && c.a < 128.0 ? c.a + 128.0 : c.a;
    if (S(writeMode) == 2) alpha = d.a;
    uint4 o = uint4(float4(rgb, clamp(alpha, 0.0, 255.0)));
    if (HAS_MASK)
    {
        uint packed = o.r | (o.g << 8) | (o.b << 16) | (o.a << 24);
        uint4 di = uint4(d);
        uint old = di.r | (di.g << 8) | (di.b << 16) | (di.a << 24);
        packed = (packed & ~u.fbmask) | (old & u.fbmask);
        o = uint4(packed & 255, (packed >> 8) & 255, (packed >> 16) & 255, packed >> 24);
    }
    return float4(o) / 255.0;
}

// Reads the pixel it writes to (programmable blending): for everything the GS mixes with or masks against the frame.
fragment float4 fragmentMain(VertexOut in [[stage_in]], float4 destination [[color(0)]], texture2d<float> tex [[texture(0)]], texture2d<float> palette [[texture(1)]],
                             sampler smp [[sampler(0)]], constant Uniforms &u [[buffer(1)]])
{
    return shade(in, destination, tex, palette, smp, u);
}

// The same without that read, for draws that just replace the pixel (no blending, no write mask, not the
// second pass of an alpha test): the GPU can then drop hidden fragments instead of shading every layer in order.
fragment float4 fragmentOpaque(VertexOut in [[stage_in]], texture2d<float> tex [[texture(0)]], texture2d<float> palette [[texture(1)]],
                               sampler smp [[sampler(0)]], constant Uniforms &u [[buffer(1)]])
{
    return shade(in, float4(0.0), tex, palette, smp, u);
}
)MSL"
