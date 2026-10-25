// Repeatable stress test (debug build). Armed by a plan file, started by the debug loader:
//   itr2_dlss_log\stress_plan.txt  (written before the game starts; one-shot, renamed to stress_plan.used.txt at start)
//     name=<run name>          free text, goes into the result file name and every row
//     warmup=<s>               not measured, fixed 100 %, still (shaders, streaming, the first DLSS features)
//     settle=<s>               the first seconds of every step are not measured (DLSS history, DRS settling, spin start)
//     run=<label>              optional: the steps after it belong to this run (the result rows' "run"; default: name)
//     step=<fixed|dyn>,<render %>,<spin deg/s>,<seconds>
//         fixed: dynamic resolution off, render held at <render %> via ScreenPercentageOverride (DLSS Performance
//                mode at any input size, as the dynamic-resolution features use)
//         dyn:   dynamic resolution on with <render %> as its ceiling (MaxPercent; floor as in the ini)
//   itr2_dlss_log\stress_go.txt    written by OverrunDebugLoader (stress=true) once the gameplay pawn has existed 30 s
//   itr2_dlss_log\stress_cmd.txt   written here for the loader, which turns the player: spin=<deg/s>, step=<n>, done=<0|1>
// While running: per-setup profiles learn nothing (ProfileLearn), F10 / F11 refuse, the perf CSV gets the step number
// (stress_step). At each step's end one row goes to itr2_dlss_stress_<date>_<time>_<name>.csv (';', decimal comma):
// frame time median / p90 / p99 / max, skipped refreshes, late frames, GPU / game / render thread, render % reached,
// DLSS ms per eye, VRAM. Everything the plan changed is put back at the end.
// No VR frames (headset asleep / Link off: the game draws one non-stereo view and the mod never runs) -> the warm-up
// restarts, up to kNoVrRetries times, then the test is cancelled (log + voice).
#include "common.h"
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <ctime>

namespace {

struct Step { bool dyn; float pct; float spin; float secs; std::string run; };
constexpr int kNoVrRetries = 4;   // warm-ups without VR frames before giving up (4 x 40 s)
struct Plan { std::string name; float warmup = 30.f, settle = 12.f; std::vector<Step> steps; };
struct Saved { bool dlss, drs; int quality, preset; float drsMax, override_; };

enum SState { SS_Idle, SS_Armed, SS_Warmup, SS_Settle, SS_Measure, SS_Done };

std::atomic<bool> g_active{ false };
std::atomic<int> g_stepNo{ 0 };          // for the perf CSV: 0 = none / warm-up, else 1-based step
std::atomic<bool> g_measuring{ false };
SState g_st = SS_Idle;
Plan g_plan;
Saved g_saved{};
size_t g_cur = 0;
ULONGLONG g_t = 0, g_nextCheck = 0, g_armedAt = 0;
int g_noVr = 0;
wchar_t g_resultPath[MAX_PATH] = {};

// samples of the measuring window (game thread only: no lock needed)
std::vector<float> g_frame, g_gpu, g_game, g_render, g_pctS, g_dlss, g_vram;
unsigned g_skipped = 0, g_lateGpu = 0, g_lateCpu = 0, g_timingN = 0;
ULONGLONG g_nextVram = 0;

void PathIn(const wchar_t* file, wchar_t* out) { wcscpy_s(out, MAX_PATH, g_logDir); wcscat_s(out, MAX_PATH, file); }
bool Exists(const wchar_t* file) { wchar_t p[MAX_PATH]; PathIn(file, p); return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

bool ReadPlan(Plan& pl) {
    wchar_t p[MAX_PATH]; PathIn(L"stress_plan.txt", p);
    FILE* f = nullptr;
    if (_wfopen_s(&f, p, L"rb") || !f) return false;
    char line[256];
    pl = Plan{};
    std::string run;
    while (fgets(line, sizeof(line), f)) {
        char* s = line; while (*s == ' ' || *s == '\t') ++s;
        for (char* e = s + strlen(s); e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' '); ) *--e = 0;
        if (!*s || *s == ';' || *s == '#') continue;
        char* eq = strchr(s, '='); if (!eq) continue;
        *eq = 0; const char* k = s; const char* v = eq + 1;
        if (!_stricmp(k, "name")) pl.name = v;
        else if (!_stricmp(k, "run")) run = v;
        else if (!_stricmp(k, "warmup")) pl.warmup = (float)atof(v);
        else if (!_stricmp(k, "settle")) pl.settle = (float)atof(v);
        else if (!_stricmp(k, "step")) {
            char mode[16] = ""; float pct = 0.f, spin = 0.f, secs = 0.f;
            if (sscanf_s(v, "%15[^,],%f,%f,%f", mode, (unsigned)sizeof(mode), &pct, &spin, &secs) == 4 && pct >= 25.f && pct <= 100.f && secs > 0.f)
                pl.steps.push_back(Step{ !_stricmp(mode, "dyn"), pct, spin, secs, run });
            else Log("stress: plan line ignored: step=%s", v);
        }
    }
    fclose(f);
    for (char& c : pl.name) if (!isalnum((unsigned char)c) && c != '-' && c != '_') c = '_';
    if (pl.name.empty()) pl.name = "plan";
    for (Step& st : pl.steps) {
        if (st.run.empty()) st.run = pl.name;
        for (char& c : st.run) if (!isalnum((unsigned char)c) && c != '-' && c != '_') c = '_';
    }
    return !pl.steps.empty();
}

void WriteCmd(float spin, int step, bool done) {
    wchar_t p[MAX_PATH], tmp[MAX_PATH]; PathIn(L"stress_cmd.txt", p); PathIn(L"stress_cmd.tmp", tmp);
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp, L"wb") || !f) return;
    fprintf(f, "spin=%.1f\nstep=%d\ndone=%d\n", spin, step, done ? 1 : 0);
    fclose(f);
    MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING);   // the loader never sees a half-written file
}

