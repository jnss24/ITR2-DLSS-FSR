// dlss.cpp - the upscale step: our motion-vector pass, then NGX DLSS (one feature per eye) or FSR (fsr.cpp),
// plus the passthrough copy. RHI thread only, except DlssInitOnInitThread (once, before the first frame) and
// key 0's side thread (DLSS centre models, SideMain).
#include "common.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "velocity_combine_cs.h"   // g_VelocityCombineCS (fxc output, includes the root signature)
#include "fovea_blend_cs.h"        // g_FoveaBlendCS
#include "sharpen_cs.h"            // g_SharpenCS
#include "fovea_compose_cs.h"      // g_FoveaComposeCS
#include "edge_aa_cs.h"            // g_EdgeAaCS
#include "dlss5_codec_cs.h"        // g_Dlss5CodecCS
#include "probe_cs.h"              // g_ProbeCS

static ID3D12Device* g_dev = nullptr;
static bool SideInit();   // key 0's side thread (DLSS centre models), made at init
static bool g_ready = false, g_ngxOk = false;
static ID3D12RootSignature* g_rs = nullptr;
static ID3D12PipelineState* g_pso = nullptr;
static ID3D12RootSignature* g_rsBlend = nullptr;
static ID3D12PipelineState* g_psoBlend = nullptr;
static ID3D12RootSignature* g_rsSharp = nullptr;
static ID3D12PipelineState* g_psoSharp = nullptr;
static ID3D12RootSignature* g_rsCompose = nullptr;
static ID3D12PipelineState* g_psoCompose = nullptr;
static ID3D12RootSignature* g_rsAa = nullptr;
static ID3D12PipelineState* g_psoAa = nullptr;
static ID3D12RootSignature* g_rsCodec = nullptr;
static ID3D12PipelineState* g_psoCodec = nullptr;
static ID3D12RootSignature* g_rsProbe = nullptr;
static ID3D12PipelineState* g_psoProbe = nullptr;
static bool g_r11Uav = false;   // R11G11B10_FLOAT works as a typed UAV (checked at init): colour copies keep the game's format
static DXGI_FORMAT g_aaFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;   // AA textures; R11G11B10_FLOAT with [Foveated] PlainTaaSmallHistory=1
static ID3D12DescriptorHeap* g_heap = nullptr;
static UINT g_descSize = 0, g_heapCursor = 0;
// motion-vector pass: t0 depth, t1 velocity, t2 colour, t3 translucency, u0 mv, u1 depth copy, u2 colour copy,
// u3 mask, u4 stats; then the fovea blend: t0 centre upscale, u0 output
constexpr UINT kDescPerPass = 39;   // + sharpening: t0 upscaled image, u0 output; + compose: t0 edges, t1 centre, u0 output;
                                    // + plain-edge AA (16-19): t0 colour copy, t1 history, t2 motion vectors, u0 result;
                                    // + motion-vector pass, second table (20-22): u5-u7 the window crops (lean inputs);
                                    // + image probe, output (23-26) and the game's colour (27-30): t0 exposure, t1 colour,
                                    //   u0 output, u1 counters;
                                    // + DLSS 5 codec encode (31-34) and decode (35-38): t0-t2 SRVs, u0 UAV
constexpr UINT kHeapSlots = kDescPerPass * 128;

// one NGX DLSS feature and the size/model it was created for
struct NgxFeat {
    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    int rw = 0, rh = 0, ow = 0, oh = 0;
    int preset = 0; Quality quality = Quality::Quality;
    uint64_t lastFrame = 0;                      // a gap = stale history (reset it)
    uint64_t failFrame = 0; int fails = 0;       // creation failures: retry every ~2 s, log sparingly
};

struct EyeState {
    NgxFeat full;                                // the whole eye with the live model
    NgxFeat periphHalf;                          // foveated edges at half output size ([Foveated] HalfResEdges)
    NgxFeat periph, center;                      // foveated: periphery model on the whole eye + live model on the window
    ID3D12Resource* centerOut = nullptr; int coW = 0, coH = 0;
    ID3D12Resource* sharpTmp = nullptr; int stW = 0, stH = 0;    // upscaler output when sharpening is on
    ID3D12Resource* periphTmp = nullptr; int ptW = 0, ptH = 0;   // half-size edge result
    // FSR in the centre: the window's crop of colour / depth / motion vectors (FSR has no sub-rect base)
    ID3D12Resource *winColor = nullptr, *winDepth = nullptr, *winMv = nullptr; int winW = 0, winH = 0;
    ID3D12Resource* mv = nullptr; int mvW = 0, mvH = 0;
    ID3D12Resource* colorCopy = nullptr; ID3D12Resource* depthCopy = nullptr; int copyW = 0, copyH = 0;
    uint64_t copyLastUse = 0;                    // unused with lean inputs: released after ~10 s
    ID3D12Resource* mask = nullptr; int maskW = 0, maskH = 0;
    // colour copy and window colour crop format: the game's own (R11G11B10_FLOAT here), so the copies are exact and half
    // the size of RGBA16F; RGBA16F when the game's colour is another format or the GPU cannot store R11G11B10
    DXGI_FORMAT copyFmt = DXGI_FORMAT_R16G16B16A16_FLOAT, winFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    // plain-edge AA (key 1): result at render size; temporal mode ping-pongs aa[0]/aa[1] (one is the history)
    ID3D12Resource* aa[2] = {}; int aaW = 0, aaH = 0, aaCur = 0, aaPrevW = 0, aaPrevH = 0, aaMode = 0; uint64_t aaFrame = 0;
    int aaFill = 0, aaWinKey = 0;   // frames left to write every pixel (no window skip), after a reset or window change
    uint64_t aaLastUse = 0, aaLastTemporal = 0;   // last frame any AA / the temporal AA ran (released after ~10 s unused)
    // DLSS 5 (optional bridge DLL): tonemapped input, neural output, decoded HDR output for the window
    ID3D12Resource *nrIn = nullptr, *nrOut = nullptr, *nrFinal = nullptr; int nrW = 0, nrH = 0;
    uint64_t nrLastFrame = 0;
    bool nrTwo = false;   // the second DLSS 5 pass (slot 2 + eye) holds a feature
    int logLeft = 0;
    bool standIn = false;                        // last pass drew the stand-in (key 0: centre model being built)
};
static EyeState g_eye[2];

// ---------------------------------------------------------------- optional DLSS 5 bridge (itr2_dlss5_bridge.dll)
// Declared here so src/ has no header or library dependency on bridge/.
struct Dlss5Tuning {
    float    intensity              = 0.8f;
    float    localToneStrength      = 0.75f;
    float    localStructureStrength = 0.9f;
    float    skinStructureStrength  = 0.9f;
    uint32_t style                  = 3u;
    uint32_t useAutoMask            = 1u;
    uint32_t uiCorrection           = 0u;
    uint32_t performanceQuality     = 2u;
    uint32_t outputPreset           = 0u;
};
struct Dlss5EvalDesc {
    ID3D12GraphicsCommandList* cl            = nullptr;
    uint32_t                   slot          = 0u;
    ID3D12Resource*            color         = nullptr;
    ID3D12Resource*            depth         = nullptr;
    ID3D12Resource*            motionVectors = nullptr;
    ID3D12Resource*            output        = nullptr;
    uint32_t                   inputWidth    = 0u;
    uint32_t                   inputHeight   = 0u;
    uint32_t                   outputWidth   = 0u;
    uint32_t                   outputHeight  = 0u;
    float                      mvScaleX      = 1.f;
    float                      mvScaleY      = 1.f;
    uint32_t                   depthInverted = 1u;
    uint32_t                   reset         = 0u;
    Dlss5Tuning                tuning;
};
struct Dlss5BridgeApi {
    uint32_t abiVersion;
    bool        (__cdecl* Probe)(const wchar_t* explicitPathOrDir);
    bool        (__cdecl* Initialize)(ID3D12Device* device, const wchar_t* writableDataDir);
    bool        (__cdecl* Execute)(const Dlss5EvalDesc* desc);
    void        (__cdecl* ReleaseSlot)(uint32_t slot);
    void        (__cdecl* ReleaseAll)();
    void        (__cdecl* Shutdown)();
    const char* (__cdecl* GetStatusString)();
    const char* (__cdecl* GetDetailString)();
    const char* (__cdecl* GetVersionString)();
    uint32_t    (__cdecl* GetLastNgxResult)();
    uint64_t    (__cdecl* GetSuccessfulFrames)();
};
static HMODULE g_bridgeMod = nullptr;
static const Dlss5BridgeApi* g_bridge = nullptr;
static bool g_bridgeOk = false;

bool Dlss5Available() { return g_bridgeOk && g_psoCodec != nullptr; }

static void InitBridge() {
    if (!g_ngxOk) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sitr2_dlss5_bridge.dll", g_dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;   // bridge not installed: no log
    g_bridgeMod = LoadLibraryW(path);
    if (!g_bridgeMod) {
        Log("DLSS 5 bridge: LoadLibraryW(itr2_dlss5_bridge.dll) failed (%lu)", GetLastError());
        return;
    }
    using PFN_GetApi = const Dlss5BridgeApi* (__cdecl*)();
    auto getApi = (PFN_GetApi)GetProcAddress(g_bridgeMod, "ITR2_Dlss5Bridge_GetApi");
    const Dlss5BridgeApi* api = getApi ? getApi() : nullptr;
    if (!api || api->abiVersion != 1u || !api->Probe || !api->Initialize || !api->Execute) {
        Log("DLSS 5 bridge: incompatible ABI in itr2_dlss5_bridge.dll");
        FreeLibrary(g_bridgeMod); g_bridgeMod = nullptr;
        return;
    }
    if (!api->Probe(g_dir)) {
        Log("DLSS 5 bridge: probe -> %s (%s)", api->GetStatusString(), api->GetDetailString());
        FreeLibrary(g_bridgeMod); g_bridgeMod = nullptr;
        return;
    }
    if (!api->Initialize(g_dev, g_logDir)) {
        Log("DLSS 5 bridge: init -> %s (%s)", api->GetStatusString(), api->GetDetailString());
        api->Shutdown();
        FreeLibrary(g_bridgeMod); g_bridgeMod = nullptr;
        return;
    }
    g_bridge = api;
    g_bridgeOk = true;
    Log("DLSS 5 bridge: ready, %s", api->GetDetailString());
}
// DlssExecute (RHI thread) and DlssReleaseAll (game thread, while DLSS is off) never run at the same time
static SRWLOCK g_execLock = SRWLOCK_INIT;
struct ExecGuard { ExecGuard() { AcquireSRWLockExclusive(&g_execLock); } ~ExecGuard() { ReleaseSRWLockExclusive(&g_execLock); } };

struct Dead { NVSDK_NGX_Handle* h; ID3D12Resource* r[2]; uint64_t frame; };
static Dead g_grave[64];   // low-memory mode can retire several features and textures per eye in one frame
static uint64_t g_frame = 0;
static bool g_warnedVelFmt = false;

// GPU timestamps around our work for the CSV's per-pass times, ring of kTsSlots frames. kTsPer stamps per eye and
// frame: begin, after the motion-vector pass, after the whole-eye upscaler, after the centre window, after the edge
// AA, end. A stamp a frame does not reach is written with the next one, so that pass counts 0.
static ID3D12QueryHeap* g_tsHeap = nullptr;
static ID3D12Resource* g_tsReadback = nullptr;
static const uint64_t* g_tsData = nullptr;
static double g_tsToMs = 0.0;
constexpr UINT kTsSlots = 8;
constexpr UINT kTsPer = 6;

static void InitTimestamps() {
    auto q = VCall<ID3D12CommandQueue*>(GetDynamicRHI(), ue::slot::D3D12RHI_GetCommandQueue);
    UINT64 freq = 0;
    if (!q || FAILED(q->GetTimestampFrequency(&freq)) || !freq) { Log("perf: no timestamp frequency (DLSS GPU time column empty)"); return; }
    D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTsSlots * 2 * kTsPer, 0 };
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = kTsSlots * 2 * kTsPer * sizeof(uint64_t); rd.Height = 1;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* p = nullptr;
    if (FAILED(g_dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_tsHeap))) ||
        FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_tsReadback))) ||
        FAILED(g_tsReadback->Map(0, nullptr, &p))) {
        Log("perf: timestamp resources failed (DLSS GPU time column empty)"); g_tsHeap = nullptr; return;
    }
    memset(p, 0, (size_t)rd.Width);
    g_tsData = (const uint64_t*)p;
    g_tsToMs = 1000.0 / (double)freq;
}

// mask diagnostics: 4 counters filled by the motion-vector pass (eye 0, ~once a second), read back later
static ID3D12Resource* g_statsBuf = nullptr;     // default heap, UAV (raw)
static ID3D12Resource* g_statsZero = nullptr;    // upload heap, zeros
static ID3D12Resource* g_statsRb = nullptr;      // readback
static const uint32_t* g_statsData = nullptr;
static uint64_t g_statsPendingFrame = 0;
static uint32_t g_statsPixels = 0;

static ID3D12Resource* NewBuffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
    ID3D12Resource* r = nullptr;
    return SUCCEEDED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r))) ? r : nullptr;
}

static void InitStats() {
    g_statsBuf = NewBuffer(16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g_statsZero = NewBuffer(16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    g_statsRb = NewBuffer(16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    void* z = nullptr; void* rb = nullptr;
    if (!g_statsBuf || !g_statsZero || !g_statsRb || FAILED(g_statsZero->Map(0, nullptr, &z)) || FAILED(g_statsRb->Map(0, nullptr, &rb))) {
        Log("upscaler: mask diagnostics unavailable"); g_statsBuf = nullptr; return;
    }
    memset(z, 0, 16); g_statsZero->Unmap(0, nullptr);
    g_statsData = (const uint32_t*)rb;
}

static void InitProbe();   // output probe, below

// logs the counters from a few frames ago (the GPU is done with them)
static void LogStats() {
    if (!g_statsPendingFrame || g_frame < g_statsPendingFrame + 4) return;
    g_statsPendingFrame = 0;
    static int left = 240;
    if (left-- <= 0 || !g_statsPixels) return;
    const double n = g_statsPixels;
    Log("mask stats (eye 0, %u px): translucency alpha<1 %.2f%% | mask>0.5 %.2f%% | alpha outside 0..1 %.2f%% | translucency colour non-black %.2f%%",
        g_statsPixels, 100.0 * g_statsData[0] / n, 100.0 * g_statsData[1] / n, 100.0 * g_statsData[2] / n, 100.0 * g_statsData[3] / n);
}

bool DlssReady() { return g_ready; }
bool NgxAvailable() { return g_ngxOk; }

static void NgxLog(const char* msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) { Log("[NGX] %s", msg); }

// the GPU the game renders on (for bug reports, Preset=Auto and LowMemory=2)
static char g_gpuName[128] = "?";
static char g_gpuDriver[48] = "?";   // part of the performance profile's key
const char* GpuName() { return g_gpuName; }
const char* GpuDriver() { return g_gpuDriver; }
static UINT g_gpuVendor = 0;
static double g_gpuVramMb = 0.0;
// ---------------------------------------------------------------- low-VRAM test ([Debug] VramBallastMb)
// Holds N MB of video memory on the game's device, in 512 MB buffers at maximum residency priority, and touches each
// buffer ~once a second (a 256-byte copy) so Windows treats it as in use and pages the game's memory out instead.
// 8 GB held on a 16 GB card leaves the game and the VR runtime ~8 GB, as on an 8 GB card. Test only, off by default;
// the game's start-up choices (texture pool from total VRAM) still see 16 GB.
static ID3D12Resource* g_ballast[32] = {};
static int g_ballastN = 0;
static ID3D12Resource* g_ballastSink = nullptr;
static void AllocBallast() {
    const int mb = g_cfg.vramBallastMb;
    if (mb <= 0) return;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    int left = mb;
    while (left > 0 && g_ballastN < (int)_countof(g_ballast)) {
        const int chunk = left > 512 ? 512 : left;
        rd.Width = (UINT64)chunk * 1048576ull;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_ballast[g_ballastN])))) break;
        ++g_ballastN; left -= chunk;
    }
    rd.Width = 256 * 32;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_ballastSink));
    ID3D12Device1* d1 = nullptr;
    if (g_ballastN && SUCCEEDED(g_dev->QueryInterface(IID_PPV_ARGS(&d1))) && d1) {
        ID3D12Pageable* objs[32]; D3D12_RESIDENCY_PRIORITY pr[32];
        for (int i = 0; i < g_ballastN; ++i) { objs[i] = g_ballast[i]; pr[i] = D3D12_RESIDENCY_PRIORITY_MAXIMUM; }
        d1->SetResidencyPriority((UINT)g_ballastN, objs, pr);
        d1->Release();
    }
    float used = 0.f, budget = 0.f; VramQuery(used, budget);
    float nlUsed = -1.f, nlBudget = 0.f; VramQueryNonLocal(nlUsed, nlBudget);
    Log("LOW-VRAM TEST: holding %d MB of video memory (%d buffers of up to 512 MB; asked %d MB); process VRAM now %.0f of %.0f MB, non-local %.0f of %.0f MB",
        (mb - left), g_ballastN, mb, used, budget, nlUsed, nlBudget);
    snprintf(g_startNote, sizeof(g_startNote), "Low V RAM test. %d gigabytes reserved.", (mb - left + 512) / 1024);
}
// RHI thread, eye 0, every ~90 frames: a tiny copy from each ballast buffer keeps it 'in use'
static void TouchBallast(ID3D12GraphicsCommandList* cl) {
    if (!g_ballastN || !g_ballastSink) return;
    for (int i = 0; i < g_ballastN; ++i) cl->CopyBufferRegion(g_ballastSink, (UINT64)(i % 32) * 256, g_ballast[i], 0, 256);
}

