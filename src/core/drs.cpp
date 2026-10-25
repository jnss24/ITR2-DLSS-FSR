// drs.cpp - dynamic resolution: steers the render size (screen percentage) so the GPU frame time stays at
// [DynamicRes] Headroom of the headset's frame budget. Game thread, once per VR frame while active.
// Every IntervalFrames frames (or 0.2 s, whichever comes first) it takes the 75th percentile of the engine's GPU
// frame time (GGPUFrameTime) and feeds the two stages in drs_logic.h: the render percentage (fine, hitch-free)
// and, when that is stuck at its floor or ceiling, the output size (slow: the game's own resolution setting,
// xr.SecondaryScreenPercentage.HMDRenderTarget). Every frame it also checks for a collapse (emergency step).
// The output size is also what key 8 sets by hand; both go through OutputSetScale below.
#include "common.h"
#include "drs_logic.h"
#include "profile_logic.h"
#include <algorithm>
#include <cmath>

static float g_pct = 0.f;          // 0 = not running
static double g_warmUntil = 0.0;   // ignore the GPU times until then (start, output changes: rebuild frames)
static float g_win[128];
static int g_n = 0;
static double g_winStart = 0.0;
static double g_cyclesToMs = 0.0;
static int g_logsLeft = 300;
static drs::RenderState g_rs;
// every frame's GPU time for the last ~13 s (the output stage looks at a longer window and at spikes)
struct Sample { double t; float ms; float pct; };   // pct: the render % in effect (size-step measurement)
static Sample g_hist[1200];
static int g_histHead = 0, g_histCount = 0;
static double g_lastSpike = -1e9;               // last frame whose GPU time exceeded the headset's frame time
static float g_recent[6]; static int g_recentN = 0, g_recentHead = 0;   // the last 6 frames (emergency check)
// key 3 with the miss guard only (off = the plain controller above)
static drs::CostModel g_model;                  // GPU time per render area, learned from the controller's own steps
static std::atomic<float> g_share{ 0.f };       // ... its scaling share at the last interval (CSV)
static int g_stale = 0;                         // frames still measuring the previous size after a change
static bool g_guardLive = false;                // GPU-caused late frames feed the guard only while DRS runs past its warm-up
static double g_lastMoving = -1e9;              // last frame the camera turned / moved fast
static double g_learnFrom = 0.0;                // the cost model learns from steps only after this (steady running)
static void Changed() { g_stale = 3; }          // the new size reaches the next frame, its GPU time 1-2 frames later

// ---------------------------------------------------------------- output size (stage 2 and key 8)
// The game's resolution menu writes this cvar with a console command (console priority), so the mod must
// write it the same way. Its value when the mod is not changing it is "the game's value" (g_outBase); the mod
// works in percent of that. If the cvar holds anything other than what the mod last wrote, the player changed
// the menu: that becomes the new game value.
static void* g_outCv = nullptr;
static bool g_outInit = false;
static float g_outBase = 0.f;                    // the game's own value
static float g_outWritten = -1.f;                // what the mod last wrote (-1 = nothing in effect)
static std::atomic<float> g_outScale{ 100.f };   // percent of g_outBase in effect (read by the CSV thread)
static bool g_outByDrs = false;                  // the controller moved it: switching DRS off puts it back
static drs::OutputState g_os;
static bool g_bdReset = false;                   // DrsReset: the "raise blocked" diagnostics start over

static double NowS() { return (double)GetTickCount64() / 1000.0; }
static float g_resumeScale = 100.f;            // the controller's output when DRS / DLSS was switched off (resumed)
static float g_lastPct = 0.f;                   // render % when DRS / DLSS was switched off (resumed 10 points under)

// ---------------------------------------------------------------- headset refresh rate ([DynamicRes] TargetHz=0 = auto)
// From the OpenXR runtime when it reports it (XR_FB_display_refresh_rate: Oculus/Link, Virtual Desktop...), through the
// game's own UOpenXRBlueprintFunctionLibrary::execGetHMDDisplayRefreshRate; otherwise measured: the 20th percentile
// of the game thread's frame intervals, snapped to a usual headset rate (half-rate reprojection is ignored).
static std::atomic<float> g_targetHz{ 90.f };
static double g_qpcMs = 0.0, g_lastFrameMs = 0.0, g_nextDetect = 0.0;
static float g_intervals[600]; static int g_intHead = 0, g_intCount = 0;
float TargetHz() { return g_targetHz.load(std::memory_order_relaxed); }

