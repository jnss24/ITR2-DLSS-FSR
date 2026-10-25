// Shared declarations for itr2_dlss.dll.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>
#include "ue_layout.h"

// ---------------------------------------------------------------- logging
void LogInit(const wchar_t* path);
void Log(const char* fmt, ...);

// ---------------------------------------------------------------- config
enum class Quality : int { DLAA = 0, Quality = 1, Balanced = 2, Performance = 3, UltraPerformance = 4 };
constexpr int kPresetFsr = 100;     // "preset" value that selects AMD FSR instead of a DLSS model
constexpr int kPresetPlain = 101;   // foveated edges without an upscaler: the render stretched (F9 "PLAIN")
constexpr int kPresetAuto = 102;    // [DLSS] Preset=Auto: chosen from the GPU at start (RTX 40/50 M, other RTX K, else FSR)

struct Config {
    int  mode = 3;                 // 0 off, 1 hook + log, 2 passthrough copy, 3 DLSS
    bool allowUnknownBuild = false;
    Quality quality = Quality::DLAA;      // foveated upscaling and dynamic resolution keep DLAA affordable
    int  preset = 13;              // NVSDK_NGX_DLSS_Hint_Render_Preset_M
    bool autoExposure = false;
    float jitterSignX = 1.f, jitterSignY = 1.f;
    float motionSignX = 1.f, motionSignY = 1.f;
    float teleportResetCm = 50.f;
    bool disableFoveation = false;        // false: the game's menu foveation stays on with the mod
    int  toggleKey = VK_F8;        // 0 = no hotkey
    int  verboseFrames = 8;
    bool ngxLogging = false;
    float screenPercentageOverride = 0.f; // 0 = from quality
    bool dilateMotionVectors = true;      // closest-depth 3x3 dilation (less ghosting at silhouettes)
    int  presetKey = VK_F7;               // cycles presetCycle at runtime (0 = no hotkey)
    int  qualityKey = VK_F6;              // cycles qualityCycle at runtime (0 = no hotkey)
    int  presetCycle[8] = { 5, 10, 11, 12, 13, kPresetFsr };   // E, J, K, L, M, FSR (all models)
    int  presetCycleN = 6;
    Quality qualityCycle[5] = { Quality::UltraPerformance, Quality::Performance, Quality::Balanced, Quality::Quality, Quality::DLAA };
    int  qualityCycleN = 5;
    int  announce = 2;                    // 0 off, 1 beeps, 2 voice (falls back to beeps)
    int  voiceVolume = 100;               // 0..100
    int  voiceRate = 1;                   // -10..10
    wchar_t announceDevice[64] = L"";         // audio output whose name contains this; empty = a VR headset output if any, else the Windows default
    bool separateEyeInputs = false;       // DLSS: per-eye copies of colour/depth at (0,0) (helps add-ons that ignore subrects)
    bool translucencyMask = false;        // anti-ghosting mask from the game's separate translucency (UI text, particles)
    float maskScale = 1.f;                // mask strength (coverage * scale, clamped to 1)
    int  maskKey = 0;                     // toggles the mask at runtime (0 = no hotkey)
    // [Test] keys, 0 = none unless the ini sets one (the AMD test build binds F1 F2 F4, R, G, M = mask, C = screenshot)
    int  fsrExposureKey = 0;              // FSR exposure: the game's -> FSR's own -> none
    int  fsrVersionKey = 0;               // the next FSR version the runtime offers (3.1, 4, ...)
    int  cleanKey = 0;                    // output cleanup: NaN / infinite / negative -> 0, alpha 1
    int  resetKey = 0;                    // start every upscaler history again (both eyes, one frame)
    int  outFormatKey = 0;                // output in the game's own format (R11G11B10) <-> RGBA16F
    int  inCleanKey = 0;                  // the upscaler's colour input cleaned: NaN / infinite / negative -> 0, at most 4096
    int  autoResetKey = 0;                // an eye whose output goes bad (NaN, infinite, many very bright pixels) starts again
    bool outputProbe = false;             // image probe and diagnostics: bad pixels in and out, exposure and the finished image per eye,
                                          // history resets, FSR's own input checks
    bool foveaEnabled = true;             // foveated upscaling on at start
    int  peripheryPreset = kPresetPlain;  // edge model when foveated at start: plain edges (or a DLSS preset / kPresetFsr)
    int  foveaCycle[4] = { kPresetPlain, 5, kPresetFsr };   // F9: off -> each of these edge models -> off
    int  foveaCycleN = 3;
    float foveaSize = 0.6f;               // centre window, fraction of the eye's width and height
    float foveaFeather = 0.3f;            // soft edge, fraction of the window's radius
    bool foveaFollowGame = true;          // the game's foveation setting drives the foveated upscaling
    float foveaFollowMedium = 0.7f, foveaFollowHigh = 0.5f;   // window for the game's Medium / High (and High Top)
    float foveaGameCx = 0.575f, foveaGameCy = 0.5f;           // the engine's foveation centre (left eye; right = 1 - x)
    int  vramBallastMb = 0;               // TEST: hold this much VRAM on the game's GPU (simulates a smaller card), 0 = off
    int  fakeVramMb = 0;                  // TEST: VRAM size the mod's auto settings assume (LowMemory=2), 0 = the real one
    int  winDiagKey = 0;                  // debug key: diagnostic modes for the DLSS centre window (0 = none)
    int  foveaWindowCrop = 1;             // the DLSS centre window reads its own cropped inputs: 0 never, 1 always, 2 only J/K
                                          // (J/K pulse with sub-rect bases)
    float poolFlushSeconds = 5.f;         // r.RenderTargetPoolMin 0 for this long whenever the upscaler turns on (0 = off)
    bool centreDirect = true;             // DLSS centre written straight into the output (plain / half edges)
    float plainJitterSign = 1.f;          // plain edges: -1 if the edges wobble (flips the jitter compensation)
    int  foveaKey = VK_F9;                // toggles foveated upscaling at runtime (0 = no hotkey)
    bool drsEnabled = true;               // dynamic resolution on at start (render size only, OutputStage off)
    int  drsKey = VK_F8;                  // toggles it at runtime (0 = no hotkey)
    float drsTargetHz = 0.f;              // headset refresh rate, 0 = auto (VR runtime, else measured)
    float drsHeadroom = 0.85f;            // aim for this fraction of the frame budget (GPU time)
    float drsMinPercent = 50.f, drsMaxPercent = 100.f;   // render size range, screen percentage per side (DLSS: 50 at least)
    bool drsQualityCeiling = true;        // the F6 quality is the ceiling while dynamic resolution runs (DrsCeiling)
    int  drsInterval = 20;                // frames between adjustments
    bool drsOutputStage = false;          // second stage: change the output size (the game's resolution setting) when the render size is stuck;
                                          // off by default: each output change is a felt hitch (drop ~0.3 s, probe ~0.16 s)
    float drsOutMin = 70.f, drsOutMax = 100.f;   // output range, percent of the game's own resolution setting
    float drsOutCooldown = 10.f;          // seconds after any output change before a raise
    float drsOutUpHold = 15.f;            // seconds at the render ceiling with headroom before a raise
    float drsOutDownHold = 2.f;           // seconds at the render floor over budget before a drop
    float drsProbeWait = 20.f;            // seconds after an output drop before probing up again
    float drsProbeMaxWait = 240.f;        // ... doubled after every failed probe, up to this
    float drsOutUpMargin = 0.85f;         // a raise must keep the predicted p90 below this x budget
    float drsOutUpElasticity = 0.6f;      // share of the GPU time the raise prediction scales with the output area
    // miss guard (drs_logic.h): late frames from the headset's own timeline, GPU- or CPU-caused
    bool  missGuard = true;               // [DynamicRes] MissGuard: on at start (forced on in the release build)
    int   missGuardKey = '3';             // [Tweaks] MissGuardKey: toggles it (0 = no key)
    int   missSwapKey = '4';              // [Tweaks] MissSwapKey (debug): auto A/B every MissSwapSeconds between dynamic resolution
                                          //   with the DLAA ceiling (adaptive size) and fixed Quality
    float missSwapSeconds = 180.f;        // [Tweaks] MissSwapSeconds
    float abSettleSeconds = 5.f;          // [Tweaks] AbSettleSeconds: after each A/B swap dynamic resolution resumes where it was and
                                          //   holds for this long (the swap rebuilds DLSS: noisy frames)
    bool  drsAdaptiveSize = true;         // [DynamicRes] AdaptiveSize: the DLSS feature is made for a size the render uses
                                          //   (Quality size / 80 / 100, up to the ceiling) instead of always the ceiling
    bool  drsProfiles = true;             // [DynamicRes] Profiles: keep what was learned per setup (itr2_dlss_profiles.txt)
    float missGpuFrac = 0.85f;            // a late frame is GPU-caused when the GPU took at least this x the frame time
    int   missCount = 2;                  // this many GPU-caused late frames ...
    float missWindowS = 1.f;              // ... within this many seconds drop the render at once
    float missLockS = 20.f, missLockMaxS = 240.f;   // then no raise above the failed level for this long (doubling)
    // motion rule (debug build, with the guard): no raise while the camera turns / moves fast. Off until
    // the CSV columns cam_turn_dps / cam_move_mps show that the GPU time follows motion in this game
    bool  motionHold = false;             // [DynamicRes] MotionHold
    float motionTurnDps = 90.f, motionMoveMps = 1.5f;   // MotionTurnDps, MotionMoveMps
    // number-key tweaks (0 = no hotkey): 7 fovea window size, 8 output size
    int  foveaSizeKey = '7', outScaleKey = '8';
    bool halfEdges = false;               // foveated: edge model renders at half output size (at start)
    int  edgeAaKey = '1';                 // key 1: anti-aliasing of the plain foveated edges (off / spatial / temporal)
    int  jitterSignKey = '5';             // key 5: flips plainJitterSign live (A/B; debug build only)
    int  edgeAa = 2;                      // ... at start: 0 off, 1 spatial (FXAA-style), 2 temporal (small TAA, default)
    float edgeTaaBlend = 0.1f;            // temporal: weight of the current frame (lower = smoother, more ghosting)
    bool taaSmallHistory = true;          // AA textures in R11G11B10_FLOAT (4 bytes/pixel, dithered) instead of RGBA16F
    int  depthLayer = 0;                  // [DLSS] DepthLayer: 0 = the mod sets xr.OpenXRAllowDepthLayer 0 (no depth image for
                                          // the VR runtime; with it, objects near the eyes smear whenever the render is
                                          // below 100 %), 1 = leave the game's value
    bool leanInputs = true;               // lean inputs at start: no whole-eye colour/depth copies unless FSR needs them
    int  fullTaaKey = '2';                // key 2: the mod's temporal AA on the whole eye instead of DLSS / FSR (A/B vs the game's TAA)
    int  lowMemKey = 0xBD;                // '-' key: low-memory mode
    int  asyncRebuildKey = '0';           // [Tweaks] AsyncRebuildKey: key 0 toggles side-thread builds of the DLSS centre models (debug build)
    bool asyncRebuild = true;             // [DLSS] AsyncRebuild: side-thread builds at start (both builds)
    // [DLSS5]: optional standalone bridge DLL (itr2_dlss5_bridge.dll + nvngx_dlssnr.dll; keys in the debug build only)
    int   dlss5Mode = 0;                  // 0 off, 1 Order A (pre-upscale on window crops), 2 Order B (post-upscale on window)
    int   dlss5Key = 0;                   // cycles 0 -> 1 -> 2 -> 0 at runtime (0 = no key)
    int   dlss5Passes = 1;                // 1 or 2 stacked DLSS 5 passes per eye
    int   dlss5PassesKey = 0;             // switches 1 <-> 2 passes at runtime (0 = no key)
    float dlss5Intensity = 0.8f;
    float dlss5LocalTone = 0.75f;
    float dlss5LocalStructure = 0.9f;
    float dlss5SkinStructure = 0.9f;
    int   dlss5Style = 3;
    bool  dlss5AutoMask = true;
    bool  dlss5UiCorrection = false;
    int   dlss5Preset = 0;
    int  msaaKey = 0;                     // << key: game TAA / MSAA 2x / MSAA 8x with the upscaler off (ini -1 = the key that types <<)
    // [Screenshot]: a key that runs Meta's OculusMirror.exe --Screenshot (full-resolution PNG of the eye image)
    int  shotKey = VK_F11;                // 0 = no key
    wchar_t shotFolder[MAX_PATH] = {};    // empty = <dll folder>\itr2_dlss_shots\ (created when needed)
    wchar_t shotExe[MAX_PATH] = {};       // empty = <Oculus Base from the registry>\Support\oculus-diagnostics\OculusMirror.exe
    wchar_t shotArgs[256] = {};           // mirror options before --Screenshot
    int  shotMethod = 0;                  // 0 the mod copies the eye image itself (full resolution), 1 OculusMirror
    int  shotEye = 0;                     // 0 left, 1 right, 2 both side by side
    bool shotSrgb = true;                 // apply the sRGB curve: ViewFamilyTexture holds linear values
    bool shotPair = true;                 // the key takes a comparison: current state, then the upscaler toggled (Mod method, Mode=3)
    int  shotPairSettleMs = 2000;         // wait after the toggle before the second shot (temporal AA / DLSS history)
    int  shotStartDelayMs = 1500;         // wait after the key before the first burst/shot (the key press shakes the headset)
    bool shotSteady = true;               // motion outputs: shift each burst frame back into place (like the headset's timewarp)
    bool shotAlign = true;                // the comparison viewer lines the two shots up (head movement between them)
    bool shotBurst = true;                // the comparison also records N consecutive frames per side (shimmer)
    int  shotBurstFrames = 32, shotBurstW = 960, shotBurstH = 540;   // frames and crop size of each burst
    // [Benchmark] (bench.cpp): one key runs the game's TAA, then every F7 model at each listed quality, measuring GPU time
    // and VRAM, with a screenshot and a frame burst at each checkpoint, and writes a CSV and a comparison page
    int  benchKey = VK_F10;               // 0 = off
    int  benchSettleMs = 4000;            // after each model / quality change, before measuring
    int  benchBaselineMs = 12000;         // after switching the upscaler off (the mod frees its models after 10 s)
    int  benchMeasureMs = 4000;           // measuring window per checkpoint
    Quality benchQualities[5] = { Quality::UltraPerformance, Quality::Performance, Quality::Quality, Quality::DLAA };
    int  benchQualityN = 4;
    int  benchModels[8] = {};             // models to test; 0 entries = the F7 PresetCycle
    int  benchModelN = 0;
    bool benchFootage = true;             // a frame burst per checkpoint (footage and shimmer map in the page)
    bool benchShots = true;               // a full-resolution screenshot per checkpoint
    bool benchRelease = true;             // release everything the mod holds before each checkpoint (clean VRAM per setting)
    int  benchGapMs = 1000;               // upscaler off this long before the release (no frame in flight uses the models)
    int  benchStableMs = 2000, benchStableMb = 20, benchStableMaxMs = 8000;   // VRAM stable: StableMs within StableMb, wait at most StableMaxMs
    int  lowMemMode = 0;                  // low-memory mode at start: 0 off, 1 on, 2 auto (on below 10 GB of VRAM)
    float lowMemMipBias = 1.f;            // r.Streaming.MipBias while low-memory mode is on
    float fsrSharpness = 0.f;             // FSR RCAS sharpening 0..1 (0 = off)
    float fsrJitterSignX = 1.f, fsrJitterSignY = 1.f;
    wchar_t fsrVersion[32] = L"";         // part of an FSR version name to force (e.g. "3.1"); empty = AMD's choice
    bool perfEnabled = true;
    int  perfIntervalMs = 500;
    int  perfSummarySeconds = 10;
};