static void LogAdapter() {
    HMODULE dx = GetModuleHandleW(L"dxgi.dll");
    using PFN_Create = HRESULT(WINAPI*)(REFIID, void**);
    auto create = dx ? (PFN_Create)GetProcAddress(dx, "CreateDXGIFactory1") : nullptr;
    IDXGIFactory4* f = nullptr; IDXGIAdapter1* a = nullptr;
    if (!create || FAILED(create(IID_PPV_ARGS(&f))) || !f) { Log("GPU: DXGI not available"); return; }
    if (SUCCEEDED(f->EnumAdapterByLuid(g_dev->GetAdapterLuid(), IID_PPV_ARGS(&a))) && a) {
        DXGI_ADAPTER_DESC1 d = {}; a->GetDesc1(&d);
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, g_gpuName, sizeof(g_gpuName) - 1, nullptr, nullptr);
        g_gpuVendor = d.VendorId; g_gpuVramMb = d.DedicatedVideoMemory / 1048576.0;
        if (g_cfg.fakeVramMb > 0) {
            Log("GPU: [Debug] FakeVramMb: the mod's auto settings assume %d MB instead of %.0f MB", g_cfg.fakeVramMb, g_gpuVramMb);
        }
        LARGE_INTEGER umd = {}; char drv[48] = "?";
        if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            const unsigned p3 = HIWORD(umd.LowPart), p4 = LOWORD(umd.LowPart);
            if (d.VendorId == 0x10DE) snprintf(drv, sizeof(drv), "%u.%02u", ((p3 % 10) * 10000 + p4) / 100, ((p3 % 10) * 10000 + p4) % 100);
            else snprintf(drv, sizeof(drv), "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), p3, p4);
        }
        strcpy_s(g_gpuDriver, drv);
        const char* vendor = d.VendorId == 0x10DE ? "NVIDIA" : d.VendorId == 0x1002 ? "AMD" : d.VendorId == 0x8086 ? "Intel" : "other";
        Log("GPU: %s (%s, vendor 0x%04X device 0x%04X), %.0f MB VRAM, driver %s", g_gpuName, vendor, d.VendorId, d.DeviceId, g_gpuVramMb, drv);
        a->Release();
    } else Log("GPU: adapter not found by LUID");
    f->Release();
}

static bool InitNgx() {
    static wchar_t dllDir[MAX_PATH];
    wcscpy_s(dllDir, g_dir);
    size_t n = wcslen(dllDir); if (n && dllDir[n - 1] == L'\\') dllDir[n - 1] = 0;
    static const wchar_t* paths[1] = { dllDir };
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths; info.PathListInfo.Length = 1;
    if (g_cfg.ngxLogging) {
        info.LoggingInfo.LoggingCallback = NgxLog;
        info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
        info.LoggingInfo.DisableOtherLoggingSinks = true;
    }
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("6f1d2c3b-8a4e-4f5a-9b7c-2d3e4f5a6b7c",
        NVSDK_NGX_ENGINE_TYPE_UNREAL, "5.5", dllDir, g_dev, &info);
    if (NVSDK_NGX_FAILED(r)) {
        Log("DLSS: NGX init failed 0x%08X (no NVIDIA RTX GPU or driver?)", (unsigned)r);
        if (g_gpuVendor == 0x10DE) snprintf(g_startNote, sizeof(g_startNote), "D L S S could not start on this card. Using F S R.");
        return false;
    }

    NVSDK_NGX_Parameter* caps = nullptr;
    int available = 0, needsDriver = 0, minMajor = 0, minMinor = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)) && caps) {
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minMajor);
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minMinor);
        NVSDK_NGX_D3D12_DestroyParameters(caps);
    }
    Log("DLSS: NGX initialised, SuperSampling available=%d needsUpdatedDriver=%d (min %d.%d)", available, needsDriver, minMajor, minMinor);
    if (!available) {
        if (needsDriver) snprintf(g_startNote, sizeof(g_startNote), "D L S S needs NVIDIA driver %d.%d or newer. Using F S R.", minMajor, minMinor);
        else snprintf(g_startNote, sizeof(g_startNote), "D L S S is not supported on this card. Using F S R.");
        NVSDK_NGX_D3D12_Shutdown1(g_dev);
        return false;
    }
    for (auto& e : g_eye) {
        NgxFeat* feats[] = { &e.full, &e.periph, &e.center, &e.periphHalf };
        for (NgxFeat* ft : feats)
            if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&ft->params)) || !ft->params) { Log("DLSS: AllocateParameters failed"); return false; }
        // by default ReleaseFeature keeps the memory cached for a re-create (guide 310.6, 3.14), so releasing
        // models freed nothing; this hint makes every release give the VRAM back
        for (NgxFeat* ft : feats) NVSDK_NGX_Parameter_SetI(ft->params, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
    }
    return true;
}

static bool CycleHasFsr() {
    if (g_cfg.preset == kPresetFsr) return true;
    for (int i = 0; i < g_cfg.presetCycleN; ++i) if (g_cfg.presetCycle[i] == kPresetFsr) return true;
    return false;
}

bool DlssInitOnInitThread() {
    void* rhi = GetDynamicRHI();
    g_dev = VCall<ID3D12Device*>(rhi, ue::slot::D3D12RHI_GetDevice, 0u);
    if (!g_dev) { Log("DLSS: no ID3D12Device"); return false; }
    HitchInitDevice(g_dev);   // PSO creation timer (debug build)

    LogAdapter();
    AllocBallast();
    g_ngxOk = InitNgx();
    // Preset=Auto: the DLSS 4.5 models (M, L) need the FP8 hardware of RTX 40/50; older RTX cards get K
    if (g_cfg.preset == kPresetAuto || LivePreset() == kPresetAuto) {
        const bool newRtx = strstr(g_gpuName, "RTX 50") || strstr(g_gpuName, "RTX 40");
        const int pick = !g_ngxOk ? kPresetFsr : newRtx ? 13 : 11;
        g_cfg.preset = pick; g_livePreset = pick;
        Log("upscaler: Preset=Auto -> %s for %s", PresetName(pick), g_gpuName);
    }
    // LowMemory=2: on below 10 GB of VRAM (each DLSS model takes 165-860 MB per eye)
    if (g_cfg.lowMemMode == 2) {
        const double vramForAuto = g_cfg.fakeVramMb > 0 ? (double)g_cfg.fakeVramMb : g_gpuVramMb;
        g_liveLowMem = vramForAuto > 0.0 && vramForAuto < 10240.0;
        Log("low-memory mode: auto -> %s (%.0f MB VRAM)", g_liveLowMem ? "ON" : "off", g_gpuVramMb);
    }
    // FSR is always loaded when its DLLs are there: it is the fallback if DLSS fails during play, not only when
    // NGX is missing at start; CycleHasFsr only decides whether F7 offers it
    const bool fsrOk = FsrInit(g_dev);
    if (!g_ngxOk && !fsrOk) return false;
    // keep the F7 cycle to what can run (this runs before the first game frame)
    int n = 0;
    for (int i = 0; i < g_cfg.presetCycleN; ++i) {
        const int p = g_cfg.presetCycle[i];
        if ((p == kPresetFsr) ? fsrOk : g_ngxOk) g_cfg.presetCycle[n++] = p;
    }
    if (n == 0) g_cfg.presetCycle[n++] = g_ngxOk ? 13 : kPresetFsr;
    g_cfg.presetCycleN = n;
    const int live = LivePreset();
    if ((live == kPresetFsr) ? !fsrOk : !g_ngxOk) {
        g_livePreset = g_cfg.presetCycle[0];
        Log("upscaler: %s is not available, starting with %s", PresetName(live), PresetName(g_cfg.presetCycle[0]));
    }
    for (auto& e : g_eye) e.logLeft = g_cfg.verboseFrames;
    if (!g_ngxOk && !fsrOk && g_liveFovea) { g_liveFovea = false; Log("foveated: needs DLSS or FSR, disabled"); }

    if (FAILED(g_dev->CreateRootSignature(0, g_VelocityCombineCS, sizeof(g_VelocityCombineCS), IID_PPV_ARGS(&g_rs)))) {
        Log("DLSS: CreateRootSignature failed"); return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_rs; pd.CS = { g_VelocityCombineCS, sizeof(g_VelocityCombineCS) };
    if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso)))) { Log("DLSS: CreateComputePipelineState failed"); return false; }
    if (FAILED(g_dev->CreateRootSignature(0, g_FoveaBlendCS, sizeof(g_FoveaBlendCS), IID_PPV_ARGS(&g_rsBlend)))) {
        Log("DLSS: fovea blend root signature failed"); return false;
    }
    pd.pRootSignature = g_rsBlend; pd.CS = { g_FoveaBlendCS, sizeof(g_FoveaBlendCS) };
    if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_psoBlend)))) { Log("DLSS: fovea blend PSO failed"); return false; }
    if (FAILED(g_dev->CreateRootSignature(0, g_SharpenCS, sizeof(g_SharpenCS), IID_PPV_ARGS(&g_rsSharp)))) {
        Log("DLSS: sharpen root signature failed"); return false;
    }
    pd.pRootSignature = g_rsSharp; pd.CS = { g_SharpenCS, sizeof(g_SharpenCS) };
    if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_psoSharp)))) { Log("DLSS: sharpen PSO failed"); return false; }
    if (FAILED(g_dev->CreateRootSignature(0, g_FoveaComposeCS, sizeof(g_FoveaComposeCS), IID_PPV_ARGS(&g_rsCompose)))) {
        Log("DLSS: fovea compose root signature failed"); return false;
    }
    pd.pRootSignature = g_rsCompose; pd.CS = { g_FoveaComposeCS, sizeof(g_FoveaComposeCS) };
    if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_psoCompose)))) { Log("DLSS: fovea compose PSO failed"); return false; }
    // plain-edge AA is optional: without it key 1 just has no effect
    if (SUCCEEDED(g_dev->CreateRootSignature(0, g_EdgeAaCS, sizeof(g_EdgeAaCS), IID_PPV_ARGS(&g_rsAa)))) {
        pd.pRootSignature = g_rsAa; pd.CS = { g_EdgeAaCS, sizeof(g_EdgeAaCS) };
        if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_psoAa)))) { g_psoAa = nullptr; Log("DLSS: plain-edge AA PSO failed (key 1 off)"); }
    } else Log("DLSS: plain-edge AA root signature failed (key 1 off)");
    InitBridge();
    if (g_bridgeOk) {
        if (SUCCEEDED(g_dev->CreateRootSignature(0, g_Dlss5CodecCS, sizeof(g_Dlss5CodecCS), IID_PPV_ARGS(&g_rsCodec)))) {
            pd.pRootSignature = g_rsCodec; pd.CS = { g_Dlss5CodecCS, sizeof(g_Dlss5CodecCS) };
            if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_psoCodec)))) {
                g_psoCodec = nullptr; Log("DLSS 5: codec PSO failed (DLSS 5 off)");
            }
        } else Log("DLSS 5: codec root signature failed (DLSS 5 off)");
    }
    if (!Dlss5Available() && g_liveDlss5.load() != 0) {
        g_liveDlss5 = 0;
        Log("DLSS 5: requested in ini (Mode=%d), but the bridge DLL / nvngx_dlssnr.dll is not available -> off", g_cfg.dlss5Mode);
    }
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { DXGI_FORMAT_R11G11B10_FLOAT };
        g_r11Uav = SUCCEEDED(g_dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
                   (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
        Log("upscaler: R11G11B10_FLOAT typed UAV store %s", g_r11Uav ? "supported (colour copies in the game's format)" : "NOT supported (RGBA16F copies)");
    }
    if (g_psoAa && g_cfg.taaSmallHistory) {   // half the memory: 4 bytes per pixel, the temporal result dithered
        const bool ok = g_r11Uav;
        if (ok) g_aaFmt = DXGI_FORMAT_R11G11B10_FLOAT;
        Log("edge AA: small history %s", ok ? "ON (R11G11B10_FLOAT, 4 bytes per pixel, dithered)" : "not supported by this GPU: RGBA16F");
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kHeapSlots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) { Log("DLSS: CreateDescriptorHeap failed"); return false; }
    g_descSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (g_cfg.perfEnabled || g_cfg.benchKey) InitTimestamps();   // the CSV and the benchmark (F10) read them
    InitStats();
    InitProbe();
    // key 0's side thread, made here: creating it at the first rebuild costs that rebuild 4 skipped refreshes
    if (g_ngxOk) SideInit();

    g_ready = true;
    Log("upscaler ready: DLSS %s, FSR %s (quality=%s preset=%s screenPercentage=%.1f autoExposure=%d dilateMV=%d separateEyeInputs=%d)",
        g_ngxOk ? "yes" : "no", fsrOk ? "yes" : "no", QualityName(LiveQuality()), PresetName(LivePreset()),
        QualityToScreenPercentage(LiveQuality()), (int)g_cfg.autoExposure, (int)g_cfg.dilateMotionVectors, (int)g_cfg.separateEyeInputs);
    // FSR as an edge model: load it now if F7 did not, or drop it from the F9 cycle
    bool periphFsrWanted = g_cfg.peripheryPreset == kPresetFsr;
    for (int i = 0; i < g_cfg.foveaCycleN; ++i) if (g_cfg.foveaCycle[i] == kPresetFsr) periphFsrWanted = true;
    if (periphFsrWanted && !FsrReady() && !FsrInit(g_dev)) {
        int k = 0;
        for (int i = 0; i < g_cfg.foveaCycleN; ++i) if (g_cfg.foveaCycle[i] != kPresetFsr) g_cfg.foveaCycle[k++] = g_cfg.foveaCycle[i];
        if (k == 0) g_cfg.foveaCycle[k++] = 5;
        g_cfg.foveaCycleN = k;
        if (g_cfg.peripheryPreset == kPresetFsr) { g_cfg.peripheryPreset = 5; g_livePeriph = 5; }
        Log("foveated: FSR unavailable, edges use DLSS preset E");
    }
    char cyc[64] = ""; for (int i = 0; i < g_cfg.foveaCycleN; ++i) { strcat_s(cyc, i ? "," : ""); strcat_s(cyc, PresetName(g_cfg.foveaCycle[i])); }
    Log("foveated upscaling: %s at start, edges %s, F9 cycle off,%s, window %.0f%%, soft edge %.0f%%", g_liveFovea ? "ON" : "off",
        PresetName(g_livePeriph), cyc, g_cfg.foveaSize * 100.f, g_cfg.foveaFeather * 100.f);
    return true;
}

// ---------------------------------------------------------------- helpers
static uint32_t g_barrierCalls = 0;   // ResourceBarrier calls in the current eye pass (CSV barriers_eye)
static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &br);
    ++g_barrierCalls;
}
// Back-to-back barriers, one ResourceBarrier call each. One call per group cuts 13 -> 7 calls per eye but saves no
// measurable GPU time.
struct Barriers {
    D3D12_RESOURCE_BARRIER b[16] = {}; UINT n = 0;
    void T(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES s) {
        if (!r || n >= _countof(b)) return;
        D3D12_RESOURCE_BARRIER& x = b[n++]; x = {};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.pResource = r; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        x.Transition.StateBefore = a; x.Transition.StateAfter = s;
    }
    void U(ID3D12Resource* r) {
        if (!r || n >= _countof(b)) return;
        D3D12_RESOURCE_BARRIER& x = b[n++]; x = {}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; x.UAV.pResource = r;
    }
    void Flush(ID3D12GraphicsCommandList* cl) {
        if (!n) return;
        for (UINT i = 0; i < n; ++i) { cl->ResourceBarrier(1, &b[i]); ++g_barrierCalls; }
        n = 0;
    }
};

static DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
static DXGI_FORMAT VelocitySrvFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;   // UINT-packed velocity is not handled: camera-only motion vectors
    }
}
static DXGI_FORMAT ColorSrvFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

// Who is inside NGX. NGX runs one call at a time across all threads (tests/asynctest.cpp): while a create runs
// (16-35 ms of CPU), an evaluate on another thread waits for it. Key 0's side thread builds models only while it
// holds this; an eye pass takes it without waiting and, when the side thread has it, draws the stand-in (below).
static std::atomic<int> g_ngxOwner{ 0 };   // 0 nobody, 1 an eye pass or DlssReleaseAll, 2 the side thread
static bool g_ngxHeld = false;             // the current eye pass / DlssReleaseAll holds it (under g_execLock)
static bool NgxTry() {
    if (!g_ngxHeld) { int z = 0; g_ngxHeld = g_ngxOwner.compare_exchange_strong(z, 1); }
    return g_ngxHeld;
}
static void NgxWait() {   // waits for the side thread's build: setups with no stand-in
    while (!NgxTry()) Sleep(0);
}
static void NgxLeave() { if (g_ngxHeld) { g_ngxHeld = false; g_ngxOwner = 0; } }

static void Bury(NVSDK_NGX_Handle* h, ID3D12Resource* r0, ID3D12Resource* r1 = nullptr) {
    if (!h && !r0 && !r1) return;
    for (auto& d : g_grave) if (!d.h && !d.r[0] && !d.r[1]) { d = { h, { r0, r1 }, g_frame }; return; }
    // graveyard full: release the oldest entry now (rare: needs many resizes within a few frames)
    if (g_grave[0].h) { NgxWait(); NVSDK_NGX_D3D12_ReleaseFeature(g_grave[0].h); }
    for (auto* r : g_grave[0].r) if (r) r->Release();
    g_grave[0] = { h, { r0, r1 }, g_frame };
}
static void CollectGraveyard() {
    for (auto& d : g_grave)
        if ((d.h || d.r[0] || d.r[1]) && g_frame > d.frame + 16 && (!d.h || g_ngxHeld)) {   // (a model waits for a pass that holds NGX)
            if (d.h) NVSDK_NGX_D3D12_ReleaseFeature(d.h);
            for (auto* r : d.r) if (r) r->Release();
            d = {};
        }
}

// low-memory mode: retire a feature / texture this frame does not use (released 16 frames later; the
// Ensure* functions recreate it when it is needed again)
static void DropFeat(NgxFeat& ft, int eye, const char* tag) {
    if (!ft.handle) return;
    Log("DLSS: release %s feature eye=%d preset=%s %dx%d -> %dx%d (low-memory mode or VRAM over budget)", tag, eye, PresetName(ft.preset), ft.rw, ft.rh, ft.ow, ft.oh);
    Bury(ft.handle, nullptr); ft.handle = nullptr; ft.rw = ft.rh = ft.ow = ft.oh = 0;
}
static void DropTex(ID3D12Resource*& r, int& w, int& h) { if (r) { Bury(nullptr, r); r = nullptr; w = h = 0; } }

