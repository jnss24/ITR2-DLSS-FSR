// shot.cpp - screenshot key ([Screenshot] Key, F11 by default). Method=Mod (default): capture.cpp copies the finished
// eye image at full resolution. Method=Mirror: runs Meta's OculusMirror.exe with --Screenshot, which saves a PNG of
// the mirror window (limited to the monitor's size, e.g. ~1707x960 logical against a ~4080x4464 eye image; only
// works with the Oculus / Meta runtime).
// The file name says what was on screen, e.g. itr2_20261018_101530_DLSS-M-Quality.png or ..._TAA.png, so a
// TAA / DLSS pair taken with F5 in between is easy to tell apart. The mirror runs on its own thread: the game
// thread only starts it. With Pair=1 (default, Mod method, Mode=3) one press takes a comparison: the current state,
// then the upscaler toggled, then back, plus <base>_compare.html with a before/after slider (see PairStart).
#include "common.h"
#include <cstdio>
#include <vector>
#include <wincodec.h>

static std::atomic<bool> g_shotBusy{ false };

struct ShotJob { wchar_t cmd[2048]; wchar_t file[MAX_PATH]; };

static bool FileExists(const wchar_t* p) {
    const DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static ULONGLONG FileSize(const wchar_t* p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d)) return 0;
    return ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

// creates every missing folder of a path that ends with a backslash
static void MakeDirs(const wchar_t* dir) {
    wchar_t p[MAX_PATH]; wcsncpy_s(p, dir, _TRUNCATE);
    for (wchar_t* c = p + 3; *c; ++c)
        if (*c == L'\\' || *c == L'/') { const wchar_t s = *c; *c = 0; CreateDirectoryW(p, nullptr); *c = s; }
}

// OculusMirror.exe: the ini path, else the Oculus install folder from the registry, else the default folder
static bool FindMirror(wchar_t* out, size_t n) {
    if (g_cfg.shotExe[0]) { wcsncpy_s(out, n, g_cfg.shotExe, _TRUNCATE); return FileExists(out); }
    wchar_t base[MAX_PATH] = L"C:\\Program Files\\Oculus\\";
    DWORD sz = sizeof(base);
    wchar_t reg[MAX_PATH] = {};
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Oculus VR, LLC\\Oculus", L"Base",
                     RRF_RT_REG_SZ, nullptr, reg, &sz) == ERROR_SUCCESS && reg[0])
        wcsncpy_s(base, reg, _TRUNCATE);
    const size_t len = wcslen(base);
    swprintf_s(out, n, L"%s%sSupport\\oculus-diagnostics\\OculusMirror.exe", base,
               len && base[len - 1] != L'\\' ? L"\\" : L"");
    return FileExists(out);
}

// what is on screen, for the file name: TAA, MSAA2x, ModTAA, DLSS-M-Quality, FSR-Balanced (+ -foveated)
static void StateTag(char* buf, size_t n) {
    const bool upscalerOn = (g_cfg.mode == 3 && g_dlssEnabled) || g_cfg.mode == 2;
    if (g_liveFullTaa.load()) { snprintf(buf, n, "ModTAA"); return; }
    if (!upscalerOn) {
        const int aa = TweaksAaMode();
        snprintf(buf, n, "%s", aa == 1 ? "MSAA2x" : aa == 2 ? "MSAA8x" : "TAA");
        return;
    }
    const int preset = LivePreset();
    if (preset == kPresetFsr) snprintf(buf, n, "FSR-%s", QualityName(LiveQuality()));
    else snprintf(buf, n, "DLSS-%s-%s", PresetName(preset), QualityName(LiveQuality()));
    if (g_liveFovea.load()) strncat_s(buf, n, "-foveated", _TRUNCATE);
    for (char* c = buf; *c; ++c) if (*c == ' ') *c = '_';   // "Ultra Performance" -> "Ultra_Performance"
}

