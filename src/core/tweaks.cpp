// tweaks.cpp - A/B and image keys (game thread), each with a spoken confirmation.
//   7 fovea window size (0.6 / 0.5 / 0.4 / 0.7)
//   8 output size = the game's resolution setting (xr.SecondaryScreenPercentage.HMDRenderTarget), in percent
//     of the game's own value (100 / 90 / 80 / 70); shared with the dynamic resolution's second stage (drs.cpp)
//   1 anti-aliasing of the plain foveated edges (off / spatial FXAA-style / temporal small TAA)
//   2 the mod's temporal AA (key 1's) on the whole eye instead of DLSS / FSR, to compare with the game's TAA
//   3 dynamic resolution miss guard (debug build only): GPU-caused late frames, cost model, 2.5 s p90 for raises,
//     stale frames skipped, budget learned both ways, MotionHold
//   4 automatic A/B swap, 5 plain-edge jitter sign
//   - low-memory mode: the mod keeps only the DLSS/FSR features and textures the current frame uses (normally it
//     keeps the others so switching back is instant), and the engine streams textures one mip lower
//     (r.Streaming.MipBias) with the texture pool capped to the card's VRAM
//   << anti-aliasing with the upscaler off (F5): game TAA / MSAA 2x / MSAA 8x (r.AntiAliasingMethod 3 + r.MSAACount)
//   0 DLSS centre models built on a side thread, the plain edges standing in meanwhile (dlss.cpp; default), or on
//     the render path
// The sharpening pass stays in dlss.cpp, unused (g_liveSharpen = 0). SSR and SSAO do not exist in the forward renderer.
// Step 0 of every cycle puts back the value the game had when the key was first pressed.
#include "common.h"
#include <cmath>
#include <cstdarg>
#include <algorithm>

struct CvarTweak {
    const wchar_t* name;
    const char* spoken;           // what the voice says before the value
    float values[4]; int n;       // values[0] is replaced by the game's own value on first use
    const char* zeroWord;         // spoken instead of "0" (e.g. "off"), or null
    const char* suffix = "";      // spoken after the value (e.g. " percent")
    bool byConsole = false;       // the game sets it from its settings menu (console priority): we must too
    std::atomic<int> idx{ 0 };
    void* cv = nullptr; bool init = false, missing = false;
};

// key 8: output size in percent of the game's own resolution setting (drs.cpp OutputSetScale, shared with
// the dynamic resolution's second stage)
static const float kOutputSteps[4] = { 100.f, 90.f, 80.f, 70.f };
// low-memory mode: values[1] of the mip bias comes from the ini (LowMemoryMipBias)
static CvarTweak g_lm[2] = {
    { L"r.Streaming.MipBias", "Texture mip bias", { 0, 1 }, 2, nullptr },
    { L"r.Streaming.LimitPoolSizeToVRAM", "Limit pool", { 0, 1 }, 2, nullptr },
};
static bool g_lmApplied = false;   // the engine half of low-memory mode is in effect
static const float kFoveaSizes[4] = { 0.6f, 0.5f, 0.4f, 0.7f };
// <<: anti-aliasing while the upscaler is off (F5): game TAA -> MSAA 2x -> MSAA 8x. The forward renderer (r.ForwardShading=1)
// supports MSAA, which smooths edges inside each frame with no temporal blur. Changing r.MSAACount reallocates the
// render targets (one hitch). Refused while DLSS / FSR run (they need the temporal path); switching them on puts the
// game's TAA back. Console priority, like key 8: the game may set these from its own settings.
static CvarTweak g_aa[2] = {
    { L"r.MSAACount", "MSAA", { 0, 2, 8 }, 3, nullptr },
    { L"r.AntiAliasingMethod", "Anti aliasing", { 0, 3, 3 }, 3, nullptr },
};
static std::atomic<int> g_aaIdx{ 0 };   // 0 game (TAA), 1 MSAA 2x, 2 MSAA 8x
// r.RenderTargetPoolMin, read every poll on the game thread so the CSV thread can show it (RP column tag)
static void* g_rtPoolCv = nullptr; static bool g_rtPoolLooked = false;
static std::atomic<float> g_rtPoolMin{ -1.f };