// ---------------------------------------------------------------- image probe ([Test] OutputProbe, probe.hlsl)
// Per eye, two blocks of running totals: the eye's finished output (bad pixels + the game's exposure value) and the
// game's colour going into the upscaler. Copied every frame into a ring of readback slots, read kProbeLag frames
// later. Logs a line at once when an eye has NaN / infinite pixels, turns black while the other does not, or has an
// exposure value 1.5x away from the other eye's, plus a summary every 2 s. Each block also holds that frame's
// brightest colour and alpha range (bytes 36-47, zeroed before each dispatch).
constexpr UINT kProbeSlots = 8, kProbeLag = 6, kProbeBlock = 64, kProbeBlocks = 4;   // out 0, out 1, in 0, in 1
// (same lag as the timestamps, kTsSlots - 2: frames a slow GPU has not finished are skipped and counted later)
static ID3D12Resource* g_probeBuf = nullptr;    // default heap, UAV (raw), zeroed at creation
static ID3D12Resource* g_probeRb = nullptr;     // readback, kProbeSlots x kProbeBlocks blocks
static ID3D12Resource* g_probeZero = nullptr;   // upload heap, zeros: clears the per-frame maxima
static const uint8_t* g_probeData = nullptr;
struct ProbeSlot { uint64_t frame; float pre; double px, pxIn; uint32_t state; bool hadOut; };   // what each slot's frame was
static ProbeSlot g_probeSlot[kProbeSlots][2] = {};
static bool g_r11Load = false, g_rgba16Load = false;   // typed UAV loads of the output's format (the probe reads it as a UAV)
std::atomic<bool> g_liveClean{ false };
std::atomic<bool> g_liveOutGame{ false };
std::atomic<bool> g_liveInClean{ false };
std::atomic<bool> g_liveAutoReset{ false };

bool DlssGameFormatOutputOk() { return g_r11Uav; }

// UAV format for an output-sized texture: R11G11B10 when the output uses it ([Test] OutFormatKey)
static DXGI_FORMAT OutUavFormat(ID3D12Resource* r) {
    return r && r->GetDesc().Format == DXGI_FORMAT_R11G11B10_FLOAT ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

static void InitProbe() {
    if (!g_cfg.outputProbe) return;
    if (FAILED(g_dev->CreateRootSignature(0, g_ProbeCS, sizeof(g_ProbeCS), IID_PPV_ARGS(&g_rsProbe)))) { Log("probe: root signature failed, probe off"); return; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_rsProbe; pd.CS = { g_ProbeCS, sizeof(g_ProbeCS) };
    if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_psoProbe)))) { Log("probe: PSO failed, probe off"); g_psoProbe = nullptr; return; }
    g_probeBuf = NewBuffer(kProbeBlocks * kProbeBlock, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g_probeRb = NewBuffer(kProbeSlots * kProbeBlocks * kProbeBlock, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    g_probeZero = NewBuffer(kProbeBlock, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    void* rb = nullptr; void* z = nullptr;
    if (!g_probeBuf || !g_probeRb || !g_probeZero || FAILED(g_probeRb->Map(0, nullptr, &rb)) || FAILED(g_probeZero->Map(0, nullptr, &z))) {
        Log("probe: buffers failed, probe off"); g_psoProbe = nullptr; return;
    }
    memset(z, 0, kProbeBlock); g_probeZero->Unmap(0, nullptr);
    g_probeData = (const uint8_t*)rb;
    auto typedLoad = [](DXGI_FORMAT fmt) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { fmt };
        return SUCCEEDED(g_dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD);
    };
    g_r11Load = typedLoad(DXGI_FORMAT_R11G11B10_FLOAT); g_rgba16Load = typedLoad(DXGI_FORMAT_R16G16B16A16_FLOAT);
    Log("probe: on (each eye's output and the game's colour going in: bad pixels, black; the game's exposure per eye); "
        "output read %s; R11G11B10 output %s, probe on it %s", g_rgba16Load ? "possible" : "NOT possible (only the game's colour is checked)",
        g_r11Uav ? "possible" : "not possible", g_r11Load ? "possible" : "not possible");
}

static DXGI_FORMAT ExposureSrvFormat(ID3D12Resource* r) {
    if (!r) return DXGI_FORMAT_UNKNOWN;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return DXGI_FORMAT_UNKNOWN;
    switch (d.Format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R16_FLOAT: return d.Format;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// the settings of a measured frame, packed (ProbeRun) and spelled out for the log lines (ProbeRead)
enum : uint32_t { PS_Mask = 4, PS_Clean = 8, PS_GameFmt = 16, PS_Fovea = 32, PS_Fsr = 64, PS_InClean = 128, PS_AutoReset = 256 };   // bits 0-1: FSR exposure mode
static void ProbeState(char* b, size_t n, uint32_t st) {
    static const char* exp[4] = { "game exposure", "FSR auto exposure", "no exposure", "?" };
    const char* fmt = (st & PS_GameFmt) ? "game format" : "RGBA16F";
    const char* in = (st & PS_InClean) ? "on" : "off";
    const char* ar = (st & PS_AutoReset) ? "on" : "off";
    if (st & PS_Fsr) snprintf(b, n, "FSR %s, %s, mask %s, cleanup %s, input cleanup %s, auto reset %s, output %s, foveated %s", FsrRunningName(),
                              exp[st & 3], (st & PS_Mask) ? "on" : "off", (st & PS_Clean) ? "on" : "off", in, ar, fmt, (st & PS_Fovea) ? "on" : "off");
    else snprintf(b, n, "DLSS %s, cleanup %s, input cleanup %s, auto reset %s, output %s, foveated %s", PresetName(LivePreset()),
                  (st & PS_Clean) ? "on" : "off", in, ar, fmt, (st & PS_Fovea) ? "on" : "off");
}
static float ProbeFloat(const uint32_t* p, int k) { float v; memcpy(&v, &p[k], 4); return v; }

// RHI thread, each eye: the totals from kProbeLag frames ago, minus the previous read. The counters are running totals
// with a frame count, so a read covering several frames (one skipped while the GPU was behind) is divided by them.
static void ProbeRead(const NativeFrame& f) {
    if (!g_probeData || g_frame < kProbeSlots) return;
    const int eye = f.eye & 1;
    const uint64_t fr = g_frame - kProbeLag;
    const UINT slot = (UINT)(fr % kProbeSlots);
    const ProbeSlot& ps = g_probeSlot[slot][eye];
    if (ps.frame != fr) return;   // the probe did not run for this eye in that frame
    const uint32_t* co = (const uint32_t*)(g_probeData + (slot * kProbeBlocks + eye) * kProbeBlock);
    const uint32_t* ci = (const uint32_t*)(g_probeData + (slot * kProbeBlocks + 2 + eye) * kProbeBlock);
    struct Totals { double nan, inf, neg, huge, dark, alpha, px; };
    struct Acc { uint32_t prevO[9], prevI[9]; bool have; double frames, outFrames; Totals o, i; float expMin, expMax, preMin, preMax; double start;
                 float oMax, iMax, aUp, aDown; };   // brightest output / input colour, alpha above / below 1 (largest)
    static Acc acc[2] = {};
    static float lastExp[2] = {}, lastBlack[2] = {};
    static uint64_t lastFr[2] = {};
    static int seriousLines = 3000, brightLines = 200, summaryLines = 20000;
    static uint64_t lastEvent[2] = {}, lastAuto[2] = {};
    Acc& a = acc[eye];
    const uint32_t nI = a.have ? ci[8] - a.prevI[8] : 0, nO = a.have ? co[8] - a.prevO[8] : 0;
    if (a.have && ((int32_t)nI <= 0 || nI > 1000)) {   // nothing new yet (GPU behind), or a stale copy: wait
        if (nI > 1000) memcpy(a.prevI, ci, sizeof(a.prevI)), memcpy(a.prevO, co, sizeof(a.prevO));
        return;
    }
    const bool outOk = ps.hadOut && a.have && (int32_t)nO > 0 && nO <= 1000;
    uint32_t dO[6] = {}, dI[6] = {};
    if (a.have) for (int k = 0; k < 6; ++k) { dI[k] = ci[k] - a.prevI[k]; if (outOk) dO[k] = co[k] - a.prevO[k]; }
    const bool first = !a.have;
    memcpy(a.prevO, co, sizeof(a.prevO)); memcpy(a.prevI, ci, sizeof(a.prevI)); a.have = true;
    if (first) return;
    float expo; memcpy(&expo, &ci[6], 4);   // the input block holds the same texel and is written every frame
    // this frame's maxima (the last frame when the read covers several)
    const float oMax = outOk ? ProbeFloat(co, 9) : 0.f, iMax = ProbeFloat(ci, 9);
    const float aHi = outOk ? 1.f + ProbeFloat(co, 10) : 1.f, aLo = outOk ? 1.f - ProbeFloat(co, 11) : 1.f;
    const double px = ps.px * (outOk ? nO : 1), pxIn = ps.pxIn * nI;   // pixels these deltas cover
    const float black = outOk ? (float)(dO[4] / px) : (float)(dI[4] / pxIn);
    lastExp[eye] = expo; lastBlack[eye] = black; lastFr[eye] = fr;
    const float other = lastExp[eye ^ 1], otherBlack = lastBlack[eye ^ 1];
    // compare the eyes only on eye 1, against eye 0's values of the same frame (eye 0 is read first); eye 0 would see
    // eye 1's previous frame and a "difference" at every change, e.g. each loading screen
    const bool sameFrame = eye == 1 && lastFr[0] == fr;
    const bool expOdd = !(expo == expo) || (sameFrame && expo > 0.f && other > 0.f && (expo > other * 1.5f || other > expo * 1.5f));
    const bool blackOdd = sameFrame && ((black > 0.9f && otherBlack < 0.5f) || (otherBlack > 0.9f && black < 0.5f));   // one eye black (a loading screen blacks out both)
    const bool serious = dO[0] || dO[1] || dI[0] || dI[1] || blackOdd || expOdd;
    const bool bright = dO[3] > px / 1000.0;   // the sun or a lamp can do this every frame: its own small budget
    int& budget = serious ? seriousLines : brightLines;
    if ((serious || bright) && budget > 0 && g_frame >= lastEvent[eye] + 10) {
        --budget; lastEvent[eye] = g_frame;
        char st[192]; ProbeState(st, sizeof(st), ps.state);
        Log("probe eye=%d EVENT%s: output NaN %u px, infinite %u, negative %u, brighter than 4096 %u, brightest %.4g, black %.1f%% (other eye %.1f%%), "
            "alpha not 1 %.1f%% (alpha %.4g..%.4g)%s | game's colour in: NaN %u, infinite %u, brighter than 4096 %u, brightest %.4g, black %.1f%% | "
            "exposure %.5g (other eye %.5g), pre-exposure %.5g | %s%s",
            eye, serious ? "" : " (bright)", dO[0], dO[1], dO[2], dO[3], oMax, 100.0 * black, 100.0 * otherBlack, outOk ? 100.0 * dO[5] / px : 0.0,
            aLo, aHi, outOk ? "" : " (output not read)", dI[0], dI[1], dI[3], iMax, 100.0 * dI[4] / pxIn, expo, sameFrame ? other : -1.f, ps.pre, st,
            nI > 1 ? " (covers several frames)" : "");
        if (budget == 0) Log("probe: no more %s events will be logged (budget used up)", serious ? "" : "bright-pixel");
    }
    // [Test] AutoResetKey: an output with NaN / infinite pixels, or far more very bright pixels than the input (a white
    // eye: ~40 % of the window above 4096 from a normal input), resets this eye's history
    if (g_liveAutoReset.load(std::memory_order_relaxed) && outOk && g_frame >= lastAuto[eye] + 45) {
        const double hugeOut = dO[3] / px, hugeIn = dI[3] / pxIn;
        if (dO[0] || dO[1] || hugeOut > 0.002 + 4.0 * hugeIn) {
            lastAuto[eye] = g_frame;
            g_resetPending.fetch_or(1 << eye);
            static int logs = 500;
            if (logs-- > 0) Log("auto reset eye=%d: output NaN %u, infinite %u, brighter than 4096 %.2f%% (input %.2f%%)", eye, dO[0], dO[1],
                                100.0 * hugeOut, 100.0 * hugeIn);
        }
    }
    if (a.frames == 0) { a.start = (GetTickCount64() / 1000.0); a.expMin = a.expMax = expo; a.preMin = a.preMax = ps.pre;
                         a.oMax = a.iMax = 0.f; a.aUp = a.aDown = 1.f; }
    a.oMax = std::max(a.oMax, oMax); a.iMax = std::max(a.iMax, iMax); a.aUp = std::max(a.aUp, aHi); a.aDown = std::min(a.aDown, aLo);
    a.frames += nI;
    if (outOk) { a.outFrames += nO; a.o.px += px; a.o.nan += dO[0]; a.o.inf += dO[1]; a.o.neg += dO[2]; a.o.huge += dO[3]; a.o.dark += dO[4]; a.o.alpha += dO[5]; }
    a.i.px += pxIn; a.i.nan += dI[0]; a.i.inf += dI[1]; a.i.neg += dI[2]; a.i.huge += dI[3]; a.i.dark += dI[4];
    a.expMin = std::min(a.expMin, expo); a.expMax = std::max(a.expMax, expo);
    a.preMin = std::min(a.preMin, ps.pre); a.preMax = std::max(a.preMax, ps.pre);
    if ((GetTickCount64() / 1000.0) - a.start >= 2.0) {
        if (summaryLines-- > 0) {
            char st[192]; ProbeState(st, sizeof(st), ps.state);
            const double of = std::max(1.0, a.outFrames), opx = std::max(1.0, a.o.px), ipx = std::max(1.0, a.i.px);
            Log("probe eye=%d 2 s: %.0f frames | output per frame NaN %.1f px, infinite %.1f, negative %.1f, brighter than 4096 %.1f, "
                "brightest %.4g, black %.2f%%, alpha not 1 %.1f%% (alpha %.4g..%.4g)%s | game's colour in: NaN %.1f, infinite %.1f, "
                "brighter than 4096 %.1f, brightest %.4g, black %.2f%% | exposure %.5g..%.5g, pre-exposure %.5g..%.5g | %s", eye, a.frames,
                a.o.nan / of, a.o.inf / of, a.o.neg / of, a.o.huge / of, a.oMax, 100.0 * a.o.dark / opx, 100.0 * a.o.alpha / opx, a.aDown, a.aUp,
                a.outFrames < a.frames ? " (output not read in some frames)" : "", a.i.nan / a.frames, a.i.inf / a.frames,
                a.i.huge / a.frames, a.iMax, 100.0 * a.i.dark / ipx, a.expMin, a.expMax, a.preMin, a.preMax, st);
        }
        a.frames = a.outFrames = 0; a.o = Totals{}; a.i = Totals{};
    }
}

// one probe dispatch: block 'block', reading the output (UAV) or the game's colour (SRV at its eye's offset)
static void ProbeDispatch(ID3D12GraphicsCommandList* cl, D3D12_GPU_DESCRIPTOR_HANDLE table, uint32_t w, uint32_t h, uint32_t mode,
                          uint32_t block, uint32_t ox, uint32_t oy) {
    struct { uint32_t w, h, mode, base, ox, oy, pad0, pad1; } pc = { w, h, mode, block * kProbeBlock, ox, oy, 0u, 0u };
    cl->SetComputeRoot32BitConstants(0, 8, &pc, 0);
    cl->SetComputeRootDescriptorTable(1, table);
    cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER ub = {}; ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub.UAV.pResource = g_probeBuf;
    cl->ResourceBarrier(1, &ub);
}

// RHI thread, after the eye's output is final (UAV state; the game's colour is a compute SRV) and after the last
// timestamp (so the probe is not counted as upscaler cost): counts the game's colour going in and the output, and
// with cleanup on repairs the output. cpu23 / cpuIn: this pass's descriptors 23-26 and 27-30
static void ProbeRun(ID3D12GraphicsCommandList* cl, const NativeFrame& f, bool fsr, D3D12_CPU_DESCRIPTOR_HANDLE cpu23, D3D12_GPU_DESCRIPTOR_HANDLE gpu23,
                     D3D12_CPU_DESCRIPTOR_HANDLE cpuIn, D3D12_GPU_DESCRIPTOR_HANDLE gpuIn) {
    if (!g_psoProbe || !f.output || !f.color) return;
    const int eye = f.eye & 1;
    {   // which exposure texture each eye gets (one shared by both, or one per eye)
        static ID3D12Resource* last[2] = {}; static int logs = 40;
        if (f.exposure != last[eye] && logs > 0) {
            --logs; last[eye] = f.exposure;
            Log("probe: exposure texture eye=%d is %p (%s the other eye's %p)", eye, (void*)f.exposure,
                f.exposure == last[eye ^ 1] ? "same as" : "not", (void*)last[eye ^ 1]);
        }
    }
    auto at = [&](D3D12_CPU_DESCRIPTOR_HANDLE b, UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = b; h.ptr += (SIZE_T)i * g_descSize; return h; };
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    const DXGI_FORMAT ef = ExposureSrvFormat(f.exposure);
    D3D12_SHADER_RESOURCE_VIEW_DESC se = sv; se.Format = ef != DXGI_FORMAT_UNKNOWN ? ef : DXGI_FORMAT_R32G32B32A32_FLOAT;
    D3D12_SHADER_RESOURCE_VIEW_DESC sc = sv; sc.Format = ColorSrvFormat(f.color->GetDesc().Format);
    const DXGI_FORMAT of = OutUavFormat(f.output);
    const bool outReadable = of == DXGI_FORMAT_R11G11B10_FLOAT ? g_r11Load : g_rgba16Load;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; uv.Format = of;
    D3D12_UNORDERED_ACCESS_VIEW_DESC bv = {};
    bv.Format = DXGI_FORMAT_R32_TYPELESS; bv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    bv.Buffer.NumElements = kProbeBlocks * kProbeBlock / 4; bv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    // table 1 (cpu23): exposure, -, output, counters; table 2 (cpuIn): exposure, game's colour, -, counters
    for (D3D12_CPU_DESCRIPTOR_HANDLE b : { cpu23, cpuIn }) {
        g_dev->CreateShaderResourceView(ef != DXGI_FORMAT_UNKNOWN ? f.exposure : nullptr, &se, at(b, 0));
        g_dev->CreateUnorderedAccessView(g_probeBuf, nullptr, &bv, at(b, 3));
    }
    g_dev->CreateShaderResourceView(nullptr, &sc, at(cpu23, 1));
    g_dev->CreateUnorderedAccessView(outReadable ? f.output : nullptr, nullptr, &uv, at(cpu23, 2));
    g_dev->CreateShaderResourceView(f.color, &sc, at(cpuIn, 1));
    { D3D12_UNORDERED_ACCESS_VIEW_DESC nv = uv; nv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; g_dev->CreateUnorderedAccessView(nullptr, nullptr, &nv, at(cpuIn, 2)); }
    if (!outReadable) {
        static bool warned = false;
        if (!warned) { warned = true; Log("probe: this GPU cannot read the output's format (%d) in a shader: the output is not checked (nor cleaned)", (int)of); }
    }
    const bool clean = g_liveClean.load();
    D3D12_RESOURCE_BARRIER ub = {}; ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub.UAV.pResource = f.output;
    cl->ResourceBarrier(1, &ub);   // the upscaler / compose / sharpen wrote it
    ID3D12DescriptorHeap* heaps[] = { g_heap };
    // this frame's maxima start from 0 in both blocks of the eye
    Barrier(cl, g_probeBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyBufferRegion(g_probeBuf, (UINT64)eye * kProbeBlock + 36, g_probeZero, 0, 12);
    cl->CopyBufferRegion(g_probeBuf, (UINT64)(2 + eye) * kProbeBlock + 36, g_probeZero, 0, 12);
    Barrier(cl, g_probeBuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rsProbe);
    cl->SetPipelineState(g_psoProbe);
    ProbeDispatch(cl, gpuIn, (uint32_t)f.renderW, (uint32_t)f.renderH, 2u, 2u + eye, (uint32_t)f.colorRect.x0, (uint32_t)f.colorRect.y0);
    if (outReadable) {
        ProbeDispatch(cl, gpu23, (uint32_t)f.outW, (uint32_t)f.outH, clean ? 1u : 0u, (uint32_t)eye, 0u, 0u);
        cl->ResourceBarrier(1, &ub);   // the cleanup's writes, before the engine reads the output
    }
    Barrier(cl, g_probeBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const UINT slot = (UINT)(g_frame % kProbeSlots);
    cl->CopyBufferRegion(g_probeRb, (UINT64)(slot * kProbeBlocks + eye) * kProbeBlock, g_probeBuf, (UINT64)eye * kProbeBlock, kProbeBlock);
    cl->CopyBufferRegion(g_probeRb, (UINT64)(slot * kProbeBlocks + 2 + eye) * kProbeBlock, g_probeBuf, (UINT64)(2 + eye) * kProbeBlock, kProbeBlock);
    Barrier(cl, g_probeBuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ProbeSlot& ps = g_probeSlot[slot][eye];
    ps.frame = g_frame; ps.pre = f.preExposure; ps.hadOut = outReadable;
    ps.px = (double)f.outW * f.outH; ps.pxIn = (double)f.renderW * f.renderH;
    ps.state = (uint32_t)(FsrExposureMode() & 3) | (g_liveMask.load() ? PS_Mask : 0) | (clean && outReadable ? PS_Clean : 0) |
               (of == DXGI_FORMAT_R11G11B10_FLOAT ? PS_GameFmt : 0) | (f.fovea ? PS_Fovea : 0) | (fsr ? PS_Fsr : 0) |
               (g_liveInClean.load() ? PS_InClean : 0) | (g_liveAutoReset.load() ? PS_AutoReset : 0);
}

static ID3D12Resource* NewUavTexture(int w, int h, DXGI_FORMAT fmt, const char* what) {
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = (UINT64)w; rd.Height = (UINT)h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = fmt; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&r)))) {
        Log("upscaler: %s texture %dx%d failed", what, w, h); return nullptr;
    }
    return r;
}

// Per-eye textures are sized for the render; under dynamic resolution for its maximum, with only the top-left
// render-sized part used, so size changes do not recreate them.
static bool SizeOk(const NativeFrame& f, int w, int h) {
    return w >= f.renderW && h >= f.renderH && (f.drs || (w == f.renderW && h == f.renderH));
}
static int AllocW(const NativeFrame& f) { return f.drs ? f.maxRenderW : f.renderW; }
static int AllocH(const NativeFrame& f) { return f.drs ? f.maxRenderH : f.renderH; }

static bool EnsureMv(EyeState& e, const NativeFrame& f) {
    if (e.mv && SizeOk(f, e.mvW, e.mvH)) return true;
    Bury(nullptr, e.mv); e.mv = nullptr;
    e.mv = NewUavTexture(AllocW(f), AllocH(f), DXGI_FORMAT_R16G16_FLOAT, "motion vector");
    if (!e.mv) return false;
    e.mvW = AllocW(f); e.mvH = AllocH(f);
    return true;
}

// colour copy format: the game's scene colour format when it is R11G11B10_FLOAT (exact, half the bytes of RGBA16F;
// colorFmt=26 in this game) and the GPU can store it from a shader, else RGBA16F
static DXGI_FORMAT CopyFormat(const NativeFrame& f) {
    return (f.color && g_r11Uav && f.color->GetDesc().Format == DXGI_FORMAT_R11G11B10_FLOAT) ? DXGI_FORMAT_R11G11B10_FLOAT
                                                                                         : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

// this eye's colour and depth, copied by the motion-vector pass into textures that start at (0,0)
static bool EnsureCopies(EyeState& e, const NativeFrame& f) {
    const DXGI_FORMAT fmt = CopyFormat(f);
    if (e.colorCopy && SizeOk(f, e.copyW, e.copyH) && e.copyFmt == fmt) return true;
    e.copyLastUse = g_frame;   // the idle release counts from creation
    Bury(nullptr, e.colorCopy, e.depthCopy); e.colorCopy = e.depthCopy = nullptr;
    e.colorCopy = NewUavTexture(AllocW(f), AllocH(f), fmt, "colour copy");
    e.copyFmt = fmt;
    e.depthCopy = NewUavTexture(AllocW(f), AllocH(f), DXGI_FORMAT_R32_FLOAT, "depth copy");
    if (!e.colorCopy || !e.depthCopy) return false;
    e.copyW = AllocW(f); e.copyH = AllocH(f);
    return true;
}

// anti-ghosting mask (1 = trust the current frame), written by the motion-vector pass
static bool EnsureMask(EyeState& e, const NativeFrame& f) {
    if (e.mask && SizeOk(f, e.maskW, e.maskH)) return true;
    Bury(nullptr, e.mask); e.mask = nullptr;
    e.mask = NewUavTexture(AllocW(f), AllocH(f), DXGI_FORMAT_R8_UNORM, "mask");
    if (!e.mask) return false;
    e.maskW = AllocW(f); e.maskH = AllocH(f);
    return true;
}

// DLSS feature creation settings (shared by the render path and the side thread)
static void FeatCreateParams(NVSDK_NGX_Parameter* params, NVSDK_NGX_DLSS_Create_Params& cp, int preset, Quality quality,
                             int rw, int rh, int ow, int oh, const char* tag) {
    const char* keys[] = { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality };
    for (const char* k : keys) NVSDK_NGX_Parameter_SetUI(params, k, (unsigned)preset);
    cp = {};
    cp.Feature.InWidth = (unsigned)rw; cp.Feature.InHeight = (unsigned)rh;
    cp.Feature.InTargetWidth = (unsigned)ow; cp.Feature.InTargetHeight = (unsigned)oh;
    cp.Feature.InPerfQualityValue = (NVSDK_NGX_PerfQuality_Value)QualityToNgxPerfQuality(quality);
    // the centre window may write into a sub-rect of the eye's output ([Foveated] CentreDirect): NGX only accepts a
    // non-zero InOutputSubrectBase from a feature created with output sub-rects enabled (else every evaluate fails
    // with 0xBAD00005, invalid parameter)
    cp.InEnableOutputSubrects = strcmp(tag, "centre") == 0;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                              (g_cfg.autoExposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
}

static double QpcMs() {
    static double toMs = 0.0;
    if (toMs == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); toMs = 1000.0 / (double)f.QuadPart; }
    LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart * toMs;
}

// (re)creates an NGX feature for this model, quality and input/output size; the create's CPU time goes to the CSV
// (create_ms). Moving the create to another thread alone does not take it off the render path: NGX holds every
// evaluate on any thread until a create finishes (tests/asynctest.cpp: evaluate 0.1 -> 16-26 ms CPU during a
// 16-27 ms create). Key 0 below keeps the render path out of NGX meanwhile.
static bool EnsureFeat(ID3D12GraphicsCommandList* cl, NgxFeat& ft, int eye, int preset, Quality quality,
                       int rw, int rh, int ow, int oh, const char* tag) {
    if (ft.handle && ft.rw == rw && ft.rh == rh && ft.ow == ow && ft.oh == oh && ft.preset == preset && ft.quality == quality) return true;
    if (ft.fails && g_frame - ft.failFrame < 180) return false;   // failed recently: do not hitch every frame
    Bury(ft.handle, nullptr); ft.handle = nullptr;
    NVSDK_NGX_DLSS_Create_Params cp;
    FeatCreateParams(ft.params, cp, preset, quality, rw, rh, ow, oh, tag);
    const double t0 = QpcMs();
    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(cl, 1, 1, &ft.handle, ft.params, &cp);
    const double createMs = QpcMs() - t0;
    HitchAddCreate(createMs);
    if (ft.fails < 3 || (ft.fails % 50) == 0 || NVSDK_NGX_SUCCEED(r))
        Log("DLSS: create %s feature eye=%d preset=%s quality=%s render=%dx%d out=%dx%d -> 0x%08X in %.0f ms%s", tag, eye, PresetName(preset),
            QualityName(quality), rw, rh, ow, oh, (unsigned)r, createMs, NVSDK_NGX_FAILED(r) ? " (failed; retrying every 2 s)" : "");
    if (NVSDK_NGX_FAILED(r) || !ft.handle) { ft.handle = nullptr; ++ft.fails; ft.failFrame = g_frame; return false; }
    ft.fails = 0;
    ft.rw = rw; ft.rh = rh; ft.ow = ow; ft.oh = oh; ft.preset = preset; ft.quality = quality; ft.lastFrame = 0;
    return true;
}

// ---------------------------------------------------------------- key 0: DLSS centre models built on a side thread
// ([DLSS] AsyncRebuild). A size step (or the first model) is built on a thread of its own, both eyes in one go, started
// right after a frame's second eye pass. Until both are ready, an eye pass keeps the old model if this frame's window
// fits it; while the side thread is inside NGX, or with no model that fits, it draws the stand-in (plain edges over the
// whole eye, centre window included) instead of waiting. The new models are handed over together and start their
// history once. Offline (tests/createtest.cpp, 90 Hz, three size steps, both eyes): no missed refreshes against ~24
// with the creates on the render path, about one stand-in frame per model. Only for a DLSS centre with plain edges:
// other setups have no cheap stand-in and build on the render path (waiting for any side build to end).
struct SideBuild {
    HANDLE thread = nullptr, go = nullptr, gap = nullptr;   // gap: set at the end of each frame's second eye pass
    ID3D12CommandQueue* q = nullptr; ID3D12CommandAllocator* a = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Fence* fence = nullptr; UINT64 fv = 0; HANDLE ev = nullptr; NVSDK_NGX_Parameter* p = nullptr;
    std::atomic<int> state{ 0 };     // 0 idle, 1 asked / building, 2 built (waiting for an eye pass to take them)
    std::atomic<bool> cancel{ false };
    bool broken = false;             // queue / list / thread could not be made: build on the render path
    // the job (written before state 1)
    int preset = 0; Quality quality = Quality::Quality; int rw = 0, rh = 0, ow = 0, oh = 0; bool eyes[2] = {};
    uint64_t askFrame = 0; double askMs = 0;
    // the result (written before state 2)
    NVSDK_NGX_Handle* h[2] = {}; NVSDK_NGX_Result r[2] = {}; double ms[2] = {}, startMs = 0;
    int standIns = 0;                // eye passes with the stand-in since the request (RHI thread)
};
static SideBuild g_sb;

static DWORD WINAPI SideMain(void*) {
    SideBuild& s = g_sb;
    for (;;) {
        WaitForSingleObject(s.go, INFINITE);
        // start in the gap after a frame's second eye: the next evaluate is about a frame away (or after 50 ms)
        ResetEvent(s.gap);
        WaitForSingleObject(s.gap, 50);
        while (true) { int z = 0; if (g_ngxOwner.compare_exchange_weak(z, 2)) break; Sleep(0); }
        s.startMs = QpcMs();
        if (!s.p && NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&s.p))) s.p = nullptr;
        for (int eye = 0; eye < 2; ++eye) {
            s.h[eye] = nullptr; s.r[eye] = NVSDK_NGX_Result_Fail; s.ms[eye] = 0;
            if (!s.eyes[eye] || s.cancel.load() || !s.p) continue;
            s.a->Reset(); s.cl->Reset(s.a, nullptr);
            NVSDK_NGX_DLSS_Create_Params cp;
            FeatCreateParams(s.p, cp, s.preset, s.quality, s.rw, s.rh, s.ow, s.oh, "centre");
            const double t0 = QpcMs();
            s.r[eye] = NGX_D3D12_CREATE_DLSS_EXT(s.cl, 1, 1, &s.h[eye], s.p, &cp);
            s.ms[eye] = QpcMs() - t0;
            if (NVSDK_NGX_FAILED(s.r[eye])) s.h[eye] = nullptr;
            // the model's setup commands run on our queue; wait for them before the allocator is reused or an eye pass uses the model
            s.cl->Close(); ID3D12CommandList* l[] = { s.cl }; s.q->ExecuteCommandLists(1, l);
            s.q->Signal(s.fence, ++s.fv); s.fence->SetEventOnCompletion(s.fv, s.ev); WaitForSingleObject(s.ev, 5000);
        }
        HitchAddSideCreate(s.ms[0] + s.ms[1]);
        s.state = 2;      // before NGX is free: the next eye pass that gets it takes the new models, not the old ones
        g_ngxOwner = 0;
    }
}

static bool SideInit() {
    SideBuild& s = g_sb;
    if (s.thread || s.broken) return !s.broken;
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    s.go = CreateEventW(nullptr, FALSE, FALSE, nullptr); s.gap = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool ok = s.go && s.gap && s.ev && SUCCEEDED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&s.q))) &&
                    SUCCEEDED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.a))) &&
                    SUCCEEDED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.a, nullptr, IID_PPV_ARGS(&s.cl))) &&
                    SUCCEEDED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)));
    if (ok) { s.cl->Close(); s.thread = CreateThread(nullptr, 0, SideMain, nullptr, 0, nullptr); }
    if (!ok || !s.thread) { s.broken = true; Log("DLSS: the side thread for model builds could not be made: building on the render path"); return false; }
    Log("DLSS: side thread for centre model builds started (key 0)");
    return true;
}