// the mod's settings, for the capture's folder name (also when the upscaler is off at that moment, e.g. a comparison
// started from the game's TAA): DLSS-K-DLAA / FSR-Quality / ModTAA, then the foveation (fov-<edges model>-w<window %>,
// -half for half-size edges, -aa1/-aa2 for the plain-edge AA; nofov), then the output scale when not 100 % (out110)
// and _drs when dynamic resolution is on
static void ModSettingsTag(char* buf, size_t n) {
    char up[48];
    if (g_liveFullTaa.load()) snprintf(up, sizeof(up), "ModTAA");
    else if (LivePreset() == kPresetFsr) snprintf(up, sizeof(up), "FSR-%s", QualityName(LiveQuality()));
    else snprintf(up, sizeof(up), "DLSS-%s-%s", PresetName(LivePreset()), QualityName(LiveQuality()));
    char fov[48];
    if (g_liveFovea.load()) {
        const int periph = g_livePeriph.load();
        int len = snprintf(fov, sizeof(fov), "fov-%s-w%.0f", PresetName(periph), g_liveFoveaSize.load() * 100.f);
        if (g_liveHalfEdges.load() && len > 0 && len < (int)sizeof(fov)) len += snprintf(fov + len, sizeof(fov) - len, "-half");
        if (periph == kPresetPlain && g_liveEdgeAa.load() && len > 0 && len < (int)sizeof(fov))
            snprintf(fov + len, sizeof(fov) - len, "-aa%d", g_liveEdgeAa.load());
    } else snprintf(fov, sizeof(fov), "nofov");
    char out[16] = "";
    const float os = OutputScale();
    if (os > 100.5f || os < 99.5f) snprintf(out, sizeof(out), "_out%.0f", os);
    snprintf(buf, n, "%s_%s%s%s", up, fov, out, g_liveDrs.load() ? "_drs" : "");
    for (char* c = buf; *c; ++c) if (*c == ' ' || *c == '?' || *c == '\\' || *c == '/' || *c == ':') *c = '_';
}

static DWORD WINAPI ShotThread(void* p) {
    ShotJob* job = (ShotJob*)p;
    char fileA[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, job->file, -1, fileA, sizeof(fileA), nullptr, nullptr);
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;   // a window must exist for the mirror to render; don't take the focus
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, job->cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Log("screenshot: could not start the mirror (error %lu)", GetLastError());
        Announce(Tone::Change, "Screenshot failed.");
        delete job; g_shotBusy = false; return 0;
    }
    // wait up to 15 s for the PNG; it is done when its size has stayed the same for 0.5 s
    const ULONGLONG t0 = GetTickCount64();
    ULONGLONG lastSize = 0, stableSince = 0;
    bool saved = false, exited = false;
    while (GetTickCount64() - t0 < 15000) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) exited = true;
        const ULONGLONG s = FileSize(job->file);
        if (s > 0 && s == lastSize) {
            if (!stableSince) stableSince = GetTickCount64();
            else if (GetTickCount64() - stableSince >= 500) { saved = true; break; }
        } else { stableSince = 0; lastSize = s; }
        if (exited && s == 0) break;   // the mirror closed without writing anything
    }
    DWORD code = 0;
    const bool running = GetExitCodeProcess(pi.hProcess, &code) && code == STILL_ACTIVE;
    if (running) TerminateProcess(pi.hProcess, 0);   // the mirror window stays open after saving: close it
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (saved) {
        Log("screenshot: saved %s (%llu KB, %.1f s%s)", fileA, FileSize(job->file) / 1024,
            (GetTickCount64() - t0) / 1000.0, running ? ", mirror closed by the mod" : "");
        Announce(Tone::Change, "Screenshot saved.");
    } else {
        Log("screenshot: FAILED, no file after %.1f s (%s; exit code %lu). Is the game running on the Oculus runtime?",
            (GetTickCount64() - t0) / 1000.0, running ? "mirror still running, closed" : "mirror exited", code);
        Announce(Tone::Change, "Screenshot failed.");
    }
    delete job;
    g_shotBusy = false;
    return 0;
}

// <folder>\itr2_<date>_<time>_<mod settings>\itr2_<date>_<time> (no extension): every capture (a comparison's PNGs,
// viewer, maps and videos, or a single screenshot) in a folder of its own, named with the mod's settings
// (ModSettingsTag); the files inside keep the short name; creates the folders
static void ShotBase(wchar_t (&base)[MAX_PATH], wchar_t (&dir)[MAX_PATH]) {
    wchar_t root[MAX_PATH];
    if (g_cfg.shotFolder[0]) {
        const size_t l = wcslen(g_cfg.shotFolder);
        swprintf_s(root, L"%s%s", g_cfg.shotFolder, l && g_cfg.shotFolder[l - 1] != L'\\' ? L"\\" : L"");
    } else swprintf_s(root, L"%sitr2_dlss_shots\\", g_dir);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"itr2_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    char tag[160]; ModSettingsTag(tag, sizeof(tag));
    wchar_t tagW[160]; MultiByteToWideChar(CP_UTF8, 0, tag, -1, tagW, 160);
    swprintf_s(dir, L"%s%s_%s\\", root, name, tagW);
    MakeDirs(dir);
    swprintf_s(base, L"%s%s", dir, name);
}

