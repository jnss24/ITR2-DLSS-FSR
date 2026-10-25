// Performance log. Game / render / RHI threads only accumulate (a lock and a few additions per frame); a
// background thread writes one CSV row every PerfIntervalMs and a summary line to itr2_dlss.log every
// PerfSummarySeconds. Each row says whether DLSS was ON, OFF or MIX.
#include "common.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <mutex>
#include <vector>
#include <algorithm>
#include <string>
#include <share.h>
#include <cstdarg>

static SRWLOCK g_lock = SRWLOCK_INIT;
static double g_qpcToMs = 0.0;

struct Acc {
    uint32_t frames = 0, onFrames = 0, offFrames = 0;
    double sumMs = 0, minMs = 1e9, maxMs = 0;
    double gpuMs = 0, gameMs = 0, renderMs = 0, rhiMs = 0; uint32_t engineN = 0;
    double dlssMs[2] = { 0, 0 }; uint32_t dlssN[2] = { 0, 0 };
    double passMs[5] = { 0, 0, 0, 0, 0 }; uint32_t passN = 0;   // per-pass GPU time, both eyes together (per-eye average)
    uint32_t lateGpu = 0, lateCpu = 0, skipped = 0, timingN = 0;   // miss guard (debug build, drs.cpp MissObserve)
    double waitMs = 0, workMs = 0;
    double turnDps = 0, moveMps = 0, turnMax = 0; uint32_t camN = 0;   // camera motion (drs.cpp DrsObserveCamera)
};
static Acc g_acc;
static std::vector<float> g_window;         // frame times since the last summary (game thread appends)
static int64_t g_lastFrameQpc = 0;
static std::atomic<int> g_renderW{ 0 }, g_renderH{ 0 }, g_outW{ 0 }, g_outH{ 0 };

static inline double CyclesToMs(uint32_t c) { return c * g_qpcToMs; }
static inline uint32_t ReadU32(int item) { return g_res.addr[item] ? *(volatile uint32_t*)g_res.addr[item] : 0; }

void PerfOnGameFrame(bool dlssActive) {   // game thread, once per VR frame (main stereo family)
    if (!g_cfg.perfEnabled || g_qpcToMs == 0.0) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const int64_t prev = g_lastFrameQpc; g_lastFrameQpc = now.QuadPart;
    if (!prev) return;
    const double ms = (now.QuadPart - prev) * g_qpcToMs;
    if (ms <= 0.0 || ms > 1000.0) return;   // skip loading hitches > 1 s (still counted as a gap)
#ifndef ITR2_RELEASE
    HitchNoteThread(0);
    HitchOnGameFrame(ms);   // hitch.cpp: did a garbage collection run in this frame?
#endif
    const uint32_t gpu = ReadU32(ue::I_GGPUFrameTime), gt = ReadU32(ue::I_GGameThreadTime),
                   rt = ReadU32(ue::I_GRenderThreadTime), rhi = ReadU32(ue::I_GRHIThreadTime);
    AcquireSRWLockExclusive(&g_lock);
    Acc& a = g_acc;
    ++a.frames; (dlssActive ? a.onFrames : a.offFrames)++;
    a.sumMs += ms; a.minMs = std::min(a.minMs, ms); a.maxMs = std::max(a.maxMs, ms);
    a.gpuMs += CyclesToMs(gpu); a.gameMs += CyclesToMs(gt); a.renderMs += CyclesToMs(rt); a.rhiMs += CyclesToMs(rhi); ++a.engineN;
    if (g_window.size() < 100000) g_window.push_back((float)ms);
    ReleaseSRWLockExclusive(&g_lock);
#ifndef ITR2_RELEASE
    StressOnFrame((float)ms, (float)CyclesToMs(gpu), (float)CyclesToMs(gt), (float)CyclesToMs(rt));   // stress.cpp (same thread)
#endif
}

