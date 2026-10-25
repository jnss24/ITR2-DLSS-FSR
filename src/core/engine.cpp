// engine.cpp - one-time startup, game-thread hook, hand-built ISceneViewExtension, console variables
#include "common.h"

using namespace ue;

std::atomic<bool> g_dlssEnabled{ true };

void* GetDynamicRHI() { return *(void**)g_res.addr[I_GDynamicRHI]; }
static void* GetEngine() { return *(void**)g_res.addr[I_GEngine]; }
static void* GetConsoleManager() { return *(void**)g_res.addr[I_ConsoleSingleton]; }

// ================================================================ console variables (game thread)
static void* FindCVar(const wchar_t* name) {
    void* cm = GetConsoleManager();
    return cm ? VCall<void*>(cm, slot::ConsoleManager_FindConsoleVariable, name, false) : nullptr;
}
static void SetCVar(void* cv, const wchar_t* value, uint32_t setBy) {
    VCall<void>(cv, slot::ConsoleVariable_Set, value, setBy, (uint64_t)0 /* FName None */);
}
// for tweaks.cpp (game thread)
void* EngineFindCVar(const wchar_t* name) { return FindCVar(name); }
// byConsole: for cvars the game itself sets with a console command (its settings menu), which SetByCode cannot override
void EngineSetCVar(void* cv, const wchar_t* value, bool byConsole) { SetCVar(cv, value, byConsole ? ECVF_SetByConsole : ECVF_SetByCode); }
float EngineGetCVarFloat(void* cv) { return VCall<float>(cv, slot::ConsoleVariable_GetFloat); }

struct CVarState {
    bool  init = false;
    void* screenPct = nullptr; void* foveation = nullptr; void* taaUpscaler = nullptr;
    float origScreenPct = 100.f; int origFoveation = 0;
    bool  lastActive = false; bool lastValid = false;
} static g_cv;