const char* ModeName(const Step& s) { return s.dyn ? "dynamic" : "fixed"; }
void Label(const Step& s, char* buf, size_t n) { snprintf(buf, n, "%s-%.0f-%s", s.dyn ? "dyn" : "fixed", s.pct, s.spin != 0.f ? "spin" : "still"); }

void Apply(const Step& s) {
    g_dlssEnabled = true;
    if (s.dyn) {
        g_cfg.screenPercentageOverride = 0.f;
        g_liveQuality = (int)Quality::DLAA;   // QualitySetsCeiling: the ceiling is then MaxPercent = the step's %
        g_liveDrsMax = s.pct;
        if (!g_liveDrs.load()) { g_liveDrs = true; DrsReset(); }
    } else {
        if (g_liveDrs.load()) { g_liveDrs = false; DrsReset(); }
        g_liveQuality = (int)Quality::Performance;   // NGX Performance mode takes any input size (as the DRS features)
        g_cfg.screenPercentageOverride = s.pct;
    }
    g_tweakResize = true;   // the engine applies the screen percentage and settles 2 frames (engine.cpp)
}

void Restore() {
    g_cfg.screenPercentageOverride = g_saved.override_;
    g_liveQuality = g_saved.quality;
    g_livePreset = g_saved.preset;
    g_liveDrsMax = g_saved.drsMax;
    if (g_liveDrs.load() != g_saved.drs) { g_liveDrs = g_saved.drs; DrsReset(); }
    g_dlssEnabled = g_saved.dlss;
    g_tweakResize = true;
    g_active = false; g_measuring = false; g_stepNo = 0;
    Log("stress: settings put back (upscaler %s, %s, %s, dynamic resolution %s, ceiling %.0f%%)", g_saved.dlss ? "ON" : "OFF",
        PresetName(g_saved.preset), QualityName((Quality)g_saved.quality), g_saved.drs ? "ON" : "OFF", g_saved.drsMax);
}

float Pct(std::vector<float> v, double q) {
    if (v.empty()) return 0.f;
    const size_t k = std::min(v.size() - 1, (size_t)(v.size() * q));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}
float Mean(const std::vector<float>& v) { if (v.empty()) return 0.f; double s = 0; for (float x : v) s += x; return (float)(s / v.size()); }