extern Config g_cfg;
extern wchar_t g_cvarSection[8192];  // [CVars] section of the ini: "name=value\0name=value\0\0"
extern wchar_t g_logDir[MAX_PATH];  // itr2_dlss_log\ next to the DLL (log + performance CSVs), or g_dir if it cannot be made
extern wchar_t g_dir[MAX_PATH];     // folder of itr2_dlss.dll (with trailing backslash)
void LoadConfig();
float QualityToScreenPercentage(Quality q);
float DrsQualityCeiling();           // the F6 quality's size as a dynamic-resolution ceiling (DLSS: 50 at least)
float DrsCeiling();                  // the ceiling in use: MaxPercent, capped by the quality's size when QualitySetsCeiling
float DrsFloor(float ceiling);       // the floor in use: MinPercent (DLSS: 50 at least), never above the ceiling
int QualityToNgxPerfQuality(Quality q);
const char* QualityName(Quality q);
const char* PresetName(int preset);  // "A".."Z" for DLSS presets 1..26, "FSR", or "?"

// ---------------------------------------------------------------- hotkeys
// Keys are read globally (GetAsyncKeyState), so they work from the headset with the game window unfocused.
// A press with Ctrl, Alt, Shift or Win held is ignored, so shortcuts in other programs (Ctrl+F5 in a browser,
// Alt+F8...) don't toggle the mod; a hotkey that is itself a modifier still works. True on the press; wasDown is
// tracked either way, so letting go of the modifier while the key is held does not fire.
inline bool HotkeyPressed(int vk, bool& wasDown) {
    if (!vk) return false;
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown = down;
    if (!pressed) return false;
    static const int mods[] = { VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN };
    for (int m : mods) if (m == vk) return true;   // the hotkey is a modifier key itself
    for (int m : { VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN })
        if (GetAsyncKeyState(m) & 0x8000) return false;
    return true;
}

