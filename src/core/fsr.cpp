// fsr.cpp - AMD FSR upscaling through the FidelityFX API (signed amd_fidelityfx_loader_dx12.dll +
// amd_fidelityfx_upscaler_dx12.dll next to this DLL), one upscale context per eye. The upscaler DLL picks the
// version for the GPU (FSR 3.1.x anywhere, FSR 4 on RDNA 4) unless [FSR] Version forces one.
// FsrInit: init thread. Everything else: RHI thread, except the [Test] key setters (game thread), which only
// change two atomics; a context made with a different version or exposure mode is rebuilt on its next use.
#include "common.h"
#include <d3d12.h>
#include <cmath>
#include <algorithm>
#include <atomic>
#include "dx12/ffx_api_dx12.h"
#include "ffx_upscale.h"

struct FsrEye {
    ffxContext ctx = nullptr;
    int ow = 0, oh = 0, mw = 0, mh = 0;   // output size, largest render size the context was made for
    uint64_t lastFrame = 0;   // frame of the previous dispatch (a gap = stale history)
    uint64_t failFrame = 0; int fails = 0;   // creation failures: retry every ~2 s
    uint64_t createdFrame = 0;               // for the idle release
    uint64_t verId = 0; int expMode = 0;     // what it was made with
};

static HMODULE g_mod = nullptr;
static PfnFfxCreateContext g_create = nullptr;
static PfnFfxDestroyContext g_destroy = nullptr;
static PfnFfxQuery g_query = nullptr;
static PfnFfxDispatch g_dispatch = nullptr;
static ID3D12Device* g_dev = nullptr;
static bool g_ok = false;
static std::atomic<uint64_t> g_overrideId{ 0 };   // 0 = let the runtime choose
static std::atomic<int> g_expMode{ 0 };           // FsrExposureMode
static uint64_t g_verIds[16] = {}; static char g_verNames[16][48] = {}; static int g_verCount = 0, g_verIdx = 0;
static char g_running[48] = "?";
static std::atomic<bool> g_announceVer{ false };   // announce the running version once the next context is made
static std::atomic<bool> g_verByKey{ false };      // version chosen with the [Test] key (falls back if it fails)
static std::atomic<uint32_t> g_verFailed{ 0 };     // versions (bit = list index) that failed to create: F2 skips them
static bool g_noDebugCheck = false;                // creation failed with FSR's input checks: make contexts without them
static FsrEye g_eyes[2];
static FsrEye g_centre[2];   // foveated window with FSR in the centre: one more context per eye
static uint64_t g_frame = 0;
static LARGE_INTEGER g_lastQpc = {};
static double g_qpcToMs = 0.0;
static float g_frameMs = 11.1f;

struct DeadCtx { ffxContext ctx; uint64_t frame; };
static DeadCtx g_grave[32];   // room for several rebuilds within 16 frames (a [Test] key rebuilds up to 4 contexts)

// FSR's own messages. With [Test] OutputProbe its input checks repeat every frame, so each distinct text is logged
// its first 3 times, then at the 100th, 1000th and every 10000th time with the count.
static void FsrMessage(uint32_t type, const wchar_t* msg) {
    static int left = 32;
    if (!msg) return;
    wchar_t w[400]; wcsncpy_s(w, msg, _TRUNCATE);   // a longer message is cut, never left unterminated
    char a[1300] = {};
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof(a) - 1, nullptr, nullptr)) strcpy_s(a, "(message not readable)");
    if (!g_cfg.outputProbe) {
        if (left-- > 0) Log("[FSR] %s%s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error: " : "", a);
        return;
    }
    static uint64_t hashes[48] = {}; static uint32_t counts[48] = {}; static int used = 0;
    uint64_t h = 1469598103934665603ull;
    for (const char* c = a; *c; ++c) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    int k = 0;
    while (k < used && hashes[k] != h) ++k;
    if (k == used) {
        if (used == 48) {
            static bool full = false;
            if (!full) { full = true; Log("[FSR] (more than 48 different messages: the rest are not logged)"); }
            return;
        }
        hashes[used++] = h;
    }
    const uint32_t n = ++counts[k];
    if (n <= 3 || n == 100 || n == 1000 || n % 10000 == 0)
        Log("[FSR] %s%s%s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error: " : "", a,
            n > 3 ? (n == 100 ? "  (100th time)" : n == 1000 ? "  (1000th time)" : "  (another 10000 times)") : "");
}

