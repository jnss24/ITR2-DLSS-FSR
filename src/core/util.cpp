// Logging, config and the signature resolver (all one-time work).
#include "common.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <share.h>
#include <vector>
#include <string>

// ================================================================ logging
static SRWLOCK g_logLock = SRWLOCK_INIT;
static FILE* g_logFile = nullptr;

void LogInit(const wchar_t* path) {
    g_logFile = _wfsopen(path, L"w", _SH_DENYNO);   // shared: the log can be read while the game runs
}

void Log(const char* fmt, ...) {
    if (!g_logFile) return;
    char buf[1024];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d][t%05lu] ", st.wHour, st.wMinute, st.wSecond,
                     st.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&g_logLock);
    fputs(buf, g_logFile); fputc('\n', g_logFile); fflush(g_logFile);
    ReleaseSRWLockExclusive(&g_logLock);
}

// ================================================================ config
Config g_cfg;
wchar_t g_dir[MAX_PATH];
wchar_t g_logDir[MAX_PATH] = {};
wchar_t g_cvarSection[8192];

static float ReadFloat(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, float def) {
    wchar_t b[64]; GetPrivateProfileStringW(sec, key, L"", b, 64, ini);
    return b[0] ? (float)_wtof(b) : def;
}

std::atomic<int> g_livePreset{ 13 };
std::atomic<int> g_liveQuality{ (int)Quality::Quality };
std::atomic<bool> g_liveMask{ false };
std::atomic<bool> g_liveFovea{ false };
std::atomic<int> g_livePeriph{ 5 };
std::atomic<bool> g_liveDrs{ false };
std::atomic<float> g_liveSharpen{ 0.f };
std::atomic<float> g_liveFoveaSize{ 0.6f };
std::atomic<bool> g_liveHalfEdges{ false };
std::atomic<int>  g_liveEdgeAa{ 0 };
std::atomic<bool> g_liveFullTaa{ false };
std::atomic<int>  g_liveWindowCrop{ 1 };
std::atomic<bool> g_liveLean{ true };
std::atomic<float> g_liveDrsMax{ 67.f };
std::atomic<bool>  g_liveLowMem{ false };
std::atomic<bool>  g_liveOutStage{ true };
std::atomic<bool>  g_liveMissGuard{ false };
std::atomic<bool>  g_liveAsyncRebuild{ true };
std::atomic<int>   g_liveDlss5{ 0 };
std::atomic<int>   g_liveDlss5Passes{ 1 };
std::atomic<int>   g_upscalerFallback{ 0 };
std::atomic<int>   g_liveWinDiag{ 0 };
char g_startNote[160] = "";

static bool ParseQuality(const wchar_t* q, Quality& out) {
    if (!_wcsicmp(q, L"DLAA")) out = Quality::DLAA;
    else if (!_wcsicmp(q, L"Quality")) out = Quality::Quality;
    else if (!_wcsicmp(q, L"Balanced")) out = Quality::Balanced;
    else if (!_wcsicmp(q, L"Performance")) out = Quality::Performance;
    else if (!_wcsicmp(q, L"UltraPerformance")) out = Quality::UltraPerformance;
    else return false;
    return true;
}

static int ParsePreset(const wchar_t* p) {   // letter (A..Z), number, FSR, PLAIN / NONE or AUTO; 0 = invalid
    if (!_wcsicmp(p, L"FSR")) return kPresetFsr;
    if (!_wcsicmp(p, L"PLAIN") || !_wcsicmp(p, L"NONE")) return kPresetPlain;
    if (!_wcsicmp(p, L"AUTO")) return kPresetAuto;
    if (p[0] >= L'A' && p[0] <= L'Z' && p[1] == 0) return p[0] - L'A' + 1;
    if (p[0] >= L'a' && p[0] <= L'z' && p[1] == 0) return p[0] - L'a' + 1;
    return _wtoi(p);
}

// "E, K, M" -> callback per trimmed token
template <class F> static void ForEachToken(wchar_t* s, F f) {
    for (wchar_t* ctx = nullptr, *t = wcstok_s(s, L", ", &ctx); t; t = wcstok_s(nullptr, L", ", &ctx)) f(t);
}

const char* QualityName(Quality q) {
    static const char* n[] = { "DLAA", "Quality", "Balanced", "Performance", "UltraPerformance" };
    const int i = (int)q;
    return i >= 0 && i < 5 ? n[i] : "?";
}

const char* PresetName(int preset) {
    static const char* letters[26] = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                       "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" };
    if (preset == kPresetFsr) return "FSR";
    if (preset == kPresetPlain) return "PLAIN";
    if (preset == kPresetAuto) return "AUTO";
    return preset >= 1 && preset <= 26 ? letters[preset - 1] : "?";
}