void PerfOnFrameTiming(int lateKind, int skipped, float waitMs, float workMs) {   // game thread (debug build)
    if (!g_cfg.perfEnabled) return;
    AcquireSRWLockExclusive(&g_lock);
    Acc& a = g_acc;
    if (lateKind == 1) ++a.lateGpu; else if (lateKind == 2) ++a.lateCpu;
    a.skipped += (uint32_t)std::max(0, skipped); a.waitMs += waitMs; a.workMs += workMs; ++a.timingN;
    ReleaseSRWLockExclusive(&g_lock);
#ifndef ITR2_RELEASE
    StressOnTiming(lateKind, skipped);
#endif
}

void PerfOnCamera(float turnDps, float moveMps) {   // game thread (debug build)
    if (!g_cfg.perfEnabled) return;
    AcquireSRWLockExclusive(&g_lock);
    Acc& a = g_acc;
    a.turnDps += turnDps; a.moveMps += moveMps; a.turnMax = std::max(a.turnMax, (double)turnDps); ++a.camN;
    ReleaseSRWLockExclusive(&g_lock);
}

void PerfOnRenderSize(int rw, int rh, int ow, int oh) { g_renderW = rw; g_renderH = rh; g_outW = ow; g_outH = oh; }

void PerfGetRenderSize(int& rw, int& rh, int& ow, int& oh) { rw = g_renderW; rh = g_renderH; ow = g_outW; oh = g_outH; }

void PerfOnPassGpu(int eye, const double* seg) {   // RHI thread: motion pass, whole eye, centre, edge AA, final
    if (!g_cfg.perfEnabled || eye < 0 || eye > 1) return;
    AcquireSRWLockExclusive(&g_lock);
    for (int k = 0; k < 5; ++k) g_acc.passMs[k] += seg[k];
    ++g_acc.passN;
    ReleaseSRWLockExclusive(&g_lock);
}

static std::atomic<int> g_utilRecent{ -1 };       // last NVML GPU use % (size logic's idle test)
int PerfGpuUtilRecent() { return g_utilRecent.load(std::memory_order_relaxed); }
static std::atomic<float> g_dlssRecent{ 0.f };   // adaptive size: recent DLSS GPU ms per eye (smoothed, both eyes)
float PerfDlssRecentMs() { return g_dlssRecent.load(std::memory_order_relaxed); }

void PerfOnDlssGpu(int eye, double ms) {   // RHI thread
    BenchOnDlssGpu(eye, ms);   // the benchmark's own sum (works with the performance log off)
    if (ms > 0.0 && ms < 50.0) {
        const float old = g_dlssRecent.load(std::memory_order_relaxed);
        g_dlssRecent.store(old > 0.f ? 0.97f * old + 0.03f * (float)ms : (float)ms, std::memory_order_relaxed);
    }
    if (!g_cfg.perfEnabled || eye < 0 || eye > 1) return;
    AcquireSRWLockExclusive(&g_lock);
    g_acc.dlssMs[eye] += ms; ++g_acc.dlssN[eye];
    ReleaseSRWLockExclusive(&g_lock);
}

// ---------------------------------------------------------------- NVML (GPU usage), loaded dynamically
struct NvmlUtil { unsigned int gpu, memory; };
struct NvmlMem { unsigned long long total, free, used; };
using PFN_nvmlInit = int(*)();
using PFN_nvmlGetHandle = int(*)(unsigned int, void**);
using PFN_nvmlGetUtil = int(*)(void*, NvmlUtil*);
using PFN_nvmlGetMem = int(*)(void*, NvmlMem*);
using PFN_nvmlGetName = int(*)(void*, char*, unsigned int);
static PFN_nvmlGetUtil g_nvmlUtil = nullptr;
static PFN_nvmlGetMem g_nvmlMem = nullptr;
static void* g_nvmlDev = nullptr;