static void ApplyCVars(bool dlssActive) {
    if (!g_cv.init) {
        g_cv.init = true;
        g_cv.screenPct = FindCVar(L"r.ScreenPercentage");
        g_cv.foveation = FindCVar(L"xr.VRS.FoveationLevel");
        g_cv.taaUpscaler = FindCVar(L"r.TemporalAA.Upscaler");
        if (g_cv.screenPct) g_cv.origScreenPct = VCall<float>(g_cv.screenPct, slot::ConsoleVariable_GetFloat);
        if (g_cv.foveation) g_cv.origFoveation = VCall<int32_t>(g_cv.foveation, slot::ConsoleVariable_GetInt);
        Log("cvars: r.ScreenPercentage=%p (%.1f) xr.VRS.FoveationLevel=%p (%d) r.TemporalAA.Upscaler=%p (%d)",
            g_cv.screenPct, g_cv.origScreenPct, g_cv.foveation, g_cv.origFoveation, g_cv.taaUpscaler,
            g_cv.taaUpscaler ? VCall<int32_t>(g_cv.taaUpscaler, slot::ConsoleVariable_GetInt) : -1);
        if (g_cv.taaUpscaler && g_cfg.mode >= 2 && VCall<int32_t>(g_cv.taaUpscaler, slot::ConsoleVariable_GetInt) == 0)
            SetCVar(g_cv.taaUpscaler, L"1", ECVF_SetByCode);

        // diagnostics: the cvars that decide whether the engine renders per-object velocity
        static const wchar_t* diag[] = { L"r.VelocityOutputPass", L"r.Velocity.ForceOutput", L"r.BasePassOutputsVelocity",
            L"r.VelocityOutputDepth", L"r.Velocity.EnableVertexDeformation", L"r.ParallelVelocity", L"r.MotionBlurQuality",
            L"r.AntiAliasingMethod", L"r.MSAACount", L"vr.SupportMobileSpaceWarp", L"r.ForwardShading" };
        for (const wchar_t* n : diag) {
            char a[96]; WideCharToMultiByte(CP_UTF8, 0, n, -1, a, 95, nullptr, nullptr);
            if (void* cv = FindCVar(n)) {
                const uint32_t flags = VCall<uint32_t>(cv, 3 /*GetFlags*/);
                Log("cvar %-36s = %g  (flags 0x%08X%s, set by 0x%02X)", a, VCall<float>(cv, slot::ConsoleVariable_GetFloat),
                    flags, (flags & 4) ? " READONLY" : "", flags >> 24);
            } else Log("cvar %-36s : not found", a);
        }
        // [CVars] section of the ini: applied once, SetByCode (read-only cvars will refuse)
        for (const wchar_t* e = g_cvarSection; *e; e += wcslen(e) + 1) {
            const wchar_t* eq = wcschr(e, L'=');
            if (!eq || *e == L';') continue;
            wchar_t name[128] = {}; wcsncpy_s(name, e, (size_t)(eq - e));
            char a[160]; WideCharToMultiByte(CP_UTF8, 0, e, -1, a, 159, nullptr, nullptr);
            void* cv = FindCVar(name);
            if (!cv) { Log("[CVars] %s : cvar not found", a); continue; }
            const float before = VCall<float>(cv, slot::ConsoleVariable_GetFloat);
            SetCVar(cv, eq + 1, ECVF_SetByCode);
            Log("[CVars] %s : %g -> %g", a, before, VCall<float>(cv, slot::ConsoleVariable_GetFloat));
        }
        // [DLSS] DepthLayer=0 (default): no depth image for the VR runtime. With it, objects close to the eyes smear in
        // the headset (not in the PC mirror) whenever the render is below 100 %, which only the mod does (the game alone
        // renders at 100 %). Left alone when [CVars] sets the cvar itself.
        if (!g_cfg.depthLayer) {
            bool userSet = false;
            for (const wchar_t* e = g_cvarSection; *e; e += wcslen(e) + 1)
                if (!_wcsnicmp(e, L"xr.OpenXRAllowDepthLayer", 24)) userSet = true;
            if (userSet) Log("depth layer: [CVars] sets xr.OpenXRAllowDepthLayer itself, DepthLayer=0 not applied");
            else if (void* cv = FindCVar(L"xr.OpenXRAllowDepthLayer")) {
                const float before = VCall<float>(cv, slot::ConsoleVariable_GetFloat);
                SetCVar(cv, L"0", ECVF_SetByCode);
                Log("depth layer: xr.OpenXRAllowDepthLayer %g -> %g (DepthLayer=0: no depth image for the VR runtime)",
                    before, VCall<float>(cv, slot::ConsoleVariable_GetFloat));
            } else Log("depth layer: xr.OpenXRAllowDepthLayer not found");
        } else Log("depth layer: DepthLayer=1, xr.OpenXRAllowDepthLayer left at the game's value");
    }
    // r.ScreenPercentage (project value is SetByProjectSetting, so SetByCode wins); dynamic resolution's size wins
    if (g_cv.screenPct) {
        const float drs = DrsCurrentPercent();
        const float want = !dlssActive ? g_cv.origScreenPct : (g_liveDrs && drs > 0.f) ? drs : QualityToScreenPercentage(LiveQuality());
        const float cur = VCall<float>(g_cv.screenPct, slot::ConsoleVariable_GetFloat);
        if (cur < want - 0.05f || cur > want + 0.05f) {
            wchar_t b[32]; swprintf_s(b, L"%.2f", want);
            SetCVar(g_cv.screenPct, b, ECVF_SetByCode);
            static int logs = 60;   // dynamic resolution changes it often: keep the log short
            if (logs-- > 0) Log("r.ScreenPercentage %.2f -> %.2f", cur, want);
        }
    }
    // xr.VRS.FoveationLevel: the game sets it with a console command, so SetByConsole is needed
    if (g_cv.foveation && g_cfg.disableFoveation) {
        // the game's VRS foveation is only replaced when the mod's own can run (DLSS); with FSR only (AMD, Intel,
        // GTX) the player keeps the game's
        const int want = dlssActive && NgxAvailable() ? 0 : g_cv.origFoveation;
        const int cur = VCall<int32_t>(g_cv.foveation, slot::ConsoleVariable_GetInt);
        if (cur != want) {
            if (dlssActive) g_cv.origFoveation = cur;   // remember the game's latest choice
            wchar_t b[16]; swprintf_s(b, L"%d", want);
            SetCVar(g_cv.foveation, b, ECVF_SetByConsole);
            Log("xr.VRS.FoveationLevel %d -> %d", cur, want);
        }
    }
}

// ================================================================ hand-built ISceneViewExtension
struct SharedCtrl { void** vtbl; volatile LONG shared; volatile LONG weak; };
struct ExtObj { void** vtbl; void* weakObj; void* weakCtrl; void* arrData; int32_t num, max; };
static_assert(sizeof(ExtObj) == 0x28, "FSceneViewExtensionBase is 0x28 bytes");

static void* g_extVt[slot::SceneViewExtension_Count];
static void* g_ctrlVt[2];
static ExtObj g_ext;
static SharedCtrl g_ctrl;

static void ExtNoop() {}
static void* ExtDtor(void* self, uint32_t) { return self; }
static int32_t ExtGetPriority(void*) { return 0; }
static bool ExtIsActiveThisFrame(void*, void*) { return g_cfg.mode >= 1; }
static bool ExtIsActiveInternal(void*, void*) { return true; }
// slot 20: PostRenderViewFamily_RenderThread(FRDGBuilder&, FSceneViewFamily&), after every post-process pass
static void ExtPostRenderViewFamily(void*, void* builder, void* family) {
#ifndef ITR2_RELEASE
    HeartbeatObserveFamily(family);   // diagnostics only (debug build)
#else
    (void)family;
#endif
    CapturePostRenderViewFamily(builder);
}
static void CtrlDestroyObject(void*) {}
static void* CtrlDtor(void* self, uint32_t) { return self; }