void LoadConfig() {
    wchar_t ini[MAX_PATH]; wcscpy_s(ini, g_dir); wcscat_s(ini, L"itr2_dlss.ini");
    Config c;
    c.mode = GetPrivateProfileIntW(L"General", L"Mode", c.mode, ini);
    c.allowUnknownBuild = GetPrivateProfileIntW(L"General", L"AllowUnknownBuild", 0, ini) != 0;
    wchar_t q[32]; GetPrivateProfileStringW(L"DLSS", L"Quality", L"DLAA", q, 32, ini);
    if (!ParseQuality(q, c.quality)) c.quality = Quality::DLAA;
    wchar_t p[16]; GetPrivateProfileStringW(L"DLSS", L"Preset", L"M", p, 16, ini);
    c.preset = ParsePreset(p);
    c.presetKey = GetPrivateProfileIntW(L"DLSS", L"PresetKey", VK_F7, ini);
    c.qualityKey = GetPrivateProfileIntW(L"DLSS", L"QualityKey", VK_F6, ini);
    wchar_t list[128];
    GetPrivateProfileStringW(L"DLSS", L"PresetCycle", L"E,J,K,L,M,FSR", list, 128, ini);
    int n = 0;
    ForEachToken(list, [&](const wchar_t* t) { const int v = ParsePreset(t); if (v > 0 && n < 8) c.presetCycle[n++] = v; });
    if (n) c.presetCycleN = n;
    GetPrivateProfileStringW(L"DLSS", L"QualityCycle", L"UltraPerformance,Performance,Balanced,Quality,DLAA", list, 128, ini);
    n = 0;
    ForEachToken(list, [&](const wchar_t* t) { Quality v; if (ParseQuality(t, v) && n < 5) c.qualityCycle[n++] = v; });
    if (n) c.qualityCycleN = n;
    wchar_t a[16]; GetPrivateProfileStringW(L"Announce", L"Mode", L"Voice", a, 16, ini);
    c.announce = !_wcsicmp(a, L"Off") || !_wcsicmp(a, L"0") ? 0 : !_wcsicmp(a, L"Beep") || !_wcsicmp(a, L"1") ? 1 : 2;
    c.voiceVolume = GetPrivateProfileIntW(L"Announce", L"Volume", 100, ini);
    if (c.voiceVolume < 0) c.voiceVolume = 0; else if (c.voiceVolume > 100) c.voiceVolume = 100;
    c.voiceRate = GetPrivateProfileIntW(L"Announce", L"Rate", 1, ini);
    if (c.voiceRate < -10) c.voiceRate = -10; else if (c.voiceRate > 10) c.voiceRate = 10;
    GetPrivateProfileStringW(L"Announce", L"Device", L"", c.announceDevice, _countof(c.announceDevice), ini);
    c.autoExposure = GetPrivateProfileIntW(L"DLSS", L"AutoExposure", 0, ini) != 0;
    c.jitterSignX = ReadFloat(ini, L"DLSS", L"JitterSignX", 1.f);
    c.jitterSignY = ReadFloat(ini, L"DLSS", L"JitterSignY", 1.f);
    c.motionSignX = ReadFloat(ini, L"DLSS", L"MotionSignX", 1.f);
    c.motionSignY = ReadFloat(ini, L"DLSS", L"MotionSignY", 1.f);
    c.teleportResetCm = ReadFloat(ini, L"DLSS", L"TeleportResetCm", 50.f);
    c.screenPercentageOverride = ReadFloat(ini, L"DLSS", L"ScreenPercentageOverride", 0.f);
    c.disableFoveation = GetPrivateProfileIntW(L"DLSS", L"DisableFoveation", 0, ini) != 0;
    c.dilateMotionVectors = GetPrivateProfileIntW(L"DLSS", L"DilateMotionVectors", 1, ini) != 0;
    c.separateEyeInputs = GetPrivateProfileIntW(L"DLSS", L"SeparateEyeInputs", 0, ini) != 0;
    c.translucencyMask = GetPrivateProfileIntW(L"DLSS", L"TranslucencyMask", 0, ini) != 0;
    c.maskScale = ReadFloat(ini, L"DLSS", L"MaskScale", 1.f);
    if (c.maskScale < 0.f) c.maskScale = 0.f; else if (c.maskScale > 4.f) c.maskScale = 4.f;
    c.maskKey = GetPrivateProfileIntW(L"DLSS", L"MaskKey", 0, ini);
    c.fsrExposureKey = GetPrivateProfileIntW(L"Test", L"FsrExposureKey", 0, ini);
    c.fsrVersionKey = GetPrivateProfileIntW(L"Test", L"FsrVersionKey", 0, ini);
    c.cleanKey = GetPrivateProfileIntW(L"Test", L"CleanKey", 0, ini);
    c.resetKey = GetPrivateProfileIntW(L"Test", L"ResetKey", 0, ini);
    c.outFormatKey = GetPrivateProfileIntW(L"Test", L"OutFormatKey", 0, ini);
    c.inCleanKey = GetPrivateProfileIntW(L"Test", L"InCleanKey", 0, ini);
    c.autoResetKey = GetPrivateProfileIntW(L"Test", L"AutoResetKey", 0, ini);
    c.outputProbe = GetPrivateProfileIntW(L"Test", L"OutputProbe", 0, ini) != 0;
    c.foveaEnabled = GetPrivateProfileIntW(L"Foveated", L"Enabled", 1, ini) != 0;
    wchar_t pp[16]; GetPrivateProfileStringW(L"Foveated", L"PeripheryPreset", L"PLAIN", pp, 16, ini);
    c.peripheryPreset = ParsePreset(pp); if (c.peripheryPreset <= 0) c.peripheryPreset = kPresetPlain;
    GetPrivateProfileStringW(L"Foveated", L"Cycle", L"PLAIN,E,FSR", list, 128, ini);
    n = 0;
    ForEachToken(list, [&](const wchar_t* t) { const int v = ParsePreset(t); if (v > 0 && n < 4) c.foveaCycle[n++] = v; });
    if (n) c.foveaCycleN = n;
    c.foveaSize = ReadFloat(ini, L"Foveated", L"Size", 0.6f);
    if (c.foveaSize < 0.2f) c.foveaSize = 0.2f; else if (c.foveaSize > 0.95f) c.foveaSize = 0.95f;
    c.foveaFeather = ReadFloat(ini, L"Foveated", L"Feather", 0.3f);
    c.winDiagKey = GetPrivateProfileIntW(L"Debug", L"WindowDiagKey", 0, ini);
    c.vramBallastMb = std::min(14000, std::max(0, (int)GetPrivateProfileIntW(L"Debug", L"VramBallastMb", 0, ini)));
    c.fakeVramMb = std::max(0, (int)GetPrivateProfileIntW(L"Debug", L"FakeVramMb", 0, ini));
    c.foveaWindowCrop = GetPrivateProfileIntW(L"Foveated", L"WindowCrop", 1, ini);
    if (c.foveaWindowCrop < 0 || c.foveaWindowCrop > 2) c.foveaWindowCrop = 1;
    c.centreDirect = GetPrivateProfileIntW(L"Foveated", L"CentreDirect", 1, ini) != 0;
    c.poolFlushSeconds = std::min(60.f, std::max(0.f, ReadFloat(ini, L"DLSS", L"PoolFlushSeconds", 5.f)));
    c.plainJitterSign = ReadFloat(ini, L"Foveated", L"PlainJitterSign", 1.f) < 0.f ? -1.f : 1.f;
    c.foveaFollowGame = GetPrivateProfileIntW(L"Foveated", L"FollowGame", 1, ini) != 0;
    c.foveaFollowMedium = std::min(0.95f, std::max(0.2f, ReadFloat(ini, L"Foveated", L"MediumSize", 0.7f)));
    c.foveaFollowHigh = std::min(0.95f, std::max(0.2f, ReadFloat(ini, L"Foveated", L"HighSize", 0.5f)));
    c.foveaGameCx = std::min(0.9f, std::max(0.1f, ReadFloat(ini, L"Foveated", L"GameCentreX", 0.575f)));
    c.foveaGameCy = std::min(0.9f, std::max(0.1f, ReadFloat(ini, L"Foveated", L"GameCentreY", 0.5f)));
    if (c.foveaFeather < 0.02f) c.foveaFeather = 0.02f; else if (c.foveaFeather > 1.f) c.foveaFeather = 1.f;
    c.foveaKey = GetPrivateProfileIntW(L"Foveated", L"Key", VK_F9, ini);
    c.drsEnabled = GetPrivateProfileIntW(L"DynamicRes", L"Enabled", 1, ini) != 0;
    c.drsKey = GetPrivateProfileIntW(L"DynamicRes", L"Key", VK_F8, ini);
    c.drsTargetHz = ReadFloat(ini, L"DynamicRes", L"TargetHz", 0.f);   // 0 = auto
    if (c.drsTargetHz < 0.f) c.drsTargetHz = 0.f;
    else if (c.drsTargetHz > 0.f && c.drsTargetHz < 30.f) c.drsTargetHz = 30.f;
    else if (c.drsTargetHz > 240.f) c.drsTargetHz = 240.f;
    c.drsHeadroom = ReadFloat(ini, L"DynamicRes", L"Headroom", 0.85f);
    if (c.drsHeadroom < 0.5f) c.drsHeadroom = 0.5f; else if (c.drsHeadroom > 1.f) c.drsHeadroom = 1.f;
    c.drsMinPercent = ReadFloat(ini, L"DynamicRes", L"MinPercent", 50.f);
    c.drsMaxPercent = ReadFloat(ini, L"DynamicRes", L"MaxPercent", 100.f);
    c.drsQualityCeiling = GetPrivateProfileIntW(L"DynamicRes", L"QualitySetsCeiling", 1, ini) != 0;
    if (c.drsMinPercent < 25.f) c.drsMinPercent = 25.f;
    if (c.drsMaxPercent > 100.f) c.drsMaxPercent = 100.f;
    if (c.drsMaxPercent < c.drsMinPercent) c.drsMaxPercent = c.drsMinPercent;
    c.drsInterval = GetPrivateProfileIntW(L"DynamicRes", L"IntervalFrames", 20, ini);
    c.drsOutputStage = GetPrivateProfileIntW(L"DynamicRes", L"OutputStage", 0, ini) != 0;
    c.drsOutMin = ReadFloat(ini, L"DynamicRes", L"OutputMin", 70.f);
    c.drsOutMax = ReadFloat(ini, L"DynamicRes", L"OutputMax", 100.f);
    if (c.drsOutMin < 50.f) c.drsOutMin = 50.f; else if (c.drsOutMin > 100.f) c.drsOutMin = 100.f;
    if (c.drsOutMax < 100.f) c.drsOutMax = 100.f; else if (c.drsOutMax > 150.f) c.drsOutMax = 150.f;
    c.drsOutCooldown = ReadFloat(ini, L"DynamicRes", L"OutputCooldown", 10.f);
    if (c.drsOutCooldown < 3.f) c.drsOutCooldown = 3.f;
    c.drsOutUpHold = ReadFloat(ini, L"DynamicRes", L"OutputUpHold", 15.f);
    if (c.drsOutUpHold < 2.f) c.drsOutUpHold = 2.f;
    c.drsOutDownHold = ReadFloat(ini, L"DynamicRes", L"OutputDownHold", 2.f);
    if (c.drsOutDownHold < 0.5f) c.drsOutDownHold = 0.5f;
    c.drsOutUpMargin = ReadFloat(ini, L"DynamicRes", L"OutputUpMargin", 0.85f);
    c.drsOutUpElasticity = std::min(1.f, std::max(0.2f, ReadFloat(ini, L"DynamicRes", L"OutputUpElasticity", 0.6f)));
    c.drsProbeWait = std::min(600.f, std::max(5.f, ReadFloat(ini, L"DynamicRes", L"OutputProbeWait", 20.f)));
    c.drsProbeMaxWait = std::min(1200.f, std::max(c.drsProbeWait, ReadFloat(ini, L"DynamicRes", L"OutputProbeMaxWait", 240.f)));
    if (c.drsOutUpMargin < 0.5f) c.drsOutUpMargin = 0.5f; else if (c.drsOutUpMargin > 0.95f) c.drsOutUpMargin = 0.95f;
    if (c.drsInterval < 5) c.drsInterval = 5; else if (c.drsInterval > 120) c.drsInterval = 120;
    c.missGuard = GetPrivateProfileIntW(L"DynamicRes", L"MissGuard", 1, ini) != 0;
    c.missGuardKey = GetPrivateProfileIntW(L"Tweaks", L"MissGuardKey", '3', ini);
    c.missSwapKey = GetPrivateProfileIntW(L"Tweaks", L"MissSwapKey", '4', ini);
    c.missSwapSeconds = std::min(900.f, std::max(20.f, ReadFloat(ini, L"Tweaks", L"MissSwapSeconds", 180.f)));
    c.abSettleSeconds = std::min(30.f, std::max(0.f, ReadFloat(ini, L"Tweaks", L"AbSettleSeconds", 5.f)));
    c.drsAdaptiveSize = GetPrivateProfileIntW(L"DynamicRes", L"AdaptiveSize", 1, ini) != 0;
    c.drsProfiles = GetPrivateProfileIntW(L"DynamicRes", L"Profiles", 1, ini) != 0;
    c.missGpuFrac = std::min(1.f, std::max(0.5f, ReadFloat(ini, L"DynamicRes", L"MissGpuFraction", 0.85f)));
    c.missCount = std::min(10, std::max(1, (int)GetPrivateProfileIntW(L"DynamicRes", L"MissCount", 2, ini)));
    c.missWindowS = std::min(5.f, std::max(0.2f, ReadFloat(ini, L"DynamicRes", L"MissWindow", 1.f)));
    c.missLockS = std::min(600.f, std::max(2.f, ReadFloat(ini, L"DynamicRes", L"MissLock", 20.f)));
    c.missLockMaxS = std::min(1200.f, std::max(c.missLockS, ReadFloat(ini, L"DynamicRes", L"MissLockMax", 240.f)));
    c.motionHold = GetPrivateProfileIntW(L"DynamicRes", L"MotionHold", 0, ini) != 0;
    c.motionTurnDps = std::min(720.f, std::max(10.f, ReadFloat(ini, L"DynamicRes", L"MotionTurnDps", 90.f)));
    c.motionMoveMps = std::min(20.f, std::max(0.2f, ReadFloat(ini, L"DynamicRes", L"MotionMoveMps", 1.5f)));
    c.foveaSizeKey = GetPrivateProfileIntW(L"Tweaks", L"FoveaSizeKey", '7', ini);
    c.outScaleKey = GetPrivateProfileIntW(L"Tweaks", L"OutputScaleKey", '8', ini);
    c.lowMemKey = GetPrivateProfileIntW(L"Tweaks", L"LowMemoryKey", 0xBD, ini);
    c.asyncRebuildKey = GetPrivateProfileIntW(L"Tweaks", L"AsyncRebuildKey", '0', ini);
    c.asyncRebuild = GetPrivateProfileIntW(L"DLSS", L"AsyncRebuild", 1, ini) != 0;
    {   // [Benchmark]
        c.benchKey = GetPrivateProfileIntW(L"Benchmark", L"Key", VK_F10, ini);
        c.benchSettleMs = std::min(30000, std::max(1000, (int)GetPrivateProfileIntW(L"Benchmark", L"SettleMs", 4000, ini)));
        c.benchBaselineMs = std::min(60000, std::max(11000, (int)GetPrivateProfileIntW(L"Benchmark", L"BaselineMs", 12000, ini)));
        c.benchMeasureMs = std::min(30000, std::max(1000, (int)GetPrivateProfileIntW(L"Benchmark", L"MeasureMs", 4000, ini)));
        c.benchFootage = GetPrivateProfileIntW(L"Benchmark", L"Footage", 1, ini) != 0;
        c.benchShots = GetPrivateProfileIntW(L"Benchmark", L"Screenshots", 1, ini) != 0;
        c.benchRelease = GetPrivateProfileIntW(L"Benchmark", L"ReleaseBetween", 1, ini) != 0;
        c.benchGapMs = std::min(5000, std::max(300, (int)GetPrivateProfileIntW(L"Benchmark", L"GapMs", 1000, ini)));
        c.benchStableMs = std::min(10000, std::max(500, (int)GetPrivateProfileIntW(L"Benchmark", L"StableMs", 2000, ini)));
        c.benchStableMb = std::min(500, std::max(4, (int)GetPrivateProfileIntW(L"Benchmark", L"StableMb", 20, ini)));
        c.benchStableMaxMs = std::min(30000, std::max(1000, (int)GetPrivateProfileIntW(L"Benchmark", L"StableMaxMs", 8000, ini)));
        wchar_t bl[128];
        GetPrivateProfileStringW(L"Benchmark", L"Qualities", L"UltraPerformance,Performance,Quality,DLAA", bl, 128, ini);
        int bn = 0;
        ForEachToken(bl, [&](const wchar_t* t) { Quality v; if (ParseQuality(t, v) && bn < 5) c.benchQualities[bn++] = v; });
        if (bn) c.benchQualityN = bn;
        GetPrivateProfileStringW(L"Benchmark", L"Models", L"", bl, 128, ini);   // empty = the F7 PresetCycle
        bn = 0;
        ForEachToken(bl, [&](const wchar_t* t) { const int v = ParsePreset(t); if (v > 0 && v != kPresetPlain && bn < 8) c.benchModels[bn++] = v; });
        c.benchModelN = bn;
    }
    c.msaaKey = GetPrivateProfileIntW(L"Tweaks", L"MsaaKey", -1, ini);
    if (c.msaaKey < 0) {   // the key that types << on this keyboard layout (Portuguese: second key right of 0); else ]
        const SHORT k = VkKeyScanW(L'\u00AB');
        c.msaaKey = (k != -1 && (k & 0xFF00) == 0) ? (k & 0xFF) : 0xDD;
    }
    c.shotKey = GetPrivateProfileIntW(L"Screenshot", L"Key", VK_F11, ini);
    GetPrivateProfileStringW(L"Screenshot", L"Folder", L"", c.shotFolder, MAX_PATH, ini);
    GetPrivateProfileStringW(L"Screenshot", L"MirrorExe", L"", c.shotExe, MAX_PATH, ini);
    GetPrivateProfileStringW(L"Screenshot", L"MirrorArgs", L"--LeftEyeOnly --NativeResolution --DisableTimewarp",
                             c.shotArgs, _countof(c.shotArgs), ini);
    {
        wchar_t m[32]; GetPrivateProfileStringW(L"Screenshot", L"Method", L"Mod", m, 32, ini);
        c.shotMethod = _wcsicmp(m, L"Mirror") == 0 ? 1 : 0;
        wchar_t e[32]; GetPrivateProfileStringW(L"Screenshot", L"Eye", L"Left", e, 32, ini);
        c.shotEye = _wcsicmp(e, L"Right") == 0 ? 1 : _wcsicmp(e, L"Both") == 0 ? 2 : 0;
        c.shotSrgb = GetPrivateProfileIntW(L"Screenshot", L"SrgbEncode", 1, ini) != 0;
        c.shotPair = GetPrivateProfileIntW(L"Screenshot", L"Pair", 1, ini) != 0;
        c.shotAlign = GetPrivateProfileIntW(L"Screenshot", L"Align", 1, ini) != 0;
        c.shotBurst = GetPrivateProfileIntW(L"Screenshot", L"Motion", 1, ini) != 0;
        c.shotBurstFrames = std::min(64, std::max(4, (int)GetPrivateProfileIntW(L"Screenshot", L"MotionFrames", 32, ini)));
        c.shotBurstW = std::min(4096, std::max(128, (int)GetPrivateProfileIntW(L"Screenshot", L"MotionWidth", 960, ini))) & ~1;
        c.shotBurstH = std::min(4096, std::max(128, (int)GetPrivateProfileIntW(L"Screenshot", L"MotionHeight", 540, ini))) & ~1;
        // the lossless page stacks the frames vertically in one image; browsers cap images at ~32000 px
        if (c.shotBurstFrames * c.shotBurstH > 32000) c.shotBurstFrames = std::max(4, 32000 / c.shotBurstH);
        c.shotPairSettleMs = GetPrivateProfileIntW(L"Screenshot", L"PairSettleMs", 2000, ini);
        if (c.shotPairSettleMs < 300) c.shotPairSettleMs = 300; else if (c.shotPairSettleMs > 20000) c.shotPairSettleMs = 20000;
        c.shotStartDelayMs = std::min(10000, std::max(0, (int)GetPrivateProfileIntW(L"Screenshot", L"StartDelayMs", 1500, ini)));
        c.shotSteady = GetPrivateProfileIntW(L"Screenshot", L"MotionSteady", 1, ini) != 0;
    }
    c.lowMemMode = GetPrivateProfileIntW(L"Tweaks", L"LowMemory", 0, ini);
    c.lowMemMipBias = ReadFloat(ini, L"Tweaks", L"LowMemoryMipBias", 1.f);
    c.halfEdges = GetPrivateProfileIntW(L"Foveated", L"HalfResEdges", 0, ini) != 0;
    c.edgeAaKey = GetPrivateProfileIntW(L"Tweaks", L"EdgeAaKey", '1', ini);
    c.jitterSignKey = GetPrivateProfileIntW(L"Tweaks", L"JitterSignKey", '5', ini);
    c.fullTaaKey = GetPrivateProfileIntW(L"Tweaks", L"FullTaaKey", '2', ini);
    c.depthLayer = GetPrivateProfileIntW(L"DLSS", L"DepthLayer", 0, ini) != 0 ? 1 : 0;
    c.leanInputs = GetPrivateProfileIntW(L"Foveated", L"LeanInputs", 1, ini) != 0;
    c.taaSmallHistory = GetPrivateProfileIntW(L"Foveated", L"PlainTaaSmallHistory", 1, ini) != 0;
    c.edgeAa = GetPrivateProfileIntW(L"Foveated", L"PlainEdgeAA", 2, ini);
    if (c.edgeAa < 0 || c.edgeAa > 3) c.edgeAa = 0;
    c.edgeTaaBlend = std::min(1.f, std::max(0.02f, ReadFloat(ini, L"Foveated", L"PlainTaaBlend", 0.1f)));
    c.fsrSharpness = ReadFloat(ini, L"FSR", L"Sharpness", 0.f);
    if (c.fsrSharpness < 0.f) c.fsrSharpness = 0.f; else if (c.fsrSharpness > 1.f) c.fsrSharpness = 1.f;
    c.fsrJitterSignX = ReadFloat(ini, L"FSR", L"JitterSignX", 1.f);
    c.fsrJitterSignY = ReadFloat(ini, L"FSR", L"JitterSignY", 1.f);
    GetPrivateProfileStringW(L"FSR", L"Version", L"", c.fsrVersion, _countof(c.fsrVersion), ini);
    c.toggleKey = GetPrivateProfileIntW(L"DLSS", L"ToggleKey", VK_F5, ini);
    c.dlss5Mode = std::min(2, std::max(0, (int)GetPrivateProfileIntW(L"DLSS5", L"Mode", 0, ini)));
    c.dlss5Key = GetPrivateProfileIntW(L"DLSS5", L"Key", 0, ini);
    c.dlss5Passes = std::min(2, std::max(1, (int)GetPrivateProfileIntW(L"DLSS5", L"Passes", 1, ini)));
    c.dlss5PassesKey = GetPrivateProfileIntW(L"DLSS5", L"PassesKey", 0, ini);
    c.dlss5Intensity = std::min(2.f, std::max(0.f, ReadFloat(ini, L"DLSS5", L"Intensity", 0.8f)));
    c.dlss5LocalTone = std::min(2.f, std::max(0.f, ReadFloat(ini, L"DLSS5", L"LocalToneStrength", 0.75f)));
    c.dlss5LocalStructure = std::min(2.f, std::max(0.f, ReadFloat(ini, L"DLSS5", L"LocalStructureStrength", 0.9f)));
    c.dlss5SkinStructure = std::min(2.f, std::max(0.f, ReadFloat(ini, L"DLSS5", L"SkinStructureStrength", 0.9f)));
    c.dlss5Style = std::min(8, std::max(0, (int)GetPrivateProfileIntW(L"DLSS5", L"Style", 3, ini)));
    c.dlss5AutoMask = GetPrivateProfileIntW(L"DLSS5", L"UseAutoMask", 1, ini) != 0;
    c.dlss5UiCorrection = GetPrivateProfileIntW(L"DLSS5", L"UICorrection", 0, ini) != 0;
    c.dlss5Preset = std::max(0, (int)GetPrivateProfileIntW(L"DLSS5", L"Preset", 0, ini));
    c.verboseFrames = GetPrivateProfileIntW(L"Debug", L"VerboseFrames", 8, ini);
    c.ngxLogging = GetPrivateProfileIntW(L"Debug", L"NGXLogging", 0, ini) != 0;
    c.perfEnabled = GetPrivateProfileIntW(L"Perf", L"Enabled", 1, ini) != 0;
    c.perfIntervalMs = GetPrivateProfileIntW(L"Perf", L"IntervalMs", 500, ini);
    if (c.perfIntervalMs < 100) c.perfIntervalMs = 100;
    c.perfSummarySeconds = GetPrivateProfileIntW(L"Perf", L"SummarySeconds", 10, ini);
    if (c.perfSummarySeconds < 1) c.perfSummarySeconds = 1;
    g_cvarSection[0] = g_cvarSection[1] = 0;
    GetPrivateProfileSectionW(L"CVars", g_cvarSection, _countof(g_cvarSection), ini);
#ifdef ITR2_RELEASE
    // release build (Nexus, bin\release): only F5 (on/off), F6 (quality), F7 (model), F8 (dynamic
    // resolution) and F9 (foveated: off <-> PLAIN edges). Test and tuning keys are off whatever the ini says;
    // their settings keep the ini / default values (e.g. LowMemory=0 off, PlainEdgeAA=2 temporal)
    c.maskKey = 0; c.winDiagKey = 0; c.benchKey = 0; c.shotKey = 0;
    c.edgeAaKey = 0; c.jitterSignKey = 0; c.fullTaaKey = 0; c.foveaSizeKey = 0; c.outScaleKey = 0; c.lowMemKey = 0; c.msaaKey = 0;
    c.missGuardKey = 0; c.missGuard = true;             // miss guard always on, no key 3
    c.missSwapKey = 0;                                  // no key-4 auto A/B either
    c.motionHold = false;                               // motion rule: debug build only, and off there too
    c.foveaCycle[0] = kPresetPlain; c.foveaCycleN = 1;   // F9: off -> PLAIN edges -> off
    c.peripheryPreset = kPresetPlain;
    c.halfEdges = false;                                   // half-size edges only exist for DLSS (E) edges
    c.fsrExposureKey = c.fsrVersionKey = c.cleanKey = c.resetKey = c.outFormatKey = c.inCleanKey = c.autoResetKey = 0;
    c.outputProbe = false;
    c.asyncRebuildKey = 0;                              // (its ini setting, AsyncRebuild, still applies)
    c.dlss5Key = 0; c.dlss5PassesKey = 0;              // [DLSS5]: Mode and Passes from the ini, no keys
#ifdef ITR2_AMDTEST
    // AMD test build (one-eye black / white reports with FSR): the release plus test keys and the image probe. Letters
    // no other ITR2 mod here uses (F3 is UE4SS LineTraceMod's): M mask, R history reset, G game-format output, C screenshot,
    // N input cleanup, A automatic reset. They work with the game window in the background, like the F keys
    c.fsrExposureKey = VK_F1; c.fsrVersionKey = VK_F2; c.cleanKey = VK_F4; c.maskKey = 'M'; c.resetKey = 'R'; c.outFormatKey = 'G';
    c.inCleanKey = 'N'; c.autoResetKey = 'A';
    c.outputProbe = true;
    c.shotKey = 'C'; c.shotMethod = 0; c.shotEye = 2;                 // the mod's own capture, both eyes, one PNG
    c.shotPair = false; c.shotBurst = false; c.shotAlign = false;     // (no comparison sequence: it switches the upscaler)
#endif
#endif
    g_cfg = c;
    g_livePreset = c.preset;
    g_liveQuality = (int)c.quality;
    g_liveMask = c.translucencyMask;
    g_liveFovea = c.foveaEnabled;
    g_livePeriph = c.peripheryPreset;
    g_liveDrs = c.drsEnabled;
    g_liveFoveaSize = c.foveaSize;
    g_liveHalfEdges = c.halfEdges;
    g_liveWindowCrop = c.foveaWindowCrop;
    g_liveEdgeAa = c.edgeAa;
    g_liveLean = c.leanInputs;
    g_liveDrsMax = c.drsMaxPercent;
    g_liveLowMem = c.lowMemMode == 1;   // 2 (auto) is decided when the GPU is known (dlss.cpp)
    g_liveOutStage = c.drsOutputStage;
    g_liveMissGuard = c.missGuard;
    g_liveAsyncRebuild = c.asyncRebuild;
    g_liveDlss5 = c.dlss5Mode;
    g_liveDlss5Passes = c.dlss5Passes;
}