// benchmark (bench.cpp): <folder>\itr2_<date>_<time>_<kind>_<foveation, output...>\itr2_<date>_<time> (the model and
// quality part of the settings tag is left out: the benchmark goes through all of them)
void ShotNewFolder(const char* kind, wchar_t (&base)[MAX_PATH]) {
    wchar_t root[MAX_PATH];
    if (g_cfg.shotFolder[0]) {
        const size_t l = wcslen(g_cfg.shotFolder);
        swprintf_s(root, L"%s%s", g_cfg.shotFolder, l && g_cfg.shotFolder[l - 1] != L'\\' ? L"\\" : L"");
    } else swprintf_s(root, L"%sitr2_dlss_shots\\", g_dir);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"itr2_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    char tag[160]; ModSettingsTag(tag, sizeof(tag));
    const char* rest = strchr(tag, '_');
    char full[200]; snprintf(full, sizeof(full), "%s%s", kind, rest ? rest : "");
    wchar_t tagW[200]; MultiByteToWideChar(CP_UTF8, 0, full, -1, tagW, 200);
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%s%s_%s\\", root, name, tagW);
    MakeDirs(dir);
    swprintf_s(base, L"%s%s", dir, name);
}

// <base>_<state>.png
static bool ShotFileName(wchar_t (&file)[MAX_PATH]) {
    wchar_t base[MAX_PATH], dir[MAX_PATH]; ShotBase(base, dir);
    char tag[64]; StateTag(tag, sizeof(tag));
    wchar_t tagW[64]; MultiByteToWideChar(CP_UTF8, 0, tag, -1, tagW, 64);
    return swprintf_s(file, L"%s_%s.png", base, tagW) > 0;
}

// ------------------------------------------------------------------ comparison (key with Pair=1)
// Shot A in the current, settled state; once its frame is copied, the upscaler is toggled (like F5); after
// PairSettleMs shot B; once B is copied the upscaler is put back. Then <base>_compare.html shows the pair with a
// slider (the game's TAA on the left, the mod on the right). Game thread, driven every frame by ShotPoll.
// With Motion=1 each shot is preceded by a burst of consecutive frames (capture.cpp) for the shimmer outputs
// (motion.cpp): burst A, shot A, toggle, settle, burst B, shot B, put back. The burst comes first because the
// full-size copy of a shot causes a hitch (236 ms measured) that disturbed the frames after it (a near-black frame,
// no motion vectors for ~4 frames). Dynamic resolution is held for the whole comparison (DrsHold).
enum PairState { PS_Idle, PS_Delay, PS_CopyA, PS_BurstA, PS_Settle, PS_IdleB, PS_CopyB, PS_BurstB, PS_SavedB, PS_Viewer };
static struct {
    PairState st = PS_Idle;
    ULONGLONG t = 0;
    uint32_t copies = 0;
    bool startOn = false;
    bool burstOn = false;                  // bursts for this comparison (Motion=1 and both bursts so far fine)
    BurstFrames burstA, burstB;
    wchar_t base[MAX_PATH], fileA[MAX_PATH], fileB[MAX_PATH];
    char tagA[64], tagB[64];
    int alignDx = 0, alignDy = 0;          // B[y + dy][x + dx] matches A[y][x] (head movement between the shots)
    std::atomic<int> viewerDone{ 0 };      // worker: 1 ok, 2 failed
} g_pair;

// ------------------------------------------------------------------ alignment (worker thread)
// The head moves a little between the two shots (measured 7-18 px at 4080x4464). One shift for
// the middle of the view (x 25-75 %, y 20-65 %: away from the hands at the bottom and the near canopy at the top)
// is found coarse to fine on greyscale copies: 1/8 size +-12 px, 1/2 size +-3, full size +-2 (sum of absolute
// differences). Near objects move more than far ones (parallax), so one shift cannot line up everything.
static bool LoadGray(const wchar_t* file, std::vector<uint8_t>& g, int& w, int& h) {
    IWICImagingFactory* fac = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* conv = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    if (SUCCEEDED(hr)) hr = fac->CreateDecoderFromFilename(file, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &fr);
    UINT uw = 0, uh = 0;
    if (SUCCEEDED(hr)) hr = fr->GetSize(&uw, &uh);
    if (SUCCEEDED(hr)) hr = fac->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(fr, GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        try { g.resize((size_t)uw * uh); } catch (...) { hr = E_OUTOFMEMORY; }
    }
    if (SUCCEEDED(hr)) hr = conv->CopyPixels(nullptr, uw, (UINT)g.size(), g.data());
    if (conv) conv->Release(); if (fr) fr->Release(); if (dec) dec->Release(); if (fac) fac->Release();
    w = (int)uw; h = (int)uh;
    return SUCCEEDED(hr) && w > 64 && h > 64;
}