bool FsrReady() { return g_ok; }
FsrEye* FsrGetEye(int eye) { return &g_eyes[eye & 1]; }
FsrEye* FsrGetCentre(int eye) { return &g_centre[eye & 1]; }

bool FsrInit(ID3D12Device* dev) {
    if (g_ok) return true;
    g_dev = dev;
    // the loader finds amd_fidelityfx_upscaler_dx12.dll by name (normal DLL search, i.e. the exe's folder);
    // loading it first by full path makes it work wherever this DLL lives
    wchar_t path[MAX_PATH]; wcscpy_s(path, g_dir); wcscat_s(path, L"amd_fidelityfx_upscaler_dx12.dll");
    if (!LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
        Log("FSR: amd_fidelityfx_upscaler_dx12.dll not found next to itr2_dlss.dll (error %lu)", GetLastError());
    wcscpy_s(path, g_dir); wcscat_s(path, L"amd_fidelityfx_loader_dx12.dll");
    g_mod = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_mod) { Log("FSR: amd_fidelityfx_loader_dx12.dll not found next to itr2_dlss.dll (error %lu)", GetLastError()); return false; }
    g_create = (PfnFfxCreateContext)GetProcAddress(g_mod, "ffxCreateContext");
    g_destroy = (PfnFfxDestroyContext)GetProcAddress(g_mod, "ffxDestroyContext");
    g_query = (PfnFfxQuery)GetProcAddress(g_mod, "ffxQuery");
    g_dispatch = (PfnFfxDispatch)GetProcAddress(g_mod, "ffxDispatch");
    if (!g_create || !g_destroy || !g_query || !g_dispatch) { Log("FSR: loader DLL lacks the FidelityFX API exports"); return false; }

    // upscaler versions this GPU can run (the first is the runtime's default)
    uint64_t count = 0;
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = dev;
    q.outputCount = &count;
    const ffxReturnCode_t qr = g_query(nullptr, &q.header);
    if (qr != FFX_API_RETURN_OK || count == 0) {
        Log("FSR: no upscaler version available (query %u, count %llu): amd_fidelityfx_upscaler_dx12.dll missing, or no DX12 support",
            (unsigned)qr, (unsigned long long)count);
        return false;
    }
    uint64_t ids[16] = {}; const char* names[16] = {};
    if (count > 16) count = 16;
    q.versionIds = ids; q.versionNames = names;
    g_query(nullptr, &q.header);
    char want[32] = {}; WideCharToMultiByte(CP_UTF8, 0, g_cfg.fsrVersion, -1, want, sizeof(want) - 1, nullptr, nullptr);
    for (uint64_t i = 0; i < count; ++i) {
        const bool pick = want[0] && !g_overrideId && names[i] && strstr(names[i], want);
        if (pick) { g_overrideId = ids[i]; g_verIdx = (int)i; }
        Log("FSR: available upscaler '%s'%s%s", names[i] ? names[i] : "?", i == 0 ? " (default)" : "", pick ? "  <- forced by [FSR] Version" : "");
        g_verIds[i] = ids[i];
        strncpy_s(g_verNames[i], names[i] ? names[i] : "?", _TRUNCATE);
    }
    g_verCount = (int)count;
    if (want[0] && !g_overrideId) Log("FSR: no version matches [FSR] Version=%s, using the default", want);
    g_expMode = g_cfg.autoExposure ? 1 : 0;

    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcToMs = 1000.0 / (double)f.QuadPart;
    g_ok = true;
    return true;
}

void FsrNewFrame() {
    ++g_frame;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (g_lastQpc.QuadPart) {
        const double ms = (now.QuadPart - g_lastQpc.QuadPart) * g_qpcToMs;
        if (ms > 0.0 && ms < 200.0) g_frameMs = (float)ms;
    }
    g_lastQpc = now;
    for (auto& d : g_grave)   // replaced 16+ frames ago: the GPU is done with them
        if (d.ctx && g_frame > d.frame + 16) { g_destroy(&d.ctx, nullptr); d = {}; }
}

