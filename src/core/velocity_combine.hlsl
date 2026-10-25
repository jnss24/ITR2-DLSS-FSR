// DLSS/FSR motion vectors (render pixels, pointing to the previous frame) from UE's velocity buffer, with camera
// motion rebuilt from depth where no velocity was written.
// Optional extras:
//   - copy this eye's colour and depth out of the double-wide scene textures into own textures at (0,0)
//     (FSR takes no input sub-rectangle offset);
//   - anti-ghosting mask from the game's separate translucency (alpha = transmittance): 1 where UI text, particles
//     or glass cover the scene, so DLSS/FSR trust the current frame there;
//   - lean inputs (CopyInputs bit 2): the centre window's colour, depth and motion crops (render rect WinOrigin,
//     WinSize) written straight into the window textures at (0,0), saving a copy pass.
// Smear on near objects comes from the OpenXR depth layer, not from this pass.
#define RS "RootFlags(0), RootConstants(num32BitConstants=32, b0), " \
           "DescriptorTable(SRV(t0, numDescriptors=4), UAV(u0, numDescriptors=5)), " \
           "DescriptorTable(UAV(u5, numDescriptors=3))"

cbuffer Constants : register(b0)
{
    int2  SrcOrigin;                  // view rect origin inside the shared (double-wide) scene textures
    uint2 Size;                       // render rect size
    row_major float4x4 ClipToPrevClip;// UE row-vector convention: prev = mul(clip, M)
    float2 JitterNdc;                 // current projection jitter (NDC), removed from the pixel position
    float2 MvSign;                    // debug flips (+1/-1)
    uint  VelocityValid;
    uint  Dilate;                     // 1 = take the motion of the closest-depth pixel in the 3x3 neighbourhood
    uint  CopyInputs;                 // bit 0: this eye's colour/depth copies; bit 1: count mask stats;
                                      // bit 2: centre window crops; bit 3: clean the copied colour
                                      // (AMD test build key N: NaN / inf / negative -> 0, max 4096)
    float MaskScale;                  // > 0 = write OutMask = saturate((1 - translucency alpha) * MaskScale)
    int2  WinOrigin;                  // bit 2: the window's render rect
    uint2 WinSize;
};

Texture2D<float>  DepthTex : register(t0);
Texture2D<float4> VelTex   : register(t1);
Texture2D<float4> ColorTex : register(t2);
Texture2D<float4> TransTex : register(t3);
RWTexture2D<float2> OutMV    : register(u0);
RWTexture2D<float>  OutDepth : register(u1);
RWTexture2D<float4> OutColor : register(u2);
RWTexture2D<float>  OutMask  : register(u3);
RWByteAddressBuffer StatsBuf : register(u4);   // 4 x uint, diagnostic counters
RWTexture2D<float4> OutWinColor : register(u5);   // bit 2: window crops, at (0,0)
RWTexture2D<float>  OutWinDepth : register(u6);
RWTexture2D<float2> OutWinMV    : register(u7);

// Motion of render-rect pixel q (current - previous, NDC, UE convention)
float2 MotionAt(int2 q, float z)
{
    const int2 p = SrcOrigin + q;
    float4 enc = float4(0, 0, 0, 0);
    if (VelocityValid) enc = VelTex.Load(int3(p, 0));
    // camera motion from depth (used where the engine wrote no velocity)
    const float2 uv = (float2(q) + 0.5) / float2(Size);
    const float2 ndcNoJitter = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - JitterNdc;
    const float4 prevClip = mul(float4(ndcNoJitter, z, 1.0), ClipToPrevClip);
    const float2 prevNdc = prevClip.xy / max(abs(prevClip.w), 1e-8) * sign(prevClip.w);
    float2 v = ndcNoJitter - prevNdc;
    if (enc.x > 0.0)
    {
        // UE 5.5 quadratic encoding, decode taken from the game's own TAA shader (DXIL)
        //   E = enc * 4.008016 - 2.003977 = (enc - 32767/65535) / (0.499 * 0.5)
        //   V = 0.5 * E * |E|
        const float2 e = enc.xy * 4.008016032 - 2.003977437;
        v = 0.5 * e * abs(e);
    }
    return v;
}

// NaN tested by its bits (fxc may fold isnan away without /Gis); +inf ends up at the 4096 cap
float CleanChannel(float x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u ? 0.0 : clamp(x, 0.0, 4096.0); }
float4 InputColour(int2 p)
{
    float4 c = ColorTex.Load(int3(p, 0));
    if (CopyInputs & 8) c = float4(CleanChannel(c.r), CleanChannel(c.g), CleanChannel(c.b), 1.0);
    return c;
}

groupshared uint gsCount[4];

// Diagnostic counts over this eye (at most once a second): [0] translucency alpha < 0.999, [1] mask > 0.5,
// [2] alpha outside [0,1], [3] translucency colour not black. One atomic add per 8x8 group.
void Stats(uint gi, bool inside, float4 t, float m)
{
    if (gi < 4) gsCount[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (inside)
    {
        if (t.a < 0.999) InterlockedAdd(gsCount[0], 1);
        if (m > 0.5) InterlockedAdd(gsCount[1], 1);
        if (t.a < -0.001 || t.a > 1.001) InterlockedAdd(gsCount[2], 1);
        if (max(t.r, max(t.g, t.b)) > 0.001) InterlockedAdd(gsCount[3], 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 4 && gsCount[gi]) StatsBuf.InterlockedAdd(gi * 4, gsCount[gi]);
}

void Work(uint3 id)
{
    int2 best = int2(id.xy);
    const float z0 = DepthTex.Load(int3(SrcOrigin + best, 0));
    float bestZ = z0;
    if (Dilate)
    {
        // closest surface in the 3x3 neighbourhood (reversed Z: larger = closer), like UE's TAA
        [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 q = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(Size) - 1);
            const float z = DepthTex.Load(int3(SrcOrigin + q, 0));
            if (z > bestZ) { bestZ = z; best = q; }
        }
    }
    const float2 v = MotionAt(best, bestZ);
    // DLSS and FSR want pixel offsets from the current pixel to its previous position (y down)
    const float2 mv = float2(-0.5 * v.x * Size.x, 0.5 * v.y * Size.y) * MvSign;
    OutMV[id.xy] = mv;
    if (CopyInputs & 1)
    {
        OutDepth[id.xy] = z0;
        OutColor[id.xy] = InputColour(SrcOrigin + int2(id.xy));
    }
    const int2 w = int2(id.xy) - WinOrigin;
    [branch] if ((CopyInputs & 4) && all(w >= 0) && all(w < int2(WinSize)))
    {
        OutWinColor[w] = InputColour(SrcOrigin + int2(id.xy));
        OutWinDepth[w] = z0;
        OutWinMV[w] = mv;
    }
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    const bool inside = id.x < Size.x && id.y < Size.y;
    if (inside) Work(id);
    float4 t = float4(0, 0, 0, 1);
    float m = 0.0;
    if (inside && MaskScale > 0.0)
    {
        t = TransTex.Load(int3(SrcOrigin + int2(id.xy), 0));
        m = saturate((1.0 - t.a) * MaskScale);
        OutMask[id.xy] = m;
    }
    if (CopyInputs & 2) Stats(gi, inside, t, m);   // uniform branch: the whole dispatch takes it or not
}