struct GrayImg { std::vector<uint8_t> p; int w = 0, h = 0; };

static GrayImg Half(const GrayImg& s) {
    GrayImg d; d.w = s.w / 2; d.h = s.h / 2; d.p.resize((size_t)d.w * d.h);
    for (int y = 0; y < d.h; ++y) {
        const uint8_t* r0 = &s.p[(size_t)(2 * y) * s.w]; const uint8_t* r1 = r0 + s.w;
        uint8_t* o = &d.p[(size_t)y * d.w];
        for (int x = 0; x < d.w; ++x) o[x] = (uint8_t)((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) >> 2);
    }
    return d;
}

// mean absolute difference over the middle region for one offset (every 2nd pixel and row); -1 if out of range
static double Sad(const GrayImg& a, const GrayImg& b, int dx, int dy) {
    const int x0 = a.w / 4, x1 = a.w * 3 / 4, y0 = a.h / 5, y1 = a.h * 13 / 20;
    if (x0 + dx < 0 || y0 + dy < 0 || x1 + dx > b.w || y1 + dy > b.h) return -1.0;
    uint64_t sum = 0, n = 0;
    for (int y = y0; y < y1; y += 2) {
        const uint8_t* ra = &a.p[(size_t)y * a.w]; const uint8_t* rb = &b.p[(size_t)(y + dy) * b.w + dx];
        for (int x = x0; x < x1; x += 2) { sum += (uint32_t)abs(ra[x] - rb[x]); ++n; }
    }
    return n ? (double)sum / n : -1.0;
}

static bool BestAround(const GrayImg& a, const GrayImg& b, int cx, int cy, int r, int& bx, int& by, double& be) {
    bool found = false;
    for (int dy = cy - r; dy <= cy + r; ++dy)
        for (int dx = cx - r; dx <= cx + r; ++dx) {
            const double e = Sad(a, b, dx, dy);
            if (e >= 0 && (!found || e < be)) { be = e; bx = dx; by = dy; found = true; }
        }
    return found;
}

// finds (dx, dy) with B[y + dy][x + dx] ~ A[y][x]; err = mean absolute difference (0-255) at that offset
bool AlignPair(const wchar_t* fileA, const wchar_t* fileB, int& dx, int& dy, double& err, double& err0) {
    GrayImg a0, b0;
    if (!LoadGray(fileA, a0.p, a0.w, a0.h) || !LoadGray(fileB, b0.p, b0.w, b0.h)) return false;
    if (a0.w != b0.w || a0.h != b0.h) return false;
    GrayImg a1 = Half(a0), b1 = Half(b0), a2 = Half(a1), b2 = Half(b1), a3 = Half(a2), b3 = Half(b2);
    int x = 0, y = 0; double e = 0;
    err0 = Sad(a0, b0, 0, 0);
    if (!BestAround(a3, b3, 0, 0, 12, x, y, e)) return false;            // 1/8 size: +-96 px at full size
    if (!BestAround(a1, b1, x * 4, y * 4, 3, x, y, e)) return false;     // 1/2 size
    if (!BestAround(a0, b0, x * 2, y * 2, 2, x, y, e)) return false;     // full size
    dx = x; dy = y; err = e;
    return true;
}

static bool WriteViewer();
static DWORD WINAPI ViewerThread(void*) {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int dx = 0, dy = 0; double e = 0, e0 = 0;
    const ULONGLONG t0 = GetTickCount64();
    if (g_cfg.shotAlign && AlignPair(g_pair.fileA, g_pair.fileB, dx, dy, e, e0)) {
        g_pair.alignDx = dx; g_pair.alignDy = dy;
        Log("comparison: alignment B = A shifted by (%d, %d) px, middle-region difference %.2f -> %.2f (of 255), %.1f s%s",
            dx, dy, e0, e, (GetTickCount64() - t0) / 1000.0,
            (abs(dx) >= 94 || abs(dy) >= 94) ? " (at the search limit: the view moved a lot)" : "");
    } else {
        g_pair.alignDx = g_pair.alignDy = 0;
        Log("comparison: %s", g_cfg.shotAlign ? "alignment failed, viewer without it" : "alignment off ([Screenshot] Align=0)");
    }
    const bool viewerOk = WriteViewer();
    if (g_pair.burstA.data && g_pair.burstB.data) {   // the shimmer outputs: A is the mod when the upscaler was on at the start
        const BurstFrames& taa = g_pair.startOn ? g_pair.burstB : g_pair.burstA;
        const BurstFrames& mod = g_pair.startOn ? g_pair.burstA : g_pair.burstB;
        const char* taaTag = g_pair.startOn ? g_pair.tagB : g_pair.tagA;
        const char* modTag = g_pair.startOn ? g_pair.tagA : g_pair.tagB;
        const ULONGLONG tm = GetTickCount64();
        const MotionResult m = WriteMotionOutputs(g_pair.base, taa, mod, taaTag, modTag);
        Log("comparison: motion outputs in %.1f s (shimmer TAA %.2f, mod %.2f)", (GetTickCount64() - tm) / 1000.0,
            m.flickerTaa, m.flickerMod);
    }
    BurstFree(g_pair.burstA); BurstFree(g_pair.burstB);
    if (SUCCEEDED(ci)) CoUninitialize();
    g_pair.viewerDone = viewerOk ? 1 : 2;
    return 0;
}

