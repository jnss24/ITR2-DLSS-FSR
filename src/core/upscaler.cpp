// upscaler.cpp - hand-built ITemporalUpscaler / IHistory, the RDG pass and the RHI command.
// Threads: proxies are created on the game thread; AddPasses runs on the render thread;
// the RHI command runs where the RHI command list is translated (RHI thread).
#include "common.h"
#include <cmath>
#include <algorithm>

using namespace ue;

const wchar_t* const kUpscalerName = L"ITR2_DLSS";   // same pointer returned by upscaler and history

// ================================================================ engine function types
using CreateTextureFn      = void* (*)(void* builder, const void* desc, const wchar_t* name, uint32_t flags);
using SetupParameterPassFn = void* (*)(void* builder, void* pass);
using RDGPassCtorFn        = void* (*)(void* self, void* eventName, void* paramStruct, uint32_t flags, uint32_t taskMode);
using MetaCtorFn           = void* (*)(void* self, uint32_t useCase, uint32_t bindingFlags, const wchar_t* layoutName,
                                       const wchar_t* structTypeName, const wchar_t* shaderVariableName,
                                       const wchar_t* staticSlotName, const char* fileName, int32_t fileLine,
                                       uint32_t size, const void* members, bool forceComplete, void* outLayoutInit,
                                       uint32_t usageFlags);
using FlushBarriersFn      = void (*)(void* context);
using ResizeGrowFn         = void (*)(void* array, int32_t oldNum);

// ================================================================ engine structs we fill in
struct FMember {                                  // FShaderParametersMetadata::FMember (0x30)
    const wchar_t* Name; const wchar_t* ShaderType; int32_t FileLine; uint32_t Offset;
    uint8_t BaseType; uint8_t Precision; uint8_t pad0[2];
    uint32_t NumRows, NumColumns, NumElements; void* Struct;
};
static_assert(sizeof(FMember) == 0x30, "FMember size");
struct FakeTArray { void* data; int32_t num; int32_t max; };
struct TexAccess { void* texture; uint64_t range; uint32_t access; uint32_t pad; };   // FRDGTextureAccess (0x18)
static_assert(sizeof(TexAccess) == 0x18, "FRDGTextureAccess size");
struct ParamStructTemp { void* contents; void* layout; void* metadata; };          // FRDGParameterStruct (0x18)

enum { P_Color, P_Depth, P_Velocity, P_Exposure, P_Output, P_Translucency, P_Count };
constexpr uint32_t kParamsSize = P_Count * sizeof(TexAccess);   // 0x90, multiple of 16
static_assert(kParamsSize % 16 == 0, "parameter struct size");

// ================================================================ pass blocks (render thread only)
struct Payload {
    int mode, eye;
    void* tex[P_Count];                 // FRDGTexture*
    IntRect colorRect, depthRect, velocityRect;
    int renderW, renderH, outW, outH;
    float jitterX, jitterY, preExposure;
    bool reset;
    float clipToPrevClip[16];
    float jitterNdcX, jitterNdcY;
    int preset; Quality quality;
    float cameraNear, fovY;
    bool fovea; int periphPreset; float foveaNdcX, foveaNdcY;
    bool drs; int maxRenderW, maxRenderH;
    float sharpen;
    float foveaSize; bool halfEdges, lowMem; int edgeAa; bool fullTaa;
    int dlss5;
};
struct alignas(16) PassBlock {
    alignas(16) uint8_t pass[off::RDGPass_Size];   // FRDGPass
    alignas(16) TexAccess params[P_Count];
    Payload payload;
};
static PassBlock g_blocks[64];
static uint32_t g_blockIndex = 0;

static alignas(16) uint8_t g_meta[off::Meta_Size];
static FMember g_members[P_Count];
static bool g_metaOk = false, g_metaTried = false;
static uint8_t g_eventName = 0;                     // FRDGEventName is empty in this Shipping build
static void* g_passVt[3];
static void* g_cmdVt[1];