// ---------------------------------------------------------------- live settings
// Start from the ini; the hotkeys change them on the game thread, the render/RHI threads read them.
extern std::atomic<int> g_livePreset;    // NVSDK_NGX_DLSS_Hint_Render_Preset_* (E 5, K 11, M 13) or kPresetFsr
extern std::atomic<int> g_liveQuality;   // Quality
inline Quality LiveQuality() { return (Quality)g_liveQuality.load(std::memory_order_relaxed); }
inline int LivePreset() { return g_livePreset.load(std::memory_order_relaxed); }
extern std::atomic<bool> g_liveMask;     // translucency mask on/off
extern std::atomic<bool> g_liveClean;    // output cleanup on/off ([Test] CleanKey)
extern std::atomic<bool> g_liveOutGame;  // output in the game's own format ([Test] OutFormatKey)
extern std::atomic<int>  g_resetPending; // bit per eye still to start its history again ([Test] ResetKey)
extern std::atomic<bool> g_liveInClean;  // the upscaler's colour input cleaned ([Test] InCleanKey)
extern std::atomic<bool> g_liveAutoReset; // a bad output starts that eye's history again ([Test] AutoResetKey)
bool DlssGameFormatOutputOk();           // the GPU can write R11G11B10 as a UAV (dlss.cpp, after init)
extern std::atomic<bool> g_liveFovea;    // foveated upscaling on/off (F9)
extern std::atomic<int> g_livePeriph;    // foveated: model for the edges (a DLSS preset, kPresetFsr or kPresetPlain)
extern std::atomic<bool> g_liveDrs;      // dynamic resolution on/off (F8)
extern std::atomic<float> g_liveSharpen; // sharpening after the upscaler, 0 = off (no key, stays 0)
extern std::atomic<float> g_liveFoveaSize;   // fovea window size (key 7)
extern std::atomic<bool>  g_liveHalfEdges;   // foveated edges at half output size ([Foveated] HalfResEdges)
extern std::atomic<int>   g_liveEdgeAa;      // plain foveated edges: 0 no AA, 1 spatial, 2 temporal (key 1)
extern std::atomic<bool>  g_liveLean;        // lean inputs: window crops by the motion pass, plain edges read the game's colour
extern std::atomic<int>   g_liveWindowCrop;  // live [Foveated] WindowCrop
extern std::atomic<bool>  g_liveFullTaa;     // the mod's temporal AA on the whole eye instead of DLSS / FSR (key 2)
extern char g_startNote[160];                 // spoken at the first VR frame instead of the usual state (e.g. driver too old)
extern std::atomic<int>   g_liveWinDiag;     // DLSS centre window diagnostic ([Debug] WindowDiagKey): bit 0 cropped inputs, bit 1 no exposure texture
extern std::atomic<int>   g_upscalerFallback; // set by the RHI thread after repeated failures: 1 = switch to FSR, 2 = upscaler off
extern std::atomic<bool>  g_liveLowMem;      // low-memory mode (key -)
extern std::atomic<bool>  g_liveOutStage;    // dynamic resolution may change the output size ([DynamicRes] OutputStage)
extern std::atomic<float> g_liveDrsMax;      // dynamic resolution ceiling, percent ([DynamicRes] MaxPercent; the stress test changes it)
extern std::atomic<bool>  g_liveMissGuard;   // dynamic resolution reacts to GPU-caused late frames (always on in release; key 3 in the debug build)
extern std::atomic<bool>  g_liveAsyncRebuild;  // DLSS centre models built on a side thread, the plain edges standing in (key 0)
extern std::atomic<int>   g_liveDlss5;         // [DLSS5] 0 off, 1 Order A (before upscaling), 2 Order B (after upscaling)
extern std::atomic<int>   g_liveDlss5Passes;   // [DLSS5] 1 or 2 stacked passes per eye
bool Dlss5Available();                         // itr2_dlss5_bridge.dll + nvngx_dlssnr.dll initialised

