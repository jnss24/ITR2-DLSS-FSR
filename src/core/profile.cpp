// Per-setup performance profiles (logic and file format in profile_logic.h). Game thread: DrsObserveFrame calls
// ProfileTick every VR frame, with dynamic resolution on or off (else a menu change made with DRS off is seen minutes
// late). Every 2 s it rebuilds the setup's key and switches profile once a new key has held 10 s (game menu change,
// F7 / F9, another refresh rate). File: itr2_dlss_profiles.txt next to the DLL, written at most once a minute when
// something was learned, and on a switch.
// The game's graphics menu comes from its own Settings.ini (Documents\My Games\IntoTheRadius2); its [Graphics] section
// is what URadiusSettingsSubsystem::ApplyAllGraphicsSettings applies. Each quality runs the console commands of the
// game's GraphicsQuality data, so the menu values are the stable description, not the cvars.
#include "common.h"
#include "profile_logic.h"
#include <shlobj.h>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

static std::vector<prof::Profile> g_ps;
static int g_cur = -1;
static std::string g_curKey;
static bool g_loaded = false, g_dirty = false;
static double g_nextCheck = 0.0, g_nextSave = 0.0, g_readySince = -1.0;
static wchar_t g_settingsPath[MAX_PATH] = {};
static FILETIME g_settingsTime = {};
static std::string g_gameKey;   // the game's menu part of the key (re-read when Settings.ini changes)
static std::string g_pendingKey; static double g_pendingSince = 0.0;   // a new key waiting its 10 s

static void FilePath(wchar_t* out, size_t n) { wcscpy_s(out, n, g_dir); wcscat_s(out, n, L"itr2_dlss_profiles.txt"); }

static void Load() {
    wchar_t path[MAX_PATH]; FilePath(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") || !f) { Log("profile: no profile file yet (it is created when something is learned)"); return; }
    std::string text; char buf[4096]; size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < (1u << 20)) text.append(buf, r);
    fclose(f);
    int ver = 0;
    g_ps = prof::Parse(text, &ver);
    if (ver != prof::kVersion) Log("profile: itr2_dlss_profiles.txt has format version %d (this build reads %d): ignored, learning from scratch", ver, prof::kVersion);
    else Log("profile: %d profile(s) loaded", (int)g_ps.size());
}

static void Save() {
    wchar_t path[MAX_PATH], tmp[MAX_PATH]; FilePath(path, MAX_PATH);
    wcscpy_s(tmp, path); wcscat_s(tmp, L".tmp");
    const std::string text = prof::Serialize(g_ps);
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp, L"wb") || !f) { Log("profile: cannot write the profile file"); return; }
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) { Log("profile: cannot replace the profile file"); DeleteFileW(tmp); return; }
    g_dirty = false;
}

// the game's graphics menu (plus two other GPU-relevant options) from Settings.ini, re-read when the file changes
static void ReadGameSettings() {
    if (!g_settingsPath[0]) {
        PWSTR docs = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
            swprintf_s(g_settingsPath, L"%s\\My Games\\IntoTheRadius2\\Settings.ini", docs);
            CoTaskMemFree(docs);
        } else wcscpy_s(g_settingsPath, L"?");
    }
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (!GetFileAttributesExW(g_settingsPath, GetFileExInfoStandard, &a)) {
        if (g_gameKey.empty()) { g_gameKey = "game=?"; Log("profile: the game's Settings.ini was not found; its graphics menu is not part of the key"); }
        return;
    }
    if (!g_gameKey.empty() && CompareFileTime(&a.ftLastWriteTime, &g_settingsTime) == 0) return;
    g_settingsTime = a.ftLastWriteTime;
    static const struct { const wchar_t* sec; const wchar_t* key; } items[] = {
        { L"Graphics", L"OverallQuality" }, { L"Graphics", L"ViewDistance" }, { L"Graphics", L"ShadowQuality" },
        { L"Graphics", L"FoliageDensity" }, { L"Graphics", L"Effects" }, { L"Graphics", L"Textures" },
        { L"Graphics", L"Geometry" }, { L"Graphics", L"Resolution" }, { L"Graphics", L"FFRLevels" },
        { L"Graphics", L"ExperimentalOptimizations" }, { L"Game", L"LightSourcesLimit" }, { L"Spectator", L"SpectatorEnabled" },
    };
    std::string k;
    for (const auto& it : items) {
        wchar_t v[64]; GetPrivateProfileStringW(it.sec, it.key, L"-", v, 64, g_settingsPath);
        char kn[64], vn[64];
        WideCharToMultiByte(CP_UTF8, 0, it.key, -1, kn, sizeof(kn), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, v, -1, vn, sizeof(vn), nullptr, nullptr);
        for (char* c = vn; *c; ++c) if (*c == '|' || *c == '=') *c = '_';
        k += (k.empty() ? "" : "|"); k += "g."; k += kn; k += "="; k += vn;
    }
    g_gameKey = k;
}

