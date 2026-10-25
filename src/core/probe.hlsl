// Image check ([Test] OutputProbe, on in the AMD test build). Adds one eye's bad pixels (NaN, inf, negative,
// above 4096, black, alpha not 1) to that eye's running totals, keeps this frame's brightest colour and alpha range,
// and copies the game's exposure (eye adaptation texel 0).
// Runs on the eye's finished output (UAV; with Mode bit 0 the pixel is repaired: NaN -> 0, colour clamped to
// 0..MaxColour, alpha 1) or on the game's colour going into the upscaler (Mode bit 1: SRV at Offset, read only).
#define RS "RootFlags(0), RootConstants(num32BitConstants=8, b0), " \
           "DescriptorTable(SRV(t0, numDescriptors=2), UAV(u0, numDescriptors=2))"

cbuffer Constants : register(b0)
{
    uint2 Size;     // pixels to check
    uint  Mode;     // bit 0: write the repaired pixel back (UAV only); bit 1: read Source instead of Output
    uint  Base;     // byte offset of this block in Stats
    uint2 Offset;   // where the eye starts in Source (the game's colour holds both eyes)
    uint2 Pad;
};

Texture2D<float4>   Exposure : register(t0);   // the game's eye adaptation (null descriptor: reads 0)
Texture2D<float4>   Source   : register(t1);   // the game's colour (Mode bit 1)
RWTexture2D<float4> Output   : register(u0);   // the eye's output (Mode bit 1 clear)
RWByteAddressBuffer Stats    : register(u1);   // per block: 6 counters, exposure x / y, frames (running totals),
                                               // then this frame's maxima: brightest colour, alpha above 1, alpha below 1

static const float MaxColour = 32768.0;

groupshared uint g_count[6];
groupshared uint g_max[3];   // non-negative floats as their bits: the integer order is the float order

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex, uint3 gid : SV_GroupID)
{
    if (gi < 6) g_count[gi] = 0;
    if (gi < 3) g_max[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < Size.x && id.y < Size.y) {
        const bool fromSource = (Mode & 2u) != 0u;
        float4 c = fromSource ? Source.Load(int3(id.xy + Offset, 0)) : Output[id.xy];
        const bool nanPx = any(isnan(c));
        const bool infPx = !nanPx && any(isinf(c));
        const bool negPx = !nanPx && any(c.rgb < 0.0);
        const bool hugePx = !nanPx && !infPx && max(c.r, max(c.g, c.b)) > 4096.0;
        const bool darkPx = !nanPx && max(c.r, max(c.g, c.b)) < 1e-4;
        const bool alphaPx = !nanPx && abs(c.a - 1.0) > 0.01;
        if (nanPx) InterlockedAdd(g_count[0], 1u);
        if (infPx) InterlockedAdd(g_count[1], 1u);
        if (negPx) InterlockedAdd(g_count[2], 1u);
        if (hugePx) InterlockedAdd(g_count[3], 1u);
        if (darkPx) InterlockedAdd(g_count[4], 1u);
        if (alphaPx) InterlockedAdd(g_count[5], 1u);
        if (!nanPx && !infPx) {
            InterlockedMax(g_max[0], asuint(max(0.0, max(c.r, max(c.g, c.b)))));
            InterlockedMax(g_max[1], asuint(max(0.0, c.a - 1.0)));
            InterlockedMax(g_max[2], asuint(max(0.0, 1.0 - c.a)));
        }
        if (!fromSource && (Mode & 1u) != 0u && (nanPx || infPx || negPx || alphaPx || max(c.r, max(c.g, c.b)) > MaxColour)) {
            float3 rgb = c.rgb;
            rgb = float3(isnan(rgb.r) ? 0.0 : rgb.r, isnan(rgb.g) ? 0.0 : rgb.g, isnan(rgb.b) ? 0.0 : rgb.b);
            Output[id.xy] = float4(clamp(rgb, 0.0, MaxColour), 1.0);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 6 && g_count[gi] != 0u) { uint unused; Stats.InterlockedAdd(Base + gi * 4u, g_count[gi], unused); }
    if (gi < 3 && g_max[gi] != 0u) { uint unused; Stats.InterlockedMax(Base + 36u + gi * 4u, g_max[gi], unused); }
    if (gi == 0 && gid.x == 0 && gid.y == 0) {
        const float4 e = Exposure.Load(int3(0, 0, 0));
        Stats.Store2(Base + 24u, asuint(e.xy));
        uint unused; Stats.InterlockedAdd(Base + 32u, 1u, unused);
    }
}