// ---------------------------------------------------------------- hitch diagnostics (hitch.cpp)
struct HitchRow {
    double psoMs = 0, psoMaxMs = 0, psoBgMs = 0; uint32_t psoN = 0, psoBgN = 0;   // PSO creation on game/render/RHI threads, others
    uint32_t gcN = 0; double gcMs = 0;                                              // garbage collections, their extra frame time
    double barriersPerEye = -1;                                                     // ResourceBarrier calls per eye pass (-1 = none)
    double createMs = 0;                                                            // CPU ms in NGX's create call (DLSS model rebuilds)
    double sideCreateMs = 0; uint32_t standInN = 0;                                 // key 0: the side thread's creates, eye passes without the DLSS centre
};
void HitchNoteThread(int kind);                   // 0 game, 1 render, 2 RHI: the threads whose PSO creations hold a frame up
struct ID3D12Device;
void HitchInitDevice(ID3D12Device* dev);         // init thread: PSO creation timer on the game's device (debug build)
void HitchOnGameFrame(double frameMs);            // game thread, per VR frame (perf.cpp): garbage collection detector
void HitchAddBarriers(uint32_t calls);            // RHI thread, per eye pass
void HitchAddCreate(double ms);                   // RHI thread: CPU time of one NGX create call
void HitchAddSideCreate(double ms);               // side thread (key 0): CPU time of its NGX create calls
void HitchAddStandIn();                           // RHI thread: an eye pass drew the stand-in instead of the DLSS centre
void HitchTake(HitchRow& r);                      // perf thread: the sums since the last row