void ClearSamples() {
    g_frame.clear(); g_gpu.clear(); g_game.clear(); g_render.clear(); g_pctS.clear(); g_dlss.clear(); g_vram.clear();
    g_skipped = g_lateGpu = g_lateCpu = g_timingN = 0; g_nextVram = 0;
}

// decimal comma, like the mod's other CSVs
void Num(std::string& row, double v, int dec) {
    char b[48]; snprintf(b, sizeof(b), "%.*f;", dec, v);
    for (char* c = b; *c; ++c) if (*c == '.') *c = ',';
    row += b;
}

void WriteResult(size_t idx, const Step& s, double measuredS) {
    const bool fresh = GetFileAttributesW(g_resultPath) == INVALID_FILE_ATTRIBUTES;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_resultPath, L"ab") || !f) { Log("stress: cannot write the result file"); return; }
    if (fresh) fputs("run;step;label;mode;pct;spin_dps;measured_s;frames;fps;frame_ms_med;frame_ms_p90;frame_ms_p99;frame_ms_max;"
                     "skipped_refreshes;skipped_pct;late_gpu;late_cpu;gpu_ms_med;gpu_ms_p90;gpu_ms_p99;game_ms_med;render_thread_ms_med;"
                     "render_pct_mean;render_pct_min;render_pct_max;dlss_ms_eye;vram_mb_med;hz\n", f);
    char label[48]; Label(s, label, sizeof(label));
    std::string row = s.run + ";" + std::to_string(idx + 1) + ";" + label + ";" + ModeName(s) + ";";
    Num(row, s.pct, 0); Num(row, s.spin, 1); Num(row, measuredS, 1);
    row += std::to_string(g_frame.size()) + ";";
    double sum = 0; for (float v : g_frame) sum += v;
    Num(row, sum > 0 ? 1000.0 * g_frame.size() / sum : 0.0, 2);
    Num(row, Pct(g_frame, 0.5), 3); Num(row, Pct(g_frame, 0.9), 3); Num(row, Pct(g_frame, 0.99), 3);
    Num(row, g_frame.empty() ? 0.0 : *std::max_element(g_frame.begin(), g_frame.end()), 3);
    row += std::to_string(g_skipped) + ";";
    const double refreshes = measuredS * TargetHz();
    Num(row, refreshes > 0 ? 100.0 * g_skipped / refreshes : 0.0, 2);
    row += std::to_string(g_lateGpu) + ";" + std::to_string(g_lateCpu) + ";";
    Num(row, Pct(g_gpu, 0.5), 3); Num(row, Pct(g_gpu, 0.9), 3); Num(row, Pct(g_gpu, 0.99), 3);
    Num(row, Pct(g_game, 0.5), 3); Num(row, Pct(g_render, 0.5), 3);
    Num(row, Mean(g_pctS), 1);
    Num(row, g_pctS.empty() ? 0.0 : *std::min_element(g_pctS.begin(), g_pctS.end()), 1);
    Num(row, g_pctS.empty() ? 0.0 : *std::max_element(g_pctS.begin(), g_pctS.end()), 1);
    Num(row, Mean(g_dlss), 3); Num(row, Pct(g_vram, 0.5), 0);
    char hz[16]; snprintf(hz, sizeof(hz), "%.0f\n", TargetHz()); row += hz;
    fputs(row.c_str(), f);
    fclose(f);
    Log("stress: step %zu (%s): %zu frames, %.1f fps, frame p50/p99 %.2f/%.2f ms, skipped %u, GPU p50/p90 %.2f/%.2f ms, render %.0f%% (%.0f-%.0f), DLSS %.2f ms/eye",
        idx + 1, label, g_frame.size(), sum > 0 ? 1000.0 * g_frame.size() / sum : 0.0, Pct(g_frame, 0.5), Pct(g_frame, 0.99), g_skipped,
        Pct(g_gpu, 0.5), Pct(g_gpu, 0.9), Mean(g_pctS), g_pctS.empty() ? 0.f : *std::min_element(g_pctS.begin(), g_pctS.end()),
        g_pctS.empty() ? 0.f : *std::max_element(g_pctS.begin(), g_pctS.end()), Mean(g_dlss));
}

