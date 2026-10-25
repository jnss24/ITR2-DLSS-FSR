// Where CPU-side hitches come from (debug build). The missed frames left are CPU spikes (94 of 95 outside the
// bursts), not GPU load. Two suspects are measured here for the performance CSV:
//   PSO creation: every Create*PipelineState call on the game's device is timed (pso_hook.h), on the game, render or
//     RHI thread (pso_ms / pso_n / pso_max_ms) and on worker threads (pso_bg_ms / pso_bg_n). The engine compiles the
//     PSOs a new material needs on worker threads (r.AsyncPipelineCompile=1, the fork's default) and the draw waits for
//     them (r.SkipDrawOnPSOPrecaching=0 in the game), so worker time can hold a frame up too.
//   Garbage collection: GLastGCFrame changes in the frame a collection ran; its cost is that game frame's interval
//     above the usual one (gc_n / gc_ms).
// The game starts with its pipeline cache paused (r.ShaderPipelineCache.StartupMode=0) but resumes and finishes it in
// the first minute: 5932 PSOs waiting at the first VR frame, 0 a minute later.
// Also kept here (fed by dlss.cpp): ResourceBarrier calls per eye pass, CPU time of NGX's create call on the render
// path and on key 0's side thread, and the eye passes that drew the stand-in.
#include "common.h"
#include "pso_hook.h"
#include <algorithm>

using namespace ue;

static std::atomic<DWORD> g_tid[3] = {};   // game, render, RHI thread
void HitchNoteThread(int kind) {
    if (kind < 0 || kind > 2) return;
    const DWORD t = GetCurrentThreadId();
    if (g_tid[kind].load(std::memory_order_relaxed) != t) g_tid[kind].store(t, std::memory_order_relaxed);
}
static int ThreadKind() {
    const DWORD t = GetCurrentThreadId();
    for (int k = 0; k < 3; ++k) if (g_tid[k].load(std::memory_order_relaxed) == t) return k;
    return -1;
}

// sums since the last CSV row, in microseconds (the perf thread swaps them out)
static std::atomic<uint64_t> g_psoUs{ 0 }, g_psoN{ 0 }, g_psoMaxUs{ 0 }, g_psoBgUs{ 0 }, g_psoBgN{ 0 };
static std::atomic<uint64_t> g_gcN{ 0 }, g_gcUs{ 0 };
static std::atomic<uint64_t> g_barrierCalls{ 0 }, g_barrierEyes{ 0 };
static std::atomic<uint64_t> g_createUs{ 0 };
static std::atomic<uint64_t> g_sideCreateUs{ 0 }, g_standInN{ 0 };   // key 0: the side thread's creates, stand-in eye passes

static void AtomicMax(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v)) {}
}

static void OnPso(double ms, int kind, HRESULT hr) {
    const uint64_t us = (uint64_t)(ms * 1000.0);
    const int tk = ThreadKind();
    if (tk >= 0) { g_psoUs += us; ++g_psoN; AtomicMax(g_psoMaxUs, us); }
    else { g_psoBgUs += us; ++g_psoBgN; }
    // log the ones that take most of a 90 Hz frame
    if (ms >= 8.0) {
        static std::atomic<int> logs{ 300 };
        if (logs.fetch_sub(1) > 0) {
            static const char* kinds[3] = { "graphics", "compute", "stream" };
            static const char* threads[4] = { "a worker", "the game", "the render", "the RHI" };
            Log("hitch: PSO %s created in %.1f ms on %s thread%s", kinds[kind % 3], ms, threads[tk + 1], FAILED(hr) ? " (failed)" : "");
        }
    }
}

void HitchInitDevice(ID3D12Device* dev) {
#ifndef ITR2_RELEASE
    const int n = psohook::Install(dev, &OnPso);
    Log("hitch: PSO creation timer %s (%d of 3 device entries)", n ? "on" : "NOT installed", n);
#else
    (void)dev;
#endif
}

// ---------------------------------------------------------------- garbage collection (game thread, per VR frame)
void HitchOnGameFrame(double frameMs) {
    if (!g_res.addr[I_GLastGCFrame]) return;
    static uint64_t last = 0; static double usual = 0.0; static int logs = 400;
    const uint64_t v = *(volatile const uint64_t*)g_res.addr[I_GLastGCFrame];
    if (!last) { last = v; return; }
    if (v != last) {
        last = v;
        const double extra = usual > 0.0 ? std::max(0.0, frameMs - usual) : 0.0;
        ++g_gcN; g_gcUs += (uint64_t)(extra * 1000.0);
        if (logs-- > 0) Log("hitch: garbage collection (frame %llu): game frame %.1f ms, usual %.1f ms", (unsigned long long)v, frameMs, usual);
        return;
    }
    if (usual <= 0.0) usual = frameMs;
    else if (frameMs < usual * 2.0) usual += 0.02 * (frameMs - usual);
}

// ---------------------------------------------------------------- dlss.cpp
void HitchAddBarriers(uint32_t calls) { g_barrierCalls += calls; ++g_barrierEyes; }
void HitchAddCreate(double ms) { g_createUs += (uint64_t)(ms * 1000.0); }
void HitchAddSideCreate(double ms) { g_sideCreateUs += (uint64_t)(ms * 1000.0); }
void HitchAddStandIn() { ++g_standInN; }

// ---------------------------------------------------------------- perf.cpp (one CSV row)
void HitchTake(HitchRow& r) {
    r.psoMs = g_psoUs.exchange(0) / 1000.0; r.psoN = (uint32_t)g_psoN.exchange(0); r.psoMaxMs = g_psoMaxUs.exchange(0) / 1000.0;
    r.psoBgMs = g_psoBgUs.exchange(0) / 1000.0; r.psoBgN = (uint32_t)g_psoBgN.exchange(0);
    r.gcN = (uint32_t)g_gcN.exchange(0); r.gcMs = g_gcUs.exchange(0) / 1000.0;
    const uint64_t calls = g_barrierCalls.exchange(0), eyes = g_barrierEyes.exchange(0);
    r.barriersPerEye = eyes ? (double)calls / (double)eyes : -1.0;
    r.createMs = g_createUs.exchange(0) / 1000.0;
    r.sideCreateMs = g_sideCreateUs.exchange(0) / 1000.0; r.standInN = (uint32_t)g_standInN.exchange(0);
}