// ---------------------------------------------------------------- image tweaks (tweaks.cpp)
bool TweaksPollKeys();                            // game thread, every frame: the tweak keys
int  TweaksAaMode();                              // 0 game TAA, 1 MSAA 2x, 2 MSAA 8x (key <<)
void ShotPoll();                                  // game thread, every frame: screenshot key (shot.cpp)
bool CaptureRequest(const wchar_t* file, int eyeMode, bool quiet = false);   // game thread: full-res PNG of the next frame (capture.cpp); false if busy
bool CaptureBusy();                               // until the PNG is written or the capture failed
uint32_t CaptureCopyCount();                      // copies recorded on the GPU so far
void CapturePostRenderViewFamily(void* builder);  // render thread, view extension slot 20
bool WritePngBgr(const wchar_t* file, const uint8_t* bgr, int w, int h);   // 24-bit PNG via WIC (COM initialised by the caller)
// burst: N consecutive frames, a crop around the foveation centre of the left eye (comparison shimmer, capture.cpp)
struct BurstFrames { uint8_t* data = nullptr; int n = 0, w = 0, h = 0; double fps = 0; };   // n frames of w*h BGR
bool BurstRequest(int frames, int w, int h);      // game thread; false if a burst is in progress
int  BurstCopied();                               // frames copied on the GPU so far
int  BurstState();                                // 0 idle, 1 capturing, 2 converting, 3 ready, 4 failed
bool BurstTake(BurstFrames& out);                 // ready frames moved out (state -> idle); free with BurstFree
void BurstReset();                                // failed -> idle
void BurstFree(BurstFrames& f);
// motion outputs from two bursts (motion.cpp): flicker map, lossless loop pages, MP4
struct MotionResult { double flickerTaa = 0, flickerMod = 0; bool map = false, page = false, video = false; int dx = 0, dy = 0, ow = 0, oh = 0;
                      int dropTaa = 0, dropMod = 0;      // frames left out (brightness far from the rest of the burst)
                      double shiftTaa = 0, shiftMod = 0; };   // rms per-frame camera shift taken out (px), 0 = not steadied