static float RuntimeHz() {
    const uintptr_t fn = g_res.addr[ue::I_HmdRefreshRateExec];
    if (!fn) return 0.f;
    alignas(16) uint8_t frame[0x200] = {};   // dummy FFrame: only Code (+0x20) is touched
    float hz = 0.f;
    __try { ((void (*)(void*, void*, void*))fn)(nullptr, frame, &hz); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
    return hz > 30.f && hz < 500.f ? hz : 0.f;
}

// median of the last n frame intervals (ms), 0 if not enough
static float RecentFrameMs(int n) {
    if (n <= 0 || g_intCount < n) return 0.f;
    static float v[600];
    for (int i = 0; i < n; ++i) v[i] = g_intervals[(g_intHead - 1 - i + 600) % 600];
    std::nth_element(v, v + n / 2, v + n);
    return v[n / 2];
}

// ---------------------------------------------------------------- miss guard (debug build): which late frames were the GPU's
// Frames are missed inside the hold band, but missed and on-time rows have the same GPU time and the GPU is idler in
// the missed ones: most are not GPU-bound. The guard needs the cause of each late frame:
//  - the headset's own timeline: xrWaitFrame returns the predicted display time of the next frame; a step of more
//    than one refresh period means the headset skipped refreshes (shown the old frame again). The engine calls it
//    through the variable OpenXRDynamicAPI::xrWaitFrame, only from FOpenXRHMD::OnBeginSimulation_GameThread (game
//    thread), so the mod swaps that pointer on the game thread (no race) and times the call (the headset wait);
//  - the game thread's own work = frame interval - headset wait - wait for the render thread (GGameThreadWaitTime);
//  - the GPU time of the last 3 frames (GGPUFrameTime arrives 1-2 frames late).
// drs_logic.h ClassifyLate decides; GPU-caused late frames feed the guard (drop, raise cap, learned budget). The
// counting and the CSV columns run with the guard on or off (key 3), so both can be compared.
static drs::MissState g_ms;
static double g_afterglowUntil = -1e9;           // GPU spike / its afterglow: late frames until then are not counted
static float g_floorNow = 0.f;                   // the render floor of the last DRS step (no learning there)
static drs::SizeState g_sz;                      // adaptive DLSS size (kept while DRS is off: the A/B resumes it)
static std::atomic<float> g_szSize{ 0.f };       // read by the render thread through DrsFeatureCeiling
static float g_workEma = 0.f;                    // smoothed game-thread work per frame (ms), the size logic's idle test
static double g_lastFastDrop = -1e9;            // last miss-guard / emergency drop (counts against a size probe)
static double g_lastLongFrame = -1e9;           // last frame interval over 100 ms (a load / hitch)
static double g_lastStageDrop = -1e9;           // last drop by the render stage or the emergency step
static double g_lastGuardDrop = -1e9;           // last miss-guard drop (no stage / emergency drop within 0.5 s)
static double g_nextGrowLog = 0.0;              // next "growth check" log line
static double g_nextProfExport = 0.0;           // next DLSS cost / slope sample into the profile
static bool g_profPending = true;               // the active profile changed (DrsTick imports it); true: import at the start
static float g_startPct = 0.f;                  // the render % of the session's first start (0 = not started yet)

// the active performance profile's costs into the size logic (after the steps are (re)built, and on a profile
// switch: values the profile does not have start unknown again, not carried over from the other setup)
static void SizeImportProfile() {
    if (!ProfileActive() || g_sz.n == 0) return;
    for (int i = 0; i < g_sz.n; ++i) {
        g_sz.cost[i] = ProfileGet(prof::DlssName(g_sz.tiers[i]).c_str());
        if (i + 1 < g_sz.n) g_sz.stepCost[i] = ProfileGet(prof::StepName(g_sz.tiers[i], g_sz.tiers[i + 1]).c_str());
    }
    g_sz.stepFactor = ProfileGet("factor");
    // a session starts one step below this setup's best size (from 66.7 % it took 3.3-5 min of four probes to reach
    // 100 %, which fitted with the GPU at 6.1-7.3 ms of 9.44); only before the size changed in this session, and only
    // from a best measured on this setup (not one copied from another)
    int bestN = 0;
    const float best = ProfileGet("size:best", &bestN);
    g_sz.best = bestN > 0 ? best : 0.f;   // a passed step only raises it, so it must start from the profile's
    // steps that failed for GPU load in earlier sessions still wait for a lighter scene (for retryLifeS)
    const double now = NowS();
    for (int i = 0; i + 1 < g_sz.n; ++i) {
        int rn = 0;
        const float r = ProfileGet(prof::RetryName(g_sz.tiers[i], g_sz.tiers[i + 1]).c_str(), &rn);
        g_sz.retryP90[i] = rn > 0 ? r : 0.f;
        g_sz.retryUntil[i] = now + drs::SizeParams{}.retryLifeS;
        if (g_sz.retryP90[i] > 0.f)
            Log("DRS v6.11: DLSS size step %.1f -> %.1f%% failed for GPU load before: it waits for the GPU p90 under %.2f ms",
                g_sz.tiers[i], g_sz.tiers[i + 1], g_sz.retryP90[i]);
    }
    if (bestN > 0 && g_sz.changes <= 1 && g_sz.probeFrom <= 0.f) {
        const float was = g_sz.size, want = drs::SizeStartStep(g_sz, best);
        if (want > was + 0.05f) {
            drs::SizeApply(g_sz, now, want);
            g_sz.measLo = -1;
            g_szSize = want;
            Log("DRS v6.10: DLSS size %.1f -> %.1f%% at the start: one step below this setup's best size %.1f%% (%d samples)",
                was, want, best, bestN);
            // the render starts there too, while it is still the session's first value (a menu: the render stage
            // is frozen there, so it would stay at the first start's Quality until gameplay)
            if (g_startPct > 0.f && std::fabs(g_pct - g_startPct) < 0.25f && g_pct < want - 0.25f) {
                Log("DRS v6.12: render %.0f -> %.0f%% at the start (the profile's start size)", g_pct, want);
                g_pct = want; g_startPct = want; Changed();
            }
        }
    }
}
static int g_gpuMissCount = 0;                  // GPU-caused late frames counted by the guard (a probe's judgement)
static double g_lastGpuMiss = -1e9;             // the last one (a fast drop near it counts against a size probe)
static double g_measFrom = 0.0, g_measUntil = 0.0;   // the frames that measure a size step's cost
static bool g_resumeExact = false;               // A/B: the next (re)start resumes at the last render %, then holds
static float g_resumeSettle = 0.f;
static std::atomic<float> g_budgetMs{ 0.f };
float DrsBudgetMs() { return g_budgetMs.load(std::memory_order_relaxed); }
float DrsScalingShare() { return g_share.load(std::memory_order_relaxed); }
static drs::MissParams MissParamsFromCfg() {
    drs::MissParams p;
    p.count = g_cfg.missCount; p.windowS = g_cfg.missWindowS; p.lockS = g_cfg.missLockS; p.lockMaxS = g_cfg.missLockMaxS;
    return p;
}
// miss guard (both builds; key 3 only in the debug build)
using PFN_xrWaitFrame = int32_t (*)(uint64_t session, const void* info, void* state);
static PFN_xrWaitFrame g_origWaitFrame = nullptr;
static double g_xrWaitMs = 0.0;      // the last call's time inside the runtime
static int64_t g_xrPrevDisplay = 0;  // XrTime (ns) of the previous predicted display time
static int g_xrSkipped = 0;          // refreshes skipped since the last frame was observed
static double g_xrPeriodMs = 0.0;

static int32_t HookWaitFrame(uint64_t session, const void* info, void* state) {
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    const int32_t r = g_origWaitFrame(session, info, state);
    QueryPerformanceCounter(&b);
    g_xrWaitMs = (b.QuadPart - a.QuadPart) * g_qpcMs;
    if (r >= 0 && state) {   // XrFrameState: type, next, predictedDisplayTime +0x10, predictedDisplayPeriod +0x18 (ns)
        const int64_t disp = *(const int64_t*)((const uint8_t*)state + 0x10);
        const int64_t per = *(const int64_t*)((const uint8_t*)state + 0x18);
        if (per > 0) {
            g_xrPeriodMs = per / 1e6;
            if (g_xrPrevDisplay > 0 && disp > g_xrPrevDisplay) {
                const long long steps = (disp - g_xrPrevDisplay + per / 2) / per;
                if (steps > 1) g_xrSkipped += (int)std::min<long long>(steps - 1, 30);
            }
            g_xrPrevDisplay = disp;
        }
    }
    return r;
}

static void HookWaitFrameInstall() {   // game thread, every VR frame (one pointer read)
    if (!g_res.addr[ue::I_XrWaitFrame]) return;
    PFN_xrWaitFrame* var = (PFN_xrWaitFrame*)g_res.addr[ue::I_XrWaitFrame];
    const PFN_xrWaitFrame cur = *var;
    if (!cur || cur == HookWaitFrame) return;
    const bool again = g_origWaitFrame != nullptr;
    g_origWaitFrame = cur;
    *var = HookWaitFrame;
    g_xrPrevDisplay = 0; g_xrSkipped = 0;
    Log("miss guard: xrWaitFrame hooked%s (late frames from the headset's display times)", again ? " again (the engine reloaded OpenXR)" : "");
}

struct MissAcc { int gpu = 0, cpu = 0, skipped = 0, frames = 0; double wait = 0.0, work = 0.0; float gpuAtMiss = 0.f; double start = 0.0; };
static MissAcc g_macc;
#ifdef ITR2_RELEASE
static int g_lateLogs = 30, g_missSummaries = 60;      // players' logs: enough to see what happened
#else
static int g_lateLogs = 150, g_missSummaries = 2000;
#endif

static void MissObserve(double intervalMs) {   // game thread, every VR frame
    HookWaitFrameInstall();
    static float gpuLast[3] = {}; static int head = 0;
    const float gpu = g_res.addr[ue::I_GGPUFrameTime] ? (float)(*(volatile uint32_t*)g_res.addr[ue::I_GGPUFrameTime] * g_qpcMs) : 0.f;
    gpuLast[head] = gpu; head = (head + 1) % 3;
    const float gpuMax3 = std::max(gpuLast[0], std::max(gpuLast[1], gpuLast[2]));
    const float frameMs = 1000.f / TargetHz();
    const bool timeline = g_origWaitFrame && g_xrPrevDisplay > 0;
    const int skipped = g_xrSkipped; g_xrSkipped = 0;
    const float gtWait = g_res.addr[ue::I_GGameThreadWaitTime] ? (float)(*(volatile uint32_t*)g_res.addr[ue::I_GGameThreadWaitTime] * g_qpcMs) : 0.f;
    const float wait = timeline ? (float)g_xrWaitMs : 0.f;
    const float work = std::max(0.f, (float)intervalMs - wait - gtWait);
    // the render and RHI threads' own busy time (stat unit's Draw / RHIT); a late frame they explain is CPU-caused
    const float rtBusy = g_res.addr[ue::I_GRenderThreadTime] ? (float)(*(volatile uint32_t*)g_res.addr[ue::I_GRenderThreadTime] * g_qpcMs) : 0.f;
    const float rhiBusy = g_res.addr[ue::I_GRHIThreadTime] ? (float)(*(volatile uint32_t*)g_res.addr[ue::I_GRHIThreadTime] * g_qpcMs) : 0.f;
    const float renderBusy = std::max(rtBusy, rhiBusy);
    static long long frameNo = 0; ++frameNo;
    if (intervalMs > 100.0) g_lastLongFrame = NowS();   // a load / hitch (the render stage and learning skip 2 s)
    if (intervalMs > 250.0) return;   // loading / pause: not a frame-pacing question
    const bool late = timeline ? skipped > 0 : intervalMs > 1.5 * frameMs;
    const int kind = drs::ClassifyLate(late, gpuMax3, work, frameMs, g_cfg.missGpuFrac, renderBusy);
    const double now = NowS();
    // only while dynamic resolution runs past its warm-up (not with DLSS off, not in the 0.5 s after a (re)start, not
    // during an F11 comparison), so the rebuild hitch of F5/F6/F7/F9 cannot drop the render (events over 2.5 x the
    // frame time are not counted as GPU at all: ClassifyLate). One GPU spike counts once (3 frames). Only while the new
    // controller runs, not during a GPU spike over 1.5 x the frame or the 3 s after it; at the render floor counted but
    // not learned.
    const bool afterglow = drs::MissAfterglow(g_afterglowUntil, now, gpuMax3, frameMs, MissParamsFromCfg());
    const bool newOn = g_liveMissGuard.load(std::memory_order_relaxed);
    const bool atFloor = g_pct > 0.f && g_pct <= g_floorNow + 0.5f;
    // GPU times at misses within 2 s of a load are counted but not learned (they set the budget for gameplay)
    const bool counted = kind == drs::kLateGpu && g_guardLive && newOn && !afterglow &&
                         drs::MissNoteOnce(g_ms, now, gpuMax3, frameNo, !atFloor && now - g_lastLongFrame >= 2.0);
    if (counted) { ++g_gpuMissCount; g_lastGpuMiss = now; }
    PerfOnFrameTiming(kind == drs::kLateEvent ? drs::kLateCpu : kind, skipped, wait, work);
    g_workEma = g_workEma > 0.f ? 0.9f * g_workEma + 0.1f * work : work;
    if (kind != drs::kLateNone && g_lateLogs > 0) {
        --g_lateLogs;
        Log("late frame (%s): interval %.1f ms, headset skipped %d, GPU last 3 %.2f/%.2f/%.2f ms, headset wait %.2f, render-thread wait %.2f, render/RHI busy %.2f/%.2f, game work %.2f ms",
            kind == drs::kLateGpu ? (!g_guardLive ? "GPU, not counted: dynamic resolution not running"
                                     : !newOn ? "GPU, not counted: old controller"
                                     : afterglow ? "GPU, not counted: spike / after a spike"
                                     : !counted ? "GPU, same spike: counted once"
                                     : atFloor ? "GPU, at the floor: counted, not learned" : "GPU")
                                  : kind == drs::kLateEvent ? "event" : "CPU",
            intervalMs, skipped, gpuLast[0], gpuLast[1], gpuLast[2], wait, gtWait, rtBusy, rhiBusy, work);
    }
    // the refresh rate from the headset's own frame period, at once (TargetHz=0 only; usual rates only)
    if (g_cfg.drsTargetHz <= 0.f && g_xrPeriodMs > 0.0) {
        const float hz = drs::SnapHeadsetHz((float)(1000.0 / g_xrPeriodMs));
        if (hz > 0.f && std::fabs(hz - g_targetHz.load()) > 0.5f) {
            Log("target refresh rate: %.0f Hz (from the headset's frame period, xrWaitFrame); frame budget for dynamic resolution %.2f ms",
                hz, 1000.f / hz * g_cfg.drsHeadroom);
            g_targetHz = hz;
        }
    }
    MissAcc& a = g_macc;
    if (a.start <= 0.0) a.start = now;
    ++a.frames; a.wait += wait; a.work += work; a.skipped += skipped;
    if (kind == drs::kLateGpu) { ++a.gpu; a.gpuAtMiss = std::max(a.gpuAtMiss, gpuMax3); }
    else if (kind == drs::kLateCpu || kind == drs::kLateEvent) ++a.cpu;
    if (now - a.start >= 10.0) {
        if ((a.gpu || a.cpu) && g_missSummaries-- > 0)
            Log("miss guard %s: 10 s, late frames GPU %d CPU %d (headset skipped %d%s), headset wait avg %.2f ms, game work avg %.2f ms, GPU at misses up to %.2f ms, budget %.2f ms, guard drops %d",
                g_liveMissGuard.load() ? "ON" : "off", a.gpu, a.cpu, a.skipped, timeline ? "" : ", no headset timeline: by frame interval",
                a.wait / a.frames, a.work / a.frames, a.gpuAtMiss, DrsBudgetMs(), g_ms.drops);
        a = MissAcc{}; a.start = now;
    }
}

// how fast the camera turns and moves (eye 0's view matrix and origin, game thread, every VR frame), for
// the CSV (does the GPU time follow motion in this game?) and the optional [DynamicRes] MotionHold rule
void DrsObserveCamera(const double* rot9, const double* origin3) {
#ifndef ITR2_RELEASE
    static double prevR[9], prevO[3], prevT = 0.0; static bool have = false;
    static float turnEma = 0.f, moveEma = 0.f;
    if (g_qpcMs == 0.0) return;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const double t = q.QuadPart * g_qpcMs / 1000.0;
    if (have) {
        const double dt = t - prevT;
        if (dt > 0.002 && dt < 0.25) {
            const float turn = drs::TurnDegrees(prevR, rot9) / (float)dt;
            const double dx = origin3[0] - prevO[0], dy = origin3[1] - prevO[1], dz = origin3[2] - prevO[2];
            const float move = (float)(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0 / dt);   // cm -> m/s
            turnEma = 0.7f * turnEma + 0.3f * turn; moveEma = 0.7f * moveEma + 0.3f * move;   // a few frames of smoothing
            PerfOnCamera(turn, move);
            if (turnEma > g_cfg.motionTurnDps || moveEma > g_cfg.motionMoveMps) g_lastMoving = NowS();
        }
    }
    for (int i = 0; i < 9; ++i) prevR[i] = rot9[i];
    for (int i = 0; i < 3; ++i) prevO[i] = origin3[i];
    prevT = t; have = true;
#else
    (void)rot9; (void)origin3;
#endif
}

void DrsObserveFrame() {   // game thread, once per VR frame, whether DRS is on or not
    if (g_qpcMs == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcMs = 1000.0 / (double)f.QuadPart; }
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const double t = q.QuadPart * g_qpcMs;
    if (g_lastFrameMs > 0.0) MissObserve(t - g_lastFrameMs);
    if (g_lastFrameMs > 0.0) {
        const double ms = t - g_lastFrameMs;
        if (ms > 2.0 && ms < 500.0) { g_intervals[g_intHead] = (float)ms; g_intHead = (g_intHead + 1) % 600; if (g_intCount < 600) ++g_intCount; }
    }
    g_lastFrameMs = t;
    const double now = NowS();
    if (ProfileTick(now)) g_profPending = true;   // with dynamic resolution on or off (rate-limited inside)
    if (now < g_nextDetect) return;
    g_nextDetect = now + 5.0;
    float hz = g_cfg.drsTargetHz;
    const char* how = "ini";
    if (hz <= 0.f && g_xrPeriodMs > 0.0 && drs::SnapHeadsetHz((float)(1000.0 / g_xrPeriodMs)) > 0.f) return;   // MissObserve sets it
    if (hz <= 0.f) {
        const float rt = RuntimeHz();
        float measured = 0.f;
        if (g_intCount >= 300) {
            static float v[600]; const int n = g_intCount;
            for (int i = 0; i < n; ++i) v[i] = g_intervals[i];
            std::nth_element(v, v + n / 5, v + n);
            const float m = 1000.f / v[n / 5];
            for (float r : { 60.f, 72.f, 80.f, 90.f, 120.f, 144.f }) if (std::fabs(m - r) < r * 0.04f) measured = r;
        }
        if (rt > 0.f) { hz = rt; how = "from the VR runtime"; }
        else if (measured >= 70.f) { hz = measured; how = "measured from the frame rhythm"; }
        else return;   // not known yet (or only half-rate frames so far): keep the current value
    }
    if (std::fabs(hz - g_targetHz.load()) > 0.5f) {
        Log("target refresh rate: %.1f Hz (%s); frame budget for dynamic resolution %.2f ms", hz, how, 1000.f / hz * g_cfg.drsHeadroom);
        g_targetHz = hz;
    }
}

static bool OutputSync() {
    if (!g_outInit) {
        g_outInit = true;
        g_outCv = EngineFindCVar(L"xr.SecondaryScreenPercentage.HMDRenderTarget");
        if (!g_outCv) Log("output: xr.SecondaryScreenPercentage.HMDRenderTarget not found (output stage and key 8 off)");
    }
    if (!g_outCv) return false;
    const float cur = EngineGetCVarFloat(g_outCv);
    if (g_outWritten < 0.f || std::fabs(cur - g_outWritten) > 0.01f) {
        if (g_outWritten >= 0.f)
            Log("output: the game's resolution setting is now %g (the mod had set %g): taken as the new game value", cur, g_outWritten);
        g_outBase = cur; g_outWritten = -1.f; g_outScale = 100.f; g_outByDrs = false;
    }
    return g_outBase > 1.f;
}

void OutputPoll() { if (g_outInit) OutputSync(); }   // game thread, every frame (cheap: one cvar read)
float OutputScale() { return g_outScale.load(std::memory_order_relaxed); }

bool OutputSetScale(float scale, bool byDrs) {
    if (!OutputSync()) return false;
    const float v = std::round(g_outBase * scale / 10.f) / 10.f;
    wchar_t b[32]; swprintf_s(b, L"%g", v);
    const float before = EngineGetCVarFloat(g_outCv);
    EngineSetCVar(g_outCv, b, true);
    const float after = EngineGetCVarFloat(g_outCv);
    const bool ok = std::fabs(after - v) <= 0.01f;
    Log("output: %g -> %g (%.0f%% of the game's %g)%s%s", before, after, scale, g_outBase,
        byDrs ? " by dynamic resolution" : "", ok ? "" : "  <- NOT APPLIED (the engine kept its value)");
    if (!ok) { g_outWritten = -1.f; OutputSync(); return false; }
    g_outWritten = after; g_outScale = scale;
    g_outByDrs = byDrs && std::fabs(scale - 100.f) > 0.5f;
    g_os.lastChange = NowS(); g_os.lowSince = g_os.highSince = g_os.fastSince = -1.0;   // a hand change also starts the cooldown
    if (!byDrs) { g_os.lastUp = -1e9; g_os.probing = false; }   // the controller never reverts a hand change
    // ignore the rebuild / reallocation frames for 2 s; by time, not frames: at 4-36 fps 200 frames were 6-17 s blind.
    // A shorter wait took the models' re-creation hitch (235 ms frame, p75 15.4 ms) right after a probe 70 -> 80% for
    // the new size's cost and undid the probe, with ~7 ms needed in fact (estimate)
    g_n = 0; g_recentN = 0;
    if (g_pct > 0.f) g_warmUntil = NowS() + 2.0;
    g_model.prevArea = 0.f;   // another output size: the window before is not comparable
    g_sz.measLo = -1;         // nor is a DLSS size step being measured
    return true;
}

void DrsReset() {
    if (g_pct > 0.f) g_lastPct = g_pct;
    g_pct = 0.f; g_n = 0; g_recentN = 0;
    g_guardLive = false;   // late frames while it is off do not count for the guard
    // the controller's output change ends with it (DLSS off = the game as it was); only a supersampled output is
    // resumed when it comes back (DrsTick). Resuming a low one pinned the output at 70% after every toggle; restarting
    // at 100% may overload for ~1-3 s until the stages drop.
    if (g_outByDrs) { g_resumeScale = g_outScale.load(); OutputSetScale(100.f, false); }
    g_bdReset = true;   // the "raise blocked" diagnostics start over (BlockNote)
}
float DrsCurrentPercent() { return g_pct; }
float DrsFeatureCeiling() {   // render thread (upscaler.cpp): the size the features are made for while DRS runs
    const float hiUser = DrsCeiling();
    const float s = g_szSize.load(std::memory_order_relaxed);
    return g_cfg.drsAdaptiveSize && s > 0.f ? std::min(hiUser, s) : hiUser;
}
float DrsFeatureSizeNow() { return g_cfg.drsAdaptiveSize && g_pct > 0.f ? g_szSize.load(std::memory_order_relaxed) : 0.f; }
void DrsResumeExact(float settleS) { g_resumeExact = true; g_resumeSettle = settleS; }

// ---------------------------------------------------------------- "why does the output not come back up" (diagnostics)
// After a slowdown the output can stay low although the game has recovered. While the output is
// below the game's value, every 5 s one log line gives the share of time each condition blocked a raise (time-
// weighted, drs_logic.h kBlock*), with the render range, GPU time, prediction and VRAM; the CSV gets the latest set.
struct BlockAcc {
    double start = 0.0, last = 0.0, total = 0.0;
    double t[13] = {};
    float pctMin = 1e9f, pctMax = 0.f, gpuMax = 0.f, pred = 0.f, limit = 0.f;
};
static BlockAcc g_bd;
static std::atomic<unsigned> g_outBlock{ 0 };
static int g_blockLogs = 400;

static void BlockNote(double now, unsigned bits, float gpu = 0.f) {
    if (g_bdReset) { g_bdReset = false; g_bd = {}; }
    g_outBlock = bits;
    if (g_outScale.load() >= 99.5f || !g_liveOutStage) { g_bd = {}; return; }   // only while the output is held low
    if (g_bd.start <= 0.0) { g_bd.start = g_bd.last = now; }
    const double dt = std::min(1.0, now - g_bd.last);
    g_bd.last = now; g_bd.total += dt;
    for (int i = 0; i < 13; ++i) if (bits & (1u << i)) g_bd.t[i] += dt;
    if (g_pct > 0.f) { g_bd.pctMin = std::min(g_bd.pctMin, g_pct); g_bd.pctMax = std::max(g_bd.pctMax, g_pct); }
    if (gpu > 0.f) { g_bd.gpuMax = std::max(g_bd.gpuMax, gpu); g_bd.pred = g_os.predicted; g_bd.limit = g_os.predictedLimit; }
    if (now - g_bd.start < 5.0 || g_bd.total <= 0.0) return;
    if (g_blockLogs-- > 0) {
        char list[384] = ""; size_t n = 0;
        for (int i = 0; i < 13; ++i)
            if (g_bd.t[i] > 0.0 && n < sizeof(list) - 48)
                n += snprintf(list + n, sizeof(list) - n, "%s%s %.0f%%", n ? ", " : "", drs::BlockName(1u << i), 100.0 * g_bd.t[i] / g_bd.total);
        float used = 0.f, vb = 0.f; VramQuery(used, vb);
        Log("DRS: output %.0f%% held %.1f s, raise blocked by: %s | render %.0f-%.0f%%, GPU p75 max %.2f ms, prediction %.2f vs limit %.2f ms, VRAM %.0f of %.0f MB",
            g_outScale.load(), now - g_bd.start, n ? list : "nothing", g_bd.pctMin > 1e8f ? 0.f : g_bd.pctMin, g_bd.pctMax,
            g_bd.gpuMax, g_bd.pred, g_bd.limit, used, vb);
    }
    g_bd = {};
}

// key ' (game thread, like DrsTick): dynamic resolution may change the output size, or only the render size.
// Off: an output the controller had lowered (or raised) goes back to the game's value at once, so nothing stays
// blurry (the render stage comes back within seconds, the output stage could stay stuck low); a hand-set output
// (key 8) is left alone. On: the controller starts from a fresh state.
void DrsSetOutputStage(bool on) {
    g_liveOutStage = on;
    if (!on) {
        if (g_outByDrs) OutputSetScale(100.f, false);
        g_resumeScale = 100.f; g_outBlock = 0; g_bdReset = true;
    }
    g_os = {}; g_os.lastChange = NowS();
}

// the CSV column: the conditions blocking a raise at the last check, short tags joined by '+'
void DrsBlockString(char* buf, size_t n) {
    static const char* tags[13] = { "max", "cool", "wait", "ovl", "render", "head", "spike", "late", "hold", "pred", "chg", "frozen", "settle" };
    const unsigned b = g_outBlock.load();
    size_t k = 0; buf[0] = 0;
    for (int i = 0; i < 13; ++i)
        if (b & (1u << i)) { int w = snprintf(buf + k, n - k, "%s%s", k ? "+" : "", tags[i]); if (w > 0 && (size_t)w < n - k) k += (size_t)w; }
}


// F11 comparison: no size changes while it runs. The full-size screenshot copy causes a hitch (236 ms); a DRS answer
// to it (80 -> 74 %) in the middle of the DLSS burst reset the DLSS history: the frames after it jumped 1.3 px and
// wobbled 0.2-0.9 px per frame, read as "shimmer" twice that of TAA.
static bool g_hold = false, g_holdSeen = false;
void DrsHold(bool on) { g_hold = on; }

// median GPU time of the frames in [t0, t1] rendered at pct (0 when fewer than minN of them)
static float HistMedianAt(double t0, double t1, float pct, int minN) {
    static float v[_countof(g_hist)];
    int m = 0;
    for (int i = 0; i < g_histCount; ++i)
        if (g_hist[i].t >= t0 && g_hist[i].t <= t1 && std::fabs(g_hist[i].pct - pct) < 0.25f) v[m++] = g_hist[i].ms;
    if (m < minN || m == 0) return 0.f;
    std::nth_element(v, v + m / 2, v + m);
    return v[m / 2];
}

float DrsTick(float startPercent) {
    (void)startPercent;
    if (!g_res.addr[ue::I_GGPUFrameTime]) {
        static bool warned = false;
        if (!warned) { warned = true; Log("DRS: GGPUFrameTime not resolved, dynamic resolution cannot run"); }
        return -1.f;
    }
    if (g_cyclesToMs == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_cyclesToMs = 1000.0 / (double)f.QuadPart; }
    // DLSS accepts a varying input only down to 50% of the output (measured offline: below that the evaluate
    // fails); FSR has no such limit
    const float hiUser = DrsCeiling();   // the F6 quality's size (QualitySetsCeiling), capped by MaxPercent / key 0
    const float lo = DrsFloor(hiUser);
    const double now = NowS();
    // the features are made for the adaptive size (a step the render uses), not the ceiling; the render stays under it
    if (g_cfg.drsAdaptiveSize && (g_sz.size <= 0.f || std::fabs(g_sz.hiSeen - hiUser) > 0.05f)) {
        const float was = g_sz.size;
        drs::SizeStart(g_sz, drs::SizeParams{}, now, g_pct > 0.f ? g_pct : (g_lastPct > 0.f ? g_lastPct : lo), lo, hiUser,
                       QualityToScreenPercentage(Quality::Quality));
        g_szSize = g_sz.size;
        SizeImportProfile();
        char steps[64] = ""; int o = 0;
        for (int i = 0; i < g_sz.n; ++i) o += snprintf(steps + o, sizeof(steps) - o, "%s%.1f", i ? "/" : "", g_sz.tiers[i]);
        Log("DRS v6: adaptive DLSS size: steps %s %% of the output (ceiling %.0f), size %.1f%s", steps, hiUser, g_sz.size,
            was > 0.f && std::fabs(was - g_sz.size) > 0.05f ? " (the ceiling changed)" : "");
    }
    const float hi = g_cfg.drsAdaptiveSize && g_sz.size > 0.f ? std::min(hiUser, g_sz.size) : hiUser;
    const float frameMs = 1000.f / TargetHz();
    const float iniBudget = frameMs * g_cfg.drsHeadroom;
    // miss guard (key 3): the learned budget when GPU-caused late frames happened below the ini's
    const bool guard = g_liveMissGuard.load(std::memory_order_relaxed);
    const drs::MissParams mp = MissParamsFromCfg();
    g_floorNow = lo;
    {   // the new controller takes over from the old one: start without its window, lock or quiet time
        static bool lastGuard = true;
        if (guard && !lastGuard) {
            drs::MissOnSwitch(g_ms);
            Log("DRS v5: new controller (v5.2) on: miss window and lock start empty, budget %.2f ms (ini %.2f)",
                drs::MissBudget(g_ms, mp, now, iniBudget, frameMs), iniBudget);
        }
        lastGuard = guard;
    }
    {   // a new refresh rate: budget growth, GPU times at misses and cost-model steps no longer apply
        static float lastHz = 0.f;
        const float hzNow = TargetHz();
        if (std::fabs(hzNow - lastHz) > 0.5f) {
            if (lastHz > 0.f) Log("DRS v5: refresh rate %.0f -> %.0f Hz: budget growth, learned GPU times and the cost model reset", lastHz, hzNow);
            lastHz = hzNow;
            drs::MissOnHzChange(g_ms, now); drs::ModelReset(g_model);
            g_learnFrom = now + drs::kModelSettleS;
        }
    }
    const float budget = guard ? drs::MissBudget(g_ms, mp, now, iniBudget, frameMs) : iniBudget;
    g_budgetMs = budget;
    if (g_pct > 0.f && g_pct < lo) { g_pct = lo; return g_pct; }   // e.g. switched from FSR to a DLSS model
    if (g_pct > hi) { g_pct = hi; return g_pct; }                    // the ceiling was lowered (F6 or key 0)
    if (g_pct <= 0.f) {   // (re)start 10 points under where it was: resuming at the very last size gave 2.5-20 s
                          // at 45 fps, restarting at the floor took ~10 s to climb back after every F5
        const bool exact = g_resumeExact && g_lastPct > 0.f;   // A/B swap: resume where it was, then hold
        // the session's first start at Quality at least (drs_logic.h RenderStart); the profile's start size raises it
        // further when it loads (SizeImportProfile)
        g_pct = drs::RenderStart(lo, hi, QualityToScreenPercentage(Quality::Quality), g_lastPct, exact);
        if (g_lastPct <= 0.f && g_startPct <= 0.f) {
            g_startPct = g_pct;
            Log("DRS v6.12: first start at %.0f%% render (Quality at least; floor %.0f, ceiling %.0f)", g_pct, lo, hi);
        }
        g_lastPct = 0.f;
        g_n = 0; g_recentN = 0; g_rs = {};
        g_warmUntil = now + (exact ? std::max(0.5f, g_resumeSettle) : 0.5f);
        if (exact) Log("DRS: resumed at %.0f%% (A/B swap), DLSS size %.1f, no change for %.0f s", g_pct, g_sz.size, g_resumeSettle);
        if (g_cfg.drsAdaptiveSize && g_sz.size > 0.f) drs::SizeResume(g_sz, now, g_warmUntil + 2.0);
        drs::MissOnResume(g_ms, now);   // the budget growth starts again from the ini's
        g_resumeExact = false;
        drs::MissResetWindow(g_ms); g_guardLive = false; g_model.prevArea = 0.f; Changed();   // start clean
        g_learnFrom = g_warmUntil + drs::kModelSettleS;   // no cost-model steps from the first seconds
        g_os = {}; g_os.lastChange = now;
        if (g_liveOutStage && g_resumeScale > 100.5f && g_resumeScale <= g_cfg.drsOutMax + 0.5f) {
            Log("DRS: resuming the output at %.0f%%", g_resumeScale);
            OutputSetScale(g_resumeScale, true);
        } else if (g_resumeScale < 99.5f) Log("DRS: starting at the game's output (was %.0f%%: a low output is not resumed)", g_resumeScale);
        g_resumeScale = 100.f;
        return g_pct;
    }
    if (g_hold) { g_holdSeen = true; g_guardLive = false; return -1.f; }
    if (g_holdSeen) {   // comparison over: start measuring afresh (the screenshot hitches are not this size's fault)
        g_holdSeen = false;
        g_n = 0; g_recentN = 0; g_rs = {};
        g_warmUntil = now + 1.0;
        drs::MissResetWindow(g_ms); g_model.prevArea = 0.f;
        g_sz.measLo = -1;
        g_learnFrom = g_warmUntil + drs::kModelSettleS;
        Log("DRS: comparison over, measuring again from %.0f%%", g_pct);
        return -1.f;
    }
    // the setup's performance profile (checked in DrsObserveFrame; a menu change, F7 / F9 or another refresh rate
    // switches it): its costs replace the size logic's, and the next seconds are not measured (the game reallocates
    // after a menu change)
    if (g_profPending) {
        g_profPending = false;
        SizeImportProfile();
        g_sz.measLo = -1;
        g_learnFrom = std::max(g_learnFrom, now + drs::kModelSettleS);
        g_warmUntil = std::max(g_warmUntil, now + 2.0);
    }
    const double ms = *(volatile uint32_t*)g_res.addr[ue::I_GGPUFrameTime] * g_cyclesToMs;
    const bool valid = ms > 0.2 && ms < 100.0;
    if (valid && ms > frameMs) g_lastSpike = now;   // also during warm-up: a missed frame right after a change blocks raises
    g_guardLive = now >= g_warmUntil;
    if (now < g_warmUntil) { BlockNote(now, drs::kBlockWarmup); return -1.f; }
    if (!valid) return -1.f;
    // paused / loading / headset off (frames 33 ms or slower): change nothing (at 10 fps idle the tiny GPU times raised
    // the output three times)
    const float recent = RecentFrameMs(std::min(g_intCount, 45));
    if (recent > 33.f) { g_n = 0; g_recentN = 0; BlockNote(now, drs::kBlockFrozen); return -1.f; }
    // a menu / pause (velocity placeholder, little game work) or the 2 s after a load: the render stage, the
    // emergency step and the cost model skip these frames (else they raise in menus and drop at loads)
    const bool idleNow = g_velPlaceholder.load(std::memory_order_relaxed) || (g_workEma > 0.f && g_workEma < 2.0f);
    const bool afterLoad = now - g_lastLongFrame < 2.0;
    if (guard) {   // GPU-caused late frames (counted in DrsObserveFrame): drop at once, then no raise above the failed level
        const int misses = drs::MissRecent(g_ms, now, mp.windowS);
        // not within 0.5 s of a render-stage drop: its late frames were the old size's (else 75 -> 66 by the stage is
        // followed by 66 -> 59 by the guard 69 ms later for the same moment)
        const float next = now - g_lastStageDrop < 0.5 ? g_pct : drs::MissDrop(g_ms, mp, now, g_pct, lo);
        if (next < g_pct) {
            Log("DRS miss guard: %d GPU-bound late frames in %.1f s -> screen percentage %.0f -> %.0f, no raise above %.0f for %.0f s (budget %.2f ms, ini %.2f)",
                misses, mp.windowS, g_pct, next, g_ms.cap, g_ms.lockLen, budget, iniBudget);
            g_pct = next; g_n = 0; g_recentN = 0; g_rs = {}; Changed();
            g_lastFastDrop = now; g_lastGuardDrop = now;
            return g_pct;
        }
        // the first frames after a change still show the previous size (it reaches the next frame, and the GPU
        // time arrives 1-2 frames late): they go into no window, history or emergency check
        if (g_stale > 0) { --g_stale; return -1.f; }
    }
    // history (8 s p90 for raises) and the last 6 frames (emergency)
    g_hist[g_histHead] = { now, (float)ms, g_pct }; g_histHead = (g_histHead + 1) % (int)_countof(g_hist);
    if (g_histCount < (int)_countof(g_hist)) ++g_histCount;
    if (ms < 50.0) {   // a single rebuild frame is not a trend
        g_recent[g_recentHead] = (float)ms; g_recentHead = (g_recentHead + 1) % 6;
        if (g_recentN < 6) ++g_recentN;
    }
    // emergency: the frame rate is collapsing, do not wait for the interval (with the guard: not in the 2 s after a
    // load, nor within 0.5 s of a guard drop, whose frames it would count again)
    if (g_recentN == 6 && !(guard && (afterLoad || now - g_lastGuardDrop < 0.5))) {
        float sum = 0.f; for (float v : g_recent) sum += v;
        const float next = drs::RenderEmergency(g_pct, sum / 6.f, frameMs, budget, lo, RecentFrameMs(std::min(g_intCount, 6)));
        if (next < g_pct) {
            if (g_logsLeft-- > 0) Log("DRS: emergency, last 6 frames %.2f ms (frame %.2f ms) -> screen percentage %.0f -> %.0f",
                                      sum / 6.f, frameMs, g_pct, next);
            g_pct = next; g_recentN = 0; g_n = 0; g_rs = {}; Changed();
            g_lastFastDrop = now;
            g_lastStageDrop = now; drs::MissResetWindow(g_ms);   // the late frames before were the old size's
            g_model.prevArea = 0.f;   // the window before was a collapse, not the same scene at another size
            return g_pct;
        }
    }
    if (g_n == 0) g_winStart = now;
    if (g_n < (int)_countof(g_win)) g_win[g_n++] = (float)ms;
    if (g_n < g_cfg.drsInterval && !(now - g_winStart >= 0.2 && g_n >= 4)) return -1.f;

    std::sort(g_win, g_win + g_n);
    const float gpu = g_win[(g_n - 1) * 3 / 4];
    g_n = 0;

    // adaptive DLSS size (drs_logic.h SizeNext): shrink when the render stays under a smaller step, grow as a
    // probe when it sits at the top with GPU room for the bigger feature. A change rebuilds the features (a hitch).
    if (g_cfg.drsAdaptiveSize && g_sz.size > 0.f) {
        static float tmp6[_countof(g_hist)];
        int m6 = 0;
        for (int i = 0; i < g_histCount; ++i) if (now - g_hist[i].t <= 2.5) tmp6[m6++] = g_hist[i].ms;
        float p90s = gpu;
        if (m6 >= 10) { std::sort(tmp6, tmp6 + m6); p90s = tmp6[(m6 - 1) * 9 / 10]; }
        const char* why = "";
        const float old = g_sz.size, dl = PerfDlssRecentMs();
        // GPU ms per unit of render area for the growth check: the controller's learned slope once it has 3 steps,
        // else 60 % of the part that is not DLSS (DLSS follows the set-up size, not the render: with ~3.3 ms of DLSS in
        // an 8.2 ms frame, the whole-frame prior would have blocked a 66.7 -> 75 % step that ran fine)
        const float area6 = std::max(0.05f, (g_pct / 100.f) * (g_pct / 100.f));
        float slope6 = 0.6f * std::max(0.5f, gpu - 2.f * dl) / area6;
        if (drs::ModelCount(g_model, now) >= 3) slope6 = drs::ModelSlope(g_model, gpu, area6, now);
        else if (const float ps = ProfileGet("slope"); ps > 0.f) slope6 = ps;   // this setup's slope from earlier sessions
        // menus / pause / loading restart the growth wait (low GPU use does not count: drs_logic.h SizeIdle); growth
        // is checked against the ini's budget at most
        const bool idle6 = drs::SizeIdle(g_velPlaceholder.load(std::memory_order_relaxed), g_workEma, afterLoad);
        const drs::SizeParams sp6 = drs::SizeParamsFor(frameMs);   // the ms values follow the refresh rate
        const int minN6 = (int)(TargetHz() * 1.2f);   // frames for a step's median (before and after)
        // a step being measured: after the rebuild's 1.5 s, measureS of frames at the same render (held, below)
        if (g_sz.measLo >= 0 && (now >= g_measUntil || std::fabs(g_pct - g_sz.measPct) > 0.25f || idle6)) {
            const int i6 = g_sz.measLo;
            const float lo6 = g_sz.tiers[i6], hi6 = g_sz.tiers[i6 + 1];
            const float before = g_sz.measBefore, dlD = g_sz.measDlss, pct6 = g_sz.measPct;
            const float after = idle6 ? 0.f : HistMedianAt(g_measFrom, now, pct6, minN6);
            if (drs::SizeMeasureEnd(g_sz, sp6, after)) {
                if (ProfileActive()) {   // the profile keeps the measurement (fast at first, then slower)
                    g_sz.stepCost[i6] = ProfileLearn(prof::StepName(lo6, hi6).c_str(), g_sz.lastJump);
                    if (dlD > 0.05f) g_sz.stepFactor = ProfileLearn("factor", std::min(6.f, std::max(1.f, g_sz.lastJump / dlD)));
                }
                Log("DRS v6.4: DLSS size step %.1f <-> %.1f%% measured at render %.0f%%: GPU median %.2f -> %.2f ms, jump %.2f ms (DLSS timers %.2f ms both eyes) -> step cost %.2f ms, factor %.2f",
                    lo6, hi6, pct6, before, after, g_sz.lastJump, dlD, g_sz.stepCost[i6], g_sz.stepFactor);
            } else
                Log("DRS v6.4: DLSS size step %.1f <-> %.1f%% not measured (%s)", lo6, hi6,
                    idle6 ? "menu / load" : std::fabs(g_pct - pct6) > 0.25f ? "the render moved" : "too few frames");
        }
        const float ns = drs::SizeNext(g_sz, sp6, now, g_pct, p90s, std::min(budget, iniBudget), dl, &why,
                                       g_lastFastDrop, slope6, idle6, g_gpuMissCount, g_lastGpuMiss);
        if (g_sz.bestChanged) {   // the start size of the next session (one step below it)
            g_sz.bestChanged = false;
            if (ProfileActive()) {
                ProfileSet("size:best", g_sz.best);
                Log("DRS v6.10: best DLSS size for this setup %.1f%% (kept in the profile)", g_sz.best);
            }
        }
        if (g_sz.retryChanged >= 0) {   // a step that failed for GPU load waits for a lighter scene (or no longer)
            const int r = g_sz.retryChanged;
            g_sz.retryChanged = -1;
            if (r + 1 < g_sz.n) {
                if (ProfileActive()) ProfileSet(prof::RetryName(g_sz.tiers[r], g_sz.tiers[r + 1]).c_str(), g_sz.retryP90[r]);
                if (g_sz.retryP90[r] > 0.f)
                    Log("DRS v6.11: DLSS size step %.1f -> %.1f%% failed for GPU load: next try only with the GPU p90 at %.1f%% under %.2f ms (%.2f before the try, minus half the predicted rise %.2f)",
                        g_sz.tiers[r], g_sz.tiers[r + 1], g_sz.tiers[r], g_sz.retryP90[r], g_sz.probeP90From, g_sz.probeRise);
                else Log("DRS v6.11: DLSS size step %.1f -> %.1f%% passed: no lighter scene needed any more", g_sz.tiers[r], g_sz.tiers[r + 1]);
            }
        }
        if (std::fabs(ns - old) > 0.05f) {
            // measure the step when the render stays where it is (always on growth; on a shrink when it was below)
            if (g_pct <= ns + 0.05f && !idle6) {
                drs::SizeMeasureBegin(g_sz, sp6, old, ns, g_pct, HistMedianAt(now - 2.5, now, g_pct, minN6));
                g_measFrom = now + 1.5; g_measUntil = g_measFrom + sp6.measureS;
            } else g_sz.measLo = -1;
            drs::SizeApply(g_sz, now, ns);
            g_szSize = ns;
            Log("DRS v6: DLSS size %.1f -> %.1f%% of the output (%s): render %.0f%%, GPU p90(2.5 s) %.2f ms, budget %.2f ms, DLSS %.2f ms/eye%s",
                old, ns, why, g_pct, p90s, budget, dl, g_sz.measLo >= 0 ? " (measuring the step)" : "");
            if (ns > old) Log("DRS v6: predicted GPU at the %.1f%% top %.2f ms (limit %.2f ms; step %.2f ms %s, slope %.2f ms per unit area, floor %.1f)",
                              ns, g_sz.lastPredicted, sp6.upMargin * std::min(budget, iniBudget),
                              drs::SizeStepExtra(g_sz, sp6, drs::SizeIndex(g_sz) - 1),
                              g_sz.stepCost[std::max(0, drs::SizeIndex(g_sz) - 1)] > 0.f ? "measured" : "estimated",
                              slope6, sp6.minSlope);
            if (g_pct > ns) g_pct = ns;
            g_warmUntil = now + 1.5;   // the rebuild's frames are not this size's GPU time
            drs::MissResetWindow(g_ms); g_model.prevArea = 0.f; g_recentN = 0; g_rs = {}; Changed();
            return g_pct;
        }
        // every 30 s below the top step, why the size does not grow
        const int k6 = drs::SizeIndex(g_sz);
        if (now >= g_nextGrowLog && k6 + 1 < g_sz.n) {
            g_nextGrowLog = now + 30.0;
            const float limit6 = sp6.upMargin * std::min(budget, iniBudget);
            const float pred6 = drs::SizePredict(g_sz, sp6, g_pct, p90s, slope6);
            const float step6 = drs::SizeStepExtra(g_sz, sp6, k6);
            char by[256] = ""; size_t o = 0;
            auto add = [&](const char* s) { int w = snprintf(by + o, sizeof(by) - o, "%s%s", o ? ", " : "", s); if (w > 0 && (size_t)w < sizeof(by) - o) o += (size_t)w; };
            if (g_sz.probeFrom > 0.f) add("a probe on trial");
            if (now - g_sz.lastChange < sp6.minGapS) add("45 s since the last change");
            if (idle6) add("menu / load");
            if (now < g_sz.noUpUntil) add("back-off after an undone probe");
            if (now < g_sz.readyAt) add("settling after a restart");
            if (g_pct < g_sz.size - 0.75f) add("render below the top");
            if (pred6 > limit6) add("prediction over the limit");
            const bool known6 = drs::SizeStepKnown(g_sz, sp6, k6);
            if (!known6 && now - g_sz.lastChange < sp6.costWaitS) add("upscaler cost not measured yet");
            char lighter[96] = "";
            if (drs::SizeWaitsLighter(g_sz, k6, now, p90s)) {
                snprintf(lighter, sizeof(lighter), "waiting for a lighter scene (GPU p90 under %.2f ms)", g_sz.retryP90[k6]);
                add(lighter);
            }
            if (!o) add(g_sz.pinnedSince >= 0.0 ? "nothing (holding 12 s at the top)" : "nothing");
            Log("DRS v6.5: growth check at size %.1f -> %.1f%%: render %.0f%%, GPU p90(2.5 s) %.2f + step %.2f ms (%s) + render to the top = %.2f ms vs limit %.2f ms; blocked by: %s; hold restarts in 30 s: %d (prediction over the limit %d, render under the top %d)",
                g_sz.size, g_sz.tiers[k6 + 1], g_pct, p90s, step6,
                g_sz.stepCost[k6] > 0.f ? "measured" : known6 ? "estimated" : "fallback, upscaler cost unknown", pred6, limit6, by,
                g_sz.holdResets, g_sz.holdResetsOver, g_sz.holdResetsRender);
            g_sz.holdResets = g_sz.holdResetsOver = g_sz.holdResetsRender = 0;
        }
        if (g_sz.probePassed) {   // a probe kept after its trial
            g_sz.probePassed = false;
            Log("DRS v6.7: DLSS size %.1f%% kept after its %.0f s trial (from %.1f%%): render peak %.0f%%, %.0f s at the bigger size, GPU p90 over the limit %.0f s of them",
                g_sz.size, sp6.probeEvalS, g_sz.passedFrom, g_sz.passedPeak, g_sz.passedTopS, g_sz.passedOverS);
        }
        // every 30 s of gameplay, this size's DLSS cost and the learned slope into the profile
        if (ProfileActive() && now >= g_nextProfExport) {
            g_nextProfExport = now + 30.0;
            if (!idle6 && g_sz.cost[k6] > 0.f && now - g_sz.lastChange > 10.0) ProfileLearn(prof::DlssName(g_sz.size).c_str(), g_sz.cost[k6]);
            if (!idle6 && drs::ModelCount(g_model, now) >= 3) ProfileLearn("slope", drs::ModelSlope(g_model, gpu, area6, now));
        }
    }

    // stage 2 first: it only acts when stage 1 is stuck at a limit (GPU time and frame pacing only, no VRAM rule)
    if (g_liveOutStage.load(std::memory_order_relaxed) && OutputSync()) {
        const drs::OutputParams p = { g_cfg.drsOutMin, g_cfg.drsOutMax, 10.f, g_cfg.drsOutCooldown, 3.f,
                                      g_cfg.drsOutDownHold, g_cfg.drsOutUpHold, g_cfg.drsOutUpMargin,
                                      g_cfg.drsProbeWait, g_cfg.drsProbeMaxWait, g_cfg.drsOutUpElasticity };
        // p90 of every frame in the last 8 s (the 20-frame p75 above is too short to see spikes)
        static float tmp[_countof(g_hist)];
        int m = 0;
        for (int i = 0; i < g_histCount; ++i) if (now - g_hist[i].t <= 8.0) tmp[m++] = g_hist[i].ms;
        float p90 = gpu;
        if (m >= 20) { std::sort(tmp, tmp + m); p90 = tmp[(m - 1) * 9 / 10]; }
        const float frame2s = RecentFrameMs(std::min(g_intCount, (int)(TargetHz() * 2.f)));
        const bool late = frame2s > 1.2f * frameMs;   // frames are late (e.g. 45 fps reprojection): never raise
        const drs::OutputInputs in = { g_pct, gpu, p90, budget, lo, hi, now - g_lastSpike, now, late };
        const float sc = g_outScale.load();
        const float ns = drs::OutputNext(g_os, p, sc, in);
        BlockNote(now, g_os.block, gpu);
        if (ns != sc) {
            char why[128] = "";
            float used = 0.f, vbudget = 0.f; VramQuery(used, vbudget);   // for the log only
            if (ns < sc) snprintf(why, sizeof(why), " (VRAM %.0f of %.0f MB; next probe in %.0f s at the earliest)", used, vbudget, g_os.upLockLen);
            else snprintf(why, sizeof(why), " (probe; VRAM %.0f of %.0f MB)", used, vbudget);
            Log("DRS: GPU p75 %.2f ms, p90(8 s) %.2f ms, budget %.2f ms, screen percentage %.0f -> output %.0f%% -> %.0f%%%s",
                gpu, p90, budget, g_pct, sc, ns, why);
            OutputSetScale(ns, true);
            return -1.f;
        }
    }
    // learn the GPU cost per render area from this window and the one before (any step in between)
    const float area = (g_pct / 100.f) * (g_pct / 100.f);
    const bool quiet = guard && (idleNow || afterLoad);   // guard only (key 3 off: the plain controller is unaffected)
    if (quiet) g_model.prevArea = 0.f;   // menu / load frames teach the cost model nothing (they gave a slope of 2.0)
    else drs::ModelObserve(g_model, gpu, area, now, now >= g_learnFrom);   // learn only once running steadily
    g_share = drs::ModelShare(g_model, gpu, area, now);
    // with the miss guard: no raise within 3 s of a GPU-caused late frame, nor above a level that failed recently
    float hiUp = guard ? drs::MissCeiling(g_ms, mp, now, g_pct, hi) : hi;
    if (g_sz.measLo >= 0 && now < g_measUntil) hiUp = std::min(hiUp, g_pct);   // hold the render while a step is measured
    float next;
    if (quiet) { g_rs = {}; next = g_pct; }   // no render-stage step on menu / load frames
    else if (guard) {
        // raises also need the 90th percentile of the last ~2.5 s under the budget, not only this window's p75
        static float tmp[_countof(g_hist)];
        int m = 0;
        for (int i = 0; i < g_histCount; ++i) if (now - g_hist[i].t <= 2.5) tmp[m++] = g_hist[i].ms;
        float p90 = gpu;
        if (m >= 10) { std::sort(tmp, tmp + m); p90 = tmp[(m - 1) * 9 / 10]; }
        drs::MissBudgetUp(g_ms, mp, now, iniBudget, frameMs, g_pct, hi, p90, budget, lo);   // budget growth (not at the floor)
        if (g_cfg.motionHold && drs::MotionBlocksRaise(drs::MotionParams{}, now, g_lastMoving)) hiUp = std::min(hiUp, g_pct);   // MotionHold
        next = drs::RenderNextV5(g_rs, g_model, g_pct, gpu, p90, budget, lo, hiUp, 10.f, now);
        // no stage drop within 0.5 s of a guard drop: the same moment (guard 67 -> 60, then the stage 60 -> 50 310 ms
        // later)
        if (next < g_pct && now - g_lastGuardDrop < 0.5) next = g_pct;
        if (next != g_pct && g_logsLeft-- > 0)
            Log("DRS v5: GPU p75 %.2f ms, p90(2.5 s) %.2f ms, budget %.2f ms (ini %.2f), %.0f%% of the GPU time scales (%d steps learned%s) -> screen percentage %.0f -> %.0f%s",
                gpu, p90, budget, iniBudget, g_share.load() * 100.f, drs::ModelCount(g_model, now), now < g_learnFrom ? ", settling" : "",
                g_pct, next, hiUp < hi - 0.5f && next > g_pct ? " (raise capped)" : "");
        if (next < g_pct) { g_lastStageDrop = now; drs::MissResetWindow(g_ms); }   // no second drop by the guard
    } else {
        next = drs::RenderNext(g_rs, g_pct, gpu, budget, lo, hiUp);
        if (next != g_pct && g_logsLeft-- > 0) Log("DRS: GPU p75 %.2f ms, budget %.2f ms -> screen percentage %.0f -> %.0f", gpu, budget, g_pct, next);
    }
    if (next == g_pct) return -1.f;
    g_pct = next; Changed();
    return g_pct;
}