// an eye pass that holds NGX takes the built models (both eyes at once)
static void SideCollect() {
    SideBuild& s = g_sb;
    if (s.state.load() != 2 || !g_ngxHeld) return;
    const bool cancelled = s.cancel.load();
    for (int eye = 0; eye < 2; ++eye) {
        if (!s.eyes[eye]) continue;
        NgxFeat& ft = g_eye[eye].center;
        if (s.h[eye]) {
            Bury(ft.handle, nullptr); ft.handle = s.h[eye];
            ft.rw = s.rw; ft.rh = s.rh; ft.ow = s.ow; ft.oh = s.oh; ft.preset = s.preset; ft.quality = s.quality;
            ft.lastFrame = 0; ft.fails = 0;
        } else if (!cancelled) { ++ft.fails; ft.failFrame = g_frame; }
        if (!cancelled)
            Log("DLSS: create centre feature eye=%d preset=%s quality=%s render=%dx%d out=%dx%d on the side thread -> 0x%08X in %.0f ms "
                "(started %.0f ms after the request; %d eye passes drew the stand-in, %llu frames in all)", eye, PresetName(s.preset), QualityName(s.quality),
                s.rw, s.rh, s.ow, s.oh, (unsigned)s.r[eye], s.ms[eye], s.startMs - s.askMs, s.standIns, (unsigned long long)(g_frame - s.askFrame));
    }
    s.h[0] = s.h[1] = nullptr; s.standIns = 0;
    s.state = 0;
}

// the centre model through the side thread: 1 = this frame's centre runs DLSS on ft, 2 = the stand-in, 0 = failed
// (rw, rh: the model's render size = the window at the largest render; frw, frh: this frame's window)
static int CentreSide(NgxFeat& ft, int eye, int preset, Quality quality, int rw, int rh, int ow, int oh, int frw, int frh) {
    const auto same = [&](const NgxFeat& m) { return m.handle && m.rw == rw && m.rh == rh && m.ow == ow && m.oh == oh && m.preset == preset && m.quality == quality; };
    // the old model carries on while the new one is built, when this frame's window fits it
    const bool oldFits = ft.handle && ft.ow == ow && ft.oh == oh && ft.preset == preset && ft.quality == quality && frw <= ft.rw && frh <= ft.rh;
    if (!g_ngxHeld) return 2;   // the side thread is inside NGX
    if (same(ft)) return 1;
    SideBuild& s = g_sb;
    if (s.state.load() == 0) {
        if (ft.fails && g_frame - ft.failFrame < 180) return oldFits ? 1 : 0;   // failed recently: retry every ~2 s
        if (!SideInit()) return oldFits ? 1 : 0;
        s.preset = preset; s.quality = quality; s.rw = rw; s.rh = rh; s.ow = ow; s.oh = oh;
        for (int k = 0; k < 2; ++k) s.eyes[k] = k == eye || !same(g_eye[k].center);   // both eyes' windows have the same size
        s.askFrame = g_frame; s.askMs = QpcMs(); s.standIns = 0; s.cancel = false;
        s.state = 1; SetEvent(s.go);
    }
    return oldFits ? 1 : 2;
}

