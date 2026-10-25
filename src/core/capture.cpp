// capture.cpp - full-resolution screenshot of the finished image the game hands to the headset.
// The screenshot key (shot.cpp) asks for a capture; at the end of the next frame's render graph
// (ISceneViewExtension slot 20, PostRenderViewFamily_RenderThread(FRDGBuilder&, FSceneViewFamily&), i.e. after
// tonemapping and every other post-process pass) we add a copy pass that reads "ViewFamilyTexture" (both eyes side
// by side, e.g. 8160x4464 for 4080x4464 per eye). At translation time it copies the texture into a readback
// buffer; a worker thread waits for the GPU, crops the eye(s) and writes a 24-bit PNG with WIC.
// Threads: request on the game thread, pass setup on the render thread, copy on the RHI thread, PNG on a worker.
#include "common.h"
#include <d3d12.h>
#include <wincodec.h>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

using namespace ue;

using SetupParameterPassFn = void* (*)(void* builder, void* pass);
using RDGPassCtorFn        = void* (*)(void* self, void* eventName, void* paramStruct, uint32_t flags, uint32_t taskMode);
using MetaCtorFn           = void* (*)(void* self, uint32_t useCase, uint32_t bindingFlags, const wchar_t* layoutName,
                                       const wchar_t* structTypeName, const wchar_t* shaderVariableName,
                                       const wchar_t* staticSlotName, const char* fileName, int32_t fileLine,
                                       uint32_t size, const void* members, bool forceComplete, void* outLayoutInit,
                                       uint32_t usageFlags);
using FlushBarriersFn      = void (*)(void* context);
using ResizeGrowFn         = void (*)(void* array, int32_t oldNum);

namespace {
struct FMember {                                  // FShaderParametersMetadata::FMember (0x30)
    const wchar_t* Name; const wchar_t* ShaderType; int32_t FileLine; uint32_t Offset;
    uint8_t BaseType; uint8_t Precision; uint8_t pad0[2];
    uint32_t NumRows, NumColumns, NumElements; void* Struct;
};
static_assert(sizeof(FMember) == 0x30, "FMember size");
struct FakeTArray { void* data; int32_t num; int32_t max; };
struct TexAccess { void* texture; uint64_t range; uint32_t access; uint32_t pad; };   // FRDGTextureAccess (0x18)
struct ParamStructTemp { void* contents; void* layout; void* metadata; };
constexpr uint32_t kCapParamsSize = 0x20;         // one FRDGTextureAccess, padded to 16

struct alignas(16) CapBlock {
    alignas(16) uint8_t pass[off::RDGPass_Size];
    alignas(16) uint8_t params[kCapParamsSize];   // TexAccess at offset 0
};

struct CapJob {                                   // one capture in flight at a time
    wchar_t file[MAX_PATH];
    int eyeMode;                                  // 0 left, 1 right, 2 both
    bool srgb;                                    // encode linear values with the sRGB curve
    ID3D12Resource* readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 totalBytes;                            // buffer size: the last row is not padded to RowPitch
    DXGI_FORMAT format;
    int texW, texH;
    ULONGLONG t0;
};

struct CapCmd { void** vtbl; CapCmd* next; ID3D12Resource* src; };
}

static CapBlock g_capBlock;
static alignas(16) uint8_t g_capMeta[off::Meta_Size];
static FMember g_capMember;
static bool g_capMetaOk = false, g_capMetaTried = false;
static uint8_t g_capEventName = 0;
static void* g_capPassVt[3];
static void* g_capCmdVt[1];

static std::atomic<bool> g_capBusy{ false };      // from the request until the PNG is written (or it failed)
static std::atomic<bool> g_capPending{ false };   // requested, not yet added to a render graph
static std::atomic<uint32_t> g_capCopies{ 0 };    // copies recorded so far (the comparison sequence waits on it)
static std::atomic<bool> g_capQuiet{ false };     // no "Screenshot saved" voice (the comparison speaks for itself)
static CapJob g_job;

static void CapFail(const char* why) {
    Log("screenshot: FAILED (%s)", why);
    Announce(Tone::Change, "Screenshot failed.");
    if (g_job.readback) { g_job.readback->Release(); g_job.readback = nullptr; }
    g_capBusy = false;
}

// ------------------------------------------------------------------ PNG (worker thread)
static bool IsBgra(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_TYPELESS;
}
static bool IsRgba(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}
static bool IsRgb10(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_TYPELESS;
}

// The headset's image holds LINEAR values in an 8-bit B8G8R8A8 texture (raw bytes: median 30/255, much darker than
// the headset / OculusMirror; the sRGB curve matched).
// Lookup tables: 8-bit and 10-bit input -> 8-bit sRGB-encoded output (identity when SrgbEncode=0).
static void BuildLuts(bool srgb, uint8_t lut8[256], uint8_t lut10[1024]) {
    for (int i = 0; i < 1024; ++i) {
        const double l = i / 1023.0;
        const double e = !srgb ? l : l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
        lut10[i] = (uint8_t)(e * 255.0 + 0.5);
    }
    for (int i = 0; i < 256; ++i) {
        const double l = i / 255.0;
        const double e = !srgb ? l : l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
        lut8[i] = (uint8_t)(e * 255.0 + 0.5);
    }
}

// one row of n texels (4 bytes each) -> n BGR pixels
static void RowToBgr(const uint8_t* px, DXGI_FORMAT f, const uint8_t* lut8, const uint8_t* lut10, uint8_t* o, int n) {
    if (IsBgra(f)) { for (int x = 0; x < n; ++x, px += 4, o += 3) { o[0] = lut8[px[0]]; o[1] = lut8[px[1]]; o[2] = lut8[px[2]]; } }
    else if (IsRgba(f)) { for (int x = 0; x < n; ++x, px += 4, o += 3) { o[0] = lut8[px[2]]; o[1] = lut8[px[1]]; o[2] = lut8[px[0]]; } }
    else {   // R10G10B10A2: R bits 0-9, G 10-19, B 20-29
        for (int x = 0; x < n; ++x, px += 4, o += 3) {
            const uint32_t v = *(const uint32_t*)px;
            o[2] = lut10[v & 0x3FF]; o[1] = lut10[(v >> 10) & 0x3FF]; o[0] = lut10[(v >> 20) & 0x3FF];
        }
    }
}