static bool InitMetadata() {
    if (g_metaTried) return g_metaOk;
    g_metaTried = true;
    static const wchar_t* names[P_Count] = { L"ITR2_Color", L"ITR2_Depth", L"ITR2_Velocity", L"ITR2_Exposure", L"ITR2_Output", L"ITR2_Translucency" };
    for (int i = 0; i < P_Count; ++i) {
        FMember& m = g_members[i];
        memset(&m, 0, sizeof(m));
        m.Name = names[i]; m.ShaderType = L"FRDGTextureAccess"; m.FileLine = 0;
        m.Offset = i * (uint32_t)sizeof(TexAccess); m.BaseType = UBMT_RDG_TEXTURE_ACCESS;
        m.NumRows = 1; m.NumColumns = 1; m.NumElements = 0;
    }
    FakeTArray arr{ g_members, P_Count, P_Count };
    auto ctor = (MetaCtorFn)g_res.addr[I_ShaderParamsMetadataCtor];
    ctor(g_meta, 0 /*ShaderParameterStruct*/, 1 /*Shader*/, L"FITR2DLSSParameters", L"FITR2DLSSParameters",
         nullptr, nullptr, "itr2_dlss", 1, kParamsSize, &arr, true, nullptr, 0);
    g_metaOk = At<void*>(g_meta, off::Meta_Layout) != nullptr;
    Log("pass parameter metadata %s (layout=%p)", g_metaOk ? "created" : "FAILED", At<void*>(g_meta, off::Meta_Layout));
    return g_metaOk;
}

// ================================================================ RHI command (runs at translation time)
struct RhiCmd { void** vtbl; RhiCmd* next; NativeFrame f; };