float QualityToScreenPercentage(Quality q) {
    if (g_cfg.screenPercentageOverride > 0.f) return g_cfg.screenPercentageOverride;
    switch (q) {
    case Quality::DLAA: return 100.f;
    case Quality::Quality: return 66.7f;
    case Quality::Balanced: return 58.0f;
    case Quality::Performance: return 50.0f;
    case Quality::UltraPerformance: return 33.4f;
    }
    return 66.7f;
}

// Dynamic resolution's range. DLSS takes a varying input only from 50 % of the output (FSR has no such limit), so
// with DLSS Ultra Performance and Performance both give a ceiling of 50: the render then stays at 50.
float DrsQualityCeiling() {
    const float q = std::round(QualityToScreenPercentage(LiveQuality()));
    return std::min(100.f, std::max(LivePreset() == kPresetFsr ? 25.f : 50.f, q));
}
float DrsCeiling() {
    const float cap = g_liveDrsMax.load(std::memory_order_relaxed);   // MaxPercent; the stress test changes it
    return g_cfg.drsQualityCeiling ? std::min(cap, DrsQualityCeiling()) : cap;
}
float DrsFloor(float ceiling) {
    return std::min(ceiling, LivePreset() == kPresetFsr ? g_cfg.drsMinPercent : std::max(g_cfg.drsMinPercent, 50.f));
}

