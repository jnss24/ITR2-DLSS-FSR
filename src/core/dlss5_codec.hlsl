// Reversible HDR <-> tonemapped SDR bridge for DLSS 5 (Neural Rendering). The upscaler pass runs before UE's
// tonemapper (linear HDR), DLSS 5 wants a tonemapped [0..1] image.
//   Mode 0 (encode): linear HDR (t0) and optional exposure (t1) -> exposure * Reinhard x / (1 + x), then
//     x^(1/2.2) -> u0 (nrIn).
//   Mode 1 (decode): original HDR (t0), nrIn (t1), DLSS 5 output nrOut (t2). Undoes both curves on nrIn and nrOut
//     and applies their clamped ratio to the original HDR colour (keeps HDR range and exposure) -> u0 (nrFinal).
//     DLSS 5's change is applied in full; its strength is DLSSNR.Intensity.
#define RS "RootFlags(0), RootConstants(num32BitConstants=8, b0), " \
           "DescriptorTable(SRV(t0, numDescriptors=3), UAV(u0, numDescriptors=1))"

cbuffer Constants : register(b0)
{
    uint2 WidthHeight;
    uint  Mode;          // 0 = encode (HDR -> SDR), 1 = decode (apply SDR ratio -> HDR)
    uint  HasExposure;   // 1 = multiply by ExposureTex[0,0].x
    float PreExposure;   // fallback / scale
    uint  Pad0;
    uint  Pad1;
    uint  Pad2;
};

Texture2D<float4>   Src0 : register(t0);   // Mode 0: HDR input; Mode 1: original HDR input
Texture2D<float4>   Src1 : register(t1);   // Mode 0: ExposureTex (1x1); Mode 1: encoded nrIn
Texture2D<float4>   Src2 : register(t2);   // Mode 0: unused; Mode 1: DLSS 5 nrOut
RWTexture2D<float4> Out  : register(u0);

float3 EncodeTonemap(float3 hdr)
{
    float3 c = max(hdr, 0.0);
    c = c / (1.0 + c);
    return pow(saturate(c), 1.0 / 2.2);
}

float3 DecodeTonemap(float3 sdr)
{
    float3 c = pow(saturate(sdr), 2.2);
    return c / max(1.0 - c, 1e-4);
}

bool BadFloat3(float3 v)
{
    uint3 u = asuint(v);
    return any((u & 0x7F800000u) == 0x7F800000u);
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= WidthHeight.x || id.y >= WidthHeight.y) return;

    if (Mode == 0)
    {
        float3 hdr = Src0.Load(int3(id.xy, 0)).rgb;
        if (BadFloat3(hdr)) hdr = 0.0;
        float expScale = PreExposure > 0.0 ? PreExposure : 1.0;
        if (HasExposure)
        {
            float e = Src1.Load(int3(0, 0, 0)).x;
            if ((asuint(e) & 0x7F800000u) != 0x7F800000u && e > 1e-4 && e < 1e4) expScale *= e;
        }
        Out[id.xy] = float4(EncodeTonemap(hdr * expScale), 1.0);
    }
    else
    {
        float4 orig = Src0.Load(int3(id.xy, 0));
        float3 hdr  = BadFloat3(orig.rgb) ? 0.0 : max(orig.rgb, 0.0);
        float3 sdrIn  = Src1.Load(int3(id.xy, 0)).rgb;
        float3 sdrOut = Src2.Load(int3(id.xy, 0)).rgb;
        if (BadFloat3(sdrOut) || BadFloat3(sdrIn))
        {
            Out[id.xy] = float4(hdr, orig.a);
            return;
        }
        float3 linIn  = DecodeTonemap(sdrIn);
        float3 linOut = DecodeTonemap(sdrOut);
        float3 ratio  = clamp((linOut + 1e-3) / (linIn + 1e-3), 0.25, 4.0);
        Out[id.xy] = float4(max(hdr * ratio, 0.0), orig.a);
    }
}