static const wchar_t* FileOnly(const wchar_t* p) { const wchar_t* s = wcsrchr(p, L'\\'); return s ? s + 1 : p; }

static void PairRestore() {
    DrsHold(false);
    if (g_dlssEnabled.load() != g_pair.startOn) {
        g_dlssEnabled = g_pair.startOn;
        Log("comparison: upscaler back %s", g_pair.startOn ? "ON" : "OFF");
    }
}

static void PairFail(const char* why) {
    PairRestore();
    Log("comparison: FAILED (%s)", why);
    Announce(Tone::Change, "Comparison failed.");
    g_pair.st = PS_Idle;
}

static bool WriteViewer() {
    // left = the game's TAA (the shot taken with the upscaler off), right = the mod
    const bool aIsOff = !g_pair.startOn;
    const wchar_t* left = aIsOff ? g_pair.fileA : g_pair.fileB;
    const wchar_t* right = aIsOff ? g_pair.fileB : g_pair.fileA;
    const char* leftTag = aIsOff ? g_pair.tagA : g_pair.tagB;
    const char* rightTag = aIsOff ? g_pair.tagB : g_pair.tagA;
    // where to place the right image so it lines up with the left one: B = A shifted by (dx, dy), so B goes at
    // (-dx, -dy) when it is on the right, and A goes at (+dx, +dy) when B is the left one
    const int ox = aIsOff ? -g_pair.alignDx : g_pair.alignDx, oy = aIsOff ? -g_pair.alignDy : g_pair.alignDy;
    char l8[MAX_PATH * 3], r8[MAX_PATH * 3], title[MAX_PATH * 3], html[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, FileOnly(left), -1, l8, sizeof(l8), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, FileOnly(right), -1, r8, sizeof(r8), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, FileOnly(g_pair.base), -1, title, sizeof(title), nullptr, nullptr);
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%s_compare.html", g_pair.base);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, html, sizeof(html), nullptr, nullptr);
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    fprintf(f,
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>%s</title><style>\n"
        "html,body{margin:0;height:100%%;background:#111;color:#ddd;font:14px system-ui,sans-serif;overflow:hidden}\n"
        "#v{position:absolute;inset:0;overflow:hidden;cursor:crosshair}\n"
        "#s{position:absolute;left:0;top:0;transform-origin:0 0}\n"
        "#s img{position:absolute;left:0;top:0;display:block;user-select:none;-webkit-user-drag:none}\n"
        "#line{position:absolute;top:0;bottom:0;width:2px;margin-left:-1px;background:#f5b335;pointer-events:none}\n"
        ".lab{position:absolute;top:10px;padding:5px 12px;background:rgba(0,0,0,.65);border-radius:4px;font-weight:600}\n"
        "#la{left:10px}#lb{right:10px}\n"
        "#help{position:absolute;bottom:10px;left:50%%;transform:translateX(-50%%);background:rgba(0,0,0,.65);padding:5px 12px;border-radius:4px;white-space:nowrap}\n"
        "</style></head><body><div id=\"v\"><div id=\"s\"><img id=\"a\" src=\"%s\"><img id=\"b\" src=\"%s\"></div>\n"
        "<div id=\"line\"></div><div class=\"lab\" id=\"la\">Game: %s</div><div class=\"lab\" id=\"lb\">Mod: %s</div>\n"
        "<div id=\"help\">Move the mouse: slider &middot; Wheel: zoom &middot; Right-drag: pan &middot; 1: pixels 1:1 &middot; Double-click: fit &middot; A: alignment <span id=\"al\"></span></div></div>\n"
        "<script>\n"
        "var OX=%d,OY=%d;\n"
        "var v=document.getElementById('v'),s=document.getElementById('s'),b=document.getElementById('b'),ln=document.getElementById('line'),ia=document.getElementById('a'),al=document.getElementById('al');\n"
        "var z=1,x=0,y=0,split=.5,drag=null,aligned=true;\n"
        "function apply(){s.style.transform='translate('+x+'px,'+y+'px) scale('+z+')';s.style.imageRendering=z>=2?'pixelated':'auto';\n"
        " var ox=aligned?OX:0,oy=aligned?OY:0;b.style.left=ox+'px';b.style.top=oy+'px';\n"
        " al.textContent=aligned?('on (right image moved '+OX+', '+OY+' px for head movement)'):'off';\n"
        " var px=(split*v.clientWidth-x)/z-ox;b.style.clipPath='inset(0 0 0 '+Math.max(0,Math.min(ia.naturalWidth,px))+'px)';ln.style.left=(split*100)+'%%';}\n"
        "function fit(){var W=ia.naturalWidth,H=ia.naturalHeight;if(!W)return;z=Math.min(v.clientWidth/W,v.clientHeight/H);x=(v.clientWidth-W*z)/2;y=(v.clientHeight-H*z)/2;apply();}\n"
        "ia.onload=fit;if(ia.complete)fit();window.onresize=fit;\n"
        "v.oncontextmenu=function(e){e.preventDefault();};\n"
        "v.onmousedown=function(e){if(e.button==2){drag={x:x,y:y,mx:e.clientX,my:e.clientY};v.style.cursor='grabbing';}};\n"
        "window.onmouseup=function(){drag=null;v.style.cursor='crosshair';};\n"
        "v.onmousemove=function(e){if(drag){x=drag.x+e.clientX-drag.mx;y=drag.y+e.clientY-drag.my;}else split=e.clientX/v.clientWidth;apply();};\n"
        "v.onwheel=function(e){e.preventDefault();var f=e.deltaY<0?1.25:0.8,mx=e.clientX,my=e.clientY;x=mx-(mx-x)*f;y=my-(my-y)*f;z*=f;apply();};\n"
        "v.ondblclick=fit;\n"
        "window.onkeydown=function(e){if(e.key=='1'){var mx=v.clientWidth*split,my=v.clientHeight/2,f=1/z;x=mx-(mx-x)*f;y=my-(my-y)*f;z=1;apply();}\n"
        " else if(e.key=='a'||e.key=='A'){aligned=!aligned;apply();}};\n"
        "</script></body></html>\n",
        title, l8, r8, leftTag, rightTag, ox, oy);
    fclose(f);
    Log("comparison: viewer %s (left %s, right %s, right image placed at %d, %d)", html, l8, r8, ox, oy);
    return true;
}

