// The native GS renderer's shaders for Vulkan: a line-for-line port of gs_shaders_msl.inc (keep the two in step).
// One file, one entry point per build: compile with -DSTAGE_<NAME> (see gpu/native/compile_shaders.sh), e.g.
//   glslangValidator -V --target-env vulkan1.1 -S frag -DSTAGE_FS_MAIN -DGS_SPECIALIZED gs_shaders.glsl -o fs_main_spec.spv
//
// Differences from Metal, and why:
// - Metal's [[color(0)]] (the pixel being drawn to, read in the shader) is an input attachment here: the colour target
//   is bound both as attachment and as input, with a by-region self-dependency between draws (gpu_vulkan.cpp).
// - Function constants are specialization constants with the same numbers.
// - Uniforms come from one dynamic uniform buffer (binding 0) for both stages; textures and the sampler are separate
//   bindings, as in the Metal code (texture 0 -> binding 1, texture 1 -> binding 2, sampler 0 -> binding 3).
// - Vulkan's clip space has y pointing down: the vertex shaders flip it so the same pixels are covered.
// - round(): Metal rounds halves away from zero, GLSL leaves it open; every value rounded here is >= 0, so
//   floor(x + 0.5) gives Metal's result.
#version 450

#define RND(x) floor((x) + 0.5)

#if defined(STAGE_VS_MAIN) || defined(STAGE_FS_MAIN) || defined(STAGE_FS_OPAQUE) || defined(STAGE_FS_DEPTH_COPY) || defined(STAGE_FS_DEPTH_COPY_CLEAR)
layout(set = 0, binding = 0, std140) uniform UniformBlock
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
} u;
// std140 lays scalars out tightly like the C++ struct (all 4-byte members, no vectors): the same bytes work for both.
#endif

layout(set = 0, binding = 1) uniform texture2D tex0;
layout(set = 0, binding = 2) uniform texture2D tex1;
layout(set = 0, binding = 3) uniform sampler smp;
#define TEX sampler2D(tex0, smp)
#define PALETTE sampler2D(tex1, smp)

#if defined(GS_SPECIALIZED)
layout(constant_id = 0) const uint k_tme = 0u;
layout(constant_id = 1) const uint k_tfx = 0u;
layout(constant_id = 2) const uint k_tcc = 0u;
layout(constant_id = 3) const uint k_fge = 0u;
layout(constant_id = 4) const uint k_abe = 0u;
layout(constant_id = 5) const uint k_blendA = 0u;
layout(constant_id = 6) const uint k_blendB = 0u;
layout(constant_id = 7) const uint k_blendC = 0u;
layout(constant_id = 8) const uint k_blendD = 0u;
layout(constant_id = 9) const uint k_atst = 0u;
layout(constant_id = 10) const uint k_alphaMode = 0u;
layout(constant_id = 11) const uint k_writeMode = 0u;
layout(constant_id = 12) const uint k_fba = 0u;
layout(constant_id = 13) const uint k_colclamp = 0u;
layout(constant_id = 14) const uint k_destinationHasAlpha = 0u;
layout(constant_id = 15) const uint k_texAlphaFixed = 0u;
layout(constant_id = 16) const uint k_texIndexed = 0u;
layout(constant_id = 17) const uint k_texLinear = 0u;
layout(constant_id = 18) const uint k_wrapU = 0u;
layout(constant_id = 19) const uint k_wrapV = 0u;
layout(constant_id = 20) const uint k_texBox = 0u;
layout(constant_id = 21) const uint k_texSelf = 0u;
layout(constant_id = 22) const uint k_hasLevels = 0u;
layout(constant_id = 23) const uint k_lodFixed = 0u;
layout(constant_id = 24) const uint k_hasMask = 0u;
#define S(field) k_##field
#define HAS_LEVELS (k_hasLevels != 0u)
#define LOD_FIXED (k_lodFixed != 0u)
#define HAS_MASK (k_hasMask != 0u)
#else
#define S(field) u.field
#define HAS_LEVELS (u.maxLevel != 0u)
#define LOD_FIXED (u.lodFixed != 0u)
#define HAS_MASK (u.fbmask != 0u)
#endif