void StartStep(size_t i, ULONGLONG now) {
    const Step& s = g_plan.steps[i];
    Apply(s);
    g_cur = i; g_stepNo = (int)i + 1;
    g_t = now; g_st = SS_Settle; g_measuring = false;
    WriteCmd(s.spin, (int)i + 1, false);
    char label[48]; Label(s, label, sizeof(label));
    Log("stress: step %zu of %zu: %s (%s, render %.0f%%, spin %.0f deg/s, %.0f s, first %.0f s not measured)", i + 1,
        g_plan.steps.size(), label, ModeName(s), s.pct, s.spin, s.secs, g_plan.settle);
    Announce(Tone::Change, "Step %zu. %s, %.0f percent%s.", i + 1, s.dyn ? "Dynamic" : "Fixed", s.pct, s.spin != 0.f ? ", turning" : "");
}

void Finish(const char* why) {
    WriteCmd(0.f, 0, true);
    Restore();
    g_st = SS_Done;
    Log("stress: finished (%s)", why);
    if (strcmp(why, "no VR frames") != 0) Announce(Tone::Change, "Stress test done.");   // else the cancel message stays
}

}   // namespace

bool StressActive() { return g_active.load(std::memory_order_relaxed); }
int StressStep() { return g_stepNo.load(std::memory_order_relaxed); }

void StressOnFrame(float frameMs, float gpuMs, float gameMs, float renderMs) {   // game thread (perf.cpp), once per VR frame
    if (!g_measuring.load(std::memory_order_relaxed)) return;
    if (frameMs > 0.f && frameMs < 1000.f) g_frame.push_back(frameMs);
    if (gpuMs > 0.2f && gpuMs < 200.f) g_gpu.push_back(gpuMs);
    if (gameMs > 0.f) g_game.push_back(gameMs);
    if (renderMs > 0.f) g_render.push_back(renderMs);
    const float drs = DrsCurrentPercent();
    g_pctS.push_back(g_liveDrs.load() && drs > 0.f ? drs : g_plan.steps[g_cur].pct);
    const float d = PerfDlssRecentMs(); if (d > 0.f) g_dlss.push_back(d);
}

void StressOnTiming(int lateKind, int skipped) {   // game thread (perf.cpp, debug build)
    if (!g_measuring.load(std::memory_order_relaxed)) return;
    if (lateKind == 1) ++g_lateGpu; else if (lateKind == 2) ++g_lateCpu;
    g_skipped += (unsigned)std::max(0, skipped); ++g_timingN;
}