static void BurstGiveUp(const char* why);
static void PairStart() {
    wchar_t dir[MAX_PATH]; ShotBase(g_pair.base, dir);
    g_pair.startOn = g_dlssEnabled.load();
    StateTag(g_pair.tagA, sizeof(g_pair.tagA));
    wchar_t tagW[64]; MultiByteToWideChar(CP_UTF8, 0, g_pair.tagA, -1, tagW, 64);
    swprintf_s(g_pair.fileA, L"%s_%s.png", g_pair.base, tagW);
    if (CaptureBusy()) { Announce(Tone::Change, "Screenshot busy."); return; }
    BurstFree(g_pair.burstA); BurstFree(g_pair.burstB);
    g_pair.burstOn = g_cfg.shotBurst;
    BurstReset();
    if (BurstState() == 3) { BurstFrames old; BurstTake(old); BurstFree(old); }   // left over from a failed comparison
    DrsHold(true);
    g_pair.t = GetTickCount64();
    g_pair.st = PS_Delay;
    Log("comparison: key pressed (upscaler %s, dynamic resolution held), starting in %d ms", g_pair.startOn ? "ON" : "OFF",
        g_cfg.shotStartDelayMs);
    Announce(Tone::Change, "Comparison. Hold still.");
}

// after StartDelayMs: burst A (then shot A), or shot A. The delay lets the headset settle: pressing the key on a desk
// the headset rests on turns the camera by up to 0.8 px per frame for about half a second (the burst's image steps
// followed the logged camera turn with correlation -0.99 / -0.93)
static void PairBegin() {
    char a[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, g_pair.fileA, -1, a, sizeof(a), nullptr, nullptr);
    g_pair.t = GetTickCount64();
    if (g_pair.burstOn && BurstRequest(g_cfg.shotBurstFrames, g_cfg.shotBurstW, g_cfg.shotBurstH)) {
        Log("comparison: started, burst A, then shot A %s", a);
        g_pair.st = PS_BurstA;
    } else {
        if (g_pair.burstOn) BurstGiveUp("burst A could not start");
        g_pair.copies = CaptureCopyCount();
        if (!CaptureRequest(g_pair.fileA, g_cfg.shotEye, true)) { PairFail("capture busy for shot A"); return; }
        Log("comparison: started, shot A %s", a);
        g_pair.st = PS_CopyA;
    }
}

