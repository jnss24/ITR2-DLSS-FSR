// Contrast-adaptive sharpening after the upscaler (key 1), same idea as AMD's CAS: more where local contrast is
// low, less where it is already high, so edges don't ring. Colour is linear HDR here, so it goes through a
// reversible curve (c / (1 + max)) before sharpening and back after, keeping highlights from overshooting.
#define RS "RootFlags(0), RootConstants(num32BitConstants=4, b0), " \
           "DescriptorTable(SRV(t0, numDescriptors=1), UAV(u0, numDescriptors=1))"

cbuffer Constants : register(b0)
{
    uint2 Size;        // output size (pixels)
    float Sharpness;   // 0..1
    float Pad;
};

Texture2D<float4>   Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

float3 Compress(float3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
float3 Expand(float3 c)   { return c / max(1e-4, 1.0 - max(c.r, max(c.g, c.b))); }

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Size.x || id.y >= Size.y) return;
    const int2 p = int2(id.xy), hi = int2(Size) - 1;
    const float4 c4 = Src.Load(int3(p, 0));
    const float3 c = Compress(max(c4.rgb, 0.0));
    const float3 n = Compress(max(Src.Load(int3(clamp(p + int2(0, -1), 0, hi), 0)).rgb, 0.0));
    const float3 s = Compress(max(Src.Load(int3(clamp(p + int2(0,  1), 0, hi), 0)).rgb, 0.0));
    const float3 w = Compress(max(Src.Load(int3(clamp(p + int2(-1, 0), 0, hi), 0)).rgb, 0.0));
    const float3 e = Compress(max(Src.Load(int3(clamp(p + int2( 1, 0), 0, hi), 0)).rgb, 0.0));
    const float3 mn = min(c, min(min(n, s), min(w, e)));
    const float3 mx = max(c, max(max(n, s), max(w, e)));
    // amount from local contrast: high where there is headroom to sharpen without clipping
    const float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
    const float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, saturate(Sharpness)));
    // stay inside the neighbourhood's range: no halos, and Expand never sees values near 1 that were not there
    const float3 r = clamp((c + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt), mn, mx);
    Dst[p] = float4(Expand(r), c4.a);
}
