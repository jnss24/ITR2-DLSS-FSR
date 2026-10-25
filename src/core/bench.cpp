// Benchmark key ([Benchmark] Key, F10 by default). One press runs, with the view held still (headset on a table,
// proximity sensor bypassed):
//   1. the game's own TAA (upscaler off; waits BaselineMs so the mod has freed its models, 10 s after DLSS goes off)
//   2. every model of the F7 PresetCycle (or [Benchmark] Models) at each of [Benchmark] Qualities (default Performance
//      50 %, Quality 66.7 %, DLAA 100 %), with the current foveated settings; dynamic resolution paused
//   3. the game's TAA again, to show how much the scene drifted
// Per checkpoint: wait SettleMs, measure MeasureMs (engine GPU frame time, frame interval, game / render thread, the
// mod's GPU time per eye from its timestamps, process VRAM), then a burst of consecutive frames (footage and shimmer
// map) and a full-resolution screenshot.
// Clean VRAM per checkpoint (without it the values depend on the order, by an estimated 250-450 MB): the upscaler goes
// off for GapMs (no frame in flight still uses the models), everything the mod holds is released (DlssReleaseAll)
// and VRAM settles; that level ("off") is recorded, the setting applied, and the settle wait also lasts until VRAM
// has been stable for StableMs (within StableMb), at most StableMaxMs more. The peak from the switch to the end of
// the measuring is recorded too (a switch briefly needs 100-830 MB more). While running, unused models are released
// at once (as in low-memory mode), so each checkpoint's VRAM is its own.
// Bursts are processed on a worker thread during the next settle wait; measuring waits for the worker to go idle so
// its CPU work doesn't skew the numbers. Output: <base>_benchmark.csv and <base>_benchmark.html (benchreport.cpp).
// Pressing the key again stops it; everything is put back.
#include "common.h"
#include <objbase.h>
#include <vector>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <deque>

namespace {

enum BState { BS_Idle, BS_Gap, BS_Released, BS_Settle, BS_Measure, BS_Burst, BS_Shot, BS_Finish, BS_Report };
struct Step { bool taa; int preset; Quality q; };

struct Saved { bool dlss, drs, fullTaa; int preset; Quality q; };

std::atomic<bool> g_active{ false };
std::atomic<bool> g_measuring{ false };
BState g_st = BS_Idle;
std::vector<Step> g_steps;
size_t g_cur = 0;
ULONGLONG g_t = 0, g_tStart = 0;
uint32_t g_copies = 0;
Saved g_saved{};
wchar_t g_base[MAX_PATH];
char g_settings[256];
std::vector<BenchPoint> g_pts;

// samples of the measuring window
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<float> g_gpu, g_frame, g_game, g_render, g_vram;
double g_mod[2] = { 0, 0 }; int g_modN[2] = { 0, 0 };
double g_qpcToMs = 0.0;
int64_t g_lastQpc = 0;
ULONGLONG g_nextVram = 0;
// VRAM watch: every 250 ms from the release to the end of the measuring (stability, peak)
struct VSample { ULONGLONG t; float mb; };
std::vector<VSample> g_vwatch;
ULONGLONG g_nextWatch = 0, g_tApply = 0;
float g_peak = 0.f;

// burst worker
struct Job { size_t idx; BurstFrames frames; };
std::mutex g_jm; std::condition_variable g_jcv;
std::deque<Job> g_jobs;
std::atomic<int> g_jobsBusy{ 0 };   // queued + being processed
bool g_workerStarted = false;
std::vector<uint8_t> g_refMid; int g_refW = 0, g_refH = 0;
std::atomic<int> g_reportDone{ 0 };   // 1 ok, 2 failed

const wchar_t* FileOnly(const wchar_t* p) { const wchar_t* s = wcsrchr(p, L'\\'); return s ? s + 1 : p; }
void Utf8(const wchar_t* w, char* out, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, nullptr, nullptr); }