static bool EnsurePeriphTmp(EyeState& e, int w, int h) {
    if (e.periphTmp && e.ptW == w && e.ptH == h) return true;
    Bury(nullptr, e.periphTmp); e.periphTmp = nullptr;
    e.periphTmp = NewUavTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, "half-size edges");
    if (!e.periphTmp) return false;
    e.ptW = w; e.ptH = h;
    return true;
}

// plain-edge AA result (+ the temporal history), sized like the colour copy
static bool EnsureAa(EyeState& e, const NativeFrame& f, bool temporal) {
    if (e.aa[0] && SizeOk(f, e.aaW, e.aaH) && (!temporal || e.aa[1])) return true;
    if (e.aa[0] && SizeOk(f, e.aaW, e.aaH)) {   // spatial -> temporal: only the second texture is missing
        e.aa[1] = NewUavTexture(e.aaW, e.aaH, g_aaFmt, "plain-edge AA history");
        e.aaFrame = 0; e.aaLastTemporal = g_frame;
        return e.aa[1] != nullptr;
    }
    Bury(nullptr, e.aa[0], e.aa[1]); e.aa[0] = e.aa[1] = nullptr; e.aaW = e.aaH = 0; e.aaFrame = 0; e.aaCur = 0;
    e.aaLastUse = e.aaLastTemporal = g_frame;   // the idle release counts from creation
    e.aaFill = 2;                               // new textures hold garbage: write every pixel twice
    e.aa[0] = NewUavTexture(AllocW(f), AllocH(f), g_aaFmt, "plain-edge AA");
    if (temporal && e.aa[0]) e.aa[1] = NewUavTexture(AllocW(f), AllocH(f), g_aaFmt, "plain-edge AA history");
    if (!e.aa[0] || (temporal && !e.aa[1])) { Bury(nullptr, e.aa[0], e.aa[1]); e.aa[0] = e.aa[1] = nullptr; return false; }
    e.aaW = AllocW(f); e.aaH = AllocH(f);
    return true;
}

static bool EnsureSharpTmp(EyeState& e, int w, int h) {
    if (e.sharpTmp && e.stW == w && e.stH == h) return true;
    Bury(nullptr, e.sharpTmp); e.sharpTmp = nullptr;
    e.sharpTmp = NewUavTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, "sharpen input");
    if (!e.sharpTmp) return false;
    e.stW = w; e.stH = h;
    return true;
}

static bool EnsureWindowInputs(EyeState& e, int w, int h, DXGI_FORMAT fmt) {
    if (e.winColor && e.winW >= w && e.winH >= h && e.winFmt == fmt) return true;
    Bury(nullptr, e.winColor, e.winDepth); Bury(nullptr, e.winMv); e.winColor = e.winDepth = e.winMv = nullptr;
    e.winColor = NewUavTexture(w, h, fmt, "fovea window colour");
    e.winFmt = fmt;
    e.winDepth = NewUavTexture(w, h, DXGI_FORMAT_R32_FLOAT, "fovea window depth");
    e.winMv = NewUavTexture(w, h, DXGI_FORMAT_R16G16_FLOAT, "fovea window motion");
    if (!e.winColor || !e.winDepth || !e.winMv) return false;
    e.winW = w; e.winH = h;
    return true;
}

static bool EnsureCenterOut(EyeState& e, int w, int h) {
    if (e.centerOut && e.coW == w && e.coH == h) return true;
    Bury(nullptr, e.centerOut); e.centerOut = nullptr;
    e.centerOut = NewUavTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, "fovea centre");
    if (!e.centerOut) return false;
    e.coW = w; e.coH = h;
    return true;
}

static bool EnsureNrTextures(EyeState& e, int w, int h) {
    if (e.nrIn && e.nrOut && e.nrFinal && e.nrW >= w && e.nrH >= h) return true;
    Bury(nullptr, e.nrIn, e.nrOut); Bury(nullptr, e.nrFinal); e.nrIn = e.nrOut = e.nrFinal = nullptr;
    e.nrIn    = NewUavTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, "DLSS 5 encoded input");
    e.nrOut   = NewUavTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, "DLSS 5 output");
    e.nrFinal = NewUavTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, "DLSS 5 decoded HDR");
    if (!e.nrIn || !e.nrOut || !e.nrFinal) {
        Bury(nullptr, e.nrIn, e.nrOut); Bury(nullptr, e.nrFinal);
        e.nrIn = e.nrOut = e.nrFinal = nullptr; e.nrW = e.nrH = 0;
        return false;
    }
    e.nrW = w; e.nrH = h;
    return true;
}

static void DropNr(EyeState& e, int eye) {
    if (e.nrIn || e.nrOut || e.nrFinal) {
        Bury(nullptr, e.nrIn, e.nrOut); Bury(nullptr, e.nrFinal);
        e.nrIn = e.nrOut = e.nrFinal = nullptr; e.nrW = e.nrH = 0;
    }
    if (e.nrLastFrame) {
        if (g_bridgeOk && g_bridge && g_ngxHeld) {
            g_bridge->ReleaseSlot((uint32_t)(eye & 1));
            g_bridge->ReleaseSlot((uint32_t)(2 + (eye & 1)));
        }
        e.nrLastFrame = 0; e.nrTwo = false;
    }
}