static int g_familyLogLeft = 16;
static bool g_dlssInitFailed = false;

// ================================================================ hotkeys + spoken feedback (game thread)
static bool KeyPressed(int vk, bool& wasDown) { return HotkeyPressed(vk, wasDown); }   // common.h: no Ctrl/Alt/Shift/Win

static const char* SpokenQuality(Quality q) {
    switch (q) {
    case Quality::DLAA: return "D L A A";
    case Quality::UltraPerformance: return "Ultra Performance";
    default: return QualityName(q);
    }
}

// With dynamic resolution on, the features are made in DLSS's Performance setting and the render size moves between
// the floor and the ceiling; with [DynamicRes] QualitySetsCeiling=1 the ceiling is the F6 quality's size (DrsCeiling),
// so the quality says how far up it may go. Said after the quality so it does not mislead (e.g. Ultra Performance =
// 50 % with DLSS, not 33); empty when dynamic resolution or the upscaler is off. afterDrs: right after "Dynamic
// resolution on." (F8), so the words are not said twice.
static const char* DrsRangeNote(bool afterDrs = false) {
    static char buf[136];
    if (!g_liveDrs.load() || !g_dlssEnabled) return "";
    const float hi = DrsCeiling(), lo = DrsFloor(hi);
    const char* who = afterDrs ? "" : " Dynamic resolution:";
    if (!g_cfg.drsQualityCeiling) {
        snprintf(buf, sizeof(buf), "%s render %.0f to %.0f percent, the quality setting is not used.", who, lo, hi);
    } else if (hi - lo < 0.5f) {   // floor = ceiling: Performance / Ultra Performance with DLSS
        const bool up = LiveQuality() == Quality::UltraPerformance && hi > QualityToScreenPercentage(LiveQuality()) + 0.5f;
        snprintf(buf, sizeof(buf), "%s the render stays at %.0f percent.%s", who, hi,
                 up ? " For 33, turn dynamic resolution off with F 8." : "");
    } else {
        const bool capped = g_liveDrsMax.load() + 0.5f < DrsQualityCeiling();   // MaxPercent below the quality
        snprintf(buf, sizeof(buf), "%s render %.0f to %.0f percent%s.", who, lo, hi, capped ? ", limited by the maximum setting" : "");
    }
    if (afterDrs && buf[0] == ' ') buf[1] = (char)toupper((unsigned char)buf[1]);   // " Render ..." / " The render ..."
    return buf;
}

static void AnnounceState(Tone tone) {
    const int p = LivePreset();
    if (!g_dlssEnabled) Announce(tone, "%s off.", p == kPresetFsr ? "F S R" : "D L S S");
    else if (p == kPresetFsr) Announce(tone, "F S R on. %s.%s", SpokenQuality(LiveQuality()), DrsRangeNote());
    else Announce(tone, "D L S S on. Model %s. %s.%s", PresetName(p), SpokenQuality(LiveQuality()), DrsRangeNote());
}

static void AnnounceModel(int p) {
    const char* what = p == kPresetFsr ? "F S R" : nullptr;
    char model[16]; snprintf(model, sizeof(model), "Model %s", PresetName(p));
    if (g_dlssEnabled) Announce(Tone::Change, "%s.", what ? what : model);
    else Announce(Tone::Change, "%s. Upscaler is off.", what ? what : model);
}

// next entry after the current value in a cycle list (the first one if the current value is not in it)
template <class T> static T NextInCycle(const T* list, int n, T cur) {
    for (int i = 0; i < n; ++i) if (list[i] == cur) return list[(i + 1) % n];
    return list[0];
}

// the window runs the F7 model: DLSS (any edges), or FSR (plain or FSR edges only)
bool FoveaPossible() { return LivePreset() == kPresetFsr ? FsrReady() : NgxAvailable(); }
static bool EdgeAllowed(int p) {
    if (LivePreset() == kPresetFsr) return p == kPresetFsr || p == kPresetPlain;
    return p == kPresetPlain || p == kPresetFsr ? true : NgxAvailable();
}