static void Bury(ffxContext c) {
    for (auto& d : g_grave) if (!d.ctx) { d = { c, g_frame }; return; }
    g_destroy(&g_grave[0].ctx, nullptr);   // full (many resizes in a few frames): drop the oldest now
    g_grave[0] = { c, g_frame };
}

// low-memory mode: drop this eye's context when FSR is not in use (destroyed 16 frames later)
void FsrRelease(FsrEye* e) {
    if (!e->ctx) return;
    Bury(e->ctx); e->ctx = nullptr; e->ow = e->oh = 0;
    Log("FSR: released context (low-memory mode or VRAM over budget)");
}

// release this eye's contexts not dispatched for 'frames' frames (otherwise their VRAM stays allocated until exit:
// cycling F9 / F7 left ~0.85 GB)
void FsrReleaseIdle(int eye, uint64_t frames) {
    FsrEye* both[2] = { &g_eyes[eye & 1], &g_centre[eye & 1] };
    for (FsrEye* e : both) {
        if (!e->ctx) continue;
        const uint64_t last = e->lastFrame ? e->lastFrame : e->createdFrame;
        if (g_frame > last + frames) {
            Bury(e->ctx); e->ctx = nullptr; e->ow = e->oh = 0;
            Log("FSR: released an unused context (eye %d, idle %llu frames)", eye, (unsigned long long)(g_frame - last));
        }
    }
}

// game thread while DLSS is off (dlss.cpp DlssReleaseAll holds the lock): destroy every context now
int FsrReleaseAll() {
    int n = 0;
    for (auto& d : g_grave) if (d.ctx) { g_destroy(&d.ctx, nullptr); d = {}; ++n; }
    for (int i = 0; i < 2; ++i) {
        FsrEye* both[2] = { &g_eyes[i], &g_centre[i] };
        for (FsrEye* e : both) if (e->ctx) { g_destroy(&e->ctx, nullptr); e->ctx = nullptr; e->ow = e->oh = 0; ++n; }
    }
    return n;
}

