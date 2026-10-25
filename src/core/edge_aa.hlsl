// Cheap AA for the plain foveated edges (F9 "PLAIN", key 1), at render resolution, before fovea_compose.hlsl
// stretches them to the output.
//   Mode 1, spatial: FXAA 3.11-style edge search on this frame only. Smooths stair-steps, no temporal stability.
//     Keeps the camera jitter (the compose pass removes it, as for plain edges without AA).
//   Mode 2, temporal: small TAA. Current frame resampled at -jitter (result is unjittered), previous result fetched
//     along the motion vectors (Catmull-Rom), clipped to the 3x3 neighbourhood's colour range (variance clip),
//     blended with weight Alpha for the current frame. Output = next frame's history.
// Colours are linear HDR; edge detection and blending use c / (1 + max(c)) (reversible tonemap) so highlights
// don't dominate.
// Pixels fully under the centre window (its ellipse minus the soft edge and a 2.5 px margin for the stretch's
// bilinear footprint and the jitter) are skipped: fovea_compose never reads them. WinSize 0 = no window.
// Dither: in R11G11B10_FLOAT (6/6/5-bit mantissa) a 10 % blend step is often below half a format step, so the
// history would stall short of the true colour. Stochastic rounding before the store (DitherSmall) fixes the average.
// Lean inputs ([Foveated] LeanInputs, key 3): Cur is the game's double-wide R11G11B10 scene colour read at this
// eye's offset CurOrigin, not the mod's RGBA16F copy at (0,0); InvCur = 1 / that texture's size.
// Otherwise CurOrigin = 0 and InvCur = InvTex.
#define RS "RootFlags(0), RootConstants(num32BitConstants=28, b0), " \
           "DescriptorTable(SRV(t0, numDescriptors=3), UAV(u0, numDescriptors=1)), " \
           "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP)"

cbuffer Constants : register(b0)
{
    uint2  Size;         // this frame's render size (top-left part of the textures)
    uint   Mode;         // 1 spatial, 2 temporal, 3 temporal fast (cross neighbourhood: 5 samples instead of 9)
    uint   Reset;        // temporal: no usable history (first frame, camera cut, mode or size change)
    float2 JitterPx;     // temporal: this frame's jitter in render pixels (times PlainJitterSign)
    float2 InvTex;       // 1 / size of the history and result textures
    float2 HistUvScale;  // temporal: current render pixel -> history uv (previous render size / current, / texture)
    float2 HistUvMax;    // temporal: last valid history sample (uv)
    float  Alpha;        // temporal: weight of the current frame
    float  Gamma;        // temporal: variance clip width (in standard deviations)
    uint   Dither;       // 1 = the output texture is R11G11B10_FLOAT: dither the temporal result before the store
    uint   Frame;        // frame counter (dither noise)
    float2 WinOrigin;    // centre window in render pixels (skip area), size 0 = none
    float2 WinSize;
    float  Inner;        // the window weight is 1 inside this normalised radius (1 - soft edge)
    float2 InvCur;       // 1 / size of Cur
    uint   Pad2;
    int2   CurOrigin;    // this eye's render in Cur starts here (0 for the mod's copy)
    uint2  Pad3;
};

Texture2D<float4>   Cur  : register(t0);   // this eye's colour (jittered render): the mod's copy, or the game's at CurOrigin
Texture2D<float4>   Hist : register(t1);   // temporal: previous result (unjittered), else unused
Texture2D<float2>   Mv   : register(t2);   // motion vectors: render pixels from the current pixel to its previous position
SamplerState        Lin  : register(s0);
RWTexture2D<float4> Out  : register(u0);