void StressPoll() {   // game thread, every frame (tweaks.cpp)
    const ULONGLONG now = GetTickCount64();
    switch (g_st) {
    case SS_Idle:
    case SS_Done:
        if (now < g_nextCheck) return;
        g_nextCheck = now + 1000;
        if (!Exists(L"stress_plan.txt")) return;
        if (!ReadPlan(g_plan)) { Log("stress: stress_plan.txt has no valid step"); g_nextCheck = now + 30000; return; }
        g_st = SS_Armed; g_armedAt = now;
        Log("stress: plan '%s' armed: %zu steps, warm-up %.0f s, settle %.0f s; waiting for stress_go.txt from the debug loader",
            g_plan.name.c_str(), g_plan.steps.size(), g_plan.warmup, g_plan.settle);
        return;
    case SS_Armed: {
        if (now < g_nextCheck) return;
        g_nextCheck = now + 500;
        if (!Exists(L"stress_go.txt")) return;
        if (g_cfg.mode != 3) { Log("stress: needs Mode 3"); g_st = SS_Idle; g_nextCheck = now + 60000; return; }
        if (BenchActive() || ShotComparisonBusy()) return;   // wait for F10 / F11 to finish
        wchar_t a[MAX_PATH], b[MAX_PATH], go[MAX_PATH];
        PathIn(L"stress_plan.txt", a); PathIn(L"stress_plan.used.txt", b); PathIn(L"stress_go.txt", go);
        MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);   // one-shot
        DeleteFileW(go);
        g_saved = Saved{ g_dlssEnabled.load(), g_liveDrs.load(), (int)LiveQuality(), LivePreset(), g_liveDrsMax.load(), g_cfg.screenPercentageOverride };
        time_t tt = time(nullptr); tm lt{}; localtime_s(&lt, &tt);
        wchar_t name[160]; wchar_t nameW[64]; MultiByteToWideChar(CP_UTF8, 0, g_plan.name.c_str(), -1, nameW, 64);
        swprintf_s(name, L"itr2_dlss_stress_%04d%02d%02d_%02d%02d%02d_%s.csv", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                   lt.tm_hour, lt.tm_min, lt.tm_sec, nameW);
        PathIn(name, g_resultPath);
        g_active = true;
        Apply(Step{ false, 100.f, 0.f, g_plan.warmup, g_plan.name });
        g_noVr = 0;
        WriteCmd(0.f, 0, false);
        g_stepNo = 0; g_t = now; g_st = SS_Warmup;
        ClearSamples(); g_measuring = true; g_cur = 0;   // the warm-up's frames: only to check the headset paces frames
        Log("stress: started '%s' (saved: upscaler %s, %s %s, DRS %s, ceiling %.0f%%, override %.0f)", g_plan.name.c_str(),
            g_saved.dlss ? "ON" : "OFF", PresetName(g_saved.preset), QualityName((Quality)g_saved.quality), g_saved.drs ? "ON" : "OFF",
            g_saved.drsMax, g_saved.override_);
        Announce(Tone::Change, "Stress test. %zu steps.", g_plan.steps.size());
        return;
    }
    case SS_Warmup:
        if (now - g_t < (ULONGLONG)(g_plan.warmup * 1000.f)) return;
        if (g_frame.size() < (size_t)(0.5f * TargetHz() * g_plan.warmup)) {   // no (or few) VR frames: wait, then give up
            if (++g_noVr > kNoVrRetries) {
                Log("stress: CANCELLED: only %zu VR frames in the last %.0f s warm-up, %d warm-ups in a row (headset asleep or Link off?)",
                    g_frame.size(), g_plan.warmup, g_noVr);
                Announce(Tone::Off, "Stress test cancelled. No V R frames.");
                g_measuring = false;
                Finish("no VR frames");
                return;
            }
            Log("stress: only %zu VR frames in the %.0f s warm-up (expected ~%.0f): waiting for the headset, warm-up again (%d of %d)",
                g_frame.size(), g_plan.warmup, TargetHz() * g_plan.warmup, g_noVr, kNoVrRetries);
            Announce(Tone::Change, "Stress test waiting for V R frames. Wake the headset.");
            ClearSamples(); g_t = now;
            return;
        }
        g_measuring = false;
        {   // VR frame pacing check: without the headset's pacing the frames come far faster than its refresh
            const float med = Pct(g_frame, 0.5), period = 1000.f / TargetHz();
            if (!g_frame.empty() && med < 0.8f * period)
                Log("stress: WARNING frame median %.2f ms during the warm-up vs %.2f ms refresh: the headset is not pacing frames (asleep?); results are not VR timing", med, period);
            else Log("stress: warm-up frame median %.2f ms (refresh %.2f ms, %zu frames)", med, period, g_frame.size());
        }
        StartStep(0, now);
        return;
    case SS_Settle:
        if (now - g_t < (ULONGLONG)(g_plan.settle * 1000.f)) return;
        ClearSamples(); g_measuring = true; g_t = now; g_st = SS_Measure;
        return;
    case SS_Measure: {
        if (now >= g_nextVram) { float u = 0.f, b = 0.f; if (VramQuery(u, b)) g_vram.push_back(u); g_nextVram = now + 250; }
        const Step& s = g_plan.steps[g_cur];
        const double measureS = std::max(1.0, (double)(s.secs - g_plan.settle));
        if (now - g_t < (ULONGLONG)(measureS * 1000.0)) return;
        g_measuring = false;
        WriteResult(g_cur, s, (now - g_t) / 1000.0);
        if (g_cur + 1 < g_plan.steps.size()) StartStep(g_cur + 1, now);
        else Finish("all steps");
        return;
    }
    default: return;
    }
}