void Worker() {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(g_jm);
            g_jcv.wait(lk, [] { return !g_jobs.empty(); });
            j = g_jobs.front(); g_jobs.pop_front();
        }
        if (j.idx == (size_t)-1) {   // report
            g_reportDone = BenchWriteReport(g_base, g_pts, g_settings) ? 1 : 2;
            --g_jobsBusy;
            continue;
        }
        BenchPoint& p = g_pts[j.idx];
        wchar_t stack[MAX_PATH], heat[MAX_PATH], tagW[64];
        MultiByteToWideChar(CP_UTF8, 0, p.label, -1, tagW, 64);
        swprintf_s(stack, L"%s_%02zu_%s_frames.png", g_base, j.idx, tagW);
        swprintf_s(heat, L"%s_%02zu_%s_shimmer.png", g_base, j.idx, tagW);
        const ULONGLONG t0 = GetTickCount64();
        p.hasBurst = BenchProcessBurst(j.frames, stack, heat, g_refMid, g_refW, g_refH, p.burst);
        BurstFree(j.frames);
        if (p.hasBurst) { Utf8(FileOnly(stack), p.frames, sizeof(p.frames)); Utf8(FileOnly(heat), p.heat, sizeof(p.heat)); }
        Log("bench: checkpoint %zu (%s) burst processed in %.1f s", j.idx, p.label, (GetTickCount64() - t0) / 1000.0);
        --g_jobsBusy;
    }
    if (SUCCEEDED(ci)) CoUninitialize();
}

void Enqueue(size_t idx, const BurstFrames& f) {
    if (!g_workerStarted) { g_workerStarted = true; std::thread(Worker).detach(); }
    ++g_jobsBusy;
    { std::lock_guard<std::mutex> lk(g_jm); g_jobs.push_back(Job{ idx, f }); }
    g_jcv.notify_one();
}

float Median(std::vector<float> v) {
    if (v.empty()) return 0.f;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}