static bool WritePng(const wchar_t* file, const uint8_t* bgr, int w, int h);
bool WritePngBgr(const wchar_t* file, const uint8_t* bgr, int w, int h) { return WritePng(file, bgr, w, h); }

static bool WritePng(const wchar_t* file, const uint8_t* bgr, int w, int h) {
    IWICImagingFactory* fac = nullptr; IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr; IPropertyBag2* props = nullptr;
    bool ok = false;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    if (SUCCEEDED(hr)) hr = fac->CreateStream(&st);
    if (SUCCEEDED(hr)) hr = st->InitializeFromFilename(file, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) hr = frame->Initialize(props);
    if (SUCCEEDED(hr)) hr = frame->SetSize((UINT)w, (UINT)h);
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pf);
    if (SUCCEEDED(hr) && pf != GUID_WICPixelFormat24bppBGR) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = frame->WritePixels((UINT)h, (UINT)w * 3, (UINT)((size_t)w * 3 * h), (BYTE*)bgr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    ok = SUCCEEDED(hr);
    if (!ok) Log("screenshot: PNG writer failed (hr=0x%08lX)", (unsigned long)hr);
    if (props) props->Release();
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (fac) fac->Release();
    return ok;
}

static DWORD WINAPI CapWorker(void*) {
    Sleep(1500);   // the copy runs within a few frames; the readback is only mapped once the GPU is surely done
    CapJob& j = g_job;
    char fileA[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, j.file, -1, fileA, sizeof(fileA), nullptr, nullptr);
    const int eyeW = j.eyeMode == 2 ? j.texW : j.texW / 2;
    const int x0 = j.eyeMode == 1 ? j.texW / 2 : 0;
    void* mapped = nullptr;
    // read range = the buffer's real size (RowPitch * rows would overrun it by the last row's padding when the
    // row is not a multiple of 256 bytes, e.g. 8160 * 4: Map then fails with E_INVALIDARG)
    D3D12_RANGE rr{ 0, (SIZE_T)j.totalBytes };
    const HRESULT mhr = j.readback->Map(0, &rr, &mapped);
    if (FAILED(mhr) || !mapped) {
        char why[96]; snprintf(why, sizeof(why), "could not map the readback buffer, hr=0x%08lX", (unsigned long)mhr);
        CapFail(why); return 0;
    }
    std::vector<uint8_t> bgr;
    try { bgr.resize((size_t)eyeW * 3 * j.texH); } catch (...) { j.readback->Unmap(0, nullptr); CapFail("out of memory"); return 0; }
    uint8_t lut8[256], lut10[1024];
    BuildLuts(j.srgb, lut8, lut10);
    const uint8_t* base = (const uint8_t*)mapped + j.fp.Offset;
    for (int y = 0; y < j.texH; ++y)
        RowToBgr(base + (size_t)y * j.fp.Footprint.RowPitch + (size_t)x0 * 4, j.format, lut8, lut10,
                 bgr.data() + (size_t)y * eyeW * 3, eyeW);
    D3D12_RANGE none{ 0, 0 };
    j.readback->Unmap(0, &none);
    j.readback->Release(); j.readback = nullptr;
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ok = WritePng(j.file, bgr.data(), eyeW, j.texH);
    if (SUCCEEDED(ci)) CoUninitialize();
    if (ok) {
        Log("screenshot: saved %s (%dx%d, %s eye%s, format %d%s, %.1f s)", fileA, eyeW, j.texH,
            j.eyeMode == 0 ? "left" : j.eyeMode == 1 ? "right" : "both", j.eyeMode == 2 ? "s" : "", (int)j.format,
            j.srgb ? ", sRGB-encoded" : ", raw values",
            (GetTickCount64() - j.t0) / 1000.0);
        if (!g_capQuiet) Announce(Tone::Change, "Screenshot saved.");
        g_capBusy = false;
    } else CapFail("could not write the PNG");
    return 0;
}

// ------------------------------------------------------------------ RHI command (translation time)
static void CapCmdExecute(CapCmd* self, uint8_t* cmdList) {
    void* rhi = GetDynamicRHI();
    void* compCtx = At<void*>(cmdList, off::CL_ComputeContext);
    bool started = false;
    if (rhi && compCtx && self->src) {
        using GetCtxFn = void* (*)(void*, uint32_t);
        void* ctx = ((GetCtxFn)(*(void***)compCtx)[slot::ComputeContext_GetSingleDeviceContext_Off / 8])(compCtx, 0);
        if (ctx) ((FlushBarriersFn)g_res.addr[I_FlushResourceBarriers])(ctx);   // RDG's CopySrc transition
        auto gcl = VCall<ID3D12GraphicsCommandList*>(rhi, slot::D3D12RHI_GetGraphicsCommandList, (void*)cmdList, 0u);
        auto dev = VCall<ID3D12Device*>(rhi, slot::D3D12RHI_GetDevice, 0u);
        const D3D12_RESOURCE_DESC d = self->src->GetDesc();
        CapJob& j = g_job;
        j.format = d.Format; j.texW = (int)d.Width; j.texH = (int)d.Height;
        const bool fmtOk = IsBgra(d.Format) || IsRgba(d.Format) || IsRgb10(d.Format);
        if (gcl && dev && fmtOk && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count == 1) {
            UINT64 total = 0;
            dev->GetCopyableFootprints(&d, 0, 1, 0, &j.fp, nullptr, nullptr, &total);
            j.totalBytes = total;
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
            bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, IID_PPV_ARGS(&j.readback)))) {
                D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = j.readback;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = j.fp;
                D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = self->src;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
                gcl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                VCall<void>(rhi, slot::D3D12RHI_FinishExternalComputeWork, (void*)cmdList, 0u, gcl);
                ++g_capCopies;
                HANDLE h = CreateThread(nullptr, 0, CapWorker, nullptr, 0, nullptr);
                if (h) { CloseHandle(h); started = true; }
            } else Log("screenshot: readback buffer of %llu MB could not be created", total >> 20);
        } else Log("screenshot: ViewFamilyTexture %dx%d format %d samples %u not supported", j.texW, j.texH, (int)d.Format,
                   d.SampleDesc.Count);
    }
    if (!started) CapFail("copy not recorded");
    HeapFree(GetProcessHeap(), 0, self);
}