// [Foveated] FollowGame: the game's own foveation setting (graphics menu -> xr.VRS.FoveationLevel, 0 off,
// 1 Low, 2 Medium, 3 High, 4 High Top) also drives the mod's foveated upscaling. The engine shades at full rate
// inside a circle of 0.70 / 0.50 / 0.35 x the eye's half-diagonal (Low / Medium / High, read from the exe), so
// the expensive model covers that circle: Off and Low -> foveated off (the circle is nearly the whole eye),
// Medium -> MediumSize (0.7), High / High Top -> HighSize (0.5). Applied when the game's level changes (and once
// at start), so F9 / key 7 still override it until the next change in the menu.
static void FollowGameFoveation() {
    if (!g_cfg.foveaFollowGame || !g_cv.foveation || (!NgxAvailable() && !FsrReady())) return;
    static int last = -100;
    // with DisableFoveation the mod itself sets the cvar to 0 while DLSS is on: the game's choice is origFoveation
    const int level = g_cfg.disableFoveation ? g_cv.origFoveation : VCall<int32_t>(g_cv.foveation, slot::ConsoleVariable_GetInt);
    if (level == last) return;
    const bool first = last == -100;
    last = level;
    static const char* names[5] = { "off", "low", "medium", "high", "high top" };
    const char* name = level >= 0 && level <= 4 ? names[level] : "unknown";
    const bool on = level >= 2;
    const float size = level >= 3 ? g_cfg.foveaFollowHigh : g_cfg.foveaFollowMedium;
    g_liveFovea = on;
    if (on) {
        g_liveFoveaSize = size;
        bool inCycle = false;   // the edge model: keep the current one if it is one of F9's, else the ini's
        for (int i = 0; i < g_cfg.foveaCycleN; ++i) inCycle |= g_cfg.foveaCycle[i] == g_livePeriph;
        if (!inCycle) g_livePeriph = g_cfg.peripheryPreset;
        if (!EdgeAllowed(g_livePeriph)) g_livePeriph = kPresetPlain;   // FSR in the centre: plain edges
    }
    Log("foveated upscaling follows the game's foveation (%s, level %d) -> %s%s", name, level,
        on ? PresetName(g_livePeriph) : "off", on ? (size >= 0.65f ? " edges, window 70%" : " edges, window 50%") : "");
    if (first) return;   // the start-up announcement already says what runs
    if (on) Announce(Tone::Change, "Game foveation %s. Foveated, window %.0f percent.", name, size * 100.f);
    else Announce(Tone::Change, "Game foveation %s. Foveated off.", name);
}