float Pct(std::vector<float> v, double q) {
    if (v.empty()) return 0.f;
    const size_t k = std::min(v.size() - 1, (size_t)(v.size() * q));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

void LabelFor(const Step& s, char* buf, size_t n, char* model, size_t mn, char* quality, size_t qn) {
    if (s.taa) {
        const int aa = TweaksAaMode();
        snprintf(buf, n, "%s", aa == 1 ? "MSAA2x" : aa == 2 ? "MSAA8x" : "TAA");
        snprintf(model, mn, "%s", buf); quality[0] = 0;
        return;
    }
    snprintf(model, mn, "%s", PresetName(s.preset));
    snprintf(quality, qn, "%s", QualityName(s.q));
    for (char* c = quality; *c; ++c) if (*c == ' ') *c = '_';
    if (s.preset == kPresetFsr) snprintf(buf, n, "FSR-%s", quality);
    else snprintf(buf, n, "DLSS-%s-%s", model, quality);
}

void Restore() {
    g_measuring = false;
    g_dlssEnabled = g_saved.dlss;
    g_livePreset = g_saved.preset;
    g_liveQuality = (int)g_saved.q;
    g_liveFullTaa = g_saved.fullTaa;
    if (g_saved.drs) { DrsReset(); g_liveDrs = true; }
    g_active = false;
    Log("bench: settings put back (upscaler %s, model %s, %s, dynamic resolution %s)", g_saved.dlss ? "ON" : "OFF",
        PresetName(g_saved.preset), QualityName(g_saved.q), g_saved.drs ? "ON" : "OFF");
}

void Stop(const char* why, bool spoken) {
    Log("bench: stopped (%s) at checkpoint %zu of %zu", why, g_cur, g_steps.size());
    if (BurstState() == 1 || BurstState() == 4) BurstReset();
    Restore();
    g_st = BS_Idle;
    if (spoken) Announce(Tone::Change, "Benchmark stopped.");
}

// process VRAM: read every frame once the setting is applied (a switch's peak can be brief), sampled every 250 ms
// for the stability check
void Watch(ULONGLONG now) {
    const bool keep = now >= g_nextWatch;
    if (!keep && !g_tApply) return;
    float used = 0.f, budget = 0.f;
    if (!VramQuery(used, budget)) return;
    if (g_tApply && used > g_peak) g_peak = used;
    if (!keep) return;
    g_nextWatch = now + 250;
    g_vwatch.push_back(VSample{ now, used });
    if (g_vwatch.size() > 400) g_vwatch.erase(g_vwatch.begin(), g_vwatch.begin() + 200);
}
// stable: samples cover at least StableMs and stay within StableMb (max - min); out = their median
bool Stable(ULONGLONG now, float& out) {
    std::vector<float> v; ULONGLONG oldest = now;
    for (const VSample& s : g_vwatch) if (now - s.t <= (ULONGLONG)g_cfg.benchStableMs) { v.push_back(s.mb); oldest = std::min(oldest, s.t); }
    if (v.size() < 3 || now - oldest < (ULONGLONG)g_cfg.benchStableMs - 300) return false;
    const auto mm = std::minmax_element(v.begin(), v.end());
    out = Median(v);
    return *mm.second - *mm.first <= (float)g_cfg.benchStableMb;
}
void TakeBurst() {   // the previous checkpoint's burst: once converted, hand it to the worker
    if (BurstState() == 3) { BurstFrames f; if (BurstTake(f) && g_cur > 0) Enqueue(g_cur - 1, f); else BurstFree(f); }
    else if (BurstState() == 4) BurstReset();
}

// the setting itself (after the release, or at once without it)
void ApplySetting(size_t i) {
    const Step& s = g_steps[i];
    if (s.taa) g_dlssEnabled = false;
    else { g_livePreset = s.preset; g_liveQuality = (int)s.q; g_dlssEnabled = true; }
    g_t = g_tApply = GetTickCount64();
    g_peak = 0.f;
    g_vwatch.clear(); g_nextWatch = 0;   // the stability check only counts samples taken with the new setting
    g_st = BS_Settle;
}

void Apply(size_t i) {
    const Step& s = g_steps[i];
    BenchPoint p;
    LabelFor(s, p.label, sizeof(p.label), p.model, sizeof(p.model), p.quality, sizeof(p.quality));
    p.taa = s.taa;
    p.pct = s.taa ? 100.f : QualityToScreenPercentage(s.q);
    g_pts.push_back(p);
    g_vwatch.clear(); g_nextWatch = 0; g_tApply = 0; g_peak = 0.f;
    if (g_cfg.benchRelease) {   // upscaler off for GapMs, then release everything (BS_Gap), settle (BS_Released)
        g_dlssEnabled = false;
        g_t = GetTickCount64();
        g_st = BS_Gap;
    } else ApplySetting(i);
    Log("bench: checkpoint %zu of %zu: %s", i + 1, g_steps.size(), p.label);
    // a short spoken note when the model changes (not during the measuring)
    if (s.taa) Announce(Tone::Change, "T A A.");
    else if (i == 0 || g_steps[i - 1].taa || g_steps[i - 1].preset != s.preset)
        Announce(Tone::Change, "%s.", s.preset == kPresetFsr ? "F S R" : PresetName(s.preset));
}

void BeginMeasure() {
    AcquireSRWLockExclusive(&g_lock);
    g_gpu.clear(); g_frame.clear(); g_game.clear(); g_render.clear(); g_vram.clear();
    g_mod[0] = g_mod[1] = 0; g_modN[0] = g_modN[1] = 0; g_lastQpc = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_nextVram = 0;
    g_measuring = true;
    g_t = GetTickCount64();
    g_st = BS_Measure;
}

void EndMeasure() {
    g_measuring = false;
    BenchPoint& p = g_pts.back();
    AcquireSRWLockExclusive(&g_lock);
    const std::vector<float> gpu = g_gpu, frame = g_frame, game = g_game, render = g_render, vram = g_vram;
    p.mod0 = g_modN[0] ? g_mod[0] / g_modN[0] : 0; p.mod1 = g_modN[1] ? g_mod[1] / g_modN[1] : 0;
    ReleaseSRWLockExclusive(&g_lock);
    p.samples = (int)gpu.size();
    p.gpuMed = Median(gpu); p.gpuP90 = Pct(gpu, 0.9);
    p.frameMed = Median(frame); p.gameMed = Median(game); p.renderMed = Median(render);
    double sum = 0; for (float v : frame) sum += v;
    p.fps = sum > 0 ? 1000.0 * frame.size() / sum : 0;
    const float lateMs = 1.2f * 1000.f / TargetHz();
    int late = 0; for (float v : frame) if (v > lateMs) ++late;
    p.latePct = frame.empty() ? 0 : 100.0 * late / frame.size();
    p.vramMb = Median(vram);
    p.vramPeak = std::max((double)g_peak, p.vramMb);
    int rw = 0, rh = 0, ow = 0, oh = 0; PerfGetRenderSize(rw, rh, ow, oh);
    if (!p.taa) { p.renderW = rw; p.renderH = rh; p.outW = ow; p.outH = oh; }
    else if (ow) { p.renderW = ow; p.renderH = oh; p.outW = ow; p.outH = oh; }   // native: render = the last output seen
    Log("bench: %s: GPU %.2f ms (p90 %.2f), frame %.2f ms, %.1f fps, late %.1f %%, game %.2f render %.2f ms, mod %.2f / %.2f ms per eye, VRAM %.0f MB, render %dx%d of %dx%d (%zu frames)",
        p.label, p.gpuMed, p.gpuP90, p.frameMed, p.fps, p.latePct, p.gameMed, p.renderMed, p.mod0, p.mod1, p.vramMb,
        p.renderW, p.renderH, p.outW, p.outH, gpu.size());
    Log("bench: %s: VRAM %.0f MB, before the setting (all released) %.0f MB -> own %+.0f MB; peak %.0f MB (%+.0f); stable after %.1f s",
        p.label, p.vramMb, p.vramOff, p.vramOff > 0 ? p.vramMb - p.vramOff : 0.0, p.vramPeak,
        p.vramOff > 0 ? p.vramPeak - p.vramOff : 0.0, p.settleS);
}

void FillTaaSizes() {   // the TAA rows measured before any DLSS row: native = the output size seen later
    int ow = 0, oh = 0;
    for (const BenchPoint& p : g_pts) if (!p.taa && p.outW) { ow = p.outW; oh = p.outH; break; }
    for (BenchPoint& p : g_pts) if (p.taa && !p.outW && ow) { p.renderW = p.outW = ow; p.renderH = p.outH = oh; }
}

bool BuildSteps() {
    g_steps.clear();
    int models[8]; int nm = 0;
    if (g_cfg.benchModelN > 0) { nm = g_cfg.benchModelN; for (int i = 0; i < nm; ++i) models[i] = g_cfg.benchModels[i]; }
    else { nm = g_cfg.presetCycleN; for (int i = 0; i < nm; ++i) models[i] = g_cfg.presetCycle[i]; }
    g_steps.push_back(Step{ true, 0, Quality::DLAA });
    int added = 0;
    for (int m = 0; m < nm; ++m) {
        const int pr = models[m];
        if (pr == kPresetFsr ? !FsrReady() : (!NgxAvailable() || pr < 1 || pr > 26)) { Log("bench: model %s skipped (not available)", PresetName(pr)); continue; }
        for (int q = 0; q < g_cfg.benchQualityN; ++q) { g_steps.push_back(Step{ false, pr, g_cfg.benchQualities[q] }); ++added; }
    }
    g_steps.push_back(Step{ true, 0, Quality::DLAA });
    return added > 0;
}

void FovTagText(char* buf, size_t n) {
    if (!g_liveFovea.load()) { snprintf(buf, n, "foveated off"); return; }
    const int periph = g_livePeriph.load();
    snprintf(buf, n, "foveated: %s edges, window %.0f %%%s%s", PresetName(periph), g_liveFoveaSize.load() * 100.f,
             g_liveHalfEdges.load() ? ", half-size edges" : "",
             periph == kPresetPlain && g_liveEdgeAa.load() ? (g_liveEdgeAa.load() == 1 ? ", edge AA spatial" : g_liveEdgeAa.load() == 3 ? ", edge AA fast temporal" : ", edge AA temporal") : "");
}

void Start() {
    if (g_cfg.mode != 3) { Announce(Tone::Change, "Benchmark needs Mode 3."); return; }
    if (ShotComparisonBusy() || CaptureBusy()) { Announce(Tone::Change, "Screenshot busy."); return; }
    if (StressActive()) { Announce(Tone::Change, "Stress test running."); return; }
    if (g_jobsBusy.load() > 0) { Announce(Tone::Change, "Benchmark still saving."); return; }
    if (!BuildSteps()) { Log("bench: no model available"); Announce(Tone::Change, "Benchmark: no model available."); return; }
    g_saved = Saved{ g_dlssEnabled.load(), g_liveDrs.load(), g_liveFullTaa.load(), LivePreset(), LiveQuality() };
    if (g_saved.drs) { g_liveDrs = false; DrsReset(); }
    g_liveFullTaa = false;
    g_pts.clear(); g_pts.reserve(g_steps.size());
    g_refMid.clear(); g_refW = g_refH = 0;
    g_reportDone = 0;
    ShotNewFolder("benchmark", g_base);
    char fov[128]; FovTagText(fov, sizeof(fov));
    const float os = OutputScale();
    snprintf(g_settings, sizeof(g_settings), "%s; output %.0f %% of the game's resolution setting; %.0f Hz; dynamic resolution paused; "
             "settle %d ms (TAA %d ms)%s, measure %d ms per checkpoint", fov, os, TargetHz(), g_cfg.benchSettleMs,
             g_cfg.benchRelease ? g_cfg.benchSettleMs : g_cfg.benchBaselineMs,
             g_cfg.benchRelease ? " + until VRAM is stable, everything released before each checkpoint" : "", g_cfg.benchMeasureMs);
    if (g_qpcToMs == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcToMs = 1000.0 / (double)f.QuadPart; }
    g_active = true;
    g_tStart = GetTickCount64();
    const double est = ((g_cfg.benchRelease ? (g_cfg.benchSettleMs + g_cfg.benchGapMs + 4000.0) * 2.0 : g_cfg.benchBaselineMs * 2.0) +
                        (g_cfg.benchSettleMs + 2500.0 + (g_cfg.benchRelease ? g_cfg.benchGapMs + 3000.0 : 0.0)) * (g_steps.size() - 2) +
                        g_cfg.benchMeasureMs * (double)g_steps.size()) / 60000.0;
    char b8[MAX_PATH * 3]; Utf8(g_base, b8, sizeof(b8));
    Log("bench: started, %zu checkpoints (estimate %.1f min), %s -> %s", g_steps.size(), est, g_settings, b8);
    Announce(Tone::Change, "Benchmark. Hold still. About %.0f minutes.", std::max(1.0, est + 0.5));
    g_cur = 0;
    Apply(0);
}

}   // namespace