MotionResult WriteMotionOutputs(const wchar_t* base, const BurstFrames& taa, const BurstFrames& mod,
                                const char* taaTag, const char* modTag);
// benchmark (bench.cpp): one checkpoint's burst -> steadied lossless frame stack + shimmer heat map (motion.cpp); the
// shift against the first burst processed (the reference, stored in refMid) lines all checkpoints up in the page
struct BenchBurstOut { int n = 0, w = 0, h = 0, dropped = 0, dx = 0, dy = 0; double fps = 0, flicker = 0, p95 = 0, over2 = 0, shiftRms = 0;
                       bool stack = false, heat = false; };
bool BenchProcessBurst(const BurstFrames& in, const wchar_t* stackPath, const wchar_t* heatPath,
                       std::vector<uint8_t>& refMid, int& refW, int& refH, BenchBurstOut& out);
// one benchmark checkpoint (bench.cpp fills it; benchreport.cpp writes the CSV and the page)
struct BenchPoint {
    char label[64] = "", model[16] = "", quality[24] = "";
    bool taa = false;
    float pct = 0;                                    // render size in % of the output (100 for the game's TAA)
    int renderW = 0, renderH = 0, outW = 0, outH = 0;
    int samples = 0;
    double gpuMed = 0, gpuP90 = 0, frameMed = 0, fps = 0, latePct = 0, gameMed = 0, renderMed = 0;
    double mod0 = 0, mod1 = 0;                        // the mod's own GPU time per eye (upscaler, copies), ms
    double vramMb = 0;
    double vramOff = 0;                               // VRAM just before the setting, with everything of the mod released (0 = not measured)
    double vramPeak = 0;                              // highest VRAM from the switch to the end of the measuring
    double settleS = 0;                               // seconds from the switch until the measuring started
    bool hasShot = false, hasBurst = false;
    BenchBurstOut burst;
    char shot[160] = "", frames[160] = "", heat[160] = "";   // file names next to the page (UTF-8)
};
bool BenchWriteReport(const wchar_t* base, const std::vector<BenchPoint>& pts, const char* settings);
void BenchPoll();                                 // game thread, every frame: benchmark key and sequence (bench.cpp)
void BenchOnFrame();                              // game thread, once per VR frame: GPU / frame time samples
void BenchOnDlssGpu(int eye, double ms);          // RHI thread: the mod's GPU time per eye
bool BenchActive();                               // any thread: a benchmark is running (unused models are released at once)
// stress test (stress.cpp, debug build): a plan file + the debug loader's go signal run fixed / dynamic resolution steps
void StressPoll();                                // game thread, every frame
void StressOnFrame(float frameMs, float gpuMs, float gameMs, float renderMs);   // game thread (perf.cpp), per VR frame
void StressOnTiming(int lateKind, int skipped);   // game thread (perf.cpp): late frames and skipped refreshes
bool StressActive();                              // any thread: a stress test is running
int StressStep();                                 // its step (1-based; 0 = warm-up)
void ShotNewFolder(const char* kind, wchar_t (&base)[MAX_PATH]);   // shot.cpp: <folder>\itr2_<date>_<time>_<kind>_<foveation...>\itr2_<date>_<time>
bool ShotComparisonBusy();                        // shot.cpp: an F11 comparison is running
void PerfGetRenderSize(int& rw, int& rh, int& ow, int& oh);   // perf.cpp: the last render / output size per eye the upscaler saw
void TweaksJitterSample(float jx, float jy);      // render thread, eye 0 per frame: counts the jitter pattern length
void HeartbeatObserveFamily(void* family);        // render thread, end of each view family: camera jump-and-back detector
void CaptureNoteFrame(float jx, float jy, int renderW, int renderH, bool reset, float camDx, float camDy);   // render thread, eye 0: burst log
void TweaksState(char* buf, size_t n);            // any thread: short text for the CSV, e.g. "S0.5 J32 M-1 A16 R0 O0"

// console variables (engine.cpp, game thread)
void* EngineFindCVar(const wchar_t* name);
void EngineSetCVar(void* cv, const wchar_t* value, bool byConsole = false);
float EngineGetCVarFloat(void* cv);