static void CmdExecuteAndDestruct(RhiCmd* self, uint8_t* cmdList) {
    void* compCtx = At<void*>(cmdList, off::CL_ComputeContext);
    void* rhi = GetDynamicRHI();
    if (compCtx && rhi) {
        using GetCtxFn = void* (*)(void*, uint32_t);
        void* ctx = ((GetCtxFn)(*(void***)compCtx)[slot::ComputeContext_GetSingleDeviceContext_Off / 8])(compCtx, 0);
        if (ctx) ((FlushBarriersFn)g_res.addr[I_FlushResourceBarriers])(ctx);   // RDG transitions are batched
        auto gcl = VCall<ID3D12GraphicsCommandList*>(rhi, slot::D3D12RHI_GetGraphicsCommandList, (void*)cmdList, 0u);
        if (gcl) {
            const bool usedCompute = self->f.mode == 3 && DlssReady();
            if (usedCompute) DlssExecute(gcl, self->f);
            else CopyExecute(gcl, self->f);
            VCall<void>(rhi, slot::D3D12RHI_FinishExternalComputeWork, (void*)cmdList, 0u, gcl);
            if (usedCompute && ctx) {   // our PSO replaced UE's: make the state cache re-bind everything
                At<uint16_t>(ctx, off::D3D12Ctx_NeedSetPSOFlags) = 0x101;
                At<uint8_t>(ctx, off::D3D12Ctx_NeedSetComputeRS) = 1;
                At<uint8_t>(ctx, off::D3D12Ctx_LastPipelineType) = 2;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, self);
}

static ID3D12Resource* Native(void* rdgTex) {
    if (!rdgTex) return nullptr;
    void* rhi = At<void*>(rdgTex, off::RDGRes_ResourceRHI);
    return rhi ? VCall<ID3D12Resource*>(rhi, slot::RHITexture_GetNativeResource) : nullptr;
}

// ================================================================ our FRDGPass vtable
static void* PassDtor(void* self, uint32_t) { return self; }
static void PassNoop() {}

static void PassExecute(PassBlock* pb, uint8_t* cmdList) {   // FRDGPass::Execute(FRHIComputeCommandList&)
    const Payload& p = pb->payload;
    auto* cmd = (RhiCmd*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RhiCmd));
    if (!cmd) return;
    cmd->vtbl = g_cmdVt;
    NativeFrame& f = cmd->f;
    f.eye = p.eye; f.mode = p.mode;
    f.color = Native(p.tex[P_Color]); f.depth = Native(p.tex[P_Depth]); f.velocity = Native(p.tex[P_Velocity]);
    f.exposure = Native(p.tex[P_Exposure]); f.output = Native(p.tex[P_Output]);
    f.translucency = Native(p.tex[P_Translucency]);
    f.colorRect = p.colorRect; f.depthRect = p.depthRect; f.velocityRect = p.velocityRect;
    f.renderW = p.renderW; f.renderH = p.renderH; f.outW = p.outW; f.outH = p.outH;
    f.jitterX = p.jitterX; f.jitterY = p.jitterY; f.preExposure = p.preExposure; f.reset = p.reset;
    memcpy(f.clipToPrevClip, p.clipToPrevClip, sizeof(f.clipToPrevClip));
    f.jitterNdcX = p.jitterNdcX; f.jitterNdcY = p.jitterNdcY;
    f.preset = p.preset; f.quality = p.quality;
    f.cameraNear = p.cameraNear; f.fovY = p.fovY;
    f.fovea = p.fovea; f.periphPreset = p.periphPreset; f.foveaNdcX = p.foveaNdcX; f.foveaNdcY = p.foveaNdcY;
    f.drs = p.drs; f.maxRenderW = p.maxRenderW; f.maxRenderH = p.maxRenderH;
    f.sharpen = p.sharpen;
    f.foveaSize = p.foveaSize; f.halfEdges = p.halfEdges; f.lowMem = p.lowMem; f.edgeAa = p.edgeAa; f.fullTaa = p.fullTaa;
    f.dlss5 = p.dlss5;

    if (At<bool>(cmdList, off::CL_bExecuting)) {      // bottom of pipe: run now
        CmdExecuteAndDestruct(cmd, cmdList);
    } else {                                           // append to the command list (what ALLOC_COMMAND does)
        RhiCmd** link = At<RhiCmd**>(cmdList, off::CL_CommandLink);
        *link = cmd;
        At<RhiCmd**>(cmdList, off::CL_CommandLink) = &cmd->next;
        ++At<uint32_t>(cmdList, off::CL_NumCommands);
    }
}

// ================================================================ IHistory
struct History { void** vtbl; volatile LONG refs; int eye; int renderW, renderH, outW, outH; };
static void* g_histVt[6];
static void* HistDtor(History* h, uint32_t flags) { if (flags & 1) HeapFree(GetProcessHeap(), 0, h); return h; }
static uint32_t HistAddRef(History* h) { return (uint32_t)InterlockedIncrement(&h->refs); }
static uint32_t HistRelease(History* h) {
    LONG r = InterlockedDecrement(&h->refs);
    if (r == 0) HeapFree(GetProcessHeap(), 0, h);
    return (uint32_t)r;
}
static uint32_t HistGetRefCount(History* h) { return (uint32_t)h->refs; }
static const wchar_t* HistGetDebugName(void*) { return kUpscalerName; }
static uint64_t HistGetGPUSize(void*) { return 0; }

// ================================================================ math (double, UE row-vector convention)
struct M4 { double m[4][4]; };
static M4 Mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        double s = 0; for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j]; r.m[i][j] = s;
    }
    return r;
}
static bool Inverse(const M4& in, M4& out) {
    double a[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { a[i][j] = in.m[i][j]; a[i][j + 4] = (i == j); }
    for (int c = 0; c < 4; ++c) {
        int p = c; for (int r = c + 1; r < 4; ++r) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-30) return false;
        if (p != c) for (int j = 0; j < 8; ++j) { double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        const double d = a[c][c]; for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out.m[i][j] = a[i][j + 4];
    return true;
}
static const M4& Mat(const uint8_t* p, uint32_t o) { return *(const M4*)(p + o); }

// ================================================================ ITemporalUpscaler
struct UpscalerObj { void** vtbl; };
static void* g_upsVt[6];
static int g_addPassesLogLeft[2] = { 0, 0 };

static void* UpsDtor(UpscalerObj* self, uint32_t flags) { if (flags & 1) HeapFree(GetProcessHeap(), 0, self); return self; }
static const wchar_t* UpsGetDebugName(void*) { return kUpscalerName; }
static float UpsGetMin(void*) { return 0.25f; }
static float UpsGetMax(void*) { return 1.0f; }
static void* UpsFork(void*, void*) { return NewUpscalerProxy(); }

static void WriteOutputs(uint8_t* out, void* tex, int w, int h, History* hist) {
    At<void*>(out, off::Out_FullResTexture) = tex;
    At<IntRect>(out, off::Out_FullResRect) = IntRect{ 0, 0, w, h };
    if (hist) HistAddRef(hist);
    At<void*>(out, off::Out_NewHistory) = hist;
}

// A texture created earlier in this frame's render graph, by name (~70 textures per frame). The name is
// usually a string literal in the exe, so a pointer compare usually hits before any wcscmp.
static void* FindGraphTexture(void* builder, const wchar_t* wanted) {
    static const wchar_t* knownName = nullptr;   // single caller, render thread
    const uint8_t* reg = (const uint8_t*)builder + off::Builder_Textures;
    void** data = At<void**>(reg, 0);
    const int32_t num = At<int32_t>(reg, 8);
    if (!data || num <= 0 || num > 20000) return nullptr;
    for (int32_t i = num - 1; i >= 0; --i) {   // newest first: the one created this frame
        void* t = data[i];
        if (!t) continue;
        const wchar_t* nm = At<const wchar_t*>(t, 0x08);
        if (!nm) continue;
        if (nm == knownName || !wcscmp(nm, wanted)) { knownName = nm; return t; }
    }
    return nullptr;
}

// Diagnostic: logs every texture in this frame's render graph so far (name, size, format, count), to see
// whether translucency has a texture of its own (for a reactive / bias mask).
static void DumpGraphTextures(void* builder, const char* why) {
    static int dumpsLeft = 4;
    if (dumpsLeft <= 0) return;
    --dumpsLeft;
    const uint8_t* reg = (const uint8_t*)builder + off::Builder_Textures;
    void** data = At<void**>(reg, 0);
    const int32_t num = At<int32_t>(reg, 8);
    if (!data || num <= 0 || num > 20000) { Log("graph textures (%s): registry unreadable (num=%d)", why, num); return; }
    struct Entry { const wchar_t* name; int w, h, fmt, count; };
    static Entry e[400]; int n = 0;
    for (int32_t i = 0; i < num; ++i) {
        const uint8_t* t = (const uint8_t*)data[i];
        if (!t) continue;
        const wchar_t* nm = At<const wchar_t*>(t, 0x08);
        const int w = At<int32_t>(t, off::RDGTex_Desc + off::Desc_Extent), h = At<int32_t>(t, off::RDGTex_Desc + off::Desc_Extent + 4);
        const int fmt = At<uint8_t>(t, off::RDGTex_Desc + off::Desc_Format);
        int k = 0;
        for (; k < n; ++k) if (e[k].w == w && e[k].h == h && e[k].fmt == fmt && (e[k].name == nm || (nm && e[k].name && !wcscmp(e[k].name, nm)))) break;
        if (k < n) { ++e[k].count; continue; }
        if (n < (int)_countof(e)) e[n++] = { nm, w, h, fmt, 1 };
    }
    Log("graph textures (%s): %d textures, %d distinct", why, num, n);
    for (int k = 0; k < n; ++k) {
        char a[128] = "(null)";
        if (e[k].name) WideCharToMultiByte(CP_UTF8, 0, e[k].name, -1, a, sizeof(a) - 1, nullptr, nullptr);
        const bool hint = strstr(a, "ranslucen") || strstr(a, "eparate") || strstr(a, "UI") || strstr(a, "Widget");
        Log("  tex %-56s %5dx%-5d fmt=%-3d x%d%s", a, e[k].w, e[k].h, e[k].fmt, e[k].count, hint ? "   <--" : "");
    }
}

static uint8_t* UpsAddPasses(void* /*self*/, uint8_t* out, void* builder, const uint8_t* view, const uint8_t* in) {
    HitchNoteThread(1);   // render thread (its PSO creations hold a frame up: hitch.cpp)
    memset(out, 0, off::Out_Size);
    const IntRect outRect = At<IntRect>(in, off::In_OutputViewRect);
    void* colorTex = At<void*>(in, off::In_SceneColor);
    const IntRect colorRect = At<IntRect>(in, off::In_SceneColor + 8);
    void* depthTex = At<void*>(in, off::In_SceneDepth);
    const IntRect depthRect = At<IntRect>(in, off::In_SceneDepth + 8);
    void* velTex = At<void*>(in, off::In_SceneVelocity);
    const IntRect velRect = At<IntRect>(in, off::In_SceneVelocity + 8);
    void* expTex = At<void*>(in, off::In_EyeAdaptation);
    History* prev = At<History*>(in, off::In_PrevHistory);

    int eye = At<int32_t>(view, off::View_StereoViewIndex);
    if (eye < 0 || eye > 1) eye = 0;
    const int outW = outRect.w(), outH = outRect.h();
    const int renderW = colorRect.w(), renderH = colorRect.h();
    int mode = g_cfg.mode == 3 && DlssReady() ? 3 : 2;
    if (eye == 0) PerfOnRenderSize(renderW, renderH, outW, outH);

    // history: reuse ours if it belongs to this eye, else start a new one (= reset)
    bool reset = false;
    unsigned why = 0;   // reset causes, logged with [Test] OutputProbe
    History* hist = (prev && prev->vtbl == g_histVt && prev->eye == eye) ? prev : nullptr;
    if (!hist) {
        hist = (History*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(History));
        if (hist) { hist->vtbl = g_histVt; hist->eye = eye; }
        reset = true; why |= 1;
    }
    // a new render size resets the history, except under dynamic resolution (DLSS and FSR take a varying input
    // size within one feature)
    const bool sizeReset = g_liveDrs.load(std::memory_order_relaxed) ? (hist && (hist->outW != outW || hist->outH != outH))
                                                                     : (hist && (hist->renderW != renderW || hist->renderH != renderH || hist->outW != outW || hist->outH != outH));
    if (hist && (hist->renderW != renderW || hist->renderH != renderH || hist->outW != outW || hist->outH != outH)) {
        hist->renderW = renderW; hist->renderH = renderH; hist->outW = outW; hist->outH = outH;
    }
    if (sizeReset) { reset = true; why |= 2; }
    if (At<bool>(view, off::View_bCameraCut)) { reset = true; why |= 4; }
    if (At<uint32_t>(view, off::ViewInfo_StateBits) & 8) { reset = true; why |= 8; }
    const double* o0 = (const double*)(view + off::View_ViewMatrices + off::VM_ViewOrigin);
    const double* o1 = (const double*)(view + off::ViewInfo_PrevViewMatrices + off::VM_ViewOrigin);
    const double dx = o0[0] - o1[0], dy = o0[1] - o1[1], dz = o0[2] - o1[2];
    if (dx * dx + dy * dy + dz * dz > (double)g_cfg.teleportResetCm * g_cfg.teleportResetCm) { reset = true; why |= 16; }
    if (g_resetPending.load(std::memory_order_relaxed) & (1 << eye)) { g_resetPending.fetch_and(~(1 << eye)); reset = true; why |= 32; }
    if (why && g_cfg.outputProbe) {
        static int logs = 3000;
        if (logs-- > 0)
            Log("reset eye=%d:%s%s%s%s%s%s", eye, (why & 1) ? " new history" : "", (why & 2) ? " size changed" : "",
                (why & 4) ? " camera cut" : "", (why & 8) ? " view state reset" : "",
                (why & 16) ? " camera jump" : "", (why & 32) ? " reset key" : "");
    }

    if (!colorTex || outW <= 0 || outH <= 0 || renderW <= 0 || renderH <= 0 || !InitMetadata()) {
        WriteOutputs(out, colorTex, colorRect.w(), colorRect.h(), hist);   // pass the colour through
        return out;
    }

    // output texture (engine-owned RDG texture, one per eye per frame)
    uint8_t desc[off::Desc_Size];
    memcpy(desc, (const uint8_t*)colorTex + off::RDGTex_Desc, off::Desc_Size);
    At<uint64_t>(desc, off::Desc_Flags) = TexCreate_ShaderResource | TexCreate_UAV;
    At<int32_t>(desc, off::Desc_Extent) = outW; At<int32_t>(desc, off::Desc_Extent + 4) = outH;
    At<uint16_t>(desc, off::Desc_Depth) = 1; At<uint16_t>(desc, off::Desc_ArraySize) = 1;
    At<uint8_t>(desc, off::Desc_NumMips) = 1; At<uint8_t>(desc, off::Desc_NumSamples) = 1;
    At<uint8_t>(desc, off::Desc_Dimension) = 0;   // Texture2D
    // RGBA16F, or the game's own format with [Test] OutFormatKey, but only when that is R11G11B10 (PF_FloatRGB 9 or
    // PF_FloatR11G11B10 26): the only other format the mod's passes can write the output in
    const uint8_t colorFmt = At<uint8_t>(desc, off::Desc_Format);
    const bool gameFmt = g_liveOutGame.load(std::memory_order_relaxed) && DlssGameFormatOutputOk() && (colorFmt == 9 || colorFmt == 26);
    if (mode == 3 && !gameFmt) At<uint8_t>(desc, off::Desc_Format) = PF_FloatRGBA;
    At<uint8_t>(desc, off::Desc_UAVFormat) = 0;
    void* outTex = ((CreateTextureFn)g_res.addr[I_CreateTexture])(builder, desc, L"ITR2DLSS.Output", 0);
    if (!outTex) { WriteOutputs(out, colorTex, renderW, renderH, hist); return out; }

    // pass block
    PassBlock* pb = &g_blocks[g_blockIndex++ % _countof(g_blocks)];
    memset(pb, 0, sizeof(*pb));
    auto setAcc = [&](int i, void* t, uint32_t access) {
        if (!t) return;
        pb->params[i].texture = t; pb->params[i].range = At<uint64_t>(t, off::RDGTex_WholeRange); pb->params[i].access = access;
    };
    if (mode == 2) {
        setAcc(P_Color, colorTex, ERHIAccess_CopySrc);
        setAcc(P_Output, outTex, ERHIAccess_CopyDest);
    } else {
        setAcc(P_Color, colorTex, ERHIAccess_SRVCompute);
        setAcc(P_Depth, depthTex, ERHIAccess_SRVCompute);
        setAcc(P_Velocity, velTex, ERHIAccess_SRVCompute);
        setAcc(P_Exposure, expTex, ERHIAccess_SRVCompute);   // always: FSR's exposure mode can change live, and the probe reads it
        setAcc(P_Output, outTex, ERHIAccess_UAVCompute);
        // separate translucency (UI text, particles): its alpha becomes the upscaler's anti-ghosting mask
        if (g_cfg.translucencyMask || g_cfg.maskKey) {   // bound even while off, so F9 can turn it on
            void* tt = FindGraphTexture(builder, L"Translucency.All.Color");
            if (tt && At<int32_t>(tt, off::RDGTex_Desc + off::Desc_Extent) == At<int32_t>(colorTex, off::RDGTex_Desc + off::Desc_Extent) &&
                At<int32_t>(tt, off::RDGTex_Desc + off::Desc_Extent + 4) == At<int32_t>(colorTex, off::RDGTex_Desc + off::Desc_Extent + 4))
                setAcc(P_Translucency, tt, ERHIAccess_SRVCompute);
        }
    }
    Payload& p = pb->payload;
    p.mode = mode; p.eye = eye;
    for (int i = 0; i < P_Count; ++i) p.tex[i] = pb->params[i].texture;
    p.colorRect = colorRect; p.depthRect = depthRect; p.velocityRect = velRect;
    p.renderW = renderW; p.renderH = renderH; p.outW = outW; p.outH = outH;
    p.jitterX = At<float>(in, off::In_Jitter) * g_cfg.jitterSignX;
    p.jitterY = At<float>(in, off::In_Jitter + 4) * g_cfg.jitterSignY;
    p.preExposure = At<float>(in, off::In_PreExposure); if (!(p.preExposure > 0.f)) p.preExposure = 1.f;
    p.reset = reset;
    if (eye == 0) TweaksJitterSample(At<float>(in, off::In_Jitter), At<float>(in, off::In_Jitter + 4));
    p.preset = LivePreset(); p.quality = LiveQuality();
    // ClipToPrevClip = Inv(ProjNoAA_cur) * InvView_cur * View_prev * ProjNoAA_prev
    {
        const uint8_t* vm = view + off::View_ViewMatrices;
        const uint8_t* pvm = view + off::ViewInfo_PrevViewMatrices;
        M4 invProj; bool okInv = Inverse(Mat(vm, off::VM_ProjectionNoAA), invProj);
        M4 c = Mul(Mul(Mul(invProj, Mat(vm, off::VM_InvViewMatrix)), Mat(pvm, off::VM_ViewMatrix)), Mat(pvm, off::VM_ProjectionNoAA));
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) p.clipToPrevClip[i * 4 + j] = okInv ? (float)c.m[i][j] : (i == j ? 1.f : 0.f);
        const double* j = (const double*)(vm + off::VM_TemporalAAProjectionJitter);
        p.jitterNdcX = (float)j[0]; p.jitterNdcY = (float)j[1];
        if (eye == 0) {   // F11 burst diagnostics: camera turn between frames = where a distant point at the view centre
                          // was in the previous frame (render px; row vectors, reversed-Z: z ~ 0 is far)
            float camDx = 0.f, camDy = 0.f;
            if (okInv) {
                const double z = 1e-5, wx = c.m[2][0] * z + c.m[3][0], wy = c.m[2][1] * z + c.m[3][1], ww = c.m[2][3] * z + c.m[3][3];
                if (fabs(ww) > 1e-12) { camDx = (float)(wx / ww * renderW * 0.5); camDy = (float)(-wy / ww * renderH * 0.5); }
            }
            CaptureNoteFrame(At<float>(in, off::In_Jitter), At<float>(in, off::In_Jitter + 4), renderW, renderH, reset, camDx, camDy);
        }
        // camera data for FSR. UE's reversed-Z infinite projection (row vectors): M[1][1] = 1/tan(fovY/2)
        // (off-centre per eye in VR, close enough here), M[3][2] = near plane (cm).
        const M4& proj = Mat(vm, off::VM_ProjectionNoAA);
        p.fovY = proj.m[1][1] > 1e-6 ? (float)(2.0 * atan(1.0 / proj.m[1][1])) : 0.f;
        p.cameraNear = (float)proj.m[3][2];
        // the eye's forward axis (view (0,0,1)) projects to NDC (M[2][0], M[2][1]): the lens centre, offset
        // towards the nose on an off-centre VR projection
        p.foveaNdcX = (float)proj.m[2][0]; p.foveaNdcY = (float)proj.m[2][1];
        if (g_cfg.foveaFollowGame) {   // centre the window on the engine's foveation (fixed, from the exe):
            // x = 0.575 of the left eye's width (right eye 1 - 0.575), y = 0.5; texture y runs down, NDC y up
            const float ux = eye == 0 ? g_cfg.foveaGameCx : 1.f - g_cfg.foveaGameCx;
            p.foveaNdcX = ux * 2.f - 1.f; p.foveaNdcY = 1.f - 2.f * g_cfg.foveaGameCy;
        }
        p.fovea = g_liveFovea.load(std::memory_order_relaxed);
        p.periphPreset = g_livePeriph.load(std::memory_order_relaxed);
        p.sharpen = g_liveSharpen.load(std::memory_order_relaxed);
        p.foveaSize = g_liveFoveaSize.load(std::memory_order_relaxed);
        // low-memory mode (key -), VRAM over budget or benchmark (each checkpoint's VRAM = its own models):
        // keep only what this frame uses (no resolution change)
        p.lowMem = g_liveLowMem.load(std::memory_order_relaxed) || VramOverBudget() || BenchActive();
        p.halfEdges = g_liveHalfEdges.load(std::memory_order_relaxed);
        p.edgeAa = g_liveEdgeAa.load(std::memory_order_relaxed);
        p.fullTaa = g_liveFullTaa.load(std::memory_order_relaxed);
        p.dlss5 = g_liveDlss5.load(std::memory_order_relaxed);
        p.drs = g_liveDrs.load(std::memory_order_relaxed);
        p.maxRenderW = renderW; p.maxRenderH = renderH;
        if (p.drs) {
            // dynamic resolution: size the features for the largest render the controller may pick, rounded up
            // with a margin. The engine rounds the render size up; a maximum one pixel short rebuilds the
            // features (with a hitch) whenever the controller reaches the top of its range.
            const float dmax = DrsFeatureCeiling();   // feature size sets DLSS's cost: adaptive size, else the ceiling
            const int mw = (int)std::ceil(outW * dmax / 100.0) + 2, mh = (int)std::ceil(outH * dmax / 100.0) + 2;
            p.maxRenderW = std::min(outW, std::max(renderW, mw)); p.maxRenderH = std::min(outH, std::max(renderH, mh));
        }
    }

    // FRDGPass: construct with our parameter struct, then swap in our vtable
    void* layout = At<void*>(g_meta, off::Meta_Layout);
    InterlockedIncrement((volatile LONG*)((uint8_t*)layout + off::RHIResource_RefCount));   // the ctor releases its by-value arg
    ParamStructTemp ps{ pb->params, layout, g_meta };
    const uint32_t flags = (mode == 2 ? ERDGPassFlags_Copy : ERDGPassFlags_Compute) | ERDGPassFlags_NeverCull | ERDGPassFlags_NeverParallel;
    ((RDGPassCtorFn)g_res.addr[I_RDGPassCtor])(pb->pass, &g_eventName, &ps, flags, 0 /*Inline*/);
    *(void***)pb->pass = g_passVt;
    {   // register in Builder.Passes exactly like FRDGBuilder::AddPass does, then set up the pass
        uint8_t* passes = (uint8_t*)builder + off::Builder_Passes;   // TArray<FRDGPass*> {data, num, max}
        const int32_t oldNum = At<int32_t>(passes, 8);
        At<int32_t>(passes, 8) = oldNum + 1;
        if ((uint32_t)(oldNum + 1) > (uint32_t)At<int32_t>(passes, 12))
            ((ResizeGrowFn)g_res.addr[I_RDGPtrArrayResizeGrow])(passes, oldNum);
        At<void**>(passes, 0)[oldNum] = pb->pass;
        At<uint32_t>(pb->pass, off::RDGPass_Handle) = (uint32_t)oldNum;
    }
    ((SetupParameterPassFn)g_res.addr[I_SetupParameterPass])(builder, pb->pass);

    {   // log every change of the velocity input (e.g. BlackDummy -> a real velocity texture after a map loads)
        static const void* lastVelName = (const void*)1; static int changesLeft = 40;
        const void* vn = velTex ? At<const void*>(velTex, 0x08) : nullptr;
        if (eye == 0)   // menus and loading use the placeholder: dynamic resolution's size logic waits
            g_velPlaceholder.store(!vn || !wcscmp((const wchar_t*)vn, L"BlackDummy"), std::memory_order_relaxed);
        if (eye == 0 && vn != lastVelName && changesLeft > 0) {
            --changesLeft; lastVelName = vn;
            char b[96] = "(null)";
            if (vn) WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)vn, -1, b, 95, nullptr, nullptr);
            Log("velocity input is now '%s' %dx%d fmt=%u", b,
                velTex ? At<int32_t>(velTex, off::RDGTex_Desc + off::Desc_Extent) : 0,
                velTex ? At<int32_t>(velTex, off::RDGTex_Desc + off::Desc_Extent + 4) : 0,
                velTex ? At<uint8_t>(velTex, off::RDGTex_Desc + off::Desc_Format) : 0);
            DumpGraphTextures(builder, vn ? "velocity input changed" : "first frame");
        }
    }
    if (g_addPassesLogLeft[eye] == g_cfg.verboseFrames) {   // first frame per eye: what the engine actually passes us
        auto name = [](void* t) -> const char* {
            static char buf[4][96]; static int k = 0; char* b = buf[k++ & 3]; b[0] = 0;
            const wchar_t* w = t ? At<const wchar_t*>(t, 0x08) : nullptr;   // FRDGResource::Name
            if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, b, 95, nullptr, nullptr); else strcpy_s(b, 96, "(null)");
            return b;
        };
        auto ext = [](void* t, int i) { return t ? At<int32_t>(t, off::RDGTex_Desc + off::Desc_Extent + i * 4) : 0; };
        Log("inputs eye=%d: color='%s' %dx%d fmt=%u | depth='%s' %dx%d | velocity='%s' %dx%d fmt=%u | exposure='%s'",
            eye, name(colorTex), ext(colorTex, 0), ext(colorTex, 1), At<uint8_t>(colorTex, off::RDGTex_Desc + off::Desc_Format),
            name(depthTex), ext(depthTex, 0), ext(depthTex, 1),
            name(velTex), ext(velTex, 0), ext(velTex, 1), velTex ? At<uint8_t>(velTex, off::RDGTex_Desc + off::Desc_Format) : 0,
            name(expTex));
    }
    if (g_addPassesLogLeft[eye] > 0) {
        --g_addPassesLogLeft[eye];
        Log("AddPasses eye=%d mode=%d color=%p fmt=%u rect=(%d,%d)-(%d,%d) depth fmt=%u rect=(%d,%d) vel=%p fmt=%u rect=(%d,%d) "
            "out=%dx%d jitter=(%.3f,%.3f) preExp=%.4f exp=%p reset=%d prevHist=%p isr=%d",
            eye, mode, colorTex, At<uint8_t>(colorTex, off::RDGTex_Desc + off::Desc_Format), colorRect.x0, colorRect.y0,
            colorRect.x1, colorRect.y1, depthTex ? At<uint8_t>(depthTex, off::RDGTex_Desc + off::Desc_Format) : 0,
            depthRect.x0, depthRect.y0, velTex, velTex ? At<uint8_t>(velTex, off::RDGTex_Desc + off::Desc_Format) : 0,
            velRect.x0, velRect.y0, outW, outH, p.jitterX, p.jitterY, p.preExposure, expTex, (int)reset, prev,
            At<uint8_t>(view, off::View_bIsInstancedStereo));
    }
    WriteOutputs(out, outTex, outW, outH, hist);
    return out;
}