int QualityToNgxPerfQuality(Quality q) {
    // NVSDK_NGX_PerfQuality_Value: MaxPerf 0, Balanced 1, MaxQuality 2, UltraPerformance 3, UltraQuality 4, DLAA 5
    switch (q) {
    case Quality::DLAA: return 5;
    case Quality::Quality: return 2;
    case Quality::Balanced: return 1;
    case Quality::Performance: return 0;
    case Quality::UltraPerformance: return 3;
    }
    return 2;
}

// ================================================================ resolver
Resolved g_res;
bool g_resolverForceScan = false;   // test hook: ignore the known RVAs and scan

struct Pattern { std::vector<int> b; };   // -1 = wildcard

static Pattern Parse(const char* s) {
    Pattern p;
    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (s[0] == '?') { p.b.push_back(-1); s += (s[1] == '?') ? 2 : 1; continue; }
        p.b.push_back((int)strtoul(std::string(s, 2).c_str(), nullptr, 16));
        s += 2;
    }
    return p;
}

static bool MatchAt(const uint8_t* m, const Pattern& p) {
    for (size_t i = 0; i < p.b.size(); ++i)
        if (p.b[i] >= 0 && m[i] != (uint8_t)p.b[i]) return false;
    return true;
}

// Scans .text. stopAfter = how many matches to collect before stopping.
static std::vector<const uint8_t*> ScanAll(const uint8_t* start, size_t size, const Pattern& p, size_t stopAfter) {
    std::vector<const uint8_t*> out;
    // anchor on the first fixed byte to keep the scan fast
    size_t first = 0; while (first < p.b.size() && p.b[first] < 0) ++first;
    if (first == p.b.size() || size < p.b.size()) return out;
    const uint8_t anchor = (uint8_t)p.b[first];
    for (const uint8_t* c = start + first; c + (p.b.size() - first) <= start + size; ) {
        c = (const uint8_t*)memchr(c, anchor, (start + size) - c);
        if (!c) break;
        const uint8_t* m = c - first;
        if (MatchAt(m, p)) { out.push_back(m); if (out.size() >= stopAfter) break; }
        ++c;
    }
    return out;
}