static uint32_t ExeStamp() {
    const uint8_t* exe = (const uint8_t*)GetModuleHandleW(nullptr);
    const auto* dos = (const IMAGE_DOS_HEADER*)exe;
    return ((const IMAGE_NT_HEADERS*)(exe + dos->e_lfanew))->FileHeader.TimeDateStamp;
}

// the setup's key; ready = false while something is not known yet (eye size, refresh rate in the first seconds)
static std::string BuildKey(double now, bool* ready) {
    *ready = false;
    int rw, rh, ow, oh; PerfGetRenderSize(rw, rh, ow, oh);
    const float scale = OutputScale();
    if (ow <= 0 || oh <= 0 || scale < 10.f) { g_readySince = -1.0; return ""; }
    if (g_readySince < 0.0) g_readySince = now;
    if (now - g_readySince < 10.0) return "";   // the refresh rate is detected in the first seconds
    ReadGameSettings();
    // the eye image the game asks for, without the mod's own output changes (rounded: one pixel isn't a new setup)
    const int ew = (int)(std::lround(ow * 100.0 / scale / 16.0) * 16), eh = (int)(std::lround(oh * 100.0 / scale / 16.0) * 16);
    char b[512];
    const int periph = g_livePeriph.load(std::memory_order_relaxed);
    char fov[32] = "off";
    if (g_liveFovea.load(std::memory_order_relaxed)) snprintf(fov, sizeof(fov), "%s/%.0f", PresetName(periph), g_liveFoveaSize.load() * 100.f);
    snprintf(b, sizeof(b), "gpu=%s|drv=%s|game=%08X|eye=%dx%d|hz=%.0f|up=%s|fovea=%s|edges=%d|",
             GpuName(), GpuDriver(), ExeStamp(), ew, eh, TargetHz(), PresetName(LivePreset()), fov, g_liveEdgeAa.load());
    std::string k = b;
    for (char& c : k) if (c == '\n' || c == '\r') c = ' ';
    *ready = true;
    return k + g_gameKey;
}

static void LogProfile(const char* what, bool created, const std::string& diff) {
    const prof::Profile& p = g_ps[g_cur];
    std::string vals;
    char b[96];
    for (const auto& kv : p.vals) {
        snprintf(b, sizeof(b), "%s%s %.2f (%d)", vals.empty() ? "" : ", ", kv.first.c_str(), kv.second.v, kv.second.n);
        vals += b;
    }
    Log("profile: %s - %s profile %d of %d (used %d times)%s%s; values: %s", what,
        created ? (p.vals.empty() ? "new, empty" : "new, copied from the previous setup (uncertain: re-learned)") : "known",
        g_cur + 1, (int)g_ps.size(), p.uses, diff.empty() ? "" : "; changed: ", diff.c_str(), vals.empty() ? "none yet" : vals.c_str());
}

bool ProfileTick(double now) {
    if (!g_cfg.drsProfiles) return false;
    if (g_dirty && g_loaded && now >= g_nextSave) { Save(); g_nextSave = now + 60.0; }
    if (now < g_nextCheck) return false;
    g_nextCheck = now + 2.0;
    bool ready = false;
    const std::string key = BuildKey(now, &ready);
    if (!ready || key == g_curKey) { g_pendingKey.clear(); return false; }
    // a new setup must hold 10 s before the switch (cycling F7 through 5 models in 6 s would make 4 profiles that never
    // measure anything); the session's first one is taken at once
    if (!g_curKey.empty()) {
        if (key != g_pendingKey) { g_pendingKey = key; g_pendingSince = now; return false; }
        if (now - g_pendingSince < 10.0) return false;
    }
    g_pendingKey.clear();
    if (!g_loaded) { Load(); g_loaded = true; }
    bool created = false;
    const std::string diff = g_curKey.empty() ? "" : prof::KeyDiff(g_curKey, key);
    const int from = g_cur;
    g_cur = prof::Select(g_ps, key, from, (long long)_time64(nullptr), &created);
    if (g_curKey.empty()) Log("profile: setup key %s", key.c_str());
    LogProfile(g_curKey.empty() ? "this setup" : "the setup changed", created, diff);
    g_curKey = key;
    g_dirty = true;
    Save(); g_nextSave = now + 60.0;
    return true;
}

bool ProfileActive() { return g_cfg.drsProfiles && g_cur >= 0 && g_cur < (int)g_ps.size(); }

float ProfileGet(const char* name, int* samples) {
    if (!ProfileActive()) { if (samples) *samples = 0; return 0.f; }
    return prof::Get(g_ps[g_cur], name, samples);
}

float ProfileLearn(const char* name, float v) {
    if (!ProfileActive() || !(v > 0.f)) return v;
    if (StressActive()) return v;   // the stress test's forced sizes and loads teach the profiles nothing
    g_dirty = true;
    return prof::Learn(g_ps[g_cur], name, v);
}

float ProfileSet(const char* name, float v) {   // 0 allowed (clears a retry line)
    if (!ProfileActive() || !(v >= 0.f)) return v;
    if (StressActive()) return v;
    g_dirty = true;
    return prof::Set(g_ps[g_cur], name, v);
}