// ------------------------------------------------------------------ our FRDGPass
static void* CapPassDtor(void* self, uint32_t) { return self; }
static void CapPassNoop() {}
static void CapPassExecute(CapBlock* b, uint8_t* cmdList) {
    void* tex = ((TexAccess*)b->params)->texture;
    void* rhiTex = tex ? At<void*>(tex, off::RDGRes_ResourceRHI) : nullptr;
    auto* cmd = (CapCmd*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(CapCmd));
    if (!cmd) { CapFail("out of memory"); return; }
    cmd->vtbl = g_capCmdVt;
    cmd->src = rhiTex ? VCall<ID3D12Resource*>(rhiTex, slot::RHITexture_GetNativeResource) : nullptr;
    if (At<bool>(cmdList, off::CL_bExecuting)) CapCmdExecute(cmd, cmdList);
    else {
        CapCmd** link = At<CapCmd**>(cmdList, off::CL_CommandLink);
        *link = cmd;
        At<CapCmd**>(cmdList, off::CL_CommandLink) = &cmd->next;
        ++At<uint32_t>(cmdList, off::CL_NumCommands);
    }
}

static bool CapInitMetadata() {
    if (g_capMetaTried) return g_capMetaOk;
    g_capMetaTried = true;
    memset(&g_capMember, 0, sizeof(g_capMember));
    g_capMember.Name = L"ITR2_Capture"; g_capMember.ShaderType = L"FRDGTextureAccess";
    g_capMember.Offset = 0; g_capMember.BaseType = UBMT_RDG_TEXTURE_ACCESS; g_capMember.NumRows = 1; g_capMember.NumColumns = 1;
    FakeTArray arr{ &g_capMember, 1, 1 };
    ((MetaCtorFn)g_res.addr[I_ShaderParamsMetadataCtor])(g_capMeta, 0, 1, L"FITR2CaptureParameters", L"FITR2CaptureParameters",
        nullptr, nullptr, "itr2_dlss_capture", 1, kCapParamsSize, &arr, true, nullptr, 0);
    g_capMetaOk = At<void*>(g_capMeta, off::Meta_Layout) != nullptr;
    g_capPassVt[0] = (void*)&CapPassDtor; g_capPassVt[1] = (void*)&CapPassExecute; g_capPassVt[2] = (void*)&CapPassNoop;
    g_capCmdVt[0] = (void*)&CapCmdExecute;
    Log("screenshot: capture pass metadata %s", g_capMetaOk ? "created" : "FAILED");
    return g_capMetaOk;
}

static void* FindFamilyTexture(void* builder) {
    const uint8_t* reg = (const uint8_t*)builder + off::Builder_Textures;
    void** data = At<void**>(reg, 0);
    const int32_t num = At<int32_t>(reg, 8);
    if (!data || num <= 0 || num > 20000) return nullptr;
    for (int32_t i = num - 1; i >= 0; --i) {
        void* t = data[i];
        const wchar_t* nm = t ? At<const wchar_t*>(t, 0x08) : nullptr;
        if (!nm || wcscmp(nm, L"ViewFamilyTexture")) continue;
        // the headset's family is both eyes side by side (about twice as wide as tall); a scope's scene capture
        // or a flat preview is not, so skip those and wait for the headset's graph
        const int w = At<int32_t>(t, off::RDGTex_Desc + off::Desc_Extent), h = At<int32_t>(t, off::RDGTex_Desc + off::Desc_Extent + 4);
        return (h > 0 && w >= h * 3 / 2 && w >= 1024) ? t : nullptr;
    }
    return nullptr;
}

// an RDG copy pass reading tex (CopySrc), with our vtable (Execute records the native copy at translation time)
static void AddCopyPass(void* builder, void* tex, CapBlock* b, void** vt) {
    memset(b, 0, sizeof(*b));
    TexAccess* acc = (TexAccess*)b->params;
    acc->texture = tex; acc->range = At<uint64_t>(tex, off::RDGTex_WholeRange); acc->access = ERHIAccess_CopySrc;
    void* layout = At<void*>(g_capMeta, off::Meta_Layout);
    InterlockedIncrement((volatile LONG*)((uint8_t*)layout + off::RHIResource_RefCount));
    ParamStructTemp ps{ b->params, layout, g_capMeta };
    ((RDGPassCtorFn)g_res.addr[I_RDGPassCtor])(b->pass, &g_capEventName, &ps,
        ERDGPassFlags_Copy | ERDGPassFlags_NeverCull | ERDGPassFlags_NeverParallel, 0);
    *(void***)b->pass = vt;
    uint8_t* passes = (uint8_t*)builder + off::Builder_Passes;
    const int32_t oldNum = At<int32_t>(passes, 8);
    At<int32_t>(passes, 8) = oldNum + 1;
    if ((uint32_t)(oldNum + 1) > (uint32_t)At<int32_t>(passes, 12))
        ((ResizeGrowFn)g_res.addr[I_RDGPtrArrayResizeGrow])(passes, oldNum);
    At<void**>(passes, 0)[oldNum] = b->pass;
    At<uint32_t>(b->pass, off::RDGPass_Handle) = (uint32_t)oldNum;
    ((SetupParameterPassFn)g_res.addr[I_SetupParameterPass])(builder, b->pass);
}