bool FsrEnsure(FsrEye* e, int outW, int outH, int eye, int maxRenderW, int maxRenderH) {
    // internal buffers follow maxRenderSize, so size it for the largest render (dynamic resolution maximum or the
    // quality's size), not the output (379 MB per eye at 2448x2672 when sized for the output); 0 = the output
    const int mw = maxRenderW > 0 ? std::min(maxRenderW, outW) : outW, mh = maxRenderH > 0 ? std::min(maxRenderH, outH) : outH;
    if (!g_ok) return false;
    uint64_t verId = g_overrideId.load(std::memory_order_relaxed);
    const int expMode = g_expMode.load(std::memory_order_relaxed);
    if (e->ctx && e->ow == outW && e->oh == outH && e->mw == mw && e->mh == mh && e->verId == verId && e->expMode == expMode) return true;
    if (e->fails && g_frame - e->failFrame < 180) return false;
    if (e->ctx) { Bury(e->ctx); e->ctx = nullptr; }

    ffxCreateBackendDX12Desc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.device = g_dev;
    ffxCreateContextDescUpscaleVersion ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;
    ffxOverrideVersion ov = {};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = verId;
    ffxCreateContextDescUpscale cd = {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    // scene colour is linear HDR; UE uses reversed, infinite-far depth; quality changes vary the render size
    cd.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
               FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION | (expMode == 1 ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0) |
               (g_cfg.outputProbe && !g_noDebugCheck ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0);   // FSR reports bad inputs through FsrMessage
    cd.maxRenderSize = { (uint32_t)mw, (uint32_t)mh };
    cd.maxUpscaleSize = { (uint32_t)outW, (uint32_t)outH };
    cd.fpMessage = FsrMessage;
    cd.header.pNext = &be.header;
    be.header.pNext = &ver.header;
    ver.header.pNext = verId ? &ov.header : nullptr;
    ffxReturnCode_t r = g_create(&e->ctx, &cd.header, nullptr);
    // test extras must never cost the upscaler: a version that refuses the input checks gets a context without them,
    // and a version chosen with the [Test] key that does not create at all gives way to the default
    if ((r != FFX_API_RETURN_OK || !e->ctx) && (cd.flags & FFX_UPSCALE_ENABLE_DEBUG_CHECKING)) {
        e->ctx = nullptr;
        cd.flags &= ~(uint32_t)FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
        const ffxReturnCode_t r2 = g_create(&e->ctx, &cd.header, nullptr);
        if (r2 == FFX_API_RETURN_OK && e->ctx) {   // the checks were the cause: leave them out from now on
            g_noDebugCheck = true;
            Log("FSR: create context eye=%d with FSR's input checks failed (%u), without them it works: input checks off", eye, (unsigned)r);
        } else cd.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;   // some other cause (memory, version): keep the checks
        r = r2;
    }
    if ((r != FFX_API_RETURN_OK || !e->ctx) && verId && g_verByKey.load()) {
        Log("FSR: create context eye=%d with the version chosen by key (id %llu) failed (%u): back to the default version", eye,
            (unsigned long long)verId, (unsigned)r);
        for (int i = 0; i < g_verCount; ++i) if (g_verIds[i] == verId) g_verFailed.fetch_or(1u << i);   // F2 skips it
        g_overrideId = 0; g_verByKey = false; g_announceVer = false; verId = 0; e->ctx = nullptr;
        ver.header.pNext = nullptr;
        Announce(Tone::Change, "That F S R version failed. Back to the default.");
        r = g_create(&e->ctx, &cd.header, nullptr);
    }
    if (r != FFX_API_RETURN_OK || !e->ctx) {
        if (e->fails < 3 || (e->fails % 50) == 0) Log("FSR: create context eye=%d %dx%d failed (%u), retrying every 2 s", eye, outW, outH, (unsigned)r);
        e->ctx = nullptr; ++e->fails; e->failFrame = g_frame; return false;
    }
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    g_query(&e->ctx, &pv.header);
    static const char* expNames[3] = { "game exposure", "FSR auto exposure", "no exposure" };
    Log("FSR: create context eye=%d out=%dx%d max render %dx%d (%s) -> ok, running '%s'", eye, outW, outH, mw, mh,
        expNames[expMode % 3], pv.versionName ? pv.versionName : "?");
    strncpy_s(g_running, pv.versionName ? pv.versionName : "?", _TRUNCATE);
    if (g_announceVer.exchange(false)) {
        char spoken[96] = {}; size_t k = 0;   // "4.1.2" -> "4 point 1 point 2" (the voice may read it as a date)
        for (const char* c = g_running; *c && k + 8 < sizeof(spoken); ++c)
            if (*c == '.') { memcpy(spoken + k, " point ", 7); k += 7; } else spoken[k++] = *c;
        Announce(Tone::Change, "F S R %s running.", spoken);
    }
    e->ow = outW; e->oh = outH; e->mw = mw; e->mh = mh; e->lastFrame = 0; e->fails = 0; e->createdFrame = g_frame;
    e->verId = verId; e->expMode = expMode;
    return true;
}

// "no exposure" ([Test] key, mode 2): a 1x1 texture holding 1.0. FSR 4 refuses a dispatch with no exposure texture
// unless its auto exposure is on (error 6); FSR 3.1 takes a null one as 1.
static ID3D12Resource* g_one = nullptr;
static ID3D12Resource* g_oneUp = nullptr;
static bool g_oneFailed = false;
static ID3D12Resource* OneExposure(ID3D12GraphicsCommandList* cl) {
    if (g_one || g_oneFailed) return g_one;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT }, hu = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = 1; td.Height = 1; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT; td.SampleDesc.Count = 1;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *t = nullptr, *u = nullptr; void* p = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t))) ||
        FAILED(g_dev->CreateCommittedResource(&hu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&u))) ||
        FAILED(u->Map(0, nullptr, &p))) {
        if (t) t->Release();
        if (u) u->Release();
        g_oneFailed = true; Log("FSR: the exposure-1 texture could not be made (\"no exposure\" passes none)");
        return nullptr;
    }
    const float one = 1.f; memcpy(p, &one, 4); u->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = t; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = u; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 256 };
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = t; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &b);
    g_one = t; g_oneUp = u;   // both kept for the session (2 small allocations)
    return g_one;
}