void* NewUpscalerProxy() {
    static bool init = false;
    if (!init) {   // game thread, first use
        init = true;
        g_upsVt[0] = (void*)&UpsDtor; g_upsVt[1] = (void*)&UpsGetDebugName; g_upsVt[2] = (void*)&UpsAddPasses;
        g_upsVt[3] = (void*)&UpsGetMin; g_upsVt[4] = (void*)&UpsGetMax; g_upsVt[5] = (void*)&UpsFork;
        g_histVt[0] = (void*)&HistDtor; g_histVt[1] = (void*)&HistAddRef; g_histVt[2] = (void*)&HistRelease;
        g_histVt[3] = (void*)&HistGetRefCount; g_histVt[4] = (void*)&HistGetDebugName; g_histVt[5] = (void*)&HistGetGPUSize;
        g_passVt[0] = (void*)&PassDtor; g_passVt[1] = (void*)&PassExecute; g_passVt[2] = (void*)&PassNoop;
        g_cmdVt[0] = (void*)&CmdExecuteAndDestruct;
        g_addPassesLogLeft[0] = g_addPassesLogLeft[1] = g_cfg.verboseFrames;
    }
    auto* u = (UpscalerObj*)HeapAlloc(GetProcessHeap(), 0, sizeof(UpscalerObj));
    if (u) u->vtbl = g_upsVt;
    return u;
}

std::atomic<bool> g_velPlaceholder{ true };
std::atomic<int> g_resetPending{ 0 };