static bool Pressed(int vk, bool& wasDown) { return HotkeyPressed(vk, wasDown); }   // common.h: no Ctrl/Alt/Shift/Win

// sets step i of a tweak (0 = the game's own value, read the first time); false if the cvar does not exist
static bool SetStep(CvarTweak& t, int i) {
    if (!t.init) {
        t.init = true;
        t.cv = EngineFindCVar(t.name);
        if (!t.cv) { t.missing = true; }
        else t.values[0] = EngineGetCVarFloat(t.cv);   // the game's value: step 0 restores it
    }
    char a[64]; WideCharToMultiByte(CP_UTF8, 0, t.name, -1, a, sizeof(a) - 1, nullptr, nullptr);
    if (t.missing) { Log("tweak: %s not found", a); return false; }
    t.idx = i;
    wchar_t b[32]; swprintf_s(b, L"%g", t.values[i]);
    const float before = EngineGetCVarFloat(t.cv);
    EngineSetCVar(t.cv, b, t.byConsole);
    const float after = EngineGetCVarFloat(t.cv);
    Log("tweak: %s %g -> %g%s%s", a, before, after, i == 0 ? " (game value)" : "",
        std::fabs(after - t.values[i]) > 1e-3f ? "  <- NOT APPLIED (the engine kept its value)" : "");
    return true;
}

// step i of the << cycle; false if a cvar is missing (then the game's values are put back)
static bool SetAa(int i) {
    for (auto& t : g_aa) t.byConsole = true;
    bool ok;
    if (i == 0) { ok = SetStep(g_aa[1], 0); ok = SetStep(g_aa[0], 0) && ok; }   // TAA first, then the game's sample count
    else { ok = SetStep(g_aa[0], i) && SetStep(g_aa[1], i); }
    if (!ok && i != 0) { SetStep(g_aa[1], 0); SetStep(g_aa[0], 0); i = 0; }
    g_aaIdx = i;
    return ok;
}

// low-memory mode, engine half: texture streaming one mip lower, pool capped to VRAM (or back to the game's values)
static void ApplyLowMemEngine(bool on) {
    if (on == g_lmApplied) return;
    g_lm[0].values[1] = g_cfg.lowMemMipBias;
    for (auto& t : g_lm) SetStep(t, on ? 1 : 0);
    g_lmApplied = on;
}

int TweaksAaMode() { return g_aaIdx.load(); }