static void InitNvml() {
    wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat_s(p, L"\\nvml.dll");
    HMODULE m = LoadLibraryW(p);
    if (!m) { Log("perf: nvml.dll not found (GPU usage column will be empty)"); return; }
    auto init = (PFN_nvmlInit)GetProcAddress(m, "nvmlInit_v2");
    auto handle = (PFN_nvmlGetHandle)GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2");
    auto name = (PFN_nvmlGetName)GetProcAddress(m, "nvmlDeviceGetName");
    g_nvmlUtil = (PFN_nvmlGetUtil)GetProcAddress(m, "nvmlDeviceGetUtilizationRates");
    g_nvmlMem = (PFN_nvmlGetMem)GetProcAddress(m, "nvmlDeviceGetMemoryInfo");
    if (!init || !handle || !g_nvmlUtil || init() != 0 || handle(0, &g_nvmlDev) != 0) {
        Log("perf: NVML init failed (GPU usage column will be empty)"); g_nvmlUtil = nullptr; return;
    }
    char n[96] = {}; if (name) name(g_nvmlDev, n, sizeof(n));
    Log("perf: NVML device 0 = %s", n);
}

// ---------------------------------------------------------------- DXGI (process VRAM)
static IDXGIAdapter3* g_adapter = nullptr;
static void InitDxgi() {
    void* rhi = GetDynamicRHI();
    auto dev = rhi ? VCall<ID3D12Device*>(rhi, ue::slot::D3D12RHI_GetDevice, 0u) : nullptr;
    HMODULE dx = GetModuleHandleW(L"dxgi.dll");
    using PFN_Create = HRESULT(WINAPI*)(REFIID, void**);
    auto create = dx ? (PFN_Create)GetProcAddress(dx, "CreateDXGIFactory1") : nullptr;
    IDXGIFactory4* f = nullptr;
    if (!dev || !create || FAILED(create(IID_PPV_ARGS(&f))) || !f) { Log("perf: DXGI not available (VRAM columns empty)"); return; }
    if (FAILED(f->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&g_adapter)))) g_adapter = nullptr;
    f->Release();
    if (!g_adapter) Log("perf: adapter not found (VRAM columns empty)");
}
static std::once_flag g_dxgiOnce;

// the game's VRAM use and the budget Windows gives it (also used by dynamic resolution, so it works with the
// performance log off); any thread once the engine is up
bool VramQuery(float& usedMb, float& budgetMb) {
    std::call_once(g_dxgiOnce, InitDxgi);
    DXGI_QUERY_VIDEO_MEMORY_INFO vm{};
    if (!g_adapter || FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm))) return false;
    usedMb = (float)(vm.CurrentUsage / 1048576.0); budgetMb = (float)(vm.Budget / 1048576.0);
    return true;
}
// the game is over its local VRAM budget (on above the budget, off again 256 MB under it); checked at most every
// 0.25 s. While on, the low-memory path releases every model / texture the current frame doesn't use at once
// instead of after ~10 s idle. Releasing one unused periphery model (-1.6 GB) ended the worst collapse seen
// (10.9 -> 50-67 fps). Never changes the resolution.
bool VramOverBudget() {
    static std::atomic<bool> over{ false };
    static std::atomic<uint64_t> next{ 0 };
    static std::atomic<int> logs{ 40 };
    const uint64_t now = GetTickCount64();
    uint64_t due = next.load(std::memory_order_relaxed);
    if (now < due || !next.compare_exchange_strong(due, now + 250)) return over.load(std::memory_order_relaxed);
    float used = 0.f, budget = 0.f;
    if (!VramQuery(used, budget) || budget <= 0.f) return over.load(std::memory_order_relaxed);
    const bool was = over.load(std::memory_order_relaxed);
    const bool is = was ? used > budget - 256.f : used > budget;
    if (is != was) {
        over = is;
        if (logs.fetch_sub(1) > 0)
            Log(is ? "VRAM: over budget (%.0f of %.0f MB): unused models and textures are released at once"
                   : "VRAM: back under budget (%.0f of %.0f MB): unused models are kept for ~10 s again", used, budget);
    }
    return is;
}
// same for the NON_LOCAL segment group (system memory the GPU reaches over PCIe): upload/readback heaps and
// resources Windows demotes there when the local budget overflows (diagnostic)
bool VramQueryNonLocal(float& usedMb, float& budgetMb) {
    std::call_once(g_dxgiOnce, InitDxgi);
    DXGI_QUERY_VIDEO_MEMORY_INFO vm{};
    if (!g_adapter || FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &vm))) return false;
    usedMb = (float)(vm.CurrentUsage / 1048576.0); budgetMb = (float)(vm.Budget / 1048576.0);
    return true;
}