// ---------------------------------------------------------------------------------------------------------------- vertex

#if defined(STAGE_VS_MAIN)
layout(location = 0) in vec3 inPosition;
layout(location = 1) in uvec4 inColor; // uchar4 as integers (R8G8B8A8_UINT, a format every GPU reads): the GS's 0..255
layout(location = 2) in vec3 inStq;
layout(location = 3) in float inFog;
layout(location = 0) noperspective out vec4 vColor;
layout(location = 1) noperspective out vec3 vStq;
layout(location = 2) noperspective out float vFog;
void main()
{
    // The GS samples a pixel at its integer coordinate, the GPU at the centre: half a pixel of shift makes
    // both cover the same pixels.
    gl_Position = vec4((inPosition.x + 0.5) / u.targetWidth * 2.0 - 1.0, (inPosition.y + 0.5) / u.targetHeight * 2.0 - 1.0, inPosition.z, 1.0);
    vColor = vec4(inColor);
    vStq = inStq;
    vFog = inFog;
    gl_PointSize = u.pointSize;
}
#endif

#if defined(STAGE_VS_QUAD)
void main()
{
    const int id = gl_VertexIndex;
    gl_Position = vec4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0.0, 1.0);
}
#endif

// -------------------------------------------------------------------------------------------------------------- fragment

#if defined(STAGE_FS_MAIN) || defined(STAGE_FS_SHUFFLE)
// The pixel being drawn to (Metal: [[color(0)]]).
layout(input_attachment_index = 0, set = 0, binding = 4) uniform subpassInput destinationInput;
#endif

#if defined(STAGE_FS_MAIN) || defined(STAGE_FS_OPAQUE) || defined(STAGE_FS_DEPTH_COPY) || defined(STAGE_FS_DEPTH_COPY_CLEAR)
layout(location = 0) noperspective in vec4 vColor;
layout(location = 1) noperspective in vec3 vStq;
layout(location = 2) noperspective in float vFog;
#endif

#if defined(STAGE_FS_NIBBLE)
// Metal passes a uint4 at buffer(1); here the first four words of the uniform block are reinterpreted. To keep the
// layout simple the backend writes the uint4 to the start of the same dynamic buffer: read it as raw words.
layout(set = 0, binding = 0, std140) uniform NibbleBlock { uvec4 origin; } nib;
layout(location = 0) out vec4 outColor;
void main()
{
    const float a = RND(texelFetch(TEX, ivec2(uvec2(gl_FragCoord.xy) + nib.origin.xy), 0).a * 255.0);
    outColor = vec4(0.0, 0.0, 0.0, floor(a / float(nib.origin.z)) / 255.0);
}
#endif

#if defined(STAGE_FS_DEPTH_AS_COLOR)
layout(location = 0) out vec4 outColor;
void main()
{
    const float d = texelFetch(TEX, ivec2(gl_FragCoord.xy), 0).r;
    const uint z = uint(clamp(RND(d * 16777216.0), 0.0, 16777215.0));
    outColor = vec4(float(z & 255u), float((z >> 8) & 255u), float(z >> 16), 0.0) / 255.0;
}
#endif

#if defined(STAGE_FS_SHUFFLE)
layout(set = 0, binding = 0, std140) uniform ShuffleBlock { uint sourceHalf, destinationHalf, mask16, sourceIsDepth; } sh;
layout(location = 0) out vec4 outColor;
void main()
{
    const vec4 destination = subpassLoad(destinationInput);
    const uvec4 d = uvec4(RND(destination * 255.0));
    uint word = d.r | (d.g << 8) | (d.b << 16) | (d.a << 24);
    uint source = word;
    if (sh.sourceIsDepth != 0u) source = uint(texelFetch(TEX, ivec2(gl_FragCoord.xy), 0).r * 16777216.0 + 0.5);
    const uint from = (source >> (16u * sh.sourceHalf)) & 0xFFFFu;
    const uint shift = 16u * sh.destinationHalf;
    const uint old = (word >> shift) & 0xFFFFu;
    const uint merged = (old & sh.mask16) | (from & ~sh.mask16 & 0xFFFFu);
    word = (word & ~(0xFFFFu << shift)) | (merged << shift);
    outColor = vec4(float(word & 255u), float((word >> 8) & 255u), float((word >> 16) & 255u), float(word >> 24)) / 255.0;
}
#endif