static void PairToggle() {
    g_dlssEnabled = !g_pair.startOn;
    Log("comparison: upscaler %s for shot B", g_dlssEnabled ? "ON" : "OFF");
}

static void BurstGiveUp(const char* why) {
    Log("comparison: no motion outputs (%s)", why);
    g_pair.burstOn = false;
    BurstReset();
}

static void PairStep() {
    if (g_pair.st == PS_Idle) return;
    const ULONGLONG now = GetTickCount64();
    switch (g_pair.st) {
    case PS_Delay:
        if (now - g_pair.t >= (ULONGLONG)g_cfg.shotStartDelayMs) PairBegin();
        break;
    case PS_BurstA:   // all of burst A copied: shot A
        if (BurstState() == 4) BurstGiveUp("burst A failed");
        else if (BurstCopied() < g_cfg.shotBurstFrames && now - g_pair.t < 5000) break;
        else if (BurstCopied() < g_cfg.shotBurstFrames) BurstGiveUp("burst A too slow");
        g_pair.copies = CaptureCopyCount();
        if (!CaptureRequest(g_pair.fileA, g_cfg.shotEye, true)) { PairFail("capture busy for shot A"); break; }
        g_pair.st = PS_CopyA; g_pair.t = now;
        break;
    case PS_CopyA:   // shot A's frame copied: toggle the upscaler
        if (CaptureCopyCount() != g_pair.copies) {
            PairToggle();
            g_pair.st = PS_Settle; g_pair.t = now;
        } else if (!CaptureBusy() || now - g_pair.t > 5000) PairFail("shot A was not copied");
        break;
    case PS_Settle:
        if (now - g_pair.t >= (ULONGLONG)g_cfg.shotPairSettleMs) { g_pair.st = PS_IdleB; g_pair.t = now; }
        break;
    case PS_IdleB:   // settled, shot A's PNG written (and burst A converted): burst B, or shot B
        if (!CaptureBusy()) {
            if (GetFileAttributesW(g_pair.fileA) == INVALID_FILE_ATTRIBUTES) { PairFail("shot A was not saved"); break; }
            StateTag(g_pair.tagB, sizeof(g_pair.tagB));
            wchar_t tagW[64]; MultiByteToWideChar(CP_UTF8, 0, g_pair.tagB, -1, tagW, 64);
            swprintf_s(g_pair.fileB, L"%s_%s.png", g_pair.base, tagW);
            if (g_pair.burstOn) {
                const int bs = BurstState();
                if (bs == 2 && now - g_pair.t < 5000) break;          // burst A still converting
                if (bs == 3 && BurstTake(g_pair.burstA) &&
                    BurstRequest(g_cfg.shotBurstFrames, g_cfg.shotBurstW, g_cfg.shotBurstH)) {
                    g_pair.st = PS_BurstB; g_pair.t = now; break;
                }
                BurstGiveUp("burst A not ready for burst B");
            }
            g_pair.copies = CaptureCopyCount();
            if (!CaptureRequest(g_pair.fileB, g_cfg.shotEye, true)) { PairFail("capture busy for shot B"); break; }
            g_pair.st = PS_CopyB; g_pair.t = now;
        } else if (now - g_pair.t > 10000) PairFail("shot A is taking too long");
        break;
    case PS_BurstB:   // all of burst B copied: shot B
        if (BurstState() == 4) BurstGiveUp("burst B failed");
        else if (BurstCopied() < g_cfg.shotBurstFrames && now - g_pair.t < 5000) break;
        else if (BurstCopied() < g_cfg.shotBurstFrames) BurstGiveUp("burst B too slow");
        g_pair.copies = CaptureCopyCount();
        if (!CaptureRequest(g_pair.fileB, g_cfg.shotEye, true)) { PairFail("capture busy for shot B"); break; }
        g_pair.st = PS_CopyB; g_pair.t = now;
        break;
    case PS_CopyB:   // shot B's frame copied: put the upscaler back
        if (CaptureCopyCount() != g_pair.copies) {
            PairRestore();
            g_pair.st = PS_SavedB; g_pair.t = now;
        } else if (!CaptureBusy() || now - g_pair.t > 5000) PairFail("shot B was not copied");
        break;
    case PS_SavedB:   // both PNGs written (and burst B converted): align, write the viewer and the motion outputs
        if (g_pair.burstOn && !g_pair.burstB.data) {
            const int bs = BurstState();
            if (bs == 2 && now - g_pair.t < 10000) break;
            if (bs != 3 || !BurstTake(g_pair.burstB)) BurstGiveUp("burst B not ready");
        }
        if (!CaptureBusy()) {
            if (GetFileAttributesW(g_pair.fileB) == INVALID_FILE_ATTRIBUTES) { PairFail("shot B was not saved"); break; }
            g_pair.viewerDone = 0;
            HANDLE h = CreateThread(nullptr, 0, ViewerThread, nullptr, 0, nullptr);
            if (!h) { PairFail("could not start the viewer thread"); break; }
            CloseHandle(h);
            g_pair.st = PS_Viewer; g_pair.t = now;
        } else if (now - g_pair.t > 10000) PairFail("shot B is taking too long");
        break;
    case PS_Viewer:
        if (const int d = g_pair.viewerDone.load()) {
            if (d == 1) Announce(Tone::Change, "Comparison saved.");
            else { Log("comparison: could not write the viewer"); Announce(Tone::Change, "Comparison failed."); }
            g_pair.st = PS_Idle;
        } else if (now - g_pair.t > 180000) { Log("comparison: viewer thread did not finish"); Announce(Tone::Change, "Comparison failed."); g_pair.st = PS_Idle; }
        break;
    default: g_pair.st = PS_Idle; break;
    }
}