static uintptr_t Decode(const ue::Item& it, const uint8_t* m) {
    if (it.kind == ue::Kind::Func) return (uintptr_t)m;
    return (uintptr_t)m + it.endOff + *(const int32_t*)(m + it.dispOff);   // Data and CallTarget
}

bool ResolveAll(int requiredMode, HMODULE target) {
    HMODULE exe = target ? target : GetModuleHandleW(nullptr);
    g_res.base = (uintptr_t)exe;
    auto dos = (const IMAGE_DOS_HEADER*)exe;
    auto nt = (const IMAGE_NT_HEADERS64*)((const uint8_t*)exe + dos->e_lfanew);
    const uint32_t stamp = nt->FileHeader.TimeDateStamp;
    int known = -1;
    for (int i = 0; i < (int)(sizeof(ue::kKnownBuilds) / sizeof(ue::kKnownBuilds[0])); ++i)
        if (stamp == ue::kKnownBuilds[i].timeDateStamp && nt->OptionalHeader.SizeOfImage == ue::kKnownBuilds[i].sizeOfImage) known = i;
    g_res.knownBuild = known >= 0;
    const bool knownRvas = known == 0;   // knownRva in ue_layout.h is for the newest build; older ones are scanned
    Log("exe base=%p TimeDateStamp=0x%08X SizeOfImage=0x%X -> %s%s", exe, stamp, nt->OptionalHeader.SizeOfImage,
        g_res.knownBuild ? "known build, game " : "UNKNOWN build (offsets not verified)", g_res.knownBuild ? ue::kKnownBuilds[known].game : "");

    const uint8_t* text = nullptr; size_t textSize = 0;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (!memcmp(sec->Name, ".text", 5)) { text = (const uint8_t*)exe + sec->VirtualAddress; textSize = sec->Misc.VirtualSize; }
    if (!text) { Log("ERROR: .text not found"); return false; }

    bool ok = true;
    for (int i = 0; i < ue::I_Count; ++i) {
        const ue::Item& it = ue::kItems[i];
        Pattern p = Parse(it.aob);
        uintptr_t a = 0;
        const char* how = "";
        if (knownRvas && !g_resolverForceScan) {
            const uint8_t* at = (const uint8_t*)exe + it.knownRva;
            if (MatchAt(at, p)) { a = Decode(it, at); how = "verified at known RVA"; }
        }
        if (!a) {
            if (it.kind == ue::Kind::CallTarget) {
                auto ms = ScanAll(text, textSize, p, 64);
                uintptr_t t = ms.empty() ? 0 : Decode(it, ms[0]);
                for (auto m : ms) if (Decode(it, m) != t) { t = 0; break; }
                if (t) { a = t; how = "found by scan (call target agreed)"; }
                else how = ms.empty() ? "AOB not found" : "call sites disagree";
            } else {
                auto ms = ScanAll(text, textSize, p, 2);
                if (ms.size() == 1) { a = Decode(it, ms[0]); how = "found by scan"; }
                else how = ms.empty() ? "AOB not found" : "AOB not unique";
            }
        }
        if (a) {
            g_res.addr[i] = a;
            Log("  %-52s RVA 0x%07llX (%s)", it.name, (unsigned long long)(a - g_res.base), how);
        } else {
            Log("  %-52s MISSING (%s)%s", it.name, how, it.minMode <= requiredMode ? "  <- required" : "");
            if (it.minMode <= requiredMode) ok = false;
        }
    }
    if (!g_res.knownBuild && !g_cfg.allowUnknownBuild) {
        Log("Unknown exe build: struct offsets in ue_layout.h are not verified for it. "
            "Regenerate them from the new PDB, or set AllowUnknownBuild=1 at your own risk.");
        ok = false;
    }
    return ok;
}

// used for the release build's performance CSVs (perf.cpp)
int KeepNewestFiles(const wchar_t* dir, const wchar_t* pattern, int keep) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((std::wstring(dir) + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
    FindClose(h);
    if ((int)names.size() <= keep) return 0;
    std::sort(names.begin(), names.end());   // itr2_dlss_perf_YYYYMMDD_HHMMSS.csv: name order = age
    int n = 0;
    for (size_t i = 0; i + (size_t)keep < names.size(); ++i)
        if (DeleteFileW((std::wstring(dir) + names[i]).c_str())) ++n;
    return n;
}
