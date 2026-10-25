// Foveated upscaling: lays the centre upscale (expensive model, run only on a window around the eye's optical
// centre) over the full-eye upscale (cheap model), with an elliptical soft edge.
#define RS "RootFlags(0), RootConstants(num32BitConstants=8, b0), " \
           "DescriptorTable(SRV(t0, numDescriptors=1), UAV(u0, numDescriptors=1))"

cbuffer Constants : register(b0)
{
    int2  Origin;    // window origin in the output texture (pixels)
    uint2 Size;      // window size (pixels) = size of Center
    float Feather;   // width of the soft edge, as a fraction of the ellipse radius (0..1)
    uint3 Pad;
};

Texture2D<float4>   Center : register(t0);
RWTexture2D<float4> Out    : register(u0);

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Size.x || id.y >= Size.y) return;
    // position in the ellipse inscribed in the window: r = 0 at the centre, 1 on the ellipse
    const float2 d = ((float2(id.xy) + 0.5) / float2(Size)) * 2.0 - 1.0;
    const float r = length(d);
    float w = saturate((1.0 - r) / max(Feather, 1e-3));
    w = w * w * (3.0 - 2.0 * w);   // smoothstep: no visible ring at the edge
    if (w <= 0.0) return;
    const int2 p = Origin + int2(id.xy);
    Out[p] = lerp(Out[p], Center.Load(int3(id.xy, 0)), w);
}
