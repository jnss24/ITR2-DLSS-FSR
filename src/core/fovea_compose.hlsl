// Foveated compose for edges that are smaller than the output:
//   half-resolution edges (key 9): the edge model's result is smaller than the output;
//   plain edges (F9 "PLAIN"): no upscaler, the edges are the eye's render itself (its colour copy).
// One pass over the whole eye: stretches the edges to the output (bilinear) and lays the centre window over them
// with the same elliptical soft edge as fovea_blend.hlsl.
// Edge sample at uv * UvScale + UvOffset, clamped to [UvMin, UvMax] (first / last texel centre of the image).
// Plain edges: UvScale picks the render's part of a larger copy (dynamic resolution), UvOffset undoes this frame's
// camera jitter (else the edges wobble). Half-size edges: 1, 0, [0, 1].
// Also used by the upscaler-failure fallback (dlss.cpp FallbackStretch) on one eye's part of the double-wide scene
// colour, no window; the clamp keeps the other eye's pixels out of the bilinear sample.
// CentreInOut: the DLSS centre writes straight into the output's window rect (no separate centre texture); this
// pass then only writes the edges and the soft band (read-modify-write of its own pixel).
#define RS "RootFlags(0), RootConstants(num32BitConstants=16, b0), " \
           "DescriptorTable(SRV(t0, numDescriptors=2), UAV(u0, numDescriptors=1)), " \
           "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP)"

cbuffer Constants : register(b0)
{
    uint2  OutSize;   // output size (pixels)
    int2   Origin;    // window origin in the output
    uint2  WinSize;   // window size = size of Center
    float  Feather;   // soft edge, fraction of the ellipse radius
    float  CentreInOut;   // 1 = the DLSS centre already wrote the window into Out (output sub-rect): pixels it covers
                          // fully are left alone, the soft edge blends with what is there; 0 = the centre is Center
    float2 UvScale;   // edge texture: part of it that holds the image
    float2 UvOffset;  // edge texture: jitter compensation (plain edges)
    float2 UvMax;     // edge texture: last valid sample position
    float2 UvMin;     // edge texture: first valid sample position
};

Texture2D<float4>   Periph : register(t0);   // edge image: half-size model result, or the render's colour copy
Texture2D<float4>   Center : register(t1);   // centre model result, WinSize
SamplerState        Lin    : register(s0);
RWTexture2D<float4> Out    : register(u0);

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= OutSize.x || id.y >= OutSize.y) return;
    // window weight first: where the centre fully covers the pixel the edge image isn't read (edge AA skips those
    // pixels, so they may hold anything, even NaN in a new texture)
    float w = 0.0;
    const int2 q = int2(id.xy) - Origin;
    if (all(q >= 0) && all(q < int2(WinSize)))
    {
        const float2 d = ((float2(q) + 0.5) / float2(WinSize)) * 2.0 - 1.0;
        w = saturate((1.0 - length(d)) / max(Feather, 1e-3));
        w = w * w * (3.0 - 2.0 * w);
    }
    const bool inOut = CentreInOut > 0.5;
    if (inOut && w >= 1.0) return;   // centre model's own pixel, skip the write
    float4 c;
    [branch] if (w >= 1.0) c = Center.Load(int3(q, 0));
    else
    {
        const float2 uv = clamp((float2(id.xy) + 0.5) / float2(OutSize) * UvScale + UvOffset, UvMin, UvMax);
        c = Periph.SampleLevel(Lin, uv, 0);
        // each thread reads and writes only its own pixel, so reading Out is safe
        if (w > 0.0) c = lerp(c, inOut ? Out[id.xy] : Center.Load(int3(q, 0)), w);
    }
    Out[id.xy] = c;
}