bool BenchActive() { return g_active.load(std::memory_order_relaxed); }

void BenchOnFrame() {   // game thread, once per VR frame
    if (!g_measuring.load(std::memory_order_relaxed)) return;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const auto rd = [](int item) -> float {
        const uintptr_t a = g_res.addr[item];
        return a ? (float)(*(volatile uint32_t*)a * g_qpcToMs) : 0.f;
    };
    const float gpu = rd(ue::I_GGPUFrameTime), gt = rd(ue::I_GGameThreadTime), rt = rd(ue::I_GRenderThreadTime);
    AcquireSRWLockExclusive(&g_lock);
    if (g_lastQpc) {
        const float ms = (float)((q.QuadPart - g_lastQpc) * g_qpcToMs);
        if (ms > 0.f && ms < 500.f) g_frame.push_back(ms);
    }
    g_lastQpc = q.QuadPart;
    if (gpu > 0.2f && gpu < 200.f) g_gpu.push_back(gpu);
    if (gt > 0.f) g_game.push_back(gt);
    if (rt > 0.f) g_render.push_back(rt);
    ReleaseSRWLockExclusive(&g_lock);
}

void BenchOnDlssGpu(int eye, double ms) {   // RHI thread
    if (!g_measuring.load(std::memory_order_relaxed) || eye < 0 || eye > 1) return;
    AcquireSRWLockExclusive(&g_lock);
    g_mod[eye] += ms; ++g_modN[eye];
    ReleaseSRWLockExclusive(&g_lock);
}