// returns true when the render resolution may change (quality switch), so the caller lets it settle
static bool PollHotkeys() {
    static bool toggleDown = false, presetDown = false, qualityDown = false, maskDown = false, foveaDown = false, drsDown = false;
    if (g_cfg.mode != 3) return false;
    bool resize = false;
    if (KeyPressed(g_cfg.toggleKey, toggleDown)) {
        g_dlssEnabled = !g_dlssEnabled;
        Log("DLSS toggled %s", g_dlssEnabled ? "ON" : "OFF");
        AnnounceState(g_dlssEnabled ? Tone::On : Tone::Off);
    }
    if (KeyPressed(g_cfg.presetKey, presetDown)) {
        const int p = NextInCycle(g_cfg.presetCycle, g_cfg.presetCycleN, LivePreset());
        g_livePreset = p;
        Log("preset -> %s (%d)", PresetName(p), p);
        AnnounceModel(p);
    }
    if (KeyPressed(g_cfg.qualityKey, qualityDown)) {
        const Quality q = NextInCycle(g_cfg.qualityCycle, g_cfg.qualityCycleN, LiveQuality());
        g_liveQuality = (int)q;
        if (g_liveDrs.load()) Log("quality -> %s (dynamic resolution on: render %.0f-%.0f%%%s)", QualityName(q), DrsFloor(DrsCeiling()),
                                  DrsCeiling(), g_cfg.drsQualityCeiling ? ", ceiling = the quality's size" : ", the quality does not change it");
        else Log("quality -> %s (screen percentage %.1f)", QualityName(q), QualityToScreenPercentage(q));
        if (g_dlssEnabled) Announce(Tone::Change, "%s.%s", SpokenQuality(q), DrsRangeNote());
        else Announce(Tone::Change, "%s. Upscaler is off.", SpokenQuality(q));
        resize = true;
    }
    if (KeyPressed(g_cfg.maskKey, maskDown)) {
        g_liveMask = !g_liveMask;
        const bool fsrWindow = LivePreset() == kPresetFsr && g_liveFovea.load();   // the FSR window gets no mask
        Log("translucency mask toggled %s%s", g_liveMask ? "ON" : "OFF", fsrWindow ? " (FSR's foveated window does not use it: F9 off)" : "");
        Announce(Tone::Change, "Mask %s.%s", g_liveMask ? "on" : "off", fsrWindow && g_liveMask ? " Needs foveated off." : "");
    }
    {   // [Test] keys (AMD test build F1, F2, F4, R, G, N, A): FSR's exposure input, FSR version, output cleanup, history
        // reset, output format, input cleanup, automatic reset
        static bool expDown = false, verDown = false, cleanDown = false, resetDown = false, fmtDown = false, inDown = false, autoDown = false;
        const bool fsrNow = g_dlssEnabled && LivePreset() == kPresetFsr;
        if (KeyPressed(g_cfg.fsrExposureKey, expDown)) {
            if (!FsrReady()) Announce(Tone::Change, "F S R not available.");
            else {
                FsrSetExposureMode(FsrExposureMode() + 1);
                static const char* said[3] = { "game exposure", "F S R auto exposure", "no exposure" };
                Log("FSR exposure -> %d (%s)", FsrExposureMode(), FsrExposureMode() == 0 ? "the game's eye adaptation" :
                    FsrExposureMode() == 1 ? "FSR's own auto exposure" : "none, pre-exposure only");
                Announce(Tone::Change, "%s.%s", said[FsrExposureMode() % 3], fsrNow ? "" : " Applies when F S R runs.");
            }
        }
        if (KeyPressed(g_cfg.fsrVersionKey, verDown)) {
            char name[48] = {};
            if (!FsrReady()) Announce(Tone::Change, "F S R not available.");
            else if (!FsrNextVersion(name, sizeof(name))) {
                Log("FSR version: only one to choose from ('%s')", name);
                Announce(Tone::Change, "Only one F S R version available.");
            } else if (!fsrNow) Announce(Tone::Change, "F S R version %s chosen. Applies when F S R runs.", name);
            // (with FSR running, fsr.cpp says the version once the new contexts are made)
        }
        if (KeyPressed(g_cfg.cleanKey, cleanDown)) {
            g_liveClean = !g_liveClean.load();
            Log("output cleanup %s", g_liveClean.load() ? "ON (NaN / infinite / negative -> 0, alpha 1)" : "OFF");
            Announce(Tone::Change, "Cleanup %s.", g_liveClean.load() ? "on" : "off");
        }
        if (KeyPressed(g_cfg.resetKey, resetDown)) {
            g_resetPending = 3;   // bit per eye
            Log("history reset requested (both eyes, next frame)");
            Announce(Tone::Change, "History reset.");
        }
        if (KeyPressed(g_cfg.outFormatKey, fmtDown)) {
            if (!DlssGameFormatOutputOk()) {
                Log("output format: the game's format (R11G11B10) cannot be written by this GPU's compute shaders");
                Announce(Tone::Change, "Game format not supported.");
            } else {
                g_liveOutGame = !g_liveOutGame.load();
                Log("output format -> %s", g_liveOutGame.load() ? "the game's own (R11G11B10: no alpha, no negative values)" : "RGBA16F");
                Announce(Tone::Change, "Output %s.", g_liveOutGame.load() ? "game format" : "R G B A 16");
            }
        }
        if (KeyPressed(g_cfg.inCleanKey, inDown)) {
            g_liveInClean = !g_liveInClean.load();
            Log("input cleanup %s", g_liveInClean.load() ? "ON (the upscaler's colour input: NaN / infinite / negative -> 0, at most 4096)" : "OFF");
            Announce(Tone::Change, "Input cleanup %s.", g_liveInClean.load() ? "on" : "off");
        }
        if (KeyPressed(g_cfg.autoResetKey, autoDown)) {
            g_liveAutoReset = !g_liveAutoReset.load();
            Log("automatic reset %s", g_liveAutoReset.load() ? "ON (an eye whose output has NaN / infinite pixels or many very bright ones "
                "starts its history again)" : "OFF");
            Announce(Tone::Change, "Auto reset %s.", g_liveAutoReset.load() ? "on" : "off");
        }
    }
    if (KeyPressed(g_cfg.foveaKey, foveaDown)) {   // off -> each edge model of [Foveated] Cycle -> off
        if (!FoveaPossible()) {   // the centre window needs a DLSS model
            Log("foveated upscaling: not available (no DLSS for the window, and FSR not loaded)");
            Announce(Tone::Change, "Foveated not available.");
        } else {
            // next allowed edge model after the current one (with FSR in F7 only plain and FSR edges), or off
            int cur = -1;
            if (g_liveFovea) for (int i = 0; i < g_cfg.foveaCycleN; ++i) if (g_cfg.foveaCycle[i] == g_livePeriph) { cur = i; break; }
            if (g_liveFovea && cur < 0 && !EdgeAllowed(g_livePeriph)) cur = -1;   // e.g. E edges after switching F7 to FSR
            int next = -1;
            for (int i = cur + 1; i < g_cfg.foveaCycleN; ++i) if (EdgeAllowed(g_cfg.foveaCycle[i])) { next = i; break; }
            if (!g_liveFovea && next < 0) {   // the cycle has nothing allowed (e.g. only E with FSR in F7): plain edges
                g_livePeriph = kPresetPlain; g_liveFovea = true; next = -2;
            }
            if (next >= 0) { g_livePeriph = g_cfg.foveaCycle[next]; g_liveFovea = true; }
            else if (next == -1) g_liveFovea = false;
            const int p = g_livePeriph;
            Log("foveated upscaling -> %s (window %.0f%%)", g_liveFovea ? PresetName(p) : "off", g_liveFoveaSize.load() * 100.f);
            if (!g_liveFovea) Announce(Tone::Change, "Foveated off.");
            else if (p == kPresetFsr) Announce(Tone::Change, LivePreset() == kPresetFsr ? "Foveated, F S R centre and edges." : "Foveated, F S R edges.");
            else if (p == kPresetPlain) Announce(Tone::Change, LivePreset() == kPresetFsr ? "Foveated, F S R centre, plain edges." : "Foveated, plain edges, no upscaler.");
            else Announce(Tone::Change, "Foveated, %s edges.", PresetName(p));
        }
    }
    if (KeyPressed(g_cfg.drsKey, drsDown)) {
        g_liveDrs = !g_liveDrs;
        DrsReset();   // on: starts at the floor, or 10 % below where it last was (drs.cpp); off: back to the quality's size
        Log("dynamic resolution toggled %s (target %.0f Hz x %.0f%%, range %.0f-%.0f%%)", g_liveDrs ? "ON" : "OFF",
            TargetHz(), g_cfg.drsHeadroom * 100.f, DrsFloor(DrsCeiling()), DrsCeiling());
        if (g_liveDrs && g_dlssEnabled)   // say the range the render now moves in
            Announce(Tone::Change, "Dynamic resolution on.%s", DrsRangeNote(true));
        else if (!g_liveDrs && g_dlssEnabled)
            Announce(Tone::Change, "Dynamic resolution off. %s, %.0f percent.", SpokenQuality(LiveQuality()), QualityToScreenPercentage(LiveQuality()));
        else Announce(Tone::Change, "Dynamic resolution %s.", g_liveDrs ? "on" : "off");
        resize = true;   // the features are rebuilt once at the maximum size (or back at the quality size)
    }
    if (const int req = g_upscalerFallback.exchange(0)) {   // the RHI thread kept failing (e.g. out of VRAM)
        if (req == 1) {
            g_livePreset = kPresetFsr; g_liveFovea = false;
            Log("upscaler: DLSS kept failing, switched to FSR");
            Announce(Tone::Off, "D L S S failed. Switching to F S R.");
        } else {
            g_dlssEnabled = false;
            Log("upscaler: kept failing, turned off (the game's own anti-aliasing runs); see the lines above");
            Announce(Tone::Off, "Upscaler failed. Using the game's anti aliasing. Check the log.");
        }
        resize = true;
    }
    {   // [Debug] WindowDiagKey: input variants for the DLSS centre window, to find which one removes the J/K pulse
        static bool down = false;
        if (KeyPressed(g_cfg.winDiagKey, down)) {
            const int m = (g_liveWinDiag.load() + 1) % 4;
            g_liveWinDiag = m;
            static const char* names[4] = { "normal", "cropped inputs", "no exposure texture", "cropped inputs, no exposure texture" };
            Log("window diagnostic -> %d (%s)", m, names[m]);
            Announce(Tone::Change, "Window test %d. %s.", m, names[m]);
        }
    }
    FollowGameFoveation();
    TweaksPollKeys();   // number keys and the other A/B keys (tweaks.cpp)
    if (g_tweakResize.exchange(false)) resize = true;   // key 4's A/B changed DRS / quality: settle like F6 / F8
    return resize;
}
std::atomic<bool> g_tweakResize{ false };