bool FsrDispatch(ID3D12GraphicsCommandList* cl, FsrEye* e, const NativeFrame& f,
                 ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* mv, ID3D12Resource* reactive, ID3D12Resource* output,
                 int rw, int rh, int ow, int oh, float fovY) {
    if (!e->ctx) return false;
    const bool stale = e->lastFrame + 1 != g_frame;   // first dispatch, or FSR was not used last frame
    if (stale && g_cfg.outputProbe) {
        static int logs = 2000;
        if (logs-- > 0) Log("FSR history reset eye=%d: %s", f.eye, e->lastFrame ? "not used in the last frame" : "new context");
    }
    e->lastFrame = g_frame;

    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cl;
    d.color = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(mv, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    if (e->expMode == 0 && f.exposure) d.exposure = ffxApiGetResourceDX12(f.exposure, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    if (e->expMode == 2) if (ID3D12Resource* one = OneExposure(cl)) d.exposure = ffxApiGetResourceDX12(one, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    if (reactive) d.reactive = ffxApiGetResourceDX12(reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ);   // translucency mask
    d.output = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = { f.jitterX * g_cfg.fsrJitterSignX, f.jitterY * g_cfg.fsrJitterSignY };
    d.motionVectorScale = { 1.f, 1.f };                       // vectors are already in render pixels
    // sizes: the eye's, or the fovea window's (its inputs are crops starting at (0,0); vectors in render pixels
    // stay valid in a crop)
    d.renderSize = { (uint32_t)(rw > 0 ? rw : f.renderW), (uint32_t)(rh > 0 ? rh : f.renderH) };
    d.upscaleSize = { (uint32_t)(ow > 0 ? ow : f.outW), (uint32_t)(oh > 0 ? oh : f.outH) };
    d.enableSharpening = g_cfg.fsrSharpness > 0.f;
    d.sharpness = g_cfg.fsrSharpness;
    d.frameTimeDelta = g_frameMs;
    d.preExposure = f.preExposure > 0.f ? f.preExposure : 1.f;
    d.reset = f.reset || stale;
    d.cameraNear = f.cameraNear > 0.f ? f.cameraNear : 10.f;
    d.cameraFar = 1.0e7f;                                     // infinite far plane (flag set at creation)
    d.cameraFovAngleVertical = fovY > 0.f ? fovY : f.fovY > 0.f ? f.fovY : 1.6f;
    d.viewSpaceToMetersFactor = 0.01f;                        // Unreal units are centimetres
    const ffxReturnCode_t r = g_dispatch(&e->ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int left = 16;
        if (left-- > 0) Log("FSR: dispatch eye=%d failed (%u)", f.eye, (unsigned)r);
        // an exposure mode chosen with the [Test] key that FSR refuses falls back to the game's exposure (without this,
        // testing showed every dispatch failing until the upscaler was turned off)
        int m = e->expMode;
        if (m != 0 && g_expMode.compare_exchange_strong(m, 0)) {
            Log("FSR: exposure mode %d failed: back to the game's exposure", m);
            Announce(Tone::Change, "That exposure mode failed. Back to game exposure.");
        }
        return false;
    }
    return true;
}

int FsrExposureMode() { return g_expMode.load(); }
void FsrSetExposureMode(int m) { g_expMode = ((m % 3) + 3) % 3; }
const char* FsrRunningName() { return g_running; }

bool FsrNextVersion(char* name, size_t n) {
    if (name && n) name[0] = 0;
    if (g_verCount <= 0) return false;
    // current position: the version chosen by key or by [FSR] Version, else the default (index 0, also after a
    // key-chosen version failed and fell back)
    const int cur = (g_verByKey.load() || g_overrideId.load() != 0) ? g_verIdx : 0;
    const uint32_t failed = g_verFailed.load();
    int next = -1;
    for (int k = 1; k < g_verCount && next < 0; ++k) {
        const int i = (cur + k) % g_verCount;
        if (strncmp(g_verNames[i], "2.", 2) != 0 && !(failed & (1u << i))) next = i;   // skip FSR 2
    }
    if (next < 0) { if (name) strncpy_s(name, n, g_verNames[cur], _TRUNCATE); return false; }
    g_verIdx = next;
    g_overrideId = g_verIds[next];
    g_verByKey = true;
    g_announceVer = true;
    if (name) strncpy_s(name, n, g_verNames[next], _TRUNCATE);
    Log("FSR: version '%s' requested (id %llu); the contexts are rebuilt on their next use", g_verNames[next], (unsigned long long)g_verIds[next]);
    return true;
}