void BenchPoll() {
    static bool wasDown = false, logged = false;
    if (!logged) { logged = true; Log("benchmark key: virtual key 0x%02X", g_cfg.benchKey); }
    const bool pressed = HotkeyPressed(g_cfg.benchKey, wasDown);   // common.h: no Ctrl/Alt/Shift/Win
    if (pressed) {
        if (g_st == BS_Idle) { Start(); return; }
        if (g_st == BS_Report) { Announce(Tone::Change, "Benchmark still saving."); return; }
        Stop("key pressed again", true);
        return;
    }
    if (g_st == BS_Idle) return;
    const ULONGLONG now = GetTickCount64();
    const Step s = g_cur < g_steps.size() ? g_steps[g_cur] : g_steps.back();
    if (g_st >= BS_Gap && g_st <= BS_Measure) Watch(now);
    switch (g_st) {
    case BS_Gap:   // upscaler off; after GapMs no frame in flight uses the mod's models any more: release everything
        TakeBurst();
        if (now - g_t < (ULONGLONG)g_cfg.benchGapMs) break;
        if (BurstState() == 1) break;   // never release while a burst is still being copied
        {
            float before = 0.f, budget = 0.f; VramQuery(before, budget);
            const bool any = DlssReleaseAll();
            Log("bench: released everything the mod held%s (process VRAM %.0f MB before)", any ? "" : " (nothing held)", before);
        }
        g_vwatch.clear(); g_nextWatch = 0;
        g_t = now; g_st = BS_Released;
        break;
    case BS_Released: {   // wait until the VRAM has settled after the release (at most StableMaxMs), record it, apply the setting
        TakeBurst();
        float v = 0.f;
        const bool st = Stable(now, v);
        if (!st && now - g_t < (ULONGLONG)g_cfg.benchStableMaxMs) break;
        if (!st) { float u = 0.f, b = 0.f; VramQuery(u, b); v = u; Log("bench: VRAM not stable %.1f s after the release, going on", (now - g_t) / 1000.0); }
        g_pts.back().vramOff = v;
        ApplySetting(g_cur);
        break;
    }
    case BS_Settle: {
        TakeBurst();
        const ULONGLONG wait = (ULONGLONG)(s.taa && !g_cfg.benchRelease ? g_cfg.benchBaselineMs : g_cfg.benchSettleMs);
        if (now - g_t < wait) break;
        const bool burstPending = BurstState() == 1 || BurstState() == 2;
        if ((burstPending || g_jobsBusy.load() > 0 || CaptureBusy()) && now - g_t < wait + 60000) break;   // CPU work of the last checkpoint
        float v = 0.f;
        if (!Stable(now, v) && now - g_t < wait + (ULONGLONG)g_cfg.benchStableMaxMs) break;   // VRAM still moving
        if (g_upscalerFallback.load() || (!s.taa && !g_dlssEnabled.load())) { Stop("the upscaler switched itself off", true); break; }
        g_pts.back().settleS = (now - g_t) / 1000.0;
        BeginMeasure();
        break;
    }
    case BS_Measure:
        if (g_nextVram == 0 || now >= g_nextVram) {
            float used = 0.f, budget = 0.f;
            if (VramQuery(used, budget)) { AcquireSRWLockExclusive(&g_lock); g_vram.push_back(used); ReleaseSRWLockExclusive(&g_lock); }
            g_nextVram = now + 250;
        }
        if (now - g_t < (ULONGLONG)g_cfg.benchMeasureMs) break;
        EndMeasure();
        if (g_cfg.benchFootage && BurstState() == 0 && BurstRequest(g_cfg.shotBurstFrames, g_cfg.shotBurstW, g_cfg.shotBurstH)) {
            g_st = BS_Burst; g_t = now;
        } else {
            if (g_cfg.benchFootage) Log("bench: %s: no burst (capture busy)", g_pts.back().label);
            g_st = BS_Shot; g_t = 0;
        }
        break;
    case BS_Burst:
        if (BurstState() == 4) { Log("bench: %s: burst failed", g_pts.back().label); BurstReset(); }
        else if (BurstCopied() < g_cfg.shotBurstFrames && now - g_t < 5000) break;
        else if (BurstCopied() < g_cfg.shotBurstFrames) { Log("bench: %s: burst too slow", g_pts.back().label); }
        g_st = BS_Shot; g_t = 0;
        break;
    case BS_Shot: {
        BenchPoint& p = g_pts.back();
        bool done = false;
        if (g_t == 0) {   // request the screenshot (after the burst: its full-size copy is a hitch)
            if (!g_cfg.benchShots) done = true;
            else if (CaptureBusy()) break;   // the previous PNG is still being written (normally done during the settle wait)
            else {
                wchar_t file[MAX_PATH], tagW[64];
                MultiByteToWideChar(CP_UTF8, 0, p.label, -1, tagW, 64);
                swprintf_s(file, L"%s_%02zu_%s.png", g_base, g_cur, tagW);
                g_copies = CaptureCopyCount();
                if (CaptureRequest(file, g_cfg.shotEye, true)) { Utf8(FileOnly(file), p.shot, sizeof(p.shot)); p.hasShot = true; g_t = now; }
                else { Log("bench: %s: screenshot busy, skipped", p.label); done = true; }
            }
        } else if (CaptureCopyCount() != g_copies) done = true;   // the frame is copied (the PNG is written in the background)
        else if (!CaptureBusy() || now - g_t > 5000) { Log("bench: %s: screenshot not copied", p.label); p.hasShot = false; done = true; }
        if (!done) break;
        if (++g_cur < g_steps.size()) { Apply(g_cur); break; }
        Restore();
        g_st = BS_Finish; g_t = now;
        Announce(Tone::Change, "Benchmark done. Saving.");
        break;
    }
    case BS_Finish:   // last burst handed over, all bursts processed, last screenshot written: the report
        if (BurstState() == 3) { BurstFrames f; if (BurstTake(f)) Enqueue(g_steps.size() - 1, f); }
        else if (BurstState() == 4) BurstReset();
        if ((BurstState() == 1 || BurstState() == 2 || g_jobsBusy.load() > 0 || CaptureBusy()) && now - g_t < 120000) break;
        FillTaaSizes();
        ++g_jobsBusy;
        { std::lock_guard<std::mutex> lk(g_jm); g_jobs.push_back(Job{ (size_t)-1, BurstFrames{} }); }
        if (!g_workerStarted) { g_workerStarted = true; std::thread(Worker).detach(); }
        g_jcv.notify_one();
        g_st = BS_Report; g_t = now;
        break;
    case BS_Report:
        if (const int d = g_reportDone.load()) {
            Log("bench: finished in %.1f min", (now - g_tStart) / 60000.0);
            Announce(Tone::Change, d == 1 ? "Benchmark saved." : "Benchmark could not be saved.");
            g_st = BS_Idle;
        } else if (now - g_t > 120000) { Log("bench: report did not finish"); Announce(Tone::Change, "Benchmark could not be saved."); g_st = BS_Idle; }
        break;
    default: g_st = BS_Idle; break;
    }
}