static void ExtBeginRenderViewFamily(void* /*self*/, uint8_t* family) {
    static uint32_t calls = 0;
    static int settle = 0;
    ++calls;

    const bool resize = PollHotkeys();
    const bool dlssActive = g_cfg.mode == 3 && g_dlssEnabled;
    // A new r.ScreenPercentage only reaches the view family of the next frame. Installing DLSS in the
    // frame between would build a feature at the old render size and rebuild it a frame later, so for
    // a couple of frames after switching on (or changing quality) the game's own TAA runs instead.
    if (dlssActive && (resize || !g_cv.lastActive)) settle = 2;
    if (calls == 1 || (calls % 30) == 0 || dlssActive != g_cv.lastActive || !g_cv.lastValid || resize) {
        ApplyCVars(dlssActive);
        g_cv.lastActive = dlssActive; g_cv.lastValid = true;
    }

    const int32_t num = At<int32_t>(family, off::Family_ViewsNum);
    uint8_t** views = At<uint8_t**>(family, off::Family_ViewsData);
    bool stereo = (num == 2 && views);
    for (int i = 0; stereo && i < num; ++i) {
        const uint8_t* v = views[i];
        if (!v || At<int32_t>(v, off::View_StereoPass) == 0 || At<bool>(v, off::View_bIsSceneCapture) ||
            At<bool>(v, off::View_bIsReflectionCapture) || At<bool>(v, off::View_bIsPlanarReflection))
            stereo = false;
    }
    if (g_familyLogLeft > 0) {
        --g_familyLogLeft;
        Log("family %p views=%d main=%d upscaler=%p stereoFilter=%d isr=%d", family, num,
            At<uint8_t>(family, off::Family_bIsMainViewFamily), At<void*>(family, off::Family_TemporalUpscaler), stereo,
            (num > 0 && views && views[0]) ? At<uint8_t>(views[0], off::View_bIsInstancedStereo) : -1);
    }
    if (!stereo) return;
    DrsObserveFrame();   // frame rhythm (refresh-rate detection, idle detection for dynamic resolution)
    {   // camera motion for dynamic resolution: eye 0's view rotation (3x3 of the FMatrix44d) and origin
        const uint8_t* vm = views[0] + off::View_ViewMatrices;
        const double* m = (const double*)(vm + off::VM_ViewMatrix);
        const double rot[9] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] };
        DrsObserveCamera(rot, (const double*)(vm + off::VM_ViewOrigin));
    }
    {   // once, on the first VR frame: say what is running, so a glance at the log is not needed
        static bool announced = false;
        if (!announced) {
            announced = true;
            if (g_cfg.mode == 3 && g_startNote[0]) Announce(Tone::Off, "%s", g_startNote);
            else if (g_cfg.mode == 3) AnnounceState(g_dlssEnabled ? Tone::On : Tone::Off);
            else if (g_dlssInitFailed) Announce(Tone::Off, "D L S S unavailable. Check the log.");
        }
    }
    PerfOnGameFrame(dlssActive);
    BenchOnFrame();   // benchmark samples while it measures (bench.cpp)
    {   // DLSS off for 10 s: give the mod's VRAM back to the game (the per-frame cleanup only runs while DLSS is on)
        static double offSince = -1.0; static bool released = false;
        const double now = (double)GetTickCount64() / 1000.0;
        if (dlssActive || g_cfg.mode != 3) { offSince = -1.0; released = false; }
        else {
            if (offSince < 0.0) offSince = now;
            if (!released && now - offSince >= 10.0) {
                released = true;
                float before = 0.f, budget = 0.f; VramQuery(before, budget);
                float nlBefore = -1.f, nlBudget = 0.f; VramQueryNonLocal(nlBefore, nlBudget);
                if (DlssReleaseAll()) Log("DLSS off for 10 s: released all of the mod's models and textures (process VRAM %.0f MB, non-local %.0f MB before)", before, nlBefore);
            }
        }
    }
    // r.SceneRenderTargetResizeMethod: 2 (grow only) while dynamic resolution runs, so its constant size changes do not
    // reallocate the engine's render targets (a hitch each); the game's own value (0 = sized to the render) otherwise.
    // Kept at 2, the buffers stay at full size and fixed quality modes lose their VRAM saving (estimated 0.3-0.45 GB at
    // 2720x2976 per eye). Back to the game's value = one reallocation (a short hitch) when dynamic resolution goes off.
    {
        static void* rtCv = nullptr; static bool rtLooked = false, rtGrow = false; static float rtGame = 0.f;
        const bool want = dlssActive && g_liveDrs;
        if (want != rtGrow) {
            if (!rtLooked) {
                rtLooked = true;
                rtCv = FindCVar(L"r.SceneRenderTargetResizeMethod");
                if (rtCv) rtGame = VCall<float>(rtCv, slot::ConsoleVariable_GetFloat);
                else Log("DRS: r.SceneRenderTargetResizeMethod not found (render targets may be reallocated on size changes)");
            }
            rtGrow = want;
            if (rtCv) {
                const float before = VCall<float>(rtCv, slot::ConsoleVariable_GetFloat);
                wchar_t b[16]; swprintf_s(b, L"%g", want ? 2.f : rtGame);
                SetCVar(rtCv, b, ECVF_SetByCode);
                Log("DRS: r.SceneRenderTargetResizeMethod %g -> %g (%s)", before, VCall<float>(rtCv, slot::ConsoleVariable_GetFloat),
                    want ? "grow only while dynamic resolution runs" : "the game's own: render targets sized to the render again");
            }
        }
    }
    // dynamic resolution: the controller picks the screen percentage; the features were made for the
    // maximum size, so a change only needs the cvar (no settle, no rebuild)
    if (dlssActive && g_liveDrs) {
        if (DrsTick(QualityToScreenPercentage(LiveQuality())) > 0.f) ApplyCVars(dlssActive);
    } else if (DrsCurrentPercent() > 0.f) {
        DrsReset();
    }
    bool install = (g_cfg.mode == 2) || dlssActive;
    if (settle > 0) { --settle; if (g_cfg.mode == 3) install = false; }
    if (install && At<void*>(family, off::Family_TemporalUpscaler) == nullptr)
        At<void*>(family, off::Family_TemporalUpscaler) = NewUpscalerProxy();
}