// ---------------------------------------------------------------- background loop
static char g_row[1536]; static size_t g_rowLen = 0;
static void Row(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(g_row + g_rowLen, sizeof(g_row) - g_rowLen, fmt, ap);
    va_end(ap);
    if (n > 0) g_rowLen += (size_t)n;
}
static void RowFlush(FILE* f) {
    for (size_t i = 0; i < g_rowLen; ++i) if (g_row[i] == '.') g_row[i] = ',';
    fwrite(g_row, 1, g_rowLen, f); fflush(f); g_rowLen = 0;
}

static const char* StateOf(const Acc& a) { return a.onFrames && a.offFrames ? "MIX" : a.onFrames ? "ON" : "OFF"; }

void PerfRunLoop() {
    if (!g_cfg.perfEnabled) return;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    g_qpcToMs = 1000.0 / (double)f.QuadPart;
    InitNvml();
    std::call_once(g_dxgiOnce, InitDxgi);

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sitr2_dlss_perf_%04d%02d%02d_%02d%02d%02d.csv", g_logDir[0] ? g_logDir : g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
#ifdef ITR2_RELEASE
    // release build: keep the 4 newest CSVs + this one (the debug build keeps all)
    if (const int gone = KeepNewestFiles(g_logDir[0] ? g_logDir : g_dir, L"itr2_dlss_perf_*.csv", 4)) Log("perf: deleted %d old performance CSV(s), the 5 newest are kept", gone);
#endif
    FILE* csv = _wfsopen(path, L"w", _SH_DENYNO);
    if (!csv) { Log("perf: cannot create CSV"); return; }
    fputs("time_s;dlss;mode;quality;preset;fovea;drs;tweaks;render_w;render_h;out_w;out_h;fps;frame_ms_avg;frame_ms_min;frame_ms_max;"
          "gpu_ms;game_thread_ms;render_thread_ms;rhi_thread_ms;dlss_gpu_ms_eye0;dlss_gpu_ms_eye1;"
          "gpu_util_pct;vram_process_mb;vram_budget_mb;vram_total_used_mb;vram_nonlocal_mb;vram_nonlocal_budget_mb;out_scale_pct;out_raise_block;"
          "pass_motion_ms;pass_eye_ms;pass_centre_ms;pass_edge_aa_ms;pass_final_ms"
#ifndef ITR2_RELEASE
          // miss guard (debug build): late frames by cause, refreshes the headset skipped, the game thread's wait for the
          // headset (xrWaitFrame) and its own work, the GPU budget dynamic resolution steered on, guard on/off (key 3)
          ";late_gpu;late_cpu;headset_skipped;headset_wait_ms;game_work_ms;drs_budget_ms;miss_guard"
          // camera turn (mean / max, degrees per second) and movement (m/s), the share of the GPU time the cost
          // model says scales with the render area, the refresh rate the budget is based on
          ";cam_turn_dps;cam_turn_max_dps;cam_move_mps;drs_scaling_share;target_hz;drs_size_pct"
          // stress test (stress.cpp): the plan's step, 0 = warm-up, empty = not running
          ";stress_step"
          // hitch sources (hitch.cpp): PSO creation on the game / render / RHI threads (total, count, longest) and on
          // worker threads, garbage collections and their extra frame time, ResourceBarrier calls per eye pass, CPU time in
          // NGX's create call (DLSS model rebuilds); key 0: the side thread's create calls and the eye passes that drew the
          // stand-in (plain edges over the centre) meanwhile
          ";pso_ms;pso_n;pso_max_ms;pso_bg_ms;pso_bg_n;gc_n;gc_ms;barriers_eye;create_ms;side_create_ms;standin_n"
#endif
          "\n", csv);
    fflush(csv);
    char nameA[MAX_PATH]; WideCharToMultiByte(CP_UTF8, 0, path, -1, nameA, MAX_PATH, nullptr, nullptr);
    Log("perf: writing %s every %d ms, summary every %d s", nameA, g_cfg.perfIntervalMs, g_cfg.perfSummarySeconds);

    LARGE_INTEGER t0, last; QueryPerformanceCounter(&t0); last = t0;
    double summaryClock = 0; uint32_t sumOn = 0, sumOff = 0;
    for (;;) {
        Sleep((DWORD)g_cfg.perfIntervalMs);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        const double elapsedMs = (now.QuadPart - last.QuadPart) * g_qpcToMs; last = now;
        Acc a;
        AcquireSRWLockExclusive(&g_lock); a = g_acc; g_acc = Acc{}; ReleaseSRWLockExclusive(&g_lock);

        NvmlUtil u{}; const bool haveUtil = g_nvmlUtil && g_nvmlUtil(g_nvmlDev, &u) == 0;
        g_utilRecent.store(haveUtil ? (int)u.gpu : -1, std::memory_order_relaxed);
        NvmlMem nm{}; const bool haveMem = g_nvmlMem && g_nvmlMem(g_nvmlDev, &nm) == 0;
        DXGI_QUERY_VIDEO_MEMORY_INFO vm{}; const bool haveVm = g_adapter && SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm));
        DXGI_QUERY_VIDEO_MEMORY_INFO nl{}; const bool haveNl = g_adapter && SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nl));
        const double n = a.engineN ? (double)a.engineN : 1.0;
        const bool on = a.onFrames > 0;

        Row("%.3f;%s;%d;%s;%s;%s;", (now.QuadPart - t0.QuadPart) * g_qpcToMs / 1000.0, StateOf(a), g_cfg.mode,
            QualityName(LiveQuality()), PresetName(LivePreset()), g_liveFovea.load() ? PresetName(g_livePeriph.load()) : "off");
        if (g_liveDrs.load() && DrsCurrentPercent() > 0.f) Row("%.0f;", DrsCurrentPercent()); else Row("off;");
        { char tw[160]; TweaksState(tw, sizeof(tw)); Row("%s;", tw); }
        if (on) Row("%d;%d;%d;%d;", g_renderW.load(), g_renderH.load(), g_outW.load(), g_outH.load());
        else Row(";;;;");
        if (a.frames) Row("%.2f;%.3f;%.3f;%.3f;", a.frames * 1000.0 / elapsedMs, a.sumMs / a.frames, a.minMs, a.maxMs);
        else Row("0;;;;");
        if (a.engineN) Row("%.3f;%.3f;%.3f;%.3f;", a.gpuMs / n, a.gameMs / n, a.renderMs / n, a.rhiMs / n);
        else Row(";;;;");
        for (int e = 0; e < 2; ++e) { if (a.dlssN[e]) Row("%.3f;", a.dlssMs[e] / a.dlssN[e]); else Row(";"); }
        if (haveUtil) Row("%u;", u.gpu); else Row(";");
        if (haveVm) Row("%.0f;%.0f;", vm.CurrentUsage / 1048576.0, vm.Budget / 1048576.0); else Row(";;");
        if (haveMem) Row("%.0f;", nm.used / 1048576.0); else Row(";");
        if (haveNl) Row("%.0f;%.0f;", nl.CurrentUsage / 1048576.0, nl.Budget / 1048576.0); else Row(";;");
        // output scale (% of the game's resolution setting) and what blocks a raise (drs.cpp tags), DRS on only
        { char blk[96] = ""; if (g_liveDrs.load()) DrsBlockString(blk, sizeof(blk)); Row("%.0f;%s;", OutputScale(), blk); }
        // per-pass GPU time per eye (average of both eyes): motion vectors (+ copies / crops), whole-eye upscaler, centre
        // window, edge AA, final (compose / blend / sharpen / fallback)
        if (a.passN) Row("%.3f;%.3f;%.3f;%.3f;%.3f", a.passMs[0] / a.passN, a.passMs[1] / a.passN, a.passMs[2] / a.passN,
                         a.passMs[3] / a.passN, a.passMs[4] / a.passN);
        else Row(";;;;");