bool ShotComparisonBusy() { return g_pair.st != PS_Idle; }

void ShotPoll() {
    static bool wasDown = false, logged = false;
    if (!logged) {
        logged = true;
        Log("screenshot key: virtual key 0x%02X (%s%s)", g_cfg.shotKey, g_cfg.shotMethod == 0 ? "mod capture" : "OculusMirror",
            g_cfg.shotMethod == 0 && g_cfg.shotPair && g_cfg.mode == 3 ? ", comparison pair" : "");
    }
    PairStep();
    if (!g_cfg.shotKey) return;
    if (!HotkeyPressed(g_cfg.shotKey, wasDown)) return;   // common.h: no Ctrl/Alt/Shift/Win
    if (g_pair.st != PS_Idle) { Announce(Tone::Change, "Comparison busy."); return; }
    if (BenchActive()) { Announce(Tone::Change, "Benchmark running."); return; }
    if (StressActive()) { Announce(Tone::Change, "Stress test running."); return; }
    if (g_cfg.shotMethod == 0 && g_cfg.shotPair && g_cfg.mode == 3) { PairStart(); return; }
    if (g_cfg.shotMethod == 0) {   // the mod copies the finished eye image itself (capture.cpp)
        wchar_t file[MAX_PATH];
        if (!ShotFileName(file)) return;
        if (!CaptureRequest(file, g_cfg.shotEye)) { Announce(Tone::Change, "Screenshot busy."); return; }
        char a[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, file, -1, a, sizeof(a), nullptr, nullptr);
        Log("screenshot: requested %s", a);
        Announce(Tone::Change, "Screenshot.");
        return;
    }
    if (g_shotBusy.exchange(true)) { Announce(Tone::Change, "Screenshot busy."); return; }

    wchar_t exe[MAX_PATH];
    if (!FindMirror(exe, MAX_PATH)) {
        char a[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, exe, -1, a, sizeof(a), nullptr, nullptr);
        Log("screenshot: OculusMirror.exe not found (%s); set [Screenshot] MirrorExe in the ini", a);
        Announce(Tone::Change, "Mirror tool not found.");
        g_shotBusy = false;
        return;
    }
    ShotJob* job = new ShotJob{};
    if (!ShotFileName(job->file)) { delete job; g_shotBusy = false; return; }
    swprintf_s(job->cmd, L"\"%s\" %s --Screenshot \"%s\"", exe, g_cfg.shotArgs, job->file);
    char cmdA[4096]; WideCharToMultiByte(CP_UTF8, 0, job->cmd, -1, cmdA, sizeof(cmdA), nullptr, nullptr);
    Log("screenshot: %s", cmdA);
    Announce(Tone::Change, "Screenshot.");
    HANDLE h = CreateThread(nullptr, 0, ShotThread, job, 0, nullptr);
    if (h) CloseHandle(h);
    else { delete job; g_shotBusy = false; Log("screenshot: could not start the worker thread"); }
}