#if defined(STAGE_FS_PRESENT)
layout(set = 0, binding = 0, std140) uniform PresentBlock
{
    float originX, originY, scaleX, scaleY;
    float fxaa;
    float gamma, brightness;
} pu;
layout(location = 0) out vec4 outColor;
vec4 presentAdjust(vec3 c) { return vec4(clamp(pow(max(c, 0.0), vec3(1.0 / pu.gamma)) * pu.brightness, 0.0, 1.0), 1.0); }
float presentLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main()
{
    const vec2 uv = vec2(pu.originX, pu.originY) + gl_FragCoord.xy * vec2(pu.scaleX, pu.scaleY);
    const vec3 c = texture(TEX, uv).rgb;
    if (pu.fxaa == 0.0)
    {
        outColor = presentAdjust(c);
        return;
    }
    const vec2 texel = 1.0 / vec2(textureSize(TEX, 0));
    const float lC = presentLuma(c);
    const float lN = presentLuma(texture(TEX, uv + vec2(0.0, -texel.y)).rgb);
    const float lS = presentLuma(texture(TEX, uv + vec2(0.0, texel.y)).rgb);
    const float lW = presentLuma(texture(TEX, uv + vec2(-texel.x, 0.0)).rgb);
    const float lE = presentLuma(texture(TEX, uv + vec2(texel.x, 0.0)).rgb);
    const float lMin = min(lC, min(min(lN, lS), min(lW, lE)));
    const float lMax = max(lC, max(max(lN, lS), max(lW, lE)));
    const float range = lMax - lMin;
    if (range < max(0.0625, lMax * 0.125))
    {
        outColor = presentAdjust(c);
        return;
    }
    const vec2 gradient = vec2((lW + lE) - 2.0 * lC, (lN + lS) - 2.0 * lC);
    const vec2 along = abs(gradient.x) > abs(gradient.y) ? vec2(0.0, texel.y) : vec2(texel.x, 0.0);
    const vec3 a = texture(TEX, uv + along * 0.5).rgb;
    const vec3 b = texture(TEX, uv - along * 0.5).rgb;
    const vec2 across = along.x > 0.0 ? vec2(0.0, texel.y) : vec2(texel.x, 0.0);
    const vec3 across1 = texture(TEX, uv + across * 0.5).rgb;
    const vec3 across2 = texture(TEX, uv - across * 0.5).rgb;
    const float strength = clamp(range / max(lMax, 1e-4) * 2.0, 0.0, 1.0);
    const vec3 smoothed = (a + b) * 0.375 + (across1 + across2) * 0.125;
    outColor = presentAdjust(mix(c, smoothed, strength * 0.75));
}
#endif

#if defined(STAGE_FS_DEPTH_COPY)
void main()
{
    gl_FragDepth = texture(TEX, vStq.xy / vStq.z * vec2(u.texScaleU, u.texScaleV)).r;
}
#endif

#if defined(STAGE_FS_DEPTH_COPY_CLEAR)
layout(location = 0) out vec4 outColor;
void main()
{
    gl_FragDepth = texture(TEX, vStq.xy / vStq.z * vec2(u.texScaleU, u.texScaleV)).r;
    const uint word = uint(gl_FragCoord.z * 16777216.0 + 0.5);
    outColor = vec4(float(word & 255u), float((word >> 8) & 255u), float((word >> 16) & 255u), float(word >> 24)) / 255.0;
}
#endif