// Runs the DLSS 5 pass on the fovea window:
//   1. Encode linear HDR srcHdr (SRV state) -> tonemapped [0..1] e.nrIn
//   2. Evaluate Feature 18 through the bridge DLL (1 or 2 stacked passes:
//        1 pass:  e.nrIn -> e.nrOut (slot = eye)
//        2 passes: e.nrIn -> e.nrFinal (slot = eye), then e.nrFinal -> e.nrOut (slot = 2 + eye))
//   3. Decode multiplicative SDR ratio back onto srcHdr -> e.nrFinal (returned in NON_PIXEL_SHADER_RESOURCE state)
static ID3D12Resource* RunDlss5Window(ID3D12GraphicsCommandList* cl, EyeState& e, const NativeFrame& f,
                                      ID3D12Resource* srcHdr, int rw, int rh, int w, int h, Quality fq,
                                      D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    if (!g_bridgeOk || !g_bridge || !g_psoCodec || !srcHdr || !e.nrIn || !e.nrOut || !e.nrFinal || !e.winDepth || !e.winMv)
        return nullptr;
    auto slot = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE hdl = cpu; hdl.ptr += (SIZE_T)i * g_descSize; return hdl; };
    const DXGI_FORMAT srcFmt = ColorSrvFormat(srcHdr->GetDesc().Format);
    const DXGI_FORMAT expFmt = ExposureSrvFormat(f.exposure);
    const bool hasExp = !g_cfg.autoExposure && expFmt != DXGI_FORMAT_UNKNOWN;

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // table 1 (slots 31..34): encode srcHdr -> e.nrIn
    sv.Format = srcFmt;
    g_dev->CreateShaderResourceView(srcHdr, &sv, slot(31));
    sv.Format = hasExp ? expFmt : DXGI_FORMAT_R32G32B32A32_FLOAT;
    g_dev->CreateShaderResourceView(hasExp ? f.exposure : nullptr, &sv, slot(32));
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateShaderResourceView(nullptr, &sv, slot(33));
    g_dev->CreateUnorderedAccessView(e.nrIn, nullptr, &uv, slot(34));

    // table 2 (slots 35..38): decode srcHdr + e.nrIn + e.nrOut -> e.nrFinal
    sv.Format = srcFmt;
    g_dev->CreateShaderResourceView(srcHdr, &sv, slot(35));
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateShaderResourceView(e.nrIn, &sv, slot(36));
    g_dev->CreateShaderResourceView(e.nrOut, &sv, slot(37));
    g_dev->CreateUnorderedAccessView(e.nrFinal, nullptr, &uv, slot(38));

    ID3D12DescriptorHeap* heaps[] = { g_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rsCodec);
    cl->SetPipelineState(g_psoCodec);

    struct { uint32_t w, h, mode, hasExp; float preExp; uint32_t pad0, pad1, pad2; } cc =
        { (uint32_t)w, (uint32_t)h, 0u, hasExp ? 1u : 0u, f.preExposure, 0u, 0u, 0u };
    static_assert(sizeof(cc) == 8 * 4, "dlss5 codec constants");
    D3D12_GPU_DESCRIPTOR_HANDLE ge = gpu; ge.ptr += (UINT64)31 * g_descSize;
    cl->SetComputeRoot32BitConstants(0, 8, &cc, 0);
    cl->SetComputeRootDescriptorTable(1, ge);
    cl->Dispatch((UINT)(w + 7) / 8, (UINT)(h + 7) / 8, 1);

    Barrier(cl, e.nrIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const bool stale = e.nrLastFrame + 1 != g_frame;
    const bool twoPasses = g_liveDlss5Passes.load(std::memory_order_relaxed) >= 2;
    if (!twoPasses && e.nrTwo && g_ngxHeld) {   // back to 1 pass: free the second pass's feature (its history restarts next time)
        g_bridge->ReleaseSlot((uint32_t)(2 + (f.eye & 1)));
        e.nrTwo = false;
    }
    Dlss5EvalDesc ed = {};
    ed.cl            = cl;
    ed.slot          = (uint32_t)(f.eye & 1);
    ed.color         = e.nrIn;
    ed.depth         = e.winDepth;
    ed.motionVectors = e.winMv;
    ed.output        = twoPasses ? e.nrFinal : e.nrOut;
    ed.inputWidth    = (uint32_t)rw;
    ed.inputHeight   = (uint32_t)rh;
    ed.outputWidth   = (uint32_t)w;
    ed.outputHeight  = (uint32_t)h;
    ed.mvScaleX      = 1.f;
    ed.mvScaleY      = 1.f;
    ed.depthInverted = 1u;
    ed.reset         = (f.reset || stale) ? 1u : 0u;
    ed.tuning.intensity              = g_cfg.dlss5Intensity;
    ed.tuning.localToneStrength      = g_cfg.dlss5LocalTone;
    ed.tuning.localStructureStrength = g_cfg.dlss5LocalStructure;
    ed.tuning.skinStructureStrength  = g_cfg.dlss5SkinStructure;
    ed.tuning.style                  = (uint32_t)g_cfg.dlss5Style;
    ed.tuning.useAutoMask            = g_cfg.dlss5AutoMask ? 1u : 0u;
    ed.tuning.uiCorrection           = g_cfg.dlss5UiCorrection ? 1u : 0u;
    ed.tuning.performanceQuality     = (uint32_t)QualityToNgxPerfQuality(fq);
    ed.tuning.outputPreset           = (uint32_t)g_cfg.dlss5Preset;

    bool ok = g_bridge->Execute(&ed);
    if (ok && twoPasses) {
        Barrier(cl, e.nrFinal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ed.slot       = (uint32_t)(2 + (f.eye & 1));
        ed.color      = e.nrFinal;
        ed.output     = e.nrOut;
        ed.inputWidth = (uint32_t)w;
        ed.inputHeight = (uint32_t)h;
        ed.mvScaleX   = (float)w / (float)std::max(1, rw);
        ed.mvScaleY   = (float)h / (float)std::max(1, rh);
        ok = g_bridge->Execute(&ed);
        e.nrTwo = true;
        Barrier(cl, e.nrFinal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (!ok) {
        Barrier(cl, e.nrIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        static int failLogs = 16;
        if (failLogs-- > 0) Log("DLSS 5: bridge Execute failed eye=%d (%s)", f.eye, g_bridge->GetDetailString());
        return nullptr;
    }
    e.nrLastFrame = g_frame;

    Barrier(cl, e.nrOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // NGX bound its own heap and root signature inside Execute; rebind ours for the decode pass
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rsCodec);
    cl->SetPipelineState(g_psoCodec);
    cc.mode = 1u;
    D3D12_GPU_DESCRIPTOR_HANDLE gd = gpu; gd.ptr += (UINT64)35 * g_descSize;
    cl->SetComputeRoot32BitConstants(0, 8, &cc, 0);
    cl->SetComputeRootDescriptorTable(1, gd);
    cl->Dispatch((UINT)(w + 7) / 8, (UINT)(h + 7) / 8, 1);

    Barriers bg;
    bg.T(e.nrIn,    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bg.T(e.nrOut,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bg.T(e.nrFinal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bg.Flush(cl);
    return e.nrFinal;
}

// The fovea window: a rectangle around the eye's optical centre, in render pixels (r*) and output pixels (o*),
// clamped inside the eye. Sizes are multiples of 8 in the output; the render rect is the matching area.
struct FoveaRect { int rx, ry, rw, rh, ox, oy, ow, oh; };
// renderW/renderH: the render size to place it in (the actual one, or the maximum under dynamic resolution)
static FoveaRect ComputeFovea(const NativeFrame& f, int renderW, int renderH) {
    FoveaRect c;
    const double sx = (double)renderW / f.outW, sy = (double)renderH / f.outH;
    c.ow = ((int)(f.outW * f.foveaSize) + 7) & ~7; if (c.ow > f.outW) c.ow = f.outW;
    c.oh = ((int)(f.outH * f.foveaSize) + 7) & ~7; if (c.oh > f.outH) c.oh = f.outH;
    const double cx = (f.foveaNdcX * 0.5 + 0.5) * f.outW, cy = (0.5 - f.foveaNdcY * 0.5) * f.outH;
    // place the render rect first (whole render pixels), then derive the output rect from it
    c.rw = (int)(c.ow * sx + 0.5); c.rh = (int)(c.oh * sy + 0.5);
    if (c.rw > renderW) c.rw = renderW;
    if (c.rh > renderH) c.rh = renderH;
    c.rx = (int)((cx - c.ow * 0.5) * sx + 0.5); c.ry = (int)((cy - c.oh * 0.5) * sy + 0.5);
    if (c.rx < 0) c.rx = 0; if (c.rx > renderW - c.rw) c.rx = renderW - c.rw;
    if (c.ry < 0) c.ry = 0; if (c.ry > renderH - c.rh) c.ry = renderH - c.rh;
    c.ox = (int)(c.rx / sx + 0.5); c.oy = (int)(c.ry / sy + 0.5);
    if (c.ox > f.outW - c.ow) c.ox = f.outW - c.ow;
    if (c.oy > f.outH - c.oh) c.oy = f.outH - c.oh;
    return c;
}

// one DLSS evaluate over the render rect (rx, ry, rw, rh) of this eye, written to 'out' at (0,0)
// winInputs: the window's own cropped colour/depth/motion (all at (0,0)) instead of sub-rects of the eye's;
// noExposure: no exposure texture (only presets J and K read it, NVIDIA guide 310.6.0 section 3.9)
// outX/outY: where the result goes in 'out' (for the centre written straight into the eye's output)
static unsigned NgxEval(ID3D12GraphicsCommandList* cl, NgxFeat& ft, EyeState& e, const NativeFrame& f, bool copies, bool useMask,
                        int rx, int ry, int rw, int rh, ID3D12Resource* out, bool winInputs = false, bool noExposure = false,
                        int outX = 0, int outY = 0, ID3D12Resource* winColorOverride = nullptr) {
    const bool stale = ft.lastFrame + 1 != g_frame;   // first use, or not used last frame (e.g. after F9)
    ft.lastFrame = g_frame;
    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = copies ? e.colorCopy : f.color; ep.Feature.pInOutput = out;
    ep.pInDepth = copies ? e.depthCopy : f.depth; ep.pInMotionVectors = e.mv;
    ep.InJitterOffsetX = f.jitterX; ep.InJitterOffsetY = f.jitterY;
    ep.InRenderSubrectDimensions = { (unsigned)rw, (unsigned)rh };
    ep.InReset = (f.reset || stale) ? 1 : 0;
    ep.InMVScaleX = 1.f; ep.InMVScaleY = 1.f;
    ep.pInExposureTexture = g_cfg.autoExposure || noExposure ? nullptr : f.exposure;
    const unsigned cbx = copies ? 0u : (unsigned)f.colorRect.x0, cby = copies ? 0u : (unsigned)f.colorRect.y0;
    const unsigned dbx = copies ? 0u : (unsigned)f.depthRect.x0, dby = copies ? 0u : (unsigned)f.depthRect.y0;
    ep.InColorSubrectBase = { cbx + (unsigned)rx, cby + (unsigned)ry };
    ep.InDepthSubrectBase = { dbx + (unsigned)rx, dby + (unsigned)ry };
    ep.InMVSubrectBase = { (unsigned)rx, (unsigned)ry };
    if (winInputs) {
        ep.Feature.pInColor = winColorOverride ? winColorOverride : e.winColor;
        ep.pInDepth = e.winDepth; ep.pInMotionVectors = e.winMv;
        ep.InColorSubrectBase = { 0, 0 }; ep.InDepthSubrectBase = { 0, 0 }; ep.InMVSubrectBase = { 0, 0 };
    }
    ep.InOutputSubrectBase = { (unsigned)outX, (unsigned)outY };
    ep.pInBiasCurrentColorMask = useMask ? e.mask : nullptr;
    ep.InBiasCurrentColorSubrectBase = { (unsigned)rx, (unsigned)ry };
    ep.InPreExposure = f.preExposure;
    ep.InExposureScale = 1.f;
    if (ep.InReset && &ft == &e.center) {   // diagnostic: log when the centre window loses its history
        static int logs = 60;
        if (logs-- > 0) Log("DLSS: centre eye=%d history reset (%s)", f.eye, f.reset ? "engine reset / camera cut" : "first use or a skipped frame");
    }
    return (unsigned)NGX_D3D12_EVALUATE_DLSS_EXT(cl, ft.handle, ft.params, &ep);
}

// copy colour -> output (top-left aligned); computeStates = resources are in SRV/UAV states (mode 3 fallback)
static void DoCopy(ID3D12GraphicsCommandList* cl, const NativeFrame& f, bool computeStates) {
    if (!f.color || !f.output) return;
    const D3D12_RESOURCE_DESC cd = f.color->GetDesc(), od = f.output->GetDesc();
    if (cd.Format != od.Format) return;   // formats differ (mode 3 output is RGBA16F): nothing safe to copy
    if (computeStates) {
        Barrier(cl, f.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = f.output; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    src.pResource = f.color; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    const int w = f.renderW < f.outW ? f.renderW : f.outW, h = f.renderH < f.outH ? f.renderH : f.outH;
    D3D12_BOX box = { (UINT)f.colorRect.x0, (UINT)f.colorRect.y0, 0, (UINT)(f.colorRect.x0 + w), (UINT)(f.colorRect.y0 + h), 1 };
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    if (computeStates) {
        Barrier(cl, f.color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, f.output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}

void CopyExecute(ID3D12GraphicsCommandList* cl, const NativeFrame& f) { DoCopy(cl, f, f.mode == 3); }

// Game thread, once DLSS has been off for a while (the upscaler pass no longer runs, the GPU finished long ago):
// releases every DLSS model, FSR context and texture of the mod, so the game gets the VRAM back while it runs its
// own TAA. Everything is recreated when DLSS is switched back on (a short hitch). Returns true if anything was freed.
bool DlssReleaseAll() {
    if (!g_ready) return false;
    ExecGuard guard;
    int n = 0;
    if (g_sb.state.load() == 1) {   // a side-thread build: stop it after its current model
        g_sb.cancel = true;
        for (int i = 0; i < 400 && g_sb.state.load() == 1; ++i) Sleep(5);
    }
    NgxWait();
    struct Leave { ~Leave() { NgxLeave(); } } leave;
    if (g_sb.state.load() == 2) {
        for (auto*& h : g_sb.h) if (h) { NVSDK_NGX_D3D12_ReleaseFeature(h); h = nullptr; ++n; }
        g_sb.state = 0;
    }
    g_sb.cancel = false;
    for (auto& d : g_grave) {
        if (d.h) { NVSDK_NGX_D3D12_ReleaseFeature(d.h); ++n; }
        for (auto* r : d.r) if (r) { r->Release(); ++n; }
        d = {};
    }
    for (auto& e : g_eye) {
        NgxFeat* feats[4] = { &e.full, &e.periph, &e.periphHalf, &e.center };
        for (NgxFeat* ft : feats)
            if (ft->handle) { NVSDK_NGX_D3D12_ReleaseFeature(ft->handle); ft->handle = nullptr; ft->rw = ft->rh = ft->ow = ft->oh = 0; ++n; }
        ID3D12Resource** texs[] = { &e.centerOut, &e.sharpTmp, &e.periphTmp, &e.winColor, &e.winDepth, &e.winMv, &e.mv,
                                    &e.colorCopy, &e.depthCopy, &e.mask, &e.aa[0], &e.aa[1] };
        for (ID3D12Resource** t : texs) if (*t) { (*t)->Release(); *t = nullptr; ++n; }
        e.coW = e.coH = e.stW = e.stH = e.ptW = e.ptH = e.winW = e.winH = e.mvW = e.mvH = e.copyW = e.copyH = e.maskW = e.maskH = 0;
        e.aaW = e.aaH = 0; e.aaFrame = 0; e.aaCur = 0;
    }
    if (FsrReady()) n += FsrReleaseAll();
    return n > 0;
}

// Mode 3 fallback when the upscaler cannot run this frame (feature or context creation failed, e.g. out of VRAM;
// an evaluate failed; unknown depth format...): stretch this eye's render to the output with the compose pass and
// no centre window. A plain copy cannot do it: the output is RGBA16F and the scene colour R11G11B10, so a copy
// leaves the output unwritten (garbage in the headset). States on entry: colour is a compute SRV, the output a
// UAV (as the RDG pass declared them).
static void FallbackStretch(ID3D12GraphicsCommandList* cl, const NativeFrame& f) {
    if (!f.color || !f.output || !g_psoCompose || !g_heap || f.renderW <= 0 || f.renderH <= 0) return;
    const D3D12_RESOURCE_DESC cd = f.color->GetDesc();
    const UINT base = g_heapCursor; g_heapCursor = (g_heapCursor + kDescPerPass) % kHeapSlots;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += (SIZE_T)base * g_descSize;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += (UINT64)(base + 13) * g_descSize;
    auto slot = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += (SIZE_T)i * g_descSize; return h; };
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1; sv.Format = ColorSrvFormat(cd.Format);
    g_dev->CreateShaderResourceView(f.color, &sv, slot(13));
    g_dev->CreateShaderResourceView(f.color, &sv, slot(14));   // the centre is unused (window size 0) but must be valid
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {}; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; uv.Format = OutUavFormat(f.output);
    g_dev->CreateUnorderedAccessView(f.output, nullptr, &uv, slot(15));
    const float tw = (float)cd.Width, th = (float)cd.Height;
    // the eye's render is the sub-rect (x0, y0, renderW x renderH) of the shared texture; sample at -jitter
    // (clamping to the first / last texel centre keeps the other eye's pixels out of the bilinear sample)
    struct { uint32_t w, h; int32_t ox, oy; uint32_t ww, wh; float feather, pad; float sx, sy, offx, offy, maxx, maxy, minx, miny; } cc =
        { (uint32_t)f.outW, (uint32_t)f.outH, 0, 0, 0u, 0u, 0.3f, 0.f,
          f.renderW / tw, f.renderH / th, (f.colorRect.x0 - f.jitterX) / tw, (f.colorRect.y0 - f.jitterY) / th,
          (f.colorRect.x0 + f.renderW - 0.5f) / tw, (f.colorRect.y0 + f.renderH - 0.5f) / th,
          (f.colorRect.x0 + 0.5f) / tw, (f.colorRect.y0 + 0.5f) / th };
    static_assert(sizeof(cc) == 16 * 4, "compose constants");
    ID3D12DescriptorHeap* heaps[] = { g_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rsCompose);
    cl->SetPipelineState(g_psoCompose);
    cl->SetComputeRoot32BitConstants(0, 16, &cc, 0);
    cl->SetComputeRootDescriptorTable(1, gpu);
    cl->Dispatch((UINT)(f.outW + 7) / 8, (UINT)(f.outH + 7) / 8, 1);
}

// Consecutive failed eye passes: after ~1 s of them, ask the game thread (engine.cpp) to switch to FSR, or without
// FSR to turn the upscaler off (the game's own TAA), with a spoken message. Logged sparingly.
static int g_failPasses = 0;
static void Fail(ID3D12GraphicsCommandList* cl, const NativeFrame& f, const char* why, bool usingFsr) {
    FallbackStretch(cl, f);
    static int logs = 0;
    if (logs < 20 || (logs % 200) == 0) Log("upscaler: eye %d cannot run (%s): showing the plain render%s", f.eye, why, logs == 19 ? " (further lines limited)" : "");
    ++logs;
    if (++g_failPasses >= 180 && g_upscalerFallback.load() == 0) {   // ~1 s at 90 Hz, both eyes
        g_failPasses = 0;
        const int req = !usingFsr && FsrReady() ? 1 : 2;
        g_upscalerFallback = req;
        Log("upscaler: failing for about a second -> %s", req == 1 ? "switching to FSR" : "turning the upscaler off (the game's TAA)");
    }
}

void DlssExecute(ID3D12GraphicsCommandList* cl, const NativeFrame& f) {
    ExecGuard guard;
    // take NGX for this eye pass without waiting (key 0's side thread may be inside it); given back on every exit, and
    // after the second eye the side thread may start a build (the next evaluate is about a frame away)
    NgxTry();
    struct NgxPass { int eye; ~NgxPass() { NgxLeave(); if (eye == 1 && g_sb.gap) SetEvent(g_sb.gap); } } ngxPass{ f.eye };
    HitchNoteThread(2);
    g_barrierCalls = 0;
    struct BarrierTally { ~BarrierTally() { HitchAddBarriers(g_barrierCalls); } } tally;   // on every exit from this pass
    if (f.eye == 0 && (g_frame % 90) == 0) TouchBallast(cl);
    if (f.eye == 0) { ++g_frame; CollectGraveyard(); if (FsrReady()) FsrNewFrame(); LogStats(); }
    SideCollect();
    EyeState& e = g_eye[f.eye & 1];
    {   // release models unused for ~10 s (kept until then so switching back is instant; low-memory mode releases
        // them at once): otherwise switching F7 / F9 back and forth keeps ~0.85 GB of VRAM
        const uint64_t idle = (uint64_t)(TargetHz() * 10.f);
        NgxFeat* feats[4] = { &e.full, &e.periph, &e.periphHalf, &e.center };
        static const char* tags[4] = { "full", "periphery", "periphery-half", "centre" };
        for (int k = 0; k < 4; ++k)
            if (feats[k]->handle && feats[k]->lastFrame && g_frame > feats[k]->lastFrame + idle) {
                Log("DLSS: releasing the %s model of eye %d (unused for %.0f s)", tags[k], f.eye, (g_frame - feats[k]->lastFrame) / TargetHz());
                Bury(feats[k]->handle, nullptr); feats[k]->handle = nullptr; feats[k]->rw = feats[k]->rh = feats[k]->ow = feats[k]->oh = 0;
            }
        if (FsrReady()) FsrReleaseIdle(f.eye, idle);
        // the AA textures too (key 1 / key 2): the temporal history first, then the result texture (~75 MB each
        // at 2940x3209), so the CSV shows what each AA mode really holds
        if (e.aa[1] && g_frame > e.aaLastTemporal + idle) {
            Log("upscaler: releasing the temporal AA history of eye %d (unused for %.0f s)", f.eye, (g_frame - e.aaLastTemporal) / TargetHz());
            Bury(nullptr, e.aa[1]); e.aa[1] = nullptr; e.aaFrame = 0;
        }
        if (e.colorCopy && e.copyLastUse && g_frame > e.copyLastUse + idle) {
            Log("upscaler: releasing the colour / depth copies of eye %d (unused for %.0f s)", f.eye, (g_frame - e.copyLastUse) / TargetHz());
            Bury(nullptr, e.colorCopy, e.depthCopy); e.colorCopy = e.depthCopy = nullptr; e.copyW = e.copyH = 0; e.copyLastUse = 0;
        }
        if (e.aa[0] && g_frame > e.aaLastUse + idle) {
            Log("upscaler: releasing the AA texture of eye %d (unused for %.0f s)", f.eye, (g_frame - e.aaLastUse) / TargetHz());
            Bury(nullptr, e.aa[0]); e.aa[0] = nullptr; e.aaW = e.aaH = 0; e.aaCur = 0; e.aaFrame = 0;
        }
        if ((e.nrIn || e.nrLastFrame) && e.nrLastFrame && g_frame > e.nrLastFrame + idle) {
            Log("DLSS 5: releasing textures and feature of eye %d (unused for %.0f s)", f.eye, (g_frame - e.nrLastFrame) / TargetHz());
            DropNr(e, f.eye);
        }
    }
    const bool liveFsr = (f.preset == kPresetFsr || !g_ngxOk) && FsrReady();
    if (!liveFsr && !g_ngxOk) { Fail(cl, f, "no upscaler available", false); return; }
    if (!f.color || !f.output || !f.depth) { Fail(cl, f, "missing colour/depth/output", liveFsr); return; }
    // the copies read colour and depth at the same offset (true here: both rects start at the eye's x)
    const bool sameOrigin = f.colorRect.x0 == f.depthRect.x0 && f.colorRect.y0 == f.depthRect.y0;
    // key 2: the mod's own small TAA on the whole eye instead of DLSS / FSR (at render size, then stretched), to
    // compare with the game's TAA; F7 / F9 are ignored while it is on
    const bool fullTaa = f.fullTaa && sameOrigin && g_psoAa;
    // foveated: the periphery model (E, or FSR) on the whole eye, the live DLSS model on a window around the lens centre.
    // FSR as the F7 model can be foveated too: FSR on the window (cropped inputs), edges plain or FSR (a DLSS edge
    // model becomes plain edges: the point of the window is cheaper edges)
    const bool fsrCentre = f.fovea && liveFsr && sameOrigin && !fullTaa;
    int periph = f.periphPreset;
    if (fsrCentre && periph != kPresetFsr && periph != kPresetPlain) periph = kPresetPlain;
    const bool periphFsr = periph == kPresetFsr;
    const bool periphPlainWanted = periph == kPresetPlain;   // no upscaler on the edges: the render stretched
    const bool fovea = !fullTaa && (fsrCentre ||
        (f.fovea && g_ngxOk && !liveFsr && (!periphFsr || (FsrReady() && sameOrigin)) && (!periphPlainWanted || sameOrigin)));
    const bool plainEdges = fovea && periphPlainWanted;
    const bool useFsr = !fullTaa && (fovea ? periphFsr : liveFsr);   // FSR runs on the whole eye this frame
    // edge_aa.hlsl mode this frame: 1 spatial, 2 temporal, 3 temporal fast (key 2 uses the fast one when key 1 is on it)
    const int aaWanted = fullTaa ? (f.edgeAa == 3 ? 3 : 2) : plainEdges && g_psoAa ? f.edgeAa : 0;
    // key 0 ([DLSS] AsyncRebuild): a DLSS centre with plain edges gets its models from the side thread and a stand-in
    // meanwhile; every other setup calls NGX directly, so it waits for any build the side thread is doing
    const bool sideBuild = g_liveAsyncRebuild.load(std::memory_order_relaxed) && fovea && plainEdges && !fsrCentre && g_ngxOk && !g_sb.broken;
    if (!sideBuild) NgxWait();
    if (!sameOrigin) {
        static bool warned = false;
        if (!warned) { warned = true; Log("upscaler: colour and depth rects differ (%d,%d)/(%d,%d): no per-eye copies%s", f.colorRect.x0,
                                          f.colorRect.y0, f.depthRect.x0, f.depthRect.y0, useFsr ? ", FSR cannot run" : ""); }
        if (useFsr) { Fail(cl, f, "FSR needs matching colour and depth rects", true); return; }
    }
    const int winDiag = fovea && !fsrCentre ? g_liveWinDiag.load(std::memory_order_relaxed) : 0;   // [Debug] WindowDiagKey
    // [DLSS5]: runs on the foveated DLSS centre window when the bridge DLL is ready (1 = pre-upscale, 2 = post-upscale)
    const int dlss5 = (Dlss5Available() && fovea && !fsrCentre && sameOrigin) ? f.dlss5 : 0;
    // DLSS presets J and K pulse on a still image when the window is a sub-rect with a non-zero origin, so by default
    // the window gets its own crops of colour/depth/motion at (0,0). WindowCrop 2 (auto): crops only for J and K
    // (presets 10, 11)
    const int cropMode = g_liveWindowCrop.load(std::memory_order_relaxed);
    const bool cropWanted = cropMode == 1 || (cropMode == 2 && (f.preset == 10 || f.preset == 11));
    const bool winCrop = fovea && !fsrCentre && (cropWanted || (winDiag & 1) || dlss5 != 0) && sameOrigin;
    // lean inputs ([Foveated] LeanInputs): the motion-vector pass writes the window crops itself, and the plain edges /
    // the mod's TAA read the game's scene colour at the eye's offset, so whole-eye copies of colour (RGBA16F) and
    // depth are only made for what needs them at (0,0): FSR on the whole eye or edges, SeparateEyeInputs
    const bool lean = g_liveLean.load(std::memory_order_relaxed) && sameOrigin;
    const bool cropDirect = lean && (winCrop || fsrCentre);
    // [Foveated] CentreDirect: with a DLSS centre and edges made by the compose pass (plain / half size), DLSS writes the
    // window straight into the output's window rect: no centre texture, and the compose pass skips the pixels the
    // centre covers fully. Not with FSR (no output offset) or E edges (the blend needs the separate texture).
    // Post-upscale DLSS 5 (dlss5 == 2) also needs the separate centre texture, to process it before compose.
    static bool directOff = false;   // a direct centre evaluate failed: separate texture for the rest of the session
    const bool centreDirect = g_cfg.centreDirect && !directOff && fovea && !fsrCentre && dlss5 != 2;   // (half / plain edges: checked below)
    const bool copies = (useFsr || g_cfg.separateEyeInputs || (!lean && (plainEdges || fsrCentre || winCrop || fullTaa))) && sameOrigin;
    {
        static int lastLean[2] = { -1, -1 }, logs = 24;
        const int key = (lean ? 1 : 0) | (copies ? 2 : 0) | (cropDirect ? 4 : 0);
        if (key != lastLean[f.eye & 1] && logs > 0) {
            --logs; lastLean[f.eye & 1] = key;
            Log("upscaler eye=%d: lean inputs %s: whole-eye copies %s, window crops %s", f.eye, lean ? "ON" : "off", copies ? "yes" : "no",
                cropDirect ? "written by the motion pass" : (winCrop || fsrCentre) ? "copied from the copies" : "none");
        }
    }
    // the translucency texture shares SceneColor's layout (checked when the pass was added)
    const bool useMask = g_liveMask.load(std::memory_order_relaxed) && f.translucency && sameOrigin && g_cfg.maskScale > 0.f &&
                         EnsureMask(e, f);
    FsrEye* fe = useFsr ? FsrGetEye(f.eye) : nullptr;
    ProbeRead(f);
    const FoveaRect fr = fovea ? ComputeFovea(f, f.renderW, f.renderH) : FoveaRect{};
    // features are created for the largest render (dynamic resolution) and given the actual size per frame
    const int cw = f.drs ? f.maxRenderW : f.renderW, ch = f.drs ? f.maxRenderH : f.renderH;
    const FoveaRect frMax = fovea ? ComputeFovea(f, cw, ch) : FoveaRect{};
    // under dynamic resolution the features use DLSS's Performance setting: it takes any input from 50% of the
    // output up to the size the feature was made for (Ultra Performance takes only that exact size)
    const Quality fq = f.drs ? Quality::Performance : f.quality;
    bool ready = EnsureMv(e, f) && (!copies || EnsureCopies(e, f));
    if (ready && useFsr) ready = FsrEnsure(fe, f.outW, f.outH, f.eye, cw, ch);   // sized for the largest render, not the output
    // half-size edges ([Foveated] HalfResEdges): the edge model outputs about half the eye's size (never smaller than
    // its input, DLSS cannot downscale), then the compose pass stretches it and adds the centre window
    const int phw = std::min(f.outW, std::max((f.outW + 1) / 2, cw)), phh = std::min(f.outH, std::max((f.outH + 1) / 2, ch));
    const bool halfEdges = fovea && !periphFsr && !plainEdges && f.halfEdges && phw < f.outW && phh < f.outH;
    if (ready && !useFsr && halfEdges) ready = EnsureFeat(cl, e.periphHalf, f.eye, f.periphPreset, fq, cw, ch, phw, phh, "periphery-half") &&
                                    EnsurePeriphTmp(e, phw, phh);
    else if (ready && !useFsr && !plainEdges && !fullTaa) ready = fovea ? EnsureFeat(cl, e.periph, f.eye, f.periphPreset, fq, cw, ch, f.outW, f.outH, "periphery")
                                  : EnsureFeat(cl, e.full, f.eye, f.preset, fq, cw, ch, f.outW, f.outH, "full");
    FsrEye* fc = fsrCentre ? FsrGetCentre(f.eye) : nullptr;
    if (ready && fovea && fsrCentre) ready = FsrEnsure(fc, fr.ow, fr.oh, f.eye, frMax.rw, frMax.rh) && EnsureWindowInputs(e, frMax.rw, frMax.rh, CopyFormat(f)) &&
                                           EnsureCenterOut(e, fr.ow, fr.oh);
    int centreRun = 1;   // 1 DLSS centre, 2 stand-in (key 0: no model to run this frame)
    if (ready && fovea && !fsrCentre) {
        if (sideBuild) { centreRun = CentreSide(e.center, f.eye, f.preset, fq, frMax.rw, frMax.rh, fr.ow, fr.oh, fr.rw, fr.rh); ready = centreRun != 0; }
        else ready = EnsureFeat(cl, e.center, f.eye, f.preset, fq, frMax.rw, frMax.rh, fr.ow, fr.oh, "centre");
        ready = ready && ((centreDirect && (halfEdges || plainEdges)) || EnsureCenterOut(e, fr.ow, fr.oh)) && (!winCrop || EnsureWindowInputs(e, frMax.rw, frMax.rh, CopyFormat(f))) &&
                (dlss5 == 0 || EnsureNrTextures(e, dlss5 == 1 ? frMax.rw : fr.ow, dlss5 == 1 ? frMax.rh : fr.oh));
    }
    // stand-in: the plain edges (with their AA) over the whole eye, centre window included
    const bool standIn = centreRun == 2;
    if (standIn) { HitchAddStandIn(); ++g_sb.standIns; }
    const bool standInStart = standIn && !e.standIn;   // first frame: the edge AA history has no window pixels
    e.standIn = standIn;
    if (!ready) { Fail(cl, f, "model / context / texture creation failed", useFsr); return; }
    if (f.lowMem) {   // low-memory mode (key -): keep only what this frame uses
        if (useFsr || fovea || fullTaa) DropFeat(e.full, f.eye, "full");
        if (!fovea || useFsr || halfEdges || plainEdges) DropFeat(e.periph, f.eye, "periphery");
        if (!halfEdges) { DropFeat(e.periphHalf, f.eye, "periphery-half"); DropTex(e.periphTmp, e.ptW, e.ptH); }
        if (!fovea) { DropFeat(e.center, f.eye, "centre"); DropTex(e.centerOut, e.coW, e.coH); }
        if (centreDirect && (halfEdges || plainEdges)) DropTex(e.centerOut, e.coW, e.coH);
        if (fsrCentre) DropFeat(e.center, f.eye, "centre");
        if (!fsrCentre) {
            if (FsrReady()) FsrRelease(FsrGetCentre(f.eye));
            if (!winCrop && (e.winColor || e.winDepth || e.winMv)) { Bury(nullptr, e.winColor, e.winDepth); Bury(nullptr, e.winMv); e.winColor = e.winDepth = e.winMv = nullptr; e.winW = e.winH = 0; }
        }
        if (f.sharpen <= 0.f) DropTex(e.sharpTmp, e.stW, e.stH);
        if (!copies && (e.colorCopy || e.depthCopy)) { Bury(nullptr, e.colorCopy, e.depthCopy); e.colorCopy = e.depthCopy = nullptr; e.copyW = e.copyH = 0; }
        if (!useMask) DropTex(e.mask, e.maskW, e.maskH);
        if (aaWanted < 2) { if (e.aa[1]) { Bury(nullptr, e.aa[1]); e.aa[1] = nullptr; e.aaFrame = 0; } }
        if (aaWanted == 0) { int w = e.aaW, h = e.aaH; DropTex(e.aa[0], w, h); if (!e.aa[0]) { e.aaW = e.aaH = 0; e.aaCur = 0; } }
        if (dlss5 == 0) DropNr(e, f.eye);
        if (!useFsr && FsrReady()) FsrRelease(FsrGetEye(f.eye));
    }
    {   // log the window whenever foveation starts or its rect changes
        static int lastKey[2] = { -1, -1 }, logs = 24;
        const int key = fovea ? (fr.rx * 7919 + fr.ry * 31 + fr.rw * 3 + fr.rh + periph * 1000003 + (fsrCentre ? 7 : 0)) : -2;
        if (key != lastKey[f.eye & 1] && logs > 0) {
            --logs; lastKey[f.eye & 1] = key;
            if (fovea) Log("foveated eye=%d: periphery %s on %dx%d, centre %s on render (%d,%d) %dx%d -> out (%d,%d) %dx%d of %dx%d (lens centre ndc %.3f,%.3f)",
                           f.eye, PresetName(periph), f.renderW, f.renderH, PresetName(fsrCentre ? kPresetFsr : f.preset), fr.rx, fr.ry, fr.rw, fr.rh,
                           fr.ox, fr.oy, fr.ow, fr.oh, f.outW, f.outH, f.foveaNdcX, f.foveaNdcY);
            else Log("foveated eye=%d: off%s", f.eye, f.fovea ? " (needs DLSS with a DLSS model in F7)" : "");
        }
    }

    // ---- motion vectors (+ per-eye copies): our compute pass, our descriptor heap
    const D3D12_RESOURCE_DESC dd = f.depth->GetDesc();
    const DXGI_FORMAT depthFmt = DepthSrvFormat(dd.Format);
    DXGI_FORMAT velFmt = DXGI_FORMAT_UNKNOWN;
    if (f.velocity) {
        velFmt = VelocitySrvFormat(f.velocity->GetDesc().Format);
        if (velFmt == DXGI_FORMAT_UNKNOWN && !g_warnedVelFmt) {
            g_warnedVelFmt = true; Log("upscaler: velocity format %d not supported, using camera motion only", (int)f.velocity->GetDesc().Format);
        }
    }
    if (depthFmt == DXGI_FORMAT_UNKNOWN) { Log("upscaler: depth format %d not supported", (int)dd.Format); Fail(cl, f, "depth format", useFsr); return; }

    const UINT base = g_heapCursor; g_heapCursor = (g_heapCursor + kDescPerPass) % kHeapSlots;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += (SIZE_T)base * g_descSize;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += (UINT64)base * g_descSize;
    auto slot = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += (SIZE_T)i * g_descSize; return h; };
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.Format = depthFmt;
    g_dev->CreateShaderResourceView(f.depth, &sv, slot(0));
    sv.Format = velFmt != DXGI_FORMAT_UNKNOWN ? velFmt : DXGI_FORMAT_R16G16_UNORM;
    g_dev->CreateShaderResourceView(velFmt != DXGI_FORMAT_UNKNOWN ? f.velocity : nullptr, &sv, slot(1));
    const bool readColor = copies || cropDirect;
    sv.Format = readColor ? ColorSrvFormat(f.color->GetDesc().Format) : DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateShaderResourceView(readColor ? f.color : nullptr, &sv, slot(2));
    sv.Format = useMask ? ColorSrvFormat(f.translucency->GetDesc().Format) : DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateShaderResourceView(useMask ? f.translucency : nullptr, &sv, slot(3));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = DXGI_FORMAT_R16G16_FLOAT;
    g_dev->CreateUnorderedAccessView(e.mv, nullptr, &uv, slot(4));
    uv.Format = DXGI_FORMAT_R32_FLOAT;
    g_dev->CreateUnorderedAccessView(copies ? e.depthCopy : nullptr, nullptr, &uv, slot(5));
    uv.Format = copies ? e.copyFmt : DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateUnorderedAccessView(copies ? e.colorCopy : nullptr, nullptr, &uv, slot(6));
    uv.Format = DXGI_FORMAT_R8_UNORM;
    g_dev->CreateUnorderedAccessView(useMask ? e.mask : nullptr, nullptr, &uv, slot(7));
    const bool stats = useMask && g_statsBuf && f.eye == 0 && !g_statsPendingFrame && (g_frame % 90) == 0;
    D3D12_UNORDERED_ACCESS_VIEW_DESC bv = {};
    bv.Format = DXGI_FORMAT_R32_TYPELESS; bv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    bv.Buffer.NumElements = 4; bv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g_dev->CreateUnorderedAccessView(stats ? g_statsBuf : nullptr, nullptr, &bv, slot(8));
    // lean inputs: the window crops (second descriptor table, slots 20-22)
    uv.Format = cropDirect ? e.winFmt : DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateUnorderedAccessView(cropDirect ? e.winColor : nullptr, nullptr, &uv, slot(20));
    uv.Format = DXGI_FORMAT_R32_FLOAT;
    g_dev->CreateUnorderedAccessView(cropDirect ? e.winDepth : nullptr, nullptr, &uv, slot(21));
    uv.Format = DXGI_FORMAT_R16G16_FLOAT;
    g_dev->CreateUnorderedAccessView(cropDirect ? e.winMv : nullptr, nullptr, &uv, slot(22));

    struct { int32_t ox, oy; uint32_t w, h; float m[16]; float jx, jy, sx, sy; uint32_t velValid, dilate, copy; float maskScale;
             int32_t wox, woy; uint32_t ww, wh; } c = {};
    static_assert(sizeof(c) == 32 * 4, "root constants");
    c.ox = f.depthRect.x0; c.oy = f.depthRect.y0; c.w = (uint32_t)f.renderW; c.h = (uint32_t)f.renderH;
    memcpy(c.m, f.clipToPrevClip, sizeof(c.m));
    c.jx = f.jitterNdcX; c.jy = f.jitterNdcY; c.sx = g_cfg.motionSignX; c.sy = g_cfg.motionSignY;
    c.velValid = velFmt != DXGI_FORMAT_UNKNOWN ? 1u : 0u;
    c.dilate = g_cfg.dilateMotionVectors ? 1u : 0u;
    c.copy = (copies ? 1u : 0u) | (stats ? 2u : 0u) | (cropDirect ? 4u : 0u) | (g_liveInClean.load(std::memory_order_relaxed) ? 8u : 0u);
    if (cropDirect) { c.wox = fr.rx; c.woy = fr.ry; c.ww = (uint32_t)fr.rw; c.wh = (uint32_t)fr.rh; }
    if (copies) e.copyLastUse = g_frame;
    c.maskScale = useMask ? g_cfg.maskScale : 0.f;

    // timestamps: read this eye's result from kTsSlots-2 frames ago, then start this frame's set
    const UINT tsBase = (UINT)(((g_frame % kTsSlots) * 2 + (f.eye & 1)) * kTsPer);
    uint32_t tsDone = 1;   // stamps written this frame (bit k)
    auto stamp = [&](UINT k) {   // stamp k, plus any earlier one this frame skipped (those segments then count 0)
        if (!g_tsHeap || ((tsDone >> k) & 1u)) return;
        for (UINT j = 1; j <= k; ++j)
            if (!((tsDone >> j) & 1u)) { cl->EndQuery(g_tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, tsBase + j); tsDone |= 1u << j; }
    };
    if (g_tsHeap) {
        if (g_frame >= kTsSlots) {
            const UINT ob = (UINT)((((g_frame - (kTsSlots - 2)) % kTsSlots) * 2 + (f.eye & 1)) * kTsPer);
            const uint64_t* t = g_tsData + ob;
            if (t[kTsPer - 1] > t[0]) {
                const double ms = (double)(t[kTsPer - 1] - t[0]) * g_tsToMs;
                if (ms < 50.0) {
                    PerfOnDlssGpu(f.eye & 1, ms);
                    bool mono = true; for (UINT k = 1; k < kTsPer; ++k) mono = mono && t[k] >= t[k - 1];
                    if (mono) {
                        double seg[kTsPer - 1];
                        for (UINT k = 0; k + 1 < kTsPer; ++k) seg[k] = (double)(t[k + 1] - t[k]) * g_tsToMs;
                        PerfOnPassGpu(f.eye & 1, seg);
                    }
                }
            }
        }
        cl->EndQuery(g_tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, tsBase);
    }

    if (stats) {   // zero the counters, then let the pass write them
        cl->CopyBufferRegion(g_statsBuf, 0, g_statsZero, 0, 16);
        Barrier(cl, g_statsBuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    ID3D12DescriptorHeap* heaps[] = { g_heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_rs);
    cl->SetPipelineState(g_pso);
    cl->SetComputeRoot32BitConstants(0, 32, &c, 0);
    cl->SetComputeRootDescriptorTable(1, gpu);
    { D3D12_GPU_DESCRIPTOR_HANDLE gw = gpu; gw.ptr += (UINT64)20 * g_descSize; cl->SetComputeRootDescriptorTable(2, gw); }
    cl->Dispatch((UINT)(f.renderW + 7) / 8, (UINT)(f.renderH + 7) / 8, 1);
    {
        Barriers bg;
        bg.T(e.mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (cropDirect) {   // the crops are read like the copied ones (and go back to UAV after the window's upscaler)
            ID3D12Resource* w3[3] = { e.winColor, e.winDepth, e.winMv };
            for (ID3D12Resource* r : w3) bg.T(r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (copies) {
            bg.T(e.depthCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            bg.T(e.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (useMask) bg.T(e.mask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (stats) bg.T(g_statsBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        bg.Flush(cl);
    }
    if (stats) {   // counters -> readback buffer, read a few frames later by LogStats
        cl->CopyBufferRegion(g_statsRb, 0, g_statsBuf, 0, 16);
        Barrier(cl, g_statsBuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        g_statsPendingFrame = g_frame; g_statsPixels = (uint32_t)(f.renderW * f.renderH);
    }
    stamp(1);   // after the motion-vector pass (+ copies / window crops)

    // ---- upscale: the whole eye (live model, FSR, or the periphery model), then the fovea window if foveated.
    // With sharpening on, the upscalers write to sharpTmp and the sharpen pass writes the real output.
    const bool sharpen = f.sharpen > 0.f && EnsureSharpTmp(e, f.outW, f.outH);
    ID3D12Resource* upOut = sharpen ? e.sharpTmp : f.output;
    bool ok;
    unsigned ngxResult = 1;
    if (useFsr) {
        ok = FsrDispatch(cl, fe, f, e.colorCopy, e.depthCopy, e.mv, useMask ? e.mask : nullptr, upOut);
    } else if (plainEdges || fullTaa) {
        ok = true;   // the edges (or the whole eye) come from the render itself, in the passes below
    } else {
        ngxResult = NgxEval(cl, halfEdges ? e.periphHalf : fovea ? e.periph : e.full, e, f, copies, useMask, 0, 0, f.renderW, f.renderH,
                            halfEdges ? e.periphTmp : upOut);
        ok = !NVSDK_NGX_FAILED((NVSDK_NGX_Result)ngxResult);
    }
    stamp(2);   // after the whole-eye upscaler (DLSS / FSR on the eye or its edges; nothing with plain edges)
    // anti-aliasing (edge_aa.hlsl), at render size, into aa[]: spatial or temporal for the plain edges (key 1), or
    // temporal on the whole eye (key 2). Returns the result in SRV state (temporal = already unjittered), or null.
    bool aaUnjittered = false;
    auto runAa = [&](int aaMode, const char* what, bool window) -> ID3D12Resource* {
        ID3D12Resource* aaOut = nullptr;
        aaUnjittered = false;
        if (aaMode > 0 && EnsureAa(e, f, aaMode >= 2)) {
            const bool temporal = aaMode >= 2;   // 2 and 3 share the history (same result)
            const int src = e.aaCur & 1, dst = temporal ? (src ^ 1) : 0;
            const bool reset = temporal && (f.reset || standInStart || e.aaMode < 2 || !e.aaFrame || e.aaFrame + 1 != g_frame || e.aaPrevW <= 0);
            const bool readHist = temporal && !reset;
            if (readHist) Barrier(cl, e.aa[src], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ID3D12Resource* curTex = lean ? f.color : e.colorCopy;
            sv.Format = lean ? ColorSrvFormat(f.color->GetDesc().Format) : e.copyFmt;
            g_dev->CreateShaderResourceView(curTex, &sv, slot(16));
            if (!readHist) g_dev->CreateShaderResourceView(curTex, &sv, slot(17));
            sv.Format = g_aaFmt;
            if (readHist) g_dev->CreateShaderResourceView(e.aa[src], &sv, slot(17));
            sv.Format = DXGI_FORMAT_R16G16_FLOAT;
            g_dev->CreateShaderResourceView(e.mv, &sv, slot(18));
            uv.Format = g_aaFmt;
            g_dev->CreateUnorderedAccessView(e.aa[dst], nullptr, &uv, slot(19));
            // skip the pixels the centre window covers (window = plain edges), except for two frames after a reset or
            // a change of the window / render size, so both textures hold real colours everywhere it may later show
            const float sx = (float)f.renderW / f.outW, sy = (float)f.renderH / f.outH;
            const int winKey = window ? (fr.ox * 7919 + fr.oy * 31 + fr.ow * 3 + fr.oh + f.renderW * 104729 + f.renderH * 13) : -1;
            if (winKey != e.aaWinKey || reset) { e.aaWinKey = winKey; e.aaFill = 2; }
            const bool skip = window && e.aaFill <= 0 && fr.ow > 0 && fr.oh > 0;
            if (e.aaFill > 0) --e.aaFill;
            const float itw = 1.f / e.aaW, ith = 1.f / e.aaH, js = g_cfg.plainJitterSign;
            const float pw = readHist ? (float)e.aaPrevW : (float)f.renderW, ph = readHist ? (float)e.aaPrevH : (float)f.renderH;
            const float icx = lean ? 1.f / (float)f.color->GetDesc().Width : itw, icy = lean ? 1.f / (float)f.color->GetDesc().Height : ith;
            struct { uint32_t w, h, mode, reset; float jx, jy, itw, ith, hsx, hsy, hmx, hmy, alpha, gamma; uint32_t dither, frame;
                     float wox, woy, wsx, wsy, inner, icx, icy; uint32_t pad; int32_t cox, coy; uint32_t pad2, pad3; } ac =
                { (uint32_t)f.renderW, (uint32_t)f.renderH, (uint32_t)aaMode, reset ? 1u : 0u, js * f.jitterX, js * f.jitterY, itw, ith,
                  pw / f.renderW * itw, ph / f.renderH * ith, (pw - 0.5f) * itw, (ph - 0.5f) * ith, g_cfg.edgeTaaBlend, 1.25f,
                  (temporal && g_aaFmt == DXGI_FORMAT_R11G11B10_FLOAT) ? 1u : 0u, (uint32_t)g_frame,
                  skip ? fr.ox * sx : 0.f, skip ? fr.oy * sy : 0.f, skip ? fr.ow * sx : 0.f, skip ? fr.oh * sy : 0.f,
                  1.f - g_cfg.foveaFeather, icx, icy, 0u, lean ? f.colorRect.x0 : 0, lean ? f.colorRect.y0 : 0, 0u, 0u };
            static_assert(sizeof(ac) == 28 * 4, "edge AA constants");
            D3D12_GPU_DESCRIPTOR_HANDLE ga = gpu; ga.ptr += (UINT64)16 * g_descSize;
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetComputeRootSignature(g_rsAa);
            cl->SetPipelineState(g_psoAa);
            cl->SetComputeRoot32BitConstants(0, 28, &ac, 0);
            cl->SetComputeRootDescriptorTable(1, ga);
            cl->Dispatch((UINT)(f.renderW + 7) / 8, (UINT)(f.renderH + 7) / 8, 1);
            if (readHist) Barrier(cl, e.aa[src], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cl, e.aa[dst], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            aaOut = e.aa[dst]; aaUnjittered = temporal;
            e.aaLastUse = g_frame; if (temporal) e.aaLastTemporal = g_frame;
            if (temporal) { e.aaCur = dst; e.aaPrevW = f.renderW; e.aaPrevH = f.renderH; e.aaFrame = g_frame; }
        }
        if (aaMode != e.aaMode) {
            static int logs = 40;
            if (logs-- > 0) Log("%s eye=%d: anti-aliasing %s%s", what, f.eye, aaMode == 3 ? "temporal fast" : aaMode == 2 ? "temporal" : aaMode == 1 ? "spatial" : "off",
                                aaMode && !aaOut ? " (texture creation failed: none)" : "");
            e.aaMode = aaMode;
        }
        return aaOut;
    };
    if (ok && fovea) {
        // crop the window out of this eye's copies (DLSS window with WindowCrop, or FSR, which reads from (0,0)): all
        // state changes before the three copies, then all of them after
        auto cropWindow = [&]() {
            ID3D12Resource* src[3] = { e.colorCopy, e.depthCopy, e.mv };
            ID3D12Resource* dst[3] = { e.winColor, e.winDepth, e.winMv };
            Barriers bg;
            for (int k = 0; k < 3; ++k) {
                bg.T(src[k], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
                bg.T(dst[k], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            }
            bg.Flush(cl);
            for (int k = 0; k < 3; ++k) {
                D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
                dl.pResource = dst[k]; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                sl.pResource = src[k]; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                const D3D12_BOX box = { (UINT)fr.rx, (UINT)fr.ry, 0, (UINT)(fr.rx + fr.rw), (UINT)(fr.ry + fr.rh), 1 };
                cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, &box);
            }
            for (int k = 0; k < 3; ++k) {
                bg.T(src[k], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                bg.T(dst[k], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            bg.Flush(cl);
        };
        if (winCrop && !cropDirect) cropWindow();   // the DLSS window reads its own crops at (0,0), no sub-rect bases
        ID3D12Resource* nrPost = nullptr;
        if (fsrCentre) {   // crop the window out of this eye's inputs (FSR reads from (0,0)), then FSR into centerOut
            if (!cropDirect) cropWindow();
            // the window sees a narrower field of view (FSR uses it for its depth heuristics)
            const float fovW = f.fovY > 0.f && f.renderH > 0 ? 2.f * std::atan(std::tan(f.fovY * 0.5f) * fr.rh / (float)f.renderH) : 0.f;
            ok = FsrDispatch(cl, fc, f, e.winColor, e.winDepth, e.winMv, nullptr, e.centerOut, fr.rw, fr.rh, fr.ow, fr.oh, fovW);
            Barriers bg;
            for (ID3D12Resource* r : { e.winColor, e.winDepth, e.winMv }) bg.T(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (ok) bg.U(e.centerOut);   // FSR wrote centerOut as a UAV: make the write visible before the compose / blend read
            bg.Flush(cl);
        } else if (standIn) {
            // no DLSS centre this frame: the compose pass below covers the window with the plain edges
            if (winCrop) {
                Barriers bg;
                for (ID3D12Resource* r : { e.winColor, e.winDepth, e.winMv }) bg.T(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                bg.Flush(cl);
            }
        } else {
            // Order A (pre-upscale DLSS 5, dlss5 == 1): run DLSS 5 on the render-sized window crop before DLSS upscales it
            ID3D12Resource* nrPre = (dlss5 == 1) ? RunDlss5Window(cl, e, f, e.winColor, fr.rw, fr.rh, fr.rw, fr.rh, fq, cpu, gpu) : nullptr;
            const bool direct = centreDirect && (halfEdges || plainEdges);
            ngxResult = NgxEval(cl, e.center, e, f, copies, useMask, fr.rx, fr.ry, fr.rw, fr.rh, direct ? upOut : e.centerOut, winCrop,
                                (winDiag & 2) != 0, direct ? fr.ox : 0, direct ? fr.oy : 0, nrPre);
            if (direct && e.centerOut) { Bury(nullptr, e.centerOut); e.centerOut = nullptr; e.coW = e.coH = 0; }   // not needed any more
            ok = !NVSDK_NGX_FAILED((NVSDK_NGX_Result)ngxResult);
            if (direct && !ok && !directOff) {
                directOff = true;
                Log("DLSS: the centre written straight into the output failed (0x%08X): using the separate centre texture "
                    "for the rest of the session ([Foveated] CentreDirect off)", ngxResult);
            }
            // Order B (post-upscale DLSS 5, dlss5 == 2): run DLSS 5 on the upscaled centre window (e.centerOut) before compose/blend
            if (ok && dlss5 == 2 && !direct) {
                Barrier(cl, e.centerOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                nrPost = RunDlss5Window(cl, e, f, e.centerOut, fr.rw, fr.rh, fr.ow, fr.oh, fq, cpu, gpu);
                Barrier(cl, e.centerOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Barriers bg;
            if (direct) bg.U(upOut);
            if (nrPre) bg.T(nrPre, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (winCrop) for (ID3D12Resource* r : { e.winColor, e.winDepth, e.winMv }) bg.T(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            bg.Flush(cl);
        }
        stamp(3);   // after the centre window (crops + DLSS / FSR)
        ID3D12Resource* ctrTex = nrPost ? nrPost : e.centerOut;
        if (ok && (halfEdges || plainEdges)) {   // stretch the edges (half-size result, or the render itself) to the
                                                 // output and lay the centre over them, one pass
            // plain-edge AA (key 1): the compose pass stretches its result instead of the colour copy
            ID3D12Resource* edgeTex = halfEdges ? e.periphTmp : lean ? f.color : e.colorCopy;
            ID3D12Resource* aaOut = plainEdges ? runAa(aaWanted, "plain edges", !standIn) : nullptr;
            stamp(4);   // after the edge AA
            if (aaOut) edgeTex = aaOut;
            const bool inOut = centreDirect && !fsrCentre && !standIn;   // the DLSS centre is already in upOut
            const bool centreTex = !inOut && !standIn;                      // the centre comes from centerOut / nrPost
            {
                Barriers bg;
                if (halfEdges) bg.T(e.periphTmp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (centreTex && !nrPost) bg.T(e.centerOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                bg.Flush(cl);
            }
            sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            if (aaOut) sv.Format = g_aaFmt;
            else if (edgeTex == f.color) sv.Format = ColorSrvFormat(f.color->GetDesc().Format);
            else if (edgeTex == e.colorCopy) sv.Format = e.copyFmt;
            g_dev->CreateShaderResourceView(edgeTex, &sv, slot(13));   // half-size edges RGBA16F; the AA result g_aaFmt
            if (!centreTex) g_dev->CreateShaderResourceView(edgeTex, &sv, slot(14));   // unused, must be valid
            else { sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; g_dev->CreateShaderResourceView(ctrTex, &sv, slot(14)); }
            uv.Format = OutUavFormat(upOut);
            g_dev->CreateUnorderedAccessView(upOut, nullptr, &uv, slot(15));
            D3D12_GPU_DESCRIPTOR_HANDLE gc = gpu; gc.ptr += (UINT64)13 * g_descSize;
            // plain edges: the colour copy holds the render in its top-left renderW x renderH (larger under dynamic
            // resolution) and is jittered; sample it at -jitter (the offset DLSS is given) to keep the edges still
            struct { uint32_t w, h; int32_t ox, oy; uint32_t ww, wh; float feather, pad; float sx, sy, offx, offy, maxx, maxy, minx, miny; } cc =
                { (uint32_t)f.outW, (uint32_t)f.outH, standIn ? 0 : fr.ox, standIn ? 0 : fr.oy, standIn ? 0u : (uint32_t)fr.ow,
                  standIn ? 0u : (uint32_t)fr.oh, g_cfg.foveaFeather, inOut ? 1.f : 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f };
            // (the game's colour: this eye's render is its sub-rect at the colour rect's origin, like FallbackStretch)
            const bool fromGame = !aaOut && edgeTex == f.color;
            if (plainEdges && (aaOut || fromGame || (e.copyW > 0 && e.copyH > 0))) {
                const float cw2 = (float)(aaOut ? e.aaW : fromGame ? (int)f.color->GetDesc().Width : e.copyW);
                const float ch2 = (float)(aaOut ? e.aaH : fromGame ? (int)f.color->GetDesc().Height : e.copyH);
                const float js = aaUnjittered ? 0.f : g_cfg.plainJitterSign;
                const float ox = fromGame ? (float)f.colorRect.x0 : 0.f, oy = fromGame ? (float)f.colorRect.y0 : 0.f;
                cc.sx = f.renderW / cw2; cc.sy = f.renderH / ch2;
                cc.offx = (ox - js * f.jitterX) / cw2; cc.offy = (oy - js * f.jitterY) / ch2;
                cc.maxx = (ox + f.renderW - 0.5f) / cw2; cc.maxy = (oy + f.renderH - 0.5f) / ch2;
                cc.minx = (ox + 0.5f) / cw2; cc.miny = (oy + 0.5f) / ch2;
            }
            static_assert(sizeof(cc) == 16 * 4, "compose constants");
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetComputeRootSignature(g_rsCompose);
            cl->SetPipelineState(g_psoCompose);
            cl->SetComputeRoot32BitConstants(0, 16, &cc, 0);
            cl->SetComputeRootDescriptorTable(1, gc);
            cl->Dispatch((UINT)(f.outW + 7) / 8, (UINT)(f.outH + 7) / 8, 1);
            Barriers bg;
            if (halfEdges) bg.T(e.periphTmp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (aaOut) bg.T(aaOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (centreTex) bg.T(ctrTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            bg.Flush(cl);
        } else if (ok) {   // blend the centre over the whole-eye result, with a soft elliptical edge
            stamp(4);   // (no edge AA here)
            D3D12_RESOURCE_BARRIER b[2] = {};
            b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b[0].UAV.pResource = upOut;   // whole-eye write -> our read
            b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[1].Transition.pResource = e.centerOut;
            b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            cl->ResourceBarrier(nrPost ? 1 : 2, b);
            sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g_dev->CreateShaderResourceView(ctrTex, &sv, slot(9));
            uv.Format = OutUavFormat(upOut);
            g_dev->CreateUnorderedAccessView(upOut, nullptr, &uv, slot(10));
            D3D12_GPU_DESCRIPTOR_HANDLE gb = gpu; gb.ptr += (UINT64)9 * g_descSize;
            struct { int32_t ox, oy; uint32_t w, h; float feather; uint32_t pad[3]; } bc = { fr.ox, fr.oy, (uint32_t)fr.ow, (uint32_t)fr.oh, g_cfg.foveaFeather, {} };
            static_assert(sizeof(bc) == 8 * 4, "blend constants");
            cl->SetDescriptorHeaps(1, heaps);   // NGX bound its own heap and pipeline
            cl->SetComputeRootSignature(g_rsBlend);
            cl->SetPipelineState(g_psoBlend);
            cl->SetComputeRoot32BitConstants(0, 8, &bc, 0);
            cl->SetComputeRootDescriptorTable(1, gb);
            cl->Dispatch((UINT)(fr.ow + 7) / 8, (UINT)(fr.oh + 7) / 8, 1);
            Barrier(cl, ctrTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } else if (nrPost) {
            Barrier(cl, nrPost, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    if (ok && fullTaa) {   // key 2: the mod's TAA on the whole eye, stretched to the output, no centre window
        ID3D12Resource* aaOut = runAa(aaWanted, "whole-eye TAA", false);
        stamp(4);
        if (!aaOut) ok = false;   // Fail() below shows the plain render
        else {
            sv.Format = g_aaFmt;
            g_dev->CreateShaderResourceView(aaOut, &sv, slot(13));
            g_dev->CreateShaderResourceView(aaOut, &sv, slot(14));   // the centre is unused (window size 0) but must be valid
            uv.Format = OutUavFormat(upOut);
            g_dev->CreateUnorderedAccessView(upOut, nullptr, &uv, slot(15));
            D3D12_GPU_DESCRIPTOR_HANDLE gc = gpu; gc.ptr += (UINT64)13 * g_descSize;
            const float tw = (float)e.aaW, th = (float)e.aaH;
            struct { uint32_t w, h; int32_t ox, oy; uint32_t ww, wh; float feather, pad; float sx, sy, offx, offy, maxx, maxy, minx, miny; } cc =
                { (uint32_t)f.outW, (uint32_t)f.outH, 0, 0, 0u, 0u, 0.3f, 0.f, f.renderW / tw, f.renderH / th, 0.f, 0.f,
                  (f.renderW - 0.5f) / tw, (f.renderH - 0.5f) / th, 0.5f / tw, 0.5f / th };
            static_assert(sizeof(cc) == 16 * 4, "compose constants");
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetComputeRootSignature(g_rsCompose);
            cl->SetPipelineState(g_psoCompose);
            cl->SetComputeRoot32BitConstants(0, 16, &cc, 0);
            cl->SetComputeRootDescriptorTable(1, gc);
            cl->Dispatch((UINT)(f.outW + 7) / 8, (UINT)(f.outH + 7) / 8, 1);
            Barrier(cl, aaOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    if (!ok) Fail(cl, f, "evaluate / dispatch failed", useFsr);   // it left the output unwritten
    if (sharpen && ok) {   // contrast-adaptive sharpening: sharpTmp -> output
        Barrier(cl, e.sharpTmp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g_dev->CreateShaderResourceView(e.sharpTmp, &sv, slot(11));
        uv.Format = OutUavFormat(f.output);
        g_dev->CreateUnorderedAccessView(f.output, nullptr, &uv, slot(12));
        D3D12_GPU_DESCRIPTOR_HANDLE gs = gpu; gs.ptr += (UINT64)11 * g_descSize;
        struct { uint32_t w, h; float sharpness, pad; } sc = { (uint32_t)f.outW, (uint32_t)f.outH, f.sharpen, 0.f };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(g_rsSharp);
        cl->SetPipelineState(g_psoSharp);
        cl->SetComputeRoot32BitConstants(0, 4, &sc, 0);
        cl->SetComputeRootDescriptorTable(1, gs);
        cl->Dispatch((UINT)(f.outW + 7) / 8, (UINT)(f.outH + 7) / 8, 1);
        Barrier(cl, e.sharpTmp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (ok) g_failPasses = 0;
    if (g_tsHeap) {
        stamp(kTsPer - 1);   // end (and any stamp this frame did not reach)
        cl->ResolveQueryData(g_tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, tsBase, kTsPer, g_tsReadback, (UINT64)tsBase * sizeof(uint64_t));
    }
    if (ok) ProbeRun(cl, f, useFsr || fsrCentre, slot(23), D3D12_GPU_DESCRIPTOR_HANDLE{ gpu.ptr + (UINT64)23 * g_descSize },
                     slot(27), D3D12_GPU_DESCRIPTOR_HANDLE{ gpu.ptr + (UINT64)27 * g_descSize });
    {
        Barriers bg;
        bg.T(e.mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (copies) {
            bg.T(e.depthCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            bg.T(e.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (useMask) bg.T(e.mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        bg.Flush(cl);
    }
    {   // log when the mask starts / stops being used (e.g. F9, or translucency missing)
        static int lastMask = -1, logs = 40;
        if (f.eye == 0 && (int)useMask != lastMask && logs > 0) {
            --logs; lastMask = (int)useMask;
            Log("%s: translucency mask %s (texture %s, live %d)", useFsr ? "FSR" : "DLSS", useMask ? "ON" : "off",
                f.translucency ? "found" : "missing", (int)g_liveMask.load());
        }
    }

    if (e.logLeft > 0 || !ok) {
        if (e.logLeft > 0) --e.logLeft;
        static int failLogs = 32;
        if (ok || failLogs-- > 0)
            Log("%s: eval eye=%d -> %s 0x%08X depthFmt=%d velFmt=%d colorFmt=%d colorBase=(%d,%d) copies=%d lean=%d render=%dx%d out=%dx%d jitter=(%.3f,%.3f) reset=%d near=%.1f fovY=%.3f",
                useFsr ? "FSR" : "DLSS", f.eye, ok ? "ok" : "FAILED", ngxResult, (int)dd.Format, (int)velFmt, (int)f.color->GetDesc().Format,
                f.colorRect.x0, f.colorRect.y0, (int)copies, (int)lean, f.renderW, f.renderH, f.outW, f.outH, f.jitterX, f.jitterY, (int)f.reset,
                f.cameraNear, f.fovY);
    }
}