// ---------------------------------------------------------------- dynamic resolution (drs.cpp, game thread)
void DrsReset();                                  // forget the current size (when switched off / DLSS off)
float DrsTick(float startPercent);                // once per VR frame while active; new screen percentage, or < 0 = no change
float DrsCurrentPercent();                        // 0 = not running
void DrsBlockString(char* buf, size_t n);         // what blocks an output raise right now (CSV, short tags)
void DrsSetOutputStage(bool on);                  // game thread: off puts a lowered output back to the game's value
void DrsObserveFrame();                           // game thread, every VR frame: frame rhythm and refresh-rate detection
void DrsHold(bool on);                            // game thread: on = no size changes (F11 comparison), off = resume
float DrsBudgetMs();                              // the GPU budget the render stage steered on last (0 = not running)
float DrsFeatureCeiling();                        // the size (% of the output) the DLSS / FSR features are made for while DRS runs
float DrsFeatureSizeNow();                        // the adaptive size in use (0 = not running / adaptive size off), for the CSV
// per-setup performance profiles (profile.cpp; game thread, from DrsTick)
bool ProfileTick(double now);                      // every DRS interval: true when the active profile changed (load or switch)
bool ProfileActive();
float ProfileGet(const char* name, int* samples = nullptr);   // the active profile's value (0 = none)
float ProfileLearn(const char* name, float v);     // one measurement into the active profile; returns the value now kept
float ProfileSet(const char* name, float v);       // replace a value in the active profile (not averaged)
const char* GpuName();                             // dlss.cpp: the adapter's name and driver version (from the start-up log)
const char* GpuDriver();
void DrsResumeExact(float settleS);               // the next (re)start resumes at the last render % and holds for settleS (A/B)
float PerfDlssRecentMs();                         // recent DLSS GPU ms per eye (smoothed; 0 = none yet)
int PerfGpuUtilRecent();                          // the last GPU use % from NVML (perf CSV interval; -1 = unknown)
extern std::atomic<bool> g_velPlaceholder;        // the velocity input is the engine's placeholder (menus, loading)
extern std::atomic<bool> g_tweakResize;           // a tweak changed DRS / quality: the engine settles 2 frames (engine.cpp)
float DrsScalingShare();                          // cost model: share of the GPU time that scales with the render area
void DrsObserveCamera(const double* rot9, const double* origin3);   // game thread, every VR frame (eye 0): motion rule
float TargetHz();                                 // the headset refresh rate in use (ini value, or detected)
// output size = the game's resolution setting (xr.SecondaryScreenPercentage.HMDRenderTarget), in percent of
// the game's own value; set by key 8 and by the dynamic resolution's second stage (game thread)
bool FoveaPossible();                             // foveated upscaling can run: DLSS available and a DLSS model in F7
bool OutputSetScale(float scale, bool byDrs);
float OutputScale();                              // any thread: percent in effect (100 = the game's value)
// Deletes the oldest files matching pattern (e.g. L"itr2_dlss_perf_*.csv") in dir (with trailing backslash) until
// at most 'keep' remain; the names carry a date-time, so name order = age. Returns how many were deleted.
int KeepNewestFiles(const wchar_t* dir, const wchar_t* pattern, int keep);
bool VramQuery(float& usedMb, float& budgetMb);   // process VRAM use and budget (DXGI), MB
bool VramQueryNonLocal(float& usedMb, float& budgetMb);   // the same for system memory used by the GPU (NON_LOCAL)
bool VramOverBudget();                                    // process VRAM over its budget (cached 0.25 s, logs changes)
void OutputPoll();                                // game thread, every frame: notices a change in the game's menu

// ---------------------------------------------------------------- spoken / beep feedback (announce.cpp)
enum class Tone : int { None = 0, On, Off, Change };
void AnnounceStart();                                    // init thread, once
void Announce(Tone tone, const char* fmt, ...);          // any thread; never blocks (latest message wins)

// ---------------------------------------------------------------- resolver
struct Resolved {
    uintptr_t base = 0;
    uintptr_t addr[ue::I_Count] = {};
    bool knownBuild = false;
};
extern Resolved g_res;
extern bool g_resolverForceScan;
bool ResolveAll(int requiredMode, HMODULE target = nullptr);   // false if something needed for requiredMode is missing

template <class T> inline T& At(const void* p, uint32_t off) { return *(T*)((const uint8_t*)p + off); }
template <class R, class... A> inline R VCall(const void* obj, int slot, A... a) {
    using Fn = R(*)(const void*, A...);
    return ((Fn)(*(void***)obj)[slot])(obj, a...);
}

// ---------------------------------------------------------------- engine glue (engine.cpp)
extern std::atomic<bool> g_dlssEnabled;      // runtime toggle (hotkey)
void* GetDynamicRHI();                       // GDynamicRHI value
bool StartEngineHooks();                     // called from the init thread

// ---------------------------------------------------------------- upscaler (upscaler.cpp)
void* NewUpscalerProxy();
extern const wchar_t* const kUpscalerName;