float  Max3(float3 c) { return max(c.r, max(c.g, c.b)); }
float3 Tm(float3 c) { c = max(c, 0.0); return c / (1.0 + Max3(c)); }
float3 TmInv(float3 t) { return t / max(1.0 - Max3(t), 1e-3); }
float  LumaT(float3 t) { return sqrt(dot(t, float3(0.299, 0.587, 0.114))); }   // perceptual-ish luma of a tonemapped colour
// Cur access in render coordinates: pixel p / uv of the render (in Cur's texel size), moved to this eye's part
float4 CurLoad(int2 p) { return Cur.Load(int3(p + CurOrigin, 0)); }
float4 CurSample(float2 uv) { return Cur.SampleLevel(Lin, uv + float2(CurOrigin) * InvCur, 0); }
float  LumaAt(float2 uv) { return LumaT(Tm(CurSample(uv).rgb)); }
float3 LoadT(int2 p) { return Tm(CurLoad(clamp(p, int2(0, 0), int2(Size) - 1)).rgb); }
uint   Hash(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
// Stochastic rounding to a value R11G11B10_FLOAT holds exactly (R, G: 6-bit mantissa; B: 5-bit; exponent >= -14),
// down or up with the probability that makes the average exact, so the GPU's own rounding on store doesn't matter.
// The RTX 5080 truncates: plain stores come back -0.5 step on average, and the TAA feedback turns that into a
// history ~5 steps (5-11 %) too dark.
float3 DitherSmall(float3 c, int2 p)
{
    const uint h = Hash(uint(p.x) + Hash(uint(p.y) + Hash(Frame)));
    const float3 u = float3(h & 1023u, (h >> 10) & 1023u, (h >> 20) & 1023u) / 1024.0;   // [0, 1)
    c = max(c, 0.0);
    const float3 step = exp2(max(floor(log2(max(c, 1e-30))), -14.0) - float3(6.0, 6.0, 5.0));
    return floor(c / step + u) * step;
}

// ---------------------------------------------------------------- spatial (FXAA 3.11 quality, ~preset 12)
float4 Spatial(int2 p)
{
    const float2 uvMin = 0.5 * InvCur, uvMax = (float2(Size) - 0.5) * InvCur;
    const float2 uvM = (float2(p) + 0.5) * InvCur;
    const float4 cM = CurLoad(p);
    const float lM = LumaT(Tm(cM.rgb));
    float lN = LumaT(LoadT(p + int2(0, -1))), lS = LumaT(LoadT(p + int2(0, 1)));
    const float lW = LumaT(LoadT(p + int2(-1, 0))), lE = LumaT(LoadT(p + int2(1, 0)));
    const float rangeMax = max(max(lN, lS), max(lW, max(lE, lM)));
    const float rangeMin = min(min(lN, lS), min(lW, min(lE, lM)));
    const float range = rangeMax - rangeMin;
    float4 result = cM;
    [branch] if (range >= max(0.0312, rangeMax * 0.125))   // else no visible edge here
    {
    const float lNW = LumaT(LoadT(p + int2(-1, -1))), lNE = LumaT(LoadT(p + int2(1, -1)));
    const float lSW = LumaT(LoadT(p + int2(-1, 1))), lSE = LumaT(LoadT(p + int2(1, 1)));
    const float lNS = lN + lS, lWE = lW + lE, lNWSW = lNW + lSW, lNESE = lNE + lSE;
    const float edgeHorz = abs(-2.0 * lW + lNWSW) + 2.0 * abs(-2.0 * lM + lNS) + abs(-2.0 * lE + lNESE);
    const float edgeVert = abs(-2.0 * lS + lSW + lSE) + 2.0 * abs(-2.0 * lM + lWE) + abs(-2.0 * lN + lNW + lNE);
    const bool horzSpan = edgeHorz >= edgeVert;   // the edge runs horizontally: step across it vertically

    // sub-pixel aliasing (thin features): blend toward the neighbourhood average
    const float subA = saturate(abs((2.0 * (lNS + lWE) + lNWSW + lNESE) / 12.0 - lM) / range);
    const float subB = (-2.0 * subA + 3.0) * subA * subA;
    const float subpix = subB * subB * 0.75;

    if (!horzSpan) { lN = lW; lS = lE; }
    float lengthSign = horzSpan ? InvCur.y : InvCur.x;
    const float gN = lN - lM, gS = lS - lM;
    const bool pairN = abs(gN) >= abs(gS);
    const float gradientScaled = max(abs(gN), abs(gS)) * 0.25;
    if (pairN) lengthSign = -lengthSign;
    const float lNN = (pairN ? lN : lS) + lM;
    const bool lMLTZero = lM - lNN * 0.5 < 0.0;

    float2 posB = uvM;
    if (horzSpan) posB.y += lengthSign * 0.5; else posB.x += lengthSign * 0.5;
    const float2 offNP = horzSpan ? float2(InvCur.x, 0.0) : float2(0.0, InvCur.y);
    float2 posN = posB - offNP, posP = posB + offNP;
    float eN = LumaAt(clamp(posN, uvMin, uvMax)) - lNN * 0.5, eP = LumaAt(clamp(posP, uvMin, uvMax)) - lNN * 0.5;
    bool doneN = abs(eN) >= gradientScaled, doneP = abs(eP) >= gradientScaled;
    const float steps[6] = { 1.5, 2.0, 2.0, 2.0, 4.0, 8.0 };
    [unroll] for (int i = 0; i < 6; ++i)
    {
        if (doneN && doneP) break;
        if (!doneN) { posN -= offNP * steps[i]; eN = LumaAt(clamp(posN, uvMin, uvMax)) - lNN * 0.5; doneN = abs(eN) >= gradientScaled; }
        if (!doneP) { posP += offNP * steps[i]; eP = LumaAt(clamp(posP, uvMin, uvMax)) - lNN * 0.5; doneP = abs(eP) >= gradientScaled; }
    }
    const float dstN = horzSpan ? uvM.x - posN.x : uvM.y - posN.y;
    const float dstP = horzSpan ? posP.x - uvM.x : posP.y - uvM.y;
    const bool directionN = dstN < dstP;
    const bool goodSpan = directionN ? ((eN < 0.0) != lMLTZero) : ((eP < 0.0) != lMLTZero);
    const float pixelOffset = goodSpan ? 0.5 - min(dstN, dstP) / max(dstN + dstP, 1e-6) : 0.0;
    const float offset = max(pixelOffset, subpix);
    float2 uv = uvM;
    if (horzSpan) uv.y += offset * lengthSign; else uv.x += offset * lengthSign;
    result = float4(CurSample(clamp(uv, uvMin, uvMax)).rgb, cM.a);
    }
    return result;
}

// ---------------------------------------------------------------- temporal
// 5-tap Catmull-Rom (bilinear taps, corners dropped), each tap clamped to the history's valid part
float3 HistoryAt(float2 uv)
{
    const float2 texSize = 1.0 / InvTex, uvMin = 0.5 * InvTex;
    const float2 pos = uv * texSize, tc1 = floor(pos - 0.5) + 0.5, f = pos - tc1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 t0 = clamp((tc1 - 1.0) * InvTex, uvMin, HistUvMax);
    const float2 t3 = clamp((tc1 + 2.0) * InvTex, uvMin, HistUvMax);
    const float2 t12 = clamp((tc1 + w2 / w12) * InvTex, uvMin, HistUvMax);
    float3 c = Hist.SampleLevel(Lin, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y);
    c += Hist.SampleLevel(Lin, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y);
    c += Hist.SampleLevel(Lin, t12, 0).rgb * (w12.x * w12.y);
    c += Hist.SampleLevel(Lin, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y);
    c += Hist.SampleLevel(Lin, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);
    const float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(c / wsum, 0.0);
}

// Mode 3 (fast): neighbourhood range from the centre and its 4 direct neighbours (a cross) instead of the 3x3 block.
// That stage is ~35 % of the pass (0.065 -> 0.043 ms without it at 1640x1790). A 1-tap history instead of the 5-tap
// Catmull-Rom saves nothing, and a groupshared 10x10 tile is 7 % slower (the texture cache already serves the
// neighbourhood). Cost of the cross: a slightly looser clamp on diagonal edges, maybe a little more ghosting there.
float4 Temporal(int2 p)
{
    const float2 uvMin = 0.5 * InvCur, uvMax = (float2(Size) - 0.5) * InvCur;
    const float3 cur = Tm(CurSample(clamp((float2(p) + 0.5 - JitterPx) * InvCur, uvMin, uvMax)).rgb);
    float3 o = cur;
    [branch] if (!Reset)
    {
    float3 m1 = 0.0, m2 = 0.0, mn = 1e9, mx = -1e9;
    float n = 0.0;
    const bool cross = Mode == 3;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        [branch] if (!cross || dx == 0 || dy == 0)   // uniform (Mode): the cross skips the 4 diagonal samples
        {
            const float3 t = LoadT(p + int2(dx, dy));
            m1 += t; m2 += t * t; mn = min(mn, t); mx = max(mx, t); n += 1.0;
        }
    }
    const float3 mean = m1 / n, sigma = sqrt(max(m2 / n - mean * mean, 0.0));
    float3 bMin = max(mn, mean - Gamma * sigma), bMax = min(mx, mean + Gamma * sigma);
    // with only the 4 direct neighbours the range can exclude the pixel's own colour (checkerboard: all 4 are the
    // other colour) and pull the history toward them (-25 % contrast). Always include this frame's colour, as most
    // TAAs do.
    if (cross) { bMin = min(bMin, cur); bMax = max(bMax, cur); }
    const float2 prev = float2(p) + 0.5 + Mv.Load(int3(p, 0));
    const bool offscreen = any(prev < 0.0) || any(prev > float2(Size));
    float3 hist = clamp(Tm(HistoryAt(prev * HistUvScale)), bMin, bMax);
    o = lerp(hist, cur, offscreen ? 1.0 : Alpha);
    if (any(isnan(o)) || any(isinf(o))) o = cur;
    }
    float3 lin = TmInv(o);
    [branch] if (Dither) lin = DitherSmall(lin, p);   // not ?:, HLSL evaluates both sides
    return float4(lin, 1.0);
}

bool UnderWindow(float2 px)   // render pixel px is completely under the centre window (WinSize > 0)
{
    const float2 d = ((px + 0.5 - WinOrigin) / WinSize) * 2.0 - 1.0;
    return length(d) + 5.0 / min(WinSize.x, WinSize.y) <= Inner;
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Size.x || id.y >= Size.y) return;
    if (WinSize.x > 0.0 && UnderWindow(float2(id.xy))) return;   // under the centre window: never shown
    // not ?:, HLSL evaluates both sides
    [branch] if (Mode >= 2) Out[id.xy] = Temporal(int2(id.xy));
    else Out[id.xy] = Spatial(int2(id.xy));
}