// ------------------------------------------------------------------ burst (comparison: shimmer over consecutive frames)
// After a comparison shot, N consecutive frames are copied, each only a crop (BurstWidth x BurstHeight, centred on
// the game's foveation centre of the left eye) into one slice of a single readback buffer; a worker converts them to
// BGR. One burst at a time; the comparison takes the frames out (BurstTake) before asking for the next burst.
namespace {
struct BurstCmd { void** vtbl; BurstCmd* next; ID3D12Resource* src; int index; };
}
static CapBlock g_burstBlocks[16];               // one per frame in flight (a pass block must live until it runs)
static void* g_burstPassVt[3];
static void* g_burstCmdVt[1];
static std::atomic<int> g_burstState{ 0 };       // 0 idle, 1 capturing, 2 converting, 3 ready, 4 failed
static std::atomic<int> g_burstAdded{ 0 }, g_burstCopied{ 0 };
static struct {
    int n, w, h, x0, y0;                          // frames, crop size, crop origin in the texture
    bool srgb;
    ID3D12Resource* readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;        // footprint of one slice (Offset 0)
    UINT64 slice, total;
    DXGI_FORMAT format;
    LARGE_INTEGER t[64];                          // when each copy was recorded (frame rhythm)
    ID3D12Resource* src[64];                      // which texture each copy read (the swapchain rotates: changes every frame)
    float jx[64], jy[64];                         // the upscaler's jitter in the frame of each copy (render pixels, eye 0)
    int rw[64], rh[64];                           // its render size
    bool fresh[64], reset[64];                    // the upscaler ran in that frame (else the values are stale) / history reset
    float cx[64], cy[64];                         // the camera's turn since the previous frame (render px, view centre)
    uint8_t* bgr;                                 // n * w * h * 3 after the worker
    double fps;
    ULONGLONG t0;
} g_burst;

// the upscaler's last frame (render thread, eye 0); a burst frame records it when its copy pass is added (same frame)
static float g_noteJx = 0.f, g_noteJy = 0.f, g_noteCx = 0.f, g_noteCy = 0.f; static int g_noteW = 0, g_noteH = 0;
static bool g_noteReset = false;
static unsigned g_noteN = 0, g_noteSeen = 0;
void CaptureNoteFrame(float jx, float jy, int renderW, int renderH, bool reset, float camDx, float camDy) {
    g_noteJx = jx; g_noteJy = jy; g_noteW = renderW; g_noteH = renderH; g_noteReset = reset; g_noteCx = camDx; g_noteCy = camDy;
    ++g_noteN;
}

static void BurstFail(const char* why) {
    Log("burst: FAILED (%s)", why);
    if (g_burst.readback) { g_burst.readback->Release(); g_burst.readback = nullptr; }
    g_burstState = 4;
}

static DWORD WINAPI BurstWorker(void*) {
    Sleep(600);   // the last copy is recorded; give the GPU a few frames to finish it before mapping
    void* mapped = nullptr;
    D3D12_RANGE rr{ 0, (SIZE_T)g_burst.total };
    const HRESULT hr = g_burst.readback->Map(0, &rr, &mapped);
    if (FAILED(hr) || !mapped) { char w[64]; snprintf(w, sizeof(w), "map hr=0x%08lX", (unsigned long)hr); BurstFail(w); return 0; }
    const size_t frameBytes = (size_t)g_burst.w * g_burst.h * 3;
    g_burst.bgr = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, frameBytes * g_burst.n);
    if (!g_burst.bgr) { g_burst.readback->Unmap(0, nullptr); BurstFail("out of memory"); return 0; }
    uint8_t lut8[256], lut10[1024];
    BuildLuts(g_burst.srgb, lut8, lut10);
    for (int k = 0; k < g_burst.n; ++k) {
        const uint8_t* base = (const uint8_t*)mapped + g_burst.slice * k;
        for (int y = 0; y < g_burst.h; ++y)
            RowToBgr(base + (size_t)y * g_burst.fp.Footprint.RowPitch, g_burst.format, lut8, lut10,
                     g_burst.bgr + frameBytes * k + (size_t)y * g_burst.w * 3, g_burst.w);
    }
    D3D12_RANGE none{ 0, 0 };
    g_burst.readback->Unmap(0, &none);
    g_burst.readback->Release(); g_burst.readback = nullptr;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const double span = (double)(g_burst.t[g_burst.n - 1].QuadPart - g_burst.t[0].QuadPart) / f.QuadPart;
    g_burst.fps = span > 0 ? (g_burst.n - 1) / span : 72.0;
    Log("burst: %d frames %dx%d at (%d,%d), %.1f fps, ready %.1f s after the request", g_burst.n, g_burst.w, g_burst.h,
        g_burst.x0, g_burst.y0, g_burst.fps, (GetTickCount64() - g_burst.t0) / 1000.0);
    // diagnostics (a near-black first frame was seen in DLSS bursts): brightness of each frame, the time between
    // copies, and whether all copies read the same texture
    char bl[64 * 6] = "", dt[64 * 6] = ""; int lb = 0, ld = 0, srcChanges = 0;
    for (int k = 0; k < g_burst.n; ++k) {
        const uint8_t* p = g_burst.bgr + frameBytes * k; double sum = 0; size_t c = 0;
        for (size_t i = 0; i < frameBytes; i += 3 * 11, ++c) sum += 0.114 * p[i] + 0.587 * p[i + 1] + 0.299 * p[i + 2];
        lb += snprintf(bl + lb, sizeof(bl) - lb, " %.0f", c ? sum / c : 0.0);
        if (k) ld += snprintf(dt + ld, sizeof(dt) - ld, " %.0f", (g_burst.t[k].QuadPart - g_burst.t[k - 1].QuadPart) * 1000.0 / f.QuadPart);
        if (k && g_burst.src[k] != g_burst.src[k - 1]) ++srcChanges;
    }
    Log("burst: frame brightness:%s", bl);
    Log("burst: ms between copies:%s", dt);
    Log("burst: source texture %p, changed %d times during the burst", (void*)g_burst.src[0], srcChanges);
    // diagnostics (DLSS frames were seen to jump 0.3-0.75 px in about a third of the frames, TAA's did not): the
    // upscaler's jitter, render size and history reset in each frame, to compare with the frames' measured shifts
    std::string js;
    for (int k = 0; k < g_burst.n; ++k) {
        char e[64];
        if (!g_burst.fresh[k]) snprintf(e, sizeof(e), " -");
        else snprintf(e, sizeof(e), " %+.3f,%+.3f%s%s", g_burst.jx[k], g_burst.jy[k],
                      (k && g_burst.fresh[k - 1] && (g_burst.rw[k] != g_burst.rw[k - 1] || g_burst.rh[k] != g_burst.rh[k - 1])) ? "*" : "",
                      g_burst.reset[k] ? "R" : "");
        js += e;
    }
    Log("burst: jitter per frame (render px; - = upscaler off; * = render size changed; R = history reset), render %dx%d:%s",
        g_burst.rw[0], g_burst.rh[0], js.c_str());
    std::string cs;
    for (int k = 0; k < g_burst.n; ++k) {
        char e[32];
        if (!g_burst.fresh[k]) snprintf(e, sizeof(e), " -");
        else snprintf(e, sizeof(e), " %+.2f,%+.2f", g_burst.cx[k], g_burst.cy[k]);
        cs += e;
    }
    Log("burst: camera turn since the previous frame per frame (render px at the view centre; - = upscaler off):%s", cs.c_str());
    {   // summary: near 0 with the headset really still; a burst right after the key press measured rms 0.38
        double s2 = 0, mx = 0; int c = 0;
        for (int k = 1; k < g_burst.n; ++k) if (g_burst.fresh[k]) {
            const double m2 = (double)g_burst.cx[k] * g_burst.cx[k] + (double)g_burst.cy[k] * g_burst.cy[k];
            s2 += m2; mx = std::max(mx, sqrt(m2)); ++c;
        }
        if (c) Log("burst: camera turn rms %.2f px, max %.2f px per frame (%s)", sqrt(s2 / c), mx,
                   sqrt(s2 / c) > 0.15 ? "the headset moved: this burst's shimmer number includes that movement" : "steady");
    }
    g_burstState = 3;
    return 0;
}