#ifndef ITR2_RELEASE
        if (a.timingN) Row(";%u;%u;%u;%.3f;%.3f;", a.lateGpu, a.lateCpu, a.skipped, a.waitMs / a.timingN, a.workMs / a.timingN);
        else Row(";;;;;;");
        if (g_liveDrs.load() && DrsCurrentPercent() > 0.f) Row("%.2f;", DrsBudgetMs()); else Row(";");
        Row("%d;", (int)g_liveMissGuard.load());
        if (a.camN) Row("%.1f;%.1f;%.2f;", a.turnDps / a.camN, a.turnMax, a.moveMps / a.camN); else Row(";;;");
        if (g_liveDrs.load() && DrsCurrentPercent() > 0.f) Row("%.2f;", DrsScalingShare()); else Row(";");
        Row("%.0f;", TargetHz());
        if (DrsFeatureSizeNow() > 0.f) Row("%.1f", DrsFeatureSizeNow());   // adaptive DLSS size (DRS on only)
        if (StressActive()) Row(";%d", StressStep()); else Row(";");       // stress test step (stress.cpp; 0 = warm-up)
        {
            HitchRow h; HitchTake(h);
            Row(";%.3f;%u;%.3f;%.3f;%u;%u;%.3f;", h.psoMs, h.psoN, h.psoMaxMs, h.psoBgMs, h.psoBgN, h.gcN, h.gcMs);
            if (h.barriersPerEye >= 0.0) Row("%.1f", h.barriersPerEye);
            Row(";%.3f;%.3f;%u", h.createMs, h.sideCreateMs, h.standInN);
        }