#if defined(STAGE_FS_MAIN) || defined(STAGE_FS_OPAQUE)
layout(location = 0) out vec4 outColor;

// A render target's alpha channel used as an 8-bit texture: palette lookup per texel, filtered afterwards as the GS does.
vec4 indexedTexel(vec2 at, vec2 size, uint paletteScale)
{
    const uvec2 where = uvec2(clamp(at, vec2(0.0), size - 1.0));
    const uint index = uint(RND(texelFetch(TEX, ivec2(where), 0).a * 255.0));
    // The palette texture keeps the layout it has in VRAM: 16x16, with the two middle index bits swapped.
    const uint entry = (index & 0xE7u) | ((index & 0x08u) << 1) | ((index & 0x10u) >> 1);
    return RND(texelFetch(PALETTE, ivec2(uvec2(entry & 15u, entry >> 4) * paletteScale + paletteScale / 2u), 0) * 255.0);
}

// Returns false where Metal's discard_fragment() was.
bool shade(vec4 destination, out vec4 result)
{
#if defined(GS_TRIVIAL)
    result = vec4(vColor.rgb / 255.0, 1.0);
    return true;
#endif
    // Everything below is in GS units: 0..255 per channel, alpha 128 meaning 1.0.
    vec4 c = floor(vColor);
    // Derivatives outside the branches (GLSL wants them in uniform control flow).
    const vec2 baseCoordinate = vStq.xy / vStq.z;
    const vec2 stepX = dFdx(baseCoordinate) * vec2(u.texScaleU, u.texScaleV), stepY = dFdy(baseCoordinate) * vec2(u.texScaleU, u.texScaleV);
    if (S(tme) != 0u)
    {
        vec2 coordinate = baseCoordinate;
        if (S(wrapU) >= 2u || S(wrapV) >= 2u)
        {
            // Region modes work on texel coordinates: clamp to [MIN, MAX], or (texel & MIN) | MAX (unfiltered).
            vec2 texel = coordinate * vec2(u.texWidth, u.texHeight);
            if (S(wrapU) == 2u) texel.x = clamp(texel.x, u.minU + 0.5, u.maxU + 0.5);
            else if (S(wrapU) == 3u) texel.x = float((int(floor(texel.x)) & int(u.minU)) | int(u.maxU)) + 0.5;
            else if (S(wrapU) == 4u) texel.x = fract(coordinate.x) * u.texWidth;
            if (S(wrapV) == 2u) texel.y = clamp(texel.y, u.minV + 0.5, u.maxV + 0.5);
            else if (S(wrapV) == 3u) texel.y = float((int(floor(texel.y)) & int(u.minV)) | int(u.maxV)) + 0.5;
            else if (S(wrapV) == 4u) texel.y = fract(coordinate.y) * u.texHeight;
            coordinate = texel / vec2(u.texWidth, u.texHeight);
        }
        coordinate *= vec2(u.texScaleU, u.texScaleV);
        vec4 t;
        const vec2 size = vec2(textureSize(TEX, 0));
        if (S(texSelf) != 0u)
        {
            // The game re-reads the frame in place (each pixel reads itself): that is the destination pixel.
            if (S(texIndexed) != 0u)
            {
                const uint index = uint(RND(destination.a * 255.0));
                const uint entry = (index & 0xE7u) | ((index & 0x08u) << 1) | ((index & 0x10u) >> 1);
                t = RND(texelFetch(PALETTE, ivec2(uvec2(entry & 15u, entry >> 4) * u.paletteScale + u.paletteScale / 2u), 0) * 255.0);
            }
            else
                t = RND(destination * 255.0);
        }
        else if (S(texIndexed) == 2u)
        {
            // A 32-bit target read as 16-bit texels (see the Metal version).
            const uint tbw = max(u.paletteScale, 1u), scale = max(u.texBox, 1u);
            const uvec2 t16 = uvec2(clamp(floor(coordinate * vec2(u.texWidth, u.texHeight)), vec2(0.0), vec2(u.texWidth, u.texHeight) - 1.0));
            const uint page = (t16.x >> 6) + (t16.y >> 6) * tbw;
            const uvec4 map = uvec4(RND(texelFetch(PALETTE, ivec2(t16 & 63u), 0) * 255.0));
            const uvec2 p32 = uvec2((page % tbw) * 64u + map.x, (page / tbw) * 32u + map.y);
            const uvec2 at = min(p32 * scale + scale / 2u, uvec2(size) - 1u);
            const uvec4 b = uvec4(RND(texelFetch(TEX, ivec2(at), 0) * 255.0));
            const uint h = map.z != 0u ? (b.b | (b.a << 8)) : (b.r | (b.g << 8));
            t = vec4(float((h & 31u) << 3), float(((h >> 5) & 31u) << 3), float(((h >> 10) & 31u) << 3), (h >> 15) != 0u ? 128.0 : float(u.ta0));
        }
        else if (S(texIndexed) != 0u)
        {
            vec2 at = coordinate * size;
            if (S(texLinear) != 0u)
            {
                at -= 0.5;
                const vec2 base = floor(at), f = at - base;
                const vec4 top = mix(indexedTexel(base, size, u.paletteScale), indexedTexel(base + vec2(1.0, 0.0), size, u.paletteScale), f.x);
                const vec4 bottom = mix(indexedTexel(base + vec2(0.0, 1.0), size, u.paletteScale), indexedTexel(base + vec2(1.0, 1.0), size, u.paletteScale), f.x);
                t = floor(mix(top, bottom, f.y));
            }
            else
                t = indexedTexel(floor(at), size, u.paletteScale);
        }
        else
        {
            // The GS picks the mip level from Q, not from screen-space derivatives: (log2(1/Q) << L) + K, rounded.
            float lod = 0.0;
            if (HAS_LEVELS)
            {
                lod = LOD_FIXED ? u.lodK : log2(1.0 / abs(vStq.z)) * u.lodScale + u.lodK;
                lod = clamp(floor(lod + 0.5), 0.0, float(u.maxLevel));
            }
            if (u.texReplaced != 0u)
            {
                // A replacement from a texture pack: its own alpha is 0..255 (opaque = 255), the GS works in 0..128.
                // Explicit gradients: the same level choice as Metal's implicit sample, valid in non-uniform flow.
                t = RND(textureGrad(TEX, coordinate, dFdx(coordinate), dFdy(coordinate)) * 255.0);
                t.a = floor(t.a * 128.0 / 255.0 + 0.5);
            }
            else if (S(texBox) > 1u)
            {
                // A render target drawn larger, read with the GS's bilinear filter (see the Metal version).
                const vec2 reach = float(S(texBox)) * 0.25 / size;
                const vec4 sum = textureLod(TEX, coordinate + vec2(-reach.x, -reach.y), 0.0) + textureLod(TEX, coordinate + vec2(reach.x, -reach.y), 0.0) +
                                 textureLod(TEX, coordinate + vec2(-reach.x, reach.y), 0.0) + textureLod(TEX, coordinate + vec2(reach.x, reach.y), 0.0);
                t = RND(sum * 0.25 * 255.0);
            }
#if !defined(GS_HW_BILINEAR)
            else if (S(texLinear) != 0u && lod == 0.0)
            {
                // The GS's bilinear filter: weights in sixteenths of a texel, the result truncated.
                const vec2 at = coordinate * size - 0.5;
                const vec2 snapped = (floor(at) + floor(fract(at) * 16.0) / 16.0 + 0.5) / size;
                t = floor(textureLod(TEX, snapped, 0.0) * 255.0 + 1.0 / 64.0);
            }
#endif
#if !defined(GS_NO_DOWNFILTER)
            else if (u.nativeTexel != 0u && lod == 0.0 && max(length(stepX * size), length(stepY * size)) * u.pointSize > 1.05)
            {
                // A texture drawn smaller than it is, unfiltered: the texel the game's own pixel would show.
                const float scale = u.pointSize;
                const vec2 offset = (floor(gl_FragCoord.xy / scale) + 0.5) * scale - gl_FragCoord.xy;
                t = RND(textureLod(TEX, coordinate + stepX * offset.x + stepY * offset.y, 0.0) * 255.0);
            }
#endif
            else
                t = RND(textureLod(TEX, coordinate, lod) * 255.0);
        }
        if (S(texAlphaFixed) != 0u) t.a = float(u.ta0);
        const vec3 modulated = min(floor(t.rgb * c.rgb / 128.0), 255.0);
        if (S(tfx) == 0u)
        {
            c.rgb = modulated;
            if (S(tcc) != 0u) c.a = min(floor(t.a * c.a / 128.0), 255.0);
        }
        else if (S(tfx) == 1u)
        {
            c.rgb = t.rgb;
            if (S(tcc) != 0u) c.a = t.a;
        }
        else
        {
            c.rgb = min(modulated + c.a, 255.0);
            if (S(tcc) != 0u) c.a = S(tfx) == 2u ? min(t.a + c.a, 255.0) : t.a;
        }
    }
    if (S(fge) != 0u)
    {
        const float f = floor(vFog);
        c.rgb = floor((c.rgb * f + vec3(u.fogR, u.fogG, u.fogB) * (256.0 - f)) / 256.0);
    }
    if (S(alphaMode) != 0u)
    {
        const float a = c.a, r = float(u.aref);
        const uint atst = S(atst);
        const bool pass = atst == 0u ? false : atst == 1u ? true : atst == 2u ? a < r : atst == 3u ? a <= r : atst == 4u ? a == r : atst == 5u ? a >= r : atst == 6u ? a > r : a != r;
        if (pass != (S(alphaMode) == 1u))
            return false;
    }
    const vec4 d = RND(destination * 255.0);
    if (S(writeMode) == 1u)
    {
        result = destination;
        return true;
    }
    vec3 rgb = c.rgb;
    if (S(abe) != 0u)
    {
        const vec3 A = S(blendA) == 0u ? c.rgb : S(blendA) == 1u ? d.rgb : vec3(0.0);
        const vec3 B = S(blendB) == 0u ? c.rgb : S(blendB) == 1u ? d.rgb : vec3(0.0);
        const float C = S(blendC) == 0u ? c.a : S(blendC) == 1u ? (S(destinationHasAlpha) != 0u ? d.a : 128.0) : float(u.fix);
        const vec3 D = S(blendD) == 0u ? c.rgb : S(blendD) == 1u ? d.rgb : vec3(0.0);
        rgb = floor((A - B) * C / 128.0) + D;
    }
    rgb = S(colclamp) != 0u ? clamp(rgb, 0.0, 255.0) : rgb - 256.0 * floor(rgb / 256.0);
    float alpha = S(fba) != 0u && c.a < 128.0 ? c.a + 128.0 : c.a;
    if (S(writeMode) == 2u) alpha = d.a;
    uvec4 o = uvec4(vec4(rgb, clamp(alpha, 0.0, 255.0)));
    if (HAS_MASK)
    {
        uint packed = o.r | (o.g << 8) | (o.b << 16) | (o.a << 24);
        const uvec4 di = uvec4(d);
        const uint old = di.r | (di.g << 8) | (di.b << 16) | (di.a << 24);
        packed = (packed & ~u.fbmask) | (old & u.fbmask);
        o = uvec4(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u, packed >> 24);
    }
    result = vec4(o) / 255.0;
    return true;
}

void main()
{
#if defined(STAGE_FS_MAIN)
    const vec4 destination = subpassLoad(destinationInput); // reads the pixel it writes to (programmable blending)
#else
    const vec4 destination = vec4(0.0); // draws that just replace the pixel: no read of the target
#endif
    vec4 result;
    if (!shade(destination, result))
        discard;
    outColor = result;
}
#endif