// for test code: the worker's conversion on an already filled readback buffer
void BurstTestConvert(ID3D12Resource* readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT64 slice, UINT64 total,
                      DXGI_FORMAT fmt, int n, int w, int h, bool srgb) {
    memset(&g_burst, 0, sizeof(g_burst));
    g_burst.readback = readback; g_burst.fp = fp; g_burst.slice = slice; g_burst.total = total; g_burst.format = fmt;
    g_burst.n = n; g_burst.w = w; g_burst.h = h; g_burst.srgb = srgb; g_burst.t0 = GetTickCount64();
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    for (int k = 0; k < n; ++k) g_burst.t[k].QuadPart = k * f.QuadPart / 72;
    g_burstState = 2;
    BurstWorker(nullptr);
}

static void BurstCmdExecute(BurstCmd* self, uint8_t* cmdList) {
    void* rhi = GetDynamicRHI();
    void* compCtx = At<void*>(cmdList, off::CL_ComputeContext);
    if (g_burstState.load() == 1 && rhi && compCtx && self->src) {
        using GetCtxFn = void* (*)(void*, uint32_t);
        void* ctx = ((GetCtxFn)(*(void***)compCtx)[slot::ComputeContext_GetSingleDeviceContext_Off / 8])(compCtx, 0);
        if (ctx) ((FlushBarriersFn)g_res.addr[I_FlushResourceBarriers])(ctx);
        auto gcl = VCall<ID3D12GraphicsCommandList*>(rhi, slot::D3D12RHI_GetGraphicsCommandList, (void*)cmdList, 0u);
        auto dev = VCall<ID3D12Device*>(rhi, slot::D3D12RHI_GetDevice, 0u);
        const D3D12_RESOURCE_DESC d = self->src->GetDesc();
        if (self->index == 0 && gcl && dev) {   // first frame: crop position and the readback buffer for all frames
            const bool fmtOk = IsBgra(d.Format) || IsRgba(d.Format) || IsRgb10(d.Format);
            const int eyeW = (int)d.Width / 2, texH = (int)d.Height;
            g_burst.w = std::min(g_burst.w, eyeW) & ~1; g_burst.h = std::min(g_burst.h, texH) & ~1;
            const int cx = (int)(eyeW * g_cfg.foveaGameCx), cy = (int)(texH * g_cfg.foveaGameCy);
            g_burst.x0 = std::max(0, std::min(eyeW - g_burst.w, cx - g_burst.w / 2));
            g_burst.y0 = std::max(0, std::min(texH - g_burst.h, cy - g_burst.h / 2));
            g_burst.format = d.Format;
            D3D12_RESOURCE_DESC cd = d; cd.Width = g_burst.w; cd.Height = g_burst.h; cd.MipLevels = 1; cd.DepthOrArraySize = 1;
            UINT64 one = 0;
            dev->GetCopyableFootprints(&cd, 0, 1, 0, &g_burst.fp, nullptr, nullptr, &one);
            g_burst.slice = (one + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(UINT64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
            g_burst.total = g_burst.slice * (g_burst.n - 1) + one;
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = g_burst.total; bd.Height = 1;
            bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (!fmtOk || d.SampleDesc.Count != 1) BurstFail("texture format not supported");
            else if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                         nullptr, IID_PPV_ARGS(&g_burst.readback)))) BurstFail("readback buffer");
        }
        if (g_burstState.load() == 1 && g_burst.readback && gcl && self->index < g_burst.n) {
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = g_burst.readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = g_burst.fp;
            dst.PlacedFootprint.Offset = g_burst.slice * self->index;
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = self->src;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
            D3D12_BOX box{ (UINT)g_burst.x0, (UINT)g_burst.y0, 0, (UINT)(g_burst.x0 + g_burst.w), (UINT)(g_burst.y0 + g_burst.h), 1 };
            gcl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            VCall<void>(rhi, slot::D3D12RHI_FinishExternalComputeWork, (void*)cmdList, 0u, gcl);
            QueryPerformanceCounter(&g_burst.t[self->index]);
            g_burst.src[self->index] = self->src;
            if (++g_burstCopied == g_burst.n) {
                g_burstState = 2;
                HANDLE h = CreateThread(nullptr, 0, BurstWorker, nullptr, 0, nullptr);
                if (h) CloseHandle(h); else BurstFail("worker thread");
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, self);
}

static void BurstPassExecute(CapBlock* b, uint8_t* cmdList) {
    void* tex = ((TexAccess*)b->params)->texture;
    void* rhiTex = tex ? At<void*>(tex, off::RDGRes_ResourceRHI) : nullptr;
    auto* cmd = (BurstCmd*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(BurstCmd));
    if (!cmd) return;
    cmd->vtbl = g_burstCmdVt;
    cmd->src = rhiTex ? VCall<ID3D12Resource*>(rhiTex, slot::RHITexture_GetNativeResource) : nullptr;
    cmd->index = At<int>(b->params, 0x18);   // frame index, stored in the padding after the TexAccess
    if (At<bool>(cmdList, off::CL_bExecuting)) BurstCmdExecute(cmd, cmdList);
    else {
        BurstCmd** link = At<BurstCmd**>(cmdList, off::CL_CommandLink);
        *link = cmd;
        At<BurstCmd**>(cmdList, off::CL_CommandLink) = &cmd->next;
        ++At<uint32_t>(cmdList, off::CL_NumCommands);
    }
}

bool BurstRequest(int frames, int w, int h) {   // game thread
    int idle = 0;
    if (!g_burstState.compare_exchange_strong(idle, 1)) return false;
    if (g_burst.bgr) { HeapFree(GetProcessHeap(), 0, g_burst.bgr); }
    memset(&g_burst, 0, sizeof(g_burst));
    g_burst.n = std::max(2, std::min(frames, 64)); g_burst.w = std::max(64, w); g_burst.h = std::max(64, h);
    g_burst.srgb = g_cfg.shotSrgb; g_burst.t0 = GetTickCount64();
    g_burstAdded = 0; g_burstCopied = 0;
    return true;
}
int BurstCopied() { return g_burstCopied.load(); }
int BurstState() { return g_burstState.load(); }
bool BurstTake(BurstFrames& out) {   // moves the frames out (caller frees with BurstFree); state back to idle
    if (g_burstState.load() != 3) return false;
    out.data = g_burst.bgr; out.n = g_burst.n; out.w = g_burst.w; out.h = g_burst.h; out.fps = g_burst.fps;
    g_burst.bgr = nullptr;
    g_burstState = 0;
    return true;
}
void BurstReset() { if (g_burstState.load() == 4) g_burstState = 0; }
void BurstFree(BurstFrames& f) { if (f.data) HeapFree(GetProcessHeap(), 0, f.data); f = BurstFrames{}; }

// ------------------------------------------------------------------ finished-image probe ([Test] OutputProbe)
// Every 6th headset frame a 256x256 box at each eye's lens centre of the finished image (after every post-process pass:
// the VisionPP flash, the helmet...) is copied into a ring of readback slots and read kFinalLag copies later (24+ frames:
// the GPU is long done). Per eye: the share of black and of white pixels and the mean brightness, as stored (8 bits).
// The log gets a line at once when one eye is black or white and the other is not, and a summary every 2 s: next to the
// output probe (dlss.cpp) it says whether a broken eye was already broken in what the mod handed over, or only after.
namespace {
struct FinalCmd { void** vtbl; FinalCmd* next; ID3D12Resource* src; };
}
constexpr int kFinalBox = 256, kFinalSlots = 8, kFinalLag = 4;
constexpr UINT kFinalPitch = kFinalBox * 4;                        // 4 bytes per pixel, a multiple of 256
constexpr UINT64 kFinalEyeBytes = (UINT64)kFinalPitch * kFinalBox; // a multiple of 512 (placement alignment)
static CapBlock g_finalBlocks[16];
static void* g_finalPassVt[3];
static void* g_finalCmdVt[1];
static unsigned g_finalFrames = 0, g_finalAdded = 0;
static ID3D12Resource* g_finalRb = nullptr;
static const uint8_t* g_finalData = nullptr;
static DXGI_FORMAT g_finalFmt[kFinalSlots] = {};
static uint32_t g_finalSeq[kFinalSlots] = {};
static uint32_t g_finalNext = 0;

static void FinalAnalyse(UINT slot) {   // RHI thread
    static uint8_t lut8[256], lut10[1024]; static bool lutOk = false;
    if (!lutOk) { BuildLuts(false, lut8, lut10); lutOk = true; }
    struct Eye { double n, black, white, sum; };
    static Eye acc[2] = {}; static double start = 0.0; static int samples = 0;
    static int eventLines = 1000, summaryLines = 3000; static ULONGLONG lastEvent = 0;
    Eye now[2] = {};
    uint8_t row[kFinalBox * 3];
    for (int eye = 0; eye < 2; ++eye) {
        const uint8_t* base = g_finalData + (slot * 2 + eye) * kFinalEyeBytes;
        for (int y = 0; y < kFinalBox; ++y) {
            RowToBgr(base + (size_t)y * kFinalPitch, g_finalFmt[slot], lut8, lut10, row, kFinalBox);
            for (int x = 0; x < kFinalBox; ++x) {
                const int b = row[x * 3], g = row[x * 3 + 1], r = row[x * 3 + 2];
                const int mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
                now[eye].n += 1; now[eye].sum += mx;
                if (mx < 8) now[eye].black += 1;
                if (mn > 247) now[eye].white += 1;
            }
        }
    }
    auto pct = [](double a, double n) { return n > 0 ? 100.0 * a / n : 0.0; };
    const double bl[2] = { pct(now[0].black, now[0].n), pct(now[1].black, now[1].n) };
    const double wh[2] = { pct(now[0].white, now[0].n), pct(now[1].white, now[1].n) };
    const bool odd = (bl[0] > 90 && bl[1] < 50) || (bl[1] > 90 && bl[0] < 50) || (wh[0] > 50 && wh[1] < 10) || (wh[1] > 50 && wh[0] < 10);
    if (odd && eventLines > 0 && GetTickCount64() - lastEvent >= 500) {
        --eventLines; lastEvent = GetTickCount64();
        Log("final image EVENT (lens centre, after the game's effects): left eye black %.0f%% white %.0f%% mean %.0f | right eye black %.0f%% "
            "white %.0f%% mean %.0f", bl[0], wh[0], now[0].sum / std::max(1.0, now[0].n), bl[1], wh[1], now[1].sum / std::max(1.0, now[1].n));
    }
    const double t = GetTickCount64() / 1000.0;
    if (samples == 0) start = t;
    ++samples;
    for (int eye = 0; eye < 2; ++eye) { acc[eye].n += now[eye].n; acc[eye].black += now[eye].black; acc[eye].white += now[eye].white; acc[eye].sum += now[eye].sum; }
    if (t - start >= 2.0) {
        if (summaryLines-- > 0)
            Log("final image 2 s (%d samples, lens centre, after the game's effects): left eye black %.1f%% white %.1f%% mean %.0f | right eye "
                "black %.1f%% white %.1f%% mean %.0f", samples, pct(acc[0].black, acc[0].n), pct(acc[0].white, acc[0].n),
                acc[0].sum / std::max(1.0, acc[0].n), pct(acc[1].black, acc[1].n), pct(acc[1].white, acc[1].n), acc[1].sum / std::max(1.0, acc[1].n));
        samples = 0; acc[0] = acc[1] = Eye{};
    }
}

static void FinalCmdExecute(FinalCmd* self, uint8_t* cmdList) {   // RHI thread, translation time
    void* rhi = GetDynamicRHI();
    void* compCtx = At<void*>(cmdList, off::CL_ComputeContext);
    if (rhi && compCtx && self->src) {
        using GetCtxFn = void* (*)(void*, uint32_t);
        void* ctx = ((GetCtxFn)(*(void***)compCtx)[slot::ComputeContext_GetSingleDeviceContext_Off / 8])(compCtx, 0);
        if (ctx) ((FlushBarriersFn)g_res.addr[I_FlushResourceBarriers])(ctx);   // RDG's CopySrc transition
        auto gcl = VCall<ID3D12GraphicsCommandList*>(rhi, slot::D3D12RHI_GetGraphicsCommandList, (void*)cmdList, 0u);
        auto dev = VCall<ID3D12Device*>(rhi, slot::D3D12RHI_GetDevice, 0u);
        const D3D12_RESOURCE_DESC d = self->src->GetDesc();
        const bool fmtOk = IsBgra(d.Format) || IsRgba(d.Format) || IsRgb10(d.Format);
        const int eyeW = (int)d.Width / 2, h = (int)d.Height;
        static bool warned = false;
        if (!fmtOk || d.SampleDesc.Count != 1 || eyeW < kFinalBox || h < kFinalBox) {
            if (!warned) { warned = true; Log("final image probe: ViewFamilyTexture %llux%u format %d not supported, off", d.Width, d.Height, (int)d.Format); }
        } else if (gcl && dev) {
            if (!g_finalRb) {
                D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kFinalSlots * 2 * kFinalEyeBytes; bd.Height = 1;
                bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                void* m = nullptr;
                if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_finalRb))) ||
                    FAILED(g_finalRb->Map(0, nullptr, &m))) {
                    if (g_finalRb) { g_finalRb->Release(); g_finalRb = nullptr; }
                    if (!warned) { warned = true; Log("final image probe: readback buffer failed, off"); }
                } else { g_finalData = (const uint8_t*)m; Log("final image probe: on (256x256 at each eye's lens centre, every 6th frame)"); }
            }
            if (g_finalRb) {
                const uint32_t seq = g_finalNext;
                if (seq >= (uint32_t)kFinalLag) {
                    const UINT old = (seq - kFinalLag) % kFinalSlots;
                    if (g_finalSeq[old] == seq - kFinalLag) FinalAnalyse(old);
                }
                const UINT slotNow = seq % kFinalSlots;
                const int cxL = std::min(eyeW - 1, std::max(0, (int)(eyeW * g_cfg.foveaGameCx))), cy = (int)(h * g_cfg.foveaGameCy);
                for (int eye = 0; eye < 2; ++eye) {
                    const int cx = eye == 0 ? cxL : eyeW + (eyeW - 1 - cxL);   // the right eye's lens centre mirrors the left's
                    const int x0 = std::min(eye * eyeW + eyeW - kFinalBox, std::max(eye * eyeW, cx - kFinalBox / 2));
                    const int y0 = std::min(h - kFinalBox, std::max(0, cy - kFinalBox / 2));
                    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = g_finalRb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint.Offset = (slotNow * 2 + eye) * kFinalEyeBytes;
                    dst.PlacedFootprint.Footprint = { d.Format, (UINT)kFinalBox, (UINT)kFinalBox, 1, kFinalPitch };
                    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = self->src; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    D3D12_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)(x0 + kFinalBox), (UINT)(y0 + kFinalBox), 1 };
                    gcl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                }
                VCall<void>(rhi, slot::D3D12RHI_FinishExternalComputeWork, (void*)cmdList, 0u, gcl);
                g_finalFmt[slotNow] = d.Format; g_finalSeq[slotNow] = seq; g_finalNext = seq + 1;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, self);
}