// ---------------------------------------------------------------- performance log (perf.cpp)
void PerfOnGameFrame(bool dlssActive);            // game thread, once per VR frame
void PerfOnFrameTiming(int lateKind, int skipped, float waitMs, float workMs);   // game thread (miss guard, debug build)
void PerfOnCamera(float turnDps, float moveMps);  // game thread (motion rule, debug build)
void PerfOnRenderSize(int rw, int rh, int ow, int oh);
void PerfOnDlssGpu(int eye, double ms);           // RHI thread
void PerfOnPassGpu(int eye, const double* seg);   // RHI thread: 5 pass times (motion, whole eye, centre, edge AA, final)
void PerfRunLoop();                               // background thread, never returns

// ---------------------------------------------------------------- DLSS / native (dlss.cpp)
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;
struct NativeFrame {
    int eye;
    int mode;                      // 2 copy, 3 dlss
    ID3D12Resource* color; ID3D12Resource* depth; ID3D12Resource* velocity;
    ID3D12Resource* exposure; ID3D12Resource* output;
    ID3D12Resource* translucency;  // the game's separate translucency (alpha = transmittance), or null
    ue::IntRect colorRect, depthRect, velocityRect;
    int renderW, renderH, outW, outH;
    float jitterX, jitterY, preExposure;
    bool reset;
    float clipToPrevClip[16];      // row-major, row-vector convention (v * M)
    float jitterNdcX, jitterNdcY;
    int preset;                    // live settings captured when the pass was added
    Quality quality;
    float cameraNear, fovY;        // cm, radians (FSR)
    bool fovea;                    // foveated upscaling requested for this frame
    int periphPreset;              // its edge model (a DLSS preset or kPresetFsr)
    bool drs;                      // dynamic resolution: features/textures sized for maxRenderW x maxRenderH
    int maxRenderW, maxRenderH;
    float sharpen;                 // sharpening after the upscaler (0 = off)
    float foveaSize;               // fovea window size (fraction of the eye)
    bool halfEdges;                // foveated: edge model at half output size
    int edgeAa;                    // plain foveated edges: 0 no AA, 1 spatial, 2 temporal
    bool fullTaa;                  // the mod's temporal AA on the whole eye instead of DLSS / FSR
    bool lowMem;                   // release every feature and texture this frame does not use
    int dlss5;                     // [DLSS5] 0 off, 1 Order A (pre-upscale), 2 Order B (post-upscale)
    float foveaNdcX, foveaNdcY;    // where the eye's optical axis lands (NDC), the centre of the fovea window
};
bool DlssInitOnInitThread();         // NGX and/or FSR init + PSO creation
bool DlssReady();                   // at least one upscaler is usable
bool DlssReleaseAll();               // game thread, DLSS off for a while: free all models, contexts and textures
bool NgxAvailable();                 // DLSS (NGX) initialised

// ---------------------------------------------------------------- AMD FSR (fsr.cpp), RHI thread except FsrInit
struct ID3D12Device;
bool FsrInit(ID3D12Device* dev);     // init thread, once: loads AMD's signed DLLs, lists the versions
bool FsrReady();
struct FsrEye;                       // one FSR context per eye
FsrEye* FsrGetEye(int eye);
FsrEye* FsrGetCentre(int eye);       // foveated with FSR in the centre: the window's context
bool FsrEnsure(FsrEye* e, int outW, int outH, int eye, int maxRenderW = 0, int maxRenderH = 0);   // max render 0 = the output
void FsrRelease(FsrEye* e);         // low-memory mode: drop an unused context
void FsrReleaseIdle(int eye, uint64_t frames);   // release contexts unused for that many frames
int FsrReleaseAll();                 // destroy every context now (DLSS off, under DlssReleaseAll's lock)
void FsrNewFrame();                  // once per frame (eye 0): retires old contexts
// exposure given to FSR: 0 the game's eye adaptation, 1 FSR's own auto exposure, 2 none (only the pre-exposure)
int FsrExposureMode();
void FsrSetExposureMode(int m);      // game thread; the contexts are rebuilt on their next use
// game thread: switch to the next FSR version the runtime lists (2.x left out); false if there is only one.
// 'name' gets the requested version's name; the running one is announced once a context is made with it
bool FsrNextVersion(char* name, size_t n);
const char* FsrRunningName();        // provider of the last context made ("3.1.5", "4.1.2", ...)
// rw/rh/ow/oh/fovY: 0 = the eye's own (render size, output size, field of view); else the fovea window's
bool FsrDispatch(ID3D12GraphicsCommandList* cl, FsrEye* e, const NativeFrame& f,
                 ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* mv, ID3D12Resource* reactive, ID3D12Resource* output,
                 int rw = 0, int rh = 0, int ow = 0, int oh = 0, float fovY = 0.f);
void DlssExecute(ID3D12GraphicsCommandList* cl, const NativeFrame& f);   // RHI thread
void CopyExecute(ID3D12GraphicsCommandList* cl, const NativeFrame& f);   // RHI thread