bool TweaksPollKeys() {
    static bool down[20] = {};   // (17: key 0, 18: DLSS 5 mode, 19: DLSS 5 passes)
    bool changed = false;
    ShotPoll();
    BenchPoll();   // benchmark key (bench.cpp)
#ifndef ITR2_RELEASE
    StressPoll();  // stress test plan (stress.cpp)
#endif
    {   // <<: MSAA only while the upscaler is off
        const bool upscalerOn = (g_cfg.mode == 3 && g_dlssEnabled) || g_cfg.mode == 2;
        static bool aaLogged = false;
        if (!aaLogged) { aaLogged = true; Log("MSAA key (<<): virtual key 0x%02X", g_cfg.msaaKey); }
        if (g_aaIdx && upscalerOn) {   // DLSS / FSR switched back on (F5): back to the game's TAA
            SetAa(0);
            Log("tweak: anti-aliasing back to the game's (the upscaler is on)");
            Announce(Tone::Change, "M S A A off. Back to T A A.");
            changed = true;
        }
        if (Pressed(g_cfg.msaaKey, down[12])) {
            if (upscalerOn) Announce(Tone::Change, "M S A A needs D L S S off. Press F 5 first.");
            else {
                const int want = (g_aaIdx + 1) % 3;
                const bool ok = SetAa(want);
                const int i = g_aaIdx;
                Log("tweak: anti-aliasing -> %s%s", i == 0 ? "game (TAA)" : i == 1 ? "MSAA 2x" : "MSAA 8x", ok ? "" : " (a cvar is missing)");
                if (!ok) Announce(Tone::Change, "M S A A not available.");
                else Announce(Tone::Change, "%s", i == 0 ? "T A A, game default." : i == 1 ? "M S A A 2 x." : "M S A A 8 x.");
                changed = true;
            }
        }
    }
    OutputPoll();   // notices when the player changes the game's resolution menu (new "game value")
    if (Pressed(g_cfg.asyncRebuildKey, down[17])) {   // 0: DLSS centre models built on a side thread
        g_liveAsyncRebuild = !g_liveAsyncRebuild.load();
        Log("tweak: DLSS centre models built %s", g_liveAsyncRebuild.load() ? "on a side thread (the plain edges stand in meanwhile)" : "on the render path (old behaviour)");
        Announce(Tone::Change, "Side thread models %s.", g_liveAsyncRebuild.load() ? "on" : "off");
        changed = true;
    }
    if (Pressed(g_cfg.dlss5Key, down[18])) {   // [DLSS5] Key: off -> Order A (pre-upscale) -> Order B (post-upscale) -> off
        if (!Dlss5Available()) {
            Log("tweak: DLSS 5 not available (itr2_dlss5_bridge.dll / nvngx_dlssnr.dll not loaded)");
            Announce(Tone::Change, "D L S S 5 not available.");
        } else {
            const int next = (g_liveDlss5.load() + 1) % 3;
            g_liveDlss5 = next;
            static const char* names[3] = { "off", "Order A (before upscaling, window crop)", "Order B (after upscaling, window)" };
            static const char* spoken[3] = { "D L S S 5 off.", "D L S S 5, before upscaling.", "D L S S 5, after upscaling." };
            Log("tweak: DLSS 5 -> %d (%s)", next, names[next]);
            Announce(Tone::Change, "%s", spoken[next]);
            changed = true;
        }
    }
    if (Pressed(g_cfg.dlss5PassesKey, down[19])) {   // [DLSS5] PassesKey: 1 <-> 2 stacked passes per eye
        if (!Dlss5Available()) {
            Log("tweak: DLSS 5 not available (itr2_dlss5_bridge.dll / nvngx_dlssnr.dll not loaded)");
            Announce(Tone::Change, "D L S S 5 not available.");
        } else {
            const int next = g_liveDlss5Passes.load() >= 2 ? 1 : 2;
            g_liveDlss5Passes = next;
            Log("tweak: DLSS 5 passes -> %d%s", next, g_liveDlss5.load() ? "" : " (DLSS 5 is off now)");
            Announce(Tone::Change, "%s", next == 2 ? "D L S S 5, 2 passes." : "D L S S 5, 1 pass.");
            changed = true;
        }
    }
    if (g_liveLowMem && !g_lmApplied) ApplyLowMemEngine(true);   // LowMemory=1 in the ini: apply once in game
    if (Pressed(g_cfg.lowMemKey, down[10])) {   // -: low-memory mode
        g_liveLowMem = !g_liveLowMem;
        ApplyLowMemEngine(g_liveLowMem);
        Log("tweak: low-memory mode %s", g_liveLowMem ? "ON (unused DLSS/FSR features released, textures one mip lower)" : "OFF");
        Announce(Tone::Change, "Low memory %s.", g_liveLowMem ? "on" : "off");
        changed = true;
    }
    if (Pressed(g_cfg.foveaSizeKey, down[6])) {   // 7: fovea window size (the centre model is rebuilt once)
      if (!FoveaPossible()) Announce(Tone::Change, "Foveated needs D L S S.");
      else {
        const float cur = g_liveFoveaSize.load();
        int i = 0; while (i < 4 && std::fabs(kFoveaSizes[i] - cur) > 0.01f) ++i;
        const float next = kFoveaSizes[(i + 1) % 4];
        g_liveFoveaSize = next;
        Log("tweak: fovea window %.0f%% -> %.0f%%", cur * 100.f, next * 100.f);
        Announce(Tone::Change, "Window %.0f percent.", next * 100.f);
        changed = true;
      }
    }
    if (Pressed(g_cfg.outScaleKey, down[7])) {   // 8: output size (= the game's resolution setting, in % of its value)
        OutputPoll();
        const float cur = OutputScale();
        int i = 0; while (i < 4 && std::fabs(kOutputSteps[i] - cur) > 0.5f) ++i;
        const float next = i >= 4 ? 100.f : kOutputSteps[(i + 1) % 4];   // off the list (set by DRS): back to 100
        if (!OutputSetScale(next, false)) Announce(Tone::Change, "Output not available.");
        else if (next == 100.f) Announce(Tone::Change, "Output, game default.");
        else Announce(Tone::Change, "Output %.0f percent.", next);
        changed = true;
    }
    if (Pressed(g_cfg.edgeAaKey, down[0])) {     // 1: anti-aliasing of the plain foveated edges (dlss.cpp, edge_aa.hlsl)
        const int next = (g_liveEdgeAa.load() + 1) % 4;
        g_liveEdgeAa = next;
        static const char* names[4] = { "off", "spatial", "temporal", "fast temporal" };
        // plain edges are F9 "PLAIN", or any DLSS edge model while FSR runs the centre
        const bool plainNow = g_liveFovea.load() && (g_livePeriph.load() == kPresetPlain ||
                              (LivePreset() == kPresetFsr && g_livePeriph.load() != kPresetFsr));
        Log("tweak: plain-edge anti-aliasing %s%s", names[next], plainNow ? "" : " (plain edges are not on now)");
        Announce(Tone::Change, "Edge A A %s.%s", names[next], plainNow ? "" : " Works with plain edges.");
        changed = true;
    }
    // 5: A/B of the plain-edge jitter compensation sign. The engine draws a scene point at s + J (SceneVisibility.cpp:5326,
    // SceneView.h:589), so undoing the jitter means sampling at s + J; the code samples at s - PlainJitterSign*J, so -1
    // is the engine's convention. Read every frame by dlss.cpp (plain compose and the temporal edge AA): no rebuild.
    // Starts at the ini value.
    if (Pressed(g_cfg.jitterSignKey, down[15])) {
        g_cfg.plainJitterSign = g_cfg.plainJitterSign < 0.f ? 1.f : -1.f;
        const bool plainNow = g_liveFovea.load() && (g_livePeriph.load() == kPresetPlain ||
                              (LivePreset() == kPresetFsr && g_livePeriph.load() != kPresetFsr));
        Log("tweak: PlainJitterSign %+.0f (%s)%s", g_cfg.plainJitterSign, g_cfg.plainJitterSign < 0.f ? "new, engine convention" : "old",
            plainNow ? "" : " (plain edges are not on now)");
        Announce(Tone::Change, "Edge jitter, %s sign.%s", g_cfg.plainJitterSign < 0.f ? "new" : "old",
                 plainNow ? "" : " Works with plain edges.");
        changed = true;
    }
    if (Pressed(g_cfg.fullTaaKey, down[1])) {    // 2: the mod's TAA on the whole eye (runs in the upscaler's pass)
        const bool upscalerOn = g_cfg.mode == 3 && g_dlssEnabled;
        if (!upscalerOn && !g_liveFullTaa.load()) Announce(Tone::Change, "Mod T A A needs D L S S on. Press F 5 first.");
        else {
            g_liveFullTaa = !g_liveFullTaa.load();
            Log("tweak: whole-eye mod TAA %s", g_liveFullTaa.load() ? "ON (DLSS / FSR and foveation bypassed)" : "OFF");
            Announce(Tone::Change, "%s", g_liveFullTaa.load() ? "Mod T A A, whole screen." : "Mod T A A off.");
            changed = true;
        }
    }
    // 4: automatic A/B every MissSwapSeconds: dynamic resolution with the DLAA ceiling (adaptive DLSS size) <-> fixed
    // Quality (dynamic resolution off). Dynamic resolution keeps its state across the swaps (adaptive size, learned
    // budget, cost model) and resumes at its last render %, holding still for AbSettleSeconds (DrsResumeExact): the swap
    // rebuilds DLSS, whose image history starts again anyway.
    static ULONGLONG swapNext = 0;
    static bool abDynamic = true;
    auto applyAb = [&](bool dynamic, bool automatic) {
        abDynamic = dynamic;
        if (dynamic) {
            g_liveMissGuard = true;
            g_liveQuality = (int)Quality::DLAA;
            if (!g_liveDrs.load()) { DrsResumeExact(g_cfg.abSettleSeconds); g_liveDrs = true; DrsReset(); }
        } else {
            if (g_liveDrs.load()) { g_liveDrs = false; DrsReset(); }   // DrsReset keeps the render % for the resume
            g_liveQuality = (int)Quality::Quality;
        }
        g_tweakResize = true;
        Log("tweak: A/B -> %s%s", dynamic ? "dynamic resolution, DLAA ceiling (new controller, adaptive DLSS size)"
                                          : "fixed Quality (dynamic resolution off)", automatic ? " [auto A/B]" : "");
        Announce(Tone::Change, "%s", dynamic ? "Dynamic D L A A." : "Quality fixed.");
        changed = true;
    };
    if (Pressed(g_cfg.missSwapKey, down[14])) {
        if (swapNext) {
            swapNext = 0;
            Log("tweak: A/B auto swap off (staying %s)", abDynamic ? "dynamic DLAA" : "fixed Quality");
            Announce(Tone::Change, "Auto swap off. %s stays.", abDynamic ? "Dynamic D L A A" : "Quality fixed");
        } else {
            swapNext = GetTickCount64() + (ULONGLONG)(g_cfg.missSwapSeconds * 1000.f);
            Log("tweak: A/B auto swap on: dynamic DLAA <-> fixed Quality every %.0f s (settle %.0f s), starting with dynamic DLAA",
                g_cfg.missSwapSeconds, g_cfg.abSettleSeconds);
            Announce(Tone::Change, "Auto swap every %.0f seconds.", g_cfg.missSwapSeconds);
            applyAb(true, false);
        }
    }
    if (swapNext && GetTickCount64() >= swapNext) {
        swapNext = GetTickCount64() + (ULONGLONG)(g_cfg.missSwapSeconds * 1000.f);
        applyAb(!abDynamic, true);
    }
    if (Pressed(g_cfg.missGuardKey, down[13])) {   // 3: dynamic resolution's miss guard (A/B against the plain controller)
        g_liveMissGuard = !g_liveMissGuard.load();
        Log("tweak: DRS v5 %s", g_liveMissGuard.load() ? "ON (v5.2: miss guard, cost model, raise on the 2.5 s p90, stale frames skipped, budget learned both ways)"
                                                        : "OFF (the old controller; late frames and camera motion are still counted for the CSV)");
        Announce(Tone::Change, "New D R S %s.", g_liveMissGuard.load() ? "on" : "off");
        changed = true;
    }
    if (!g_rtPoolLooked) { g_rtPoolLooked = true; g_rtPoolCv = EngineFindCVar(L"r.RenderTargetPoolMin"); }
    if (g_rtPoolCv) g_rtPoolMin = EngineGetCVarFloat(g_rtPoolCv);
    // Pool flush ([DLSS] PoolFlushSeconds): when the upscaler turns on (start, F5, after a fallback) the game's own TAA
    // render targets fall out of use, and r.RenderTargetPoolMin (the game's 400 MB) lets the engine keep them. Set to 0
    // for a few seconds so the engine frees the unused targets once per frame, then put back: freed memory stays freed,
    // and the reserve still saves re-creations later. Left alone when the ini's [CVars] sets r.RenderTargetPoolMin.
    {
        static bool wasOn = false, userSet = false, checkedIni = false;
        static ULONGLONG until = 0; static float restore = -1.f, vramAtStart = 0.f;
        if (!checkedIni) {
            checkedIni = true;
            for (const wchar_t* e = g_cvarSection; *e; e += wcslen(e) + 1)
                if (!_wcsnicmp(e, L"r.RenderTargetPoolMin", 21)) userSet = true;
            if (userSet && g_cfg.poolFlushSeconds > 0.f) Log("pool flush: off ([CVars] sets r.RenderTargetPoolMin itself)");
        }
        const bool on = g_cfg.mode == 3 && g_dlssEnabled;
        if (g_rtPoolCv && g_cfg.poolFlushSeconds > 0.f && !userSet) {
            if (on && !wasOn && !until) {
                const float cur = EngineGetCVarFloat(g_rtPoolCv);
                if (cur > 0.5f) {
                    float budget = 0.f; VramQuery(vramAtStart, budget);
                    restore = cur;
                    EngineSetCVar(g_rtPoolCv, L"0", false);   // SetByCode, like the ini's [CVars]
                    until = GetTickCount64() + (ULONGLONG)(g_cfg.poolFlushSeconds * 1000.f);
                    Log("pool flush: upscaler on -> r.RenderTargetPoolMin %g -> 0 for %.0f s (process VRAM %.0f MB)", cur, g_cfg.poolFlushSeconds, vramAtStart);
                }
            }
            if (until && GetTickCount64() >= until) {
                until = 0;
                const float now = EngineGetCVarFloat(g_rtPoolCv);
                float used = 0.f, budget = 0.f; VramQuery(used, budget);
                if (now < 0.5f && restore > 0.5f) {
                    wchar_t b[16]; swprintf_s(b, L"%g", restore);
                    EngineSetCVar(g_rtPoolCv, b, false);
                    Log("pool flush: done, r.RenderTargetPoolMin back to %g (process VRAM %.0f -> %.0f MB, %+.0f)", restore, vramAtStart, used, used - vramAtStart);
                } else Log("pool flush: the value changed meanwhile, left at %g (process VRAM %.0f -> %.0f MB)", now, vramAtStart, used);
                g_rtPoolMin = EngineGetCVarFloat(g_rtPoolCv);
            }
        }
        wasOn = on;
    }
    return changed;
}