static void FinalPassExecute(CapBlock* b, uint8_t* cmdList) {
    void* tex = ((TexAccess*)b->params)->texture;
    void* rhiTex = tex ? At<void*>(tex, off::RDGRes_ResourceRHI) : nullptr;
    auto* cmd = (FinalCmd*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FinalCmd));
    if (!cmd) return;
    cmd->vtbl = g_finalCmdVt;
    cmd->src = rhiTex ? VCall<ID3D12Resource*>(rhiTex, slot::RHITexture_GetNativeResource) : nullptr;
    if (At<bool>(cmdList, off::CL_bExecuting)) FinalCmdExecute(cmd, cmdList);
    else {
        FinalCmd** link = At<FinalCmd**>(cmdList, off::CL_CommandLink);
        *link = cmd;
        At<FinalCmd**>(cmdList, off::CL_CommandLink) = &cmd->next;
        ++At<uint32_t>(cmdList, off::CL_NumCommands);
    }
}

// render thread, every view family: the headset's frames are counted only where its image is found (other families,
// e.g. a scope's scene capture, would otherwise make "every 6th" miss the headset for good)
static void FinalProbeAdd(void* builder) {
    if (!g_cfg.outputProbe || !builder) return;
    void* tex = FindFamilyTexture(builder);
    static bool seen = false; static unsigned misses = 0;
    if (!tex) {
        if (!seen && ++misses == 900)
            Log("final image probe: no headset image (ViewFamilyTexture, both eyes side by side) in 900 view families, nothing to check");
        return;
    }
    seen = true;
    if ((++g_finalFrames % 6) != 0 || !CapInitMetadata()) return;
    if (!g_finalPassVt[0]) {
        g_finalPassVt[0] = (void*)&CapPassDtor; g_finalPassVt[1] = (void*)&FinalPassExecute; g_finalPassVt[2] = (void*)&CapPassNoop;
        g_finalCmdVt[0] = (void*)&FinalCmdExecute;
    }
    AddCopyPass(builder, tex, &g_finalBlocks[g_finalAdded++ % _countof(g_finalBlocks)], g_finalPassVt);
}