static void RegisterExtensionNow() {
    for (auto& e : g_extVt) e = (void*)&ExtNoop;
    g_extVt[0] = (void*)&ExtDtor;
    g_extVt[5] = (void*)&ExtBeginRenderViewFamily;
    g_extVt[20] = (void*)&ExtPostRenderViewFamily;
    g_extVt[23] = (void*)&ExtGetPriority;
    g_extVt[24] = (void*)&ExtIsActiveThisFrame;
    g_extVt[29] = (void*)&ExtIsActiveInternal;
    g_ctrlVt[0] = (void*)&CtrlDestroyObject;
    g_ctrlVt[1] = (void*)&CtrlDtor;
    g_ctrl = { g_ctrlVt, 1, 2 };                     // our own permanent strong ref + WeakThis
    g_ext = { g_extVt, &g_ext, &g_ctrl, nullptr, 0, 0 };
    struct { void* obj; void* ctrl; } ref = { &g_ext, &g_ctrl };   // TSharedRef<ISceneViewExtension, ThreadSafe>
    ((void(*)(void*))g_res.addr[I_RegisterExtension])(&ref);
    Log("view extension registered (%p)", &g_ext);
}

// ================================================================ one-shot game-thread hook (UGameEngine::Tick)
using TickFn = void(*)(void*, float, bool);
static void** g_origEngineVt = nullptr;
static TickFn g_origTick = nullptr;
static void* g_engineVtMem[1 + 512];
static std::atomic<bool> g_registered{ false };