void TweaksState(char* buf, size_t n) {
    char q[16];
    const float os = OutputScale();
    if (std::fabs(os - 100.f) < 0.5f) snprintf(q, 16, "Q-"); else snprintf(q, 16, "Q%.0f", os);
    static const char* aa[3] = { "AA-", "AA2", "AA8" };
    // AR side-thread DLSS models (key 0); NR optional DLSS 5 bridge mode (when loaded)
    const int k = snprintf(buf, n, "W%.0f %s H%d X%.0f L%d OS%d %s PA%d FT%d LI%d RP%.0f WC%d JS%+.0f AR%d", g_liveFoveaSize.load() * 100.f, q, (int)g_liveHalfEdges.load(), DrsCeiling(), (int)g_liveLowMem.load(),
             (int)g_liveOutStage.load(), aa[g_aaIdx.load() % 3], g_liveEdgeAa.load(), (int)g_liveFullTaa.load(), (int)g_liveLean.load(), g_rtPoolMin.load(), g_liveWindowCrop.load(),
             g_cfg.plainJitterSign, (int)g_liveAsyncRebuild.load());
    if ((Dlss5Available() || g_liveDlss5.load() != 0) && k > 0 && (size_t)k + 9 < n)
        snprintf(buf + k, n - (size_t)k, " NR%d P%d", g_liveDlss5.load(), g_liveDlss5Passes.load());
}

// how many different jitter offsets the engine used over the last 256 frames: the game's own pattern length
// (diagnostics)
void TweaksJitterSample(float jx, float jy) {
    static int32_t ring[256][2]; static int n = 0, lastDistinct = -1, logs = 30;
    ring[n][0] = (int32_t)std::lround(jx * 4096.0); ring[n][1] = (int32_t)std::lround(jy * 4096.0);
    if (++n < 256) return;
    n = 0;
    int distinct = 0;
    for (int i = 0; i < 256; ++i) {
        bool seen = false;
        for (int k = 0; k < i && !seen; ++k) seen = ring[k][0] == ring[i][0] && ring[k][1] == ring[i][1];
        if (!seen) ++distinct;
    }
    if (distinct != lastDistinct && logs-- > 0) {
        lastDistinct = distinct;
        Log("jitter pattern: %d different offsets in the last 256 frames", distinct);
    }
}