// ------------------------------------------------------------------ public
uint32_t CaptureCopyCount() { return g_capCopies.load(); }

bool CaptureRequest(const wchar_t* file, int eyeMode, bool quiet) {   // game thread
    if (g_capBusy.exchange(true)) return false;
    g_capQuiet = quiet;
    memset(&g_job, 0, sizeof(g_job));
    wcsncpy_s(g_job.file, file, _TRUNCATE);
    g_job.eyeMode = eyeMode;
    g_job.srgb = g_cfg.shotSrgb;
    g_job.t0 = GetTickCount64();
    g_capPending = true;
    return true;
}

bool CaptureBusy() { return g_capBusy.load(); }

void CapturePostRenderViewFamily(void* builder) {   // render thread, end of each view family's graph
    FinalProbeAdd(builder);
    const bool shot = g_capPending.load();
    const bool burst = g_burstState.load() == 1 && g_burstAdded.load() < g_burst.n;
    if (!shot && !burst) return;
    if (!builder || !CapInitMetadata()) {
        if (shot) { g_capPending = false; CapFail("capture pass unavailable"); }
        if (burst) BurstFail("capture pass unavailable");
        return;
    }
    if (!g_burstPassVt[0]) {
        g_burstPassVt[0] = (void*)&CapPassDtor; g_burstPassVt[1] = (void*)&BurstPassExecute; g_burstPassVt[2] = (void*)&CapPassNoop;
        g_burstCmdVt[0] = (void*)&BurstCmdExecute;
    }
    void* tex = FindFamilyTexture(builder);
    if (shot) {
        if (tex) {
            g_capPending = false;
            AddCopyPass(builder, tex, &g_capBlock, g_capPassVt);
            Log("screenshot: copy pass added (ViewFamilyTexture %dx%d fmt=%u)",
                At<int32_t>(tex, off::RDGTex_Desc + off::Desc_Extent), At<int32_t>(tex, off::RDGTex_Desc + off::Desc_Extent + 4),
                At<uint8_t>(tex, off::RDGTex_Desc + off::Desc_Format));
        } else if (GetTickCount64() - g_job.t0 > 2000) {   // not in this graph (e.g. a scene capture); give up after 2 s
            g_capPending = false; CapFail("ViewFamilyTexture not found in the render graph");
        }
    }
    if (burst) {
        if (tex) {
            const int k = g_burstAdded++;
            if (k < 64) {
                g_burst.fresh[k] = g_noteN != g_noteSeen; g_noteSeen = g_noteN;
                g_burst.jx[k] = g_noteJx; g_burst.jy[k] = g_noteJy; g_burst.rw[k] = g_noteW; g_burst.rh[k] = g_noteH;
                g_burst.reset[k] = g_noteReset; g_burst.cx[k] = g_noteCx; g_burst.cy[k] = g_noteCy;
            }
            CapBlock* b = &g_burstBlocks[k % _countof(g_burstBlocks)];
            AddCopyPass(builder, tex, b, g_burstPassVt);
            At<int>(b->params, 0x18) = k;
        } else if (g_burstAdded.load() == 0 && GetTickCount64() - g_burst.t0 > 3000) BurstFail("ViewFamilyTexture not found");
    }
}