static void HookedTick(void* engine, float dt, bool idle) {
    if (!g_registered.exchange(true)) {
        InterlockedExchangePointer((void**)engine, g_origEngineVt);   // restore the real vtable first
        RegisterExtensionNow();
    }
    g_origTick(engine, dt, idle);
}

static bool InstallTickHook(void* engine) {
    g_origEngineVt = *(void***)engine;
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(g_origEngineVt - 1, &mbi, sizeof(mbi));
    const uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    size_t n = (regionEnd - (uintptr_t)(g_origEngineVt - 1)) / sizeof(void*);
    if (n > _countof(g_engineVtMem)) n = _countof(g_engineVtMem);
    if (n < 1 + slot::Engine_Tick + 1) { Log("ERROR: engine vtable too short"); return false; }
    memcpy(g_engineVtMem, g_origEngineVt - 1, n * sizeof(void*));    // includes the RTTI slot at [-1]
    void** copy = g_engineVtMem + 1;
    g_origTick = (TickFn)copy[slot::Engine_Tick];
    copy[slot::Engine_Tick] = (void*)&HookedTick;
    InterlockedExchangePointer((void**)engine, copy);
    Log("game-thread hook armed (UGameEngine::Tick at RVA 0x%llX)", (unsigned long long)((uintptr_t)g_origTick - g_res.base));
    return true;
}

// ================================================================ startup (init thread)
bool StartEngineHooks() {
    void* engine = nullptr;
    for (int i = 0; i < 6000; ++i) {   // up to 10 minutes
        engine = GetEngine();
        if (engine && At<void*>(engine, off::Engine_ViewExtensions) && GetDynamicRHI() && GetConsoleManager()) break;
        engine = nullptr;
        Sleep(100);
    }
    if (!engine) { Log("ERROR: engine never became ready"); return false; }

    const wchar_t* rhiName = VCall<const wchar_t*>(GetDynamicRHI(), slot::DynRHI_GetName);
    char nameA[64] = {}; WideCharToMultiByte(CP_UTF8, 0, rhiName ? rhiName : L"?", -1, nameA, 63, nullptr, nullptr);
    Log("engine ready: GEngine=%p RHI=%s", engine, nameA);
    if (!rhiName || wcscmp(rhiName, L"D3D12") != 0) {
        Log("RHI is not D3D12 (start the game with -dx12). Staying inactive.");
        return false;
    }
    if (g_cfg.mode >= 3 && !DlssInitOnInitThread()) {
        Log("DLSS init failed: falling back to Mode=1 (the game keeps its own TAA)");
        g_cfg.mode = 1;
        g_dlssInitFailed = true;
    }
    AnnounceStart();
    return InstallTickHook(engine);
}