#endif
        Row("\n");
        RowFlush(csv);

        // summary line with 1% low over the last PerfSummarySeconds
        sumOn += a.onFrames; sumOff += a.offFrames;
        summaryClock += elapsedMs;
        if (summaryClock >= g_cfg.perfSummarySeconds * 1000.0) {
            std::vector<float> w;
            AcquireSRWLockExclusive(&g_lock); w.swap(g_window); ReleaseSRWLockExclusive(&g_lock);
            if (!w.empty()) {
                double sum = 0; for (float x : w) sum += x;
                std::sort(w.begin(), w.end());
                const float p99 = w[std::min(w.size() - 1, (size_t)(w.size() * 0.99))];
                Log("PERF %s: fps avg=%.1f 1%%low=%.1f | frame avg=%.2fms p99=%.2fms max=%.2fms | GPU util=%s VRAM proc=%.0fMB non-local=%.0fMB | frames=%zu",
                    sumOn && sumOff ? "MIX" : sumOn ? "DLSS=ON" : "DLSS=OFF", 1000.0 * w.size() / sum, 1000.0 / p99,
                    sum / w.size(), p99, w.back(), haveUtil ? std::to_string(u.gpu).append("%").c_str() : "n/a",
                    haveVm ? vm.CurrentUsage / 1048576.0 : -1.0, haveNl ? nl.CurrentUsage / 1048576.0 : -1.0, w.size());
            }
            summaryClock = 0; sumOn = sumOff = 0;
        }
    }
}
