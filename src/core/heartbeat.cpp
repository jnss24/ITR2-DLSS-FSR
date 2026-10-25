// Diagnostics: detects one-frame camera jumps that come straight back (the "heartbeat" seen with the head still), in
// every mode, from the engine's view matrices at the end of each frame (engine.cpp, view extension slot 20). Logs a
// summary every 10 s and the frame timing around the first events.
#include "common.h"
#include <cmath>
#include <algorithm>

using namespace ue;

namespace {
struct M4 { double m[4][4]; };
M4 Mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        double s = 0; for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j]; r.m[i][j] = s;
    }
    return r;
}
bool Inverse(const M4& in, M4& out) {
    double a[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { a[i][j] = in.m[i][j]; a[i][j + 4] = (i == j); }
    for (int c = 0; c < 4; ++c) {
        int p = c; for (int r = c + 1; r < 4; ++r) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-30) return false;
        if (p != c) for (int j = 0; j < 8; ++j) { double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        const double d = a[c][c]; for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out.m[i][j] = a[i][j + 4];
    return true;
}
const M4& Mat(const uint8_t* p, uint32_t o) { return *(const M4*)(p + o); }
}   // namespace

// Heartbeat: with the head still the image shakes a little and comes straight back. Burst captures show the engine's
// camera itself turning 0.5-0.9 render px for ONE frame and back, every 3-5 frames, with DLSS and with TAA.
// At the end of every frame, in every mode, the left eye's turn since the previous frame comes from the view matrices
// (where a distant point at the view centre was in the previous frame, as an angle). A frame turning more than
// 0.7 arcmin (about 0.3 render px at 2720 px per eye, estimate) followed by one turning back by at least half of it
// counts as one heartbeat. Normal head movement turns the same way frame to frame and isn't counted. A summary line
// every 10 s; the first events of each 10 s get their own line with the frame timing around them (are the jumps on
// late frames?).
namespace {
struct HbFrame { double tx, ty, intervalMs, gpuMs; bool on; };
}
static HbFrame g_hbPrev{}, g_hbPrev2{};
static bool g_hbHavePrev = false, g_hbHavePrev2 = false;
static LARGE_INTEGER g_hbLastQpc{}, g_hbWindowStart{};
static double g_hbQpcToMs = 0.0;
static int g_hbFrames = 0, g_hbEvents = 0, g_hbFramesOn = 0, g_hbEventsOn = 0, g_hbLogged = 0;
static double g_hbSumJump = 0, g_hbSumIntJump = 0, g_hbSumInt = 0, g_hbSumGpuJump = 0, g_hbSumGpu = 0, g_hbMaxJump = 0;
static int g_hbLate = 0, g_hbLateJump = 0;

void HeartbeatObserveFamily(void* family) {
    if (!family) return;
    if (!At<bool>(family, off::Family_bIsMainViewFamily)) return;
    if (At<int32_t>(family, off::Family_ViewsNum) < 1) return;
    const uint8_t* view = (const uint8_t*)At<void**>(family, off::Family_ViewsData)[0];
    if (!view) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (g_hbQpcToMs == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_hbQpcToMs = 1000.0 / (double)f.QuadPart; g_hbWindowStart = now; }
    const double interval = g_hbLastQpc.QuadPart ? (now.QuadPart - g_hbLastQpc.QuadPart) * g_hbQpcToMs : 0.0;
    g_hbLastQpc = now;
    const uint8_t* vm = view + off::View_ViewMatrices;
    const uint8_t* pvm = view + off::ViewInfo_PrevViewMatrices;
    M4 invProj;
    if (!Inverse(Mat(vm, off::VM_ProjectionNoAA), invProj)) return;
    const M4 c = Mul(Mul(Mul(invProj, Mat(vm, off::VM_InvViewMatrix)), Mat(pvm, off::VM_ViewMatrix)), Mat(pvm, off::VM_ProjectionNoAA));
    const double z = 1e-5, wx = c.m[2][0] * z + c.m[3][0], wy = c.m[2][1] * z + c.m[3][1], ww = c.m[2][3] * z + c.m[3][3];
    if (fabs(ww) < 1e-12) return;
    const M4& proj = Mat(vm, off::VM_ProjectionNoAA);
    if (proj.m[0][0] < 1e-6 || proj.m[1][1] < 1e-6) return;
    const double toArcmin = 180.0 / 3.14159265358979 * 60.0;
    HbFrame f;
    f.tx = (wx / ww) / proj.m[0][0] * toArcmin;   // NDC offset / focal = tangent ~ angle near the centre
    f.ty = (wy / ww) / proj.m[1][1] * toArcmin;
    f.intervalMs = interval;
    f.gpuMs = g_res.addr[I_GGPUFrameTime] ? *(volatile uint32_t*)g_res.addr[I_GGPUFrameTime] * g_hbQpcToMs : 0.0;
    f.on = g_dlssEnabled.load(std::memory_order_relaxed);
    const bool cut = hypot(f.tx, f.ty) > 120.0;   // camera cut / teleport / first frame: start over
    if (cut) { g_hbHavePrev = g_hbHavePrev2 = false; return; }
    ++g_hbFrames; if (f.on) ++g_hbFramesOn;
    if (interval > 0) { g_hbSumInt += interval; g_hbSumGpu += f.gpuMs; if (interval > 16.5) ++g_hbLate; }
    if (g_hbHavePrev) {
        const double jx = g_hbPrev.tx, jy = g_hbPrev.ty, j = hypot(jx, jy);
        const double back = -(jx * f.tx + jy * f.ty) / (j > 0 ? j : 1);   // how much of the jump this frame undid
        const double net = hypot(jx + f.tx, jy + f.ty);
        if (j > 0.7 && back > 0.5 * j && net < 0.5 * j) {
            ++g_hbEvents; if (g_hbPrev.on) ++g_hbEventsOn;
            g_hbSumJump += j; g_hbMaxJump = std::max(g_hbMaxJump, j);
            g_hbSumIntJump += g_hbPrev.intervalMs; g_hbSumGpuJump += g_hbPrev.gpuMs;
            if (g_hbPrev.intervalMs > 16.5) ++g_hbLateJump;
            if (g_hbLogged < 6) {
                ++g_hbLogged;
                Log("heartbeat: camera turned %+.2f,%+.2f arcmin for one frame and %+.2f,%+.2f back; frame intervals %.1f / %.1f / %.1f ms "
                    "(before / jump / after), GPU %.1f ms at the jump; upscaler %s",
                    jx, jy, f.tx, f.ty, g_hbHavePrev2 ? g_hbPrev2.intervalMs : 0.0, g_hbPrev.intervalMs, f.intervalMs, g_hbPrev.gpuMs,
                    g_hbPrev.on ? "ON" : "OFF");
            }
            g_hbHavePrev2 = false; g_hbHavePrev = false;   // the return frame is not the start of another jump
            return;
        }
    }
    g_hbPrev2 = g_hbPrev; g_hbHavePrev2 = g_hbHavePrev;
    g_hbPrev = f; g_hbHavePrev = true;
    const double winMs = (now.QuadPart - g_hbWindowStart.QuadPart) * g_hbQpcToMs;
    if (winMs >= 10000.0 && g_hbFrames > 0) {
        const char* mode = g_hbFramesOn == g_hbFrames ? "ON" : g_hbFramesOn == 0 ? "OFF" : "MIX";
        Log("heartbeat: last %.0f s, upscaler %s: %d camera jumps-and-back in %d frames (%.1f per s), mean %.2f arcmin, largest %.2f; "
            "frame interval at the jumps %.1f ms vs %.1f ms overall, GPU %.1f vs %.1f ms; late frames (>16.5 ms) %d of the jumps, %d overall",
            winMs / 1000.0, mode, g_hbEvents, g_hbFrames, g_hbEvents / (winMs / 1000.0), g_hbEvents ? g_hbSumJump / g_hbEvents : 0.0, g_hbMaxJump,
            g_hbEvents ? g_hbSumIntJump / g_hbEvents : 0.0, g_hbSumInt / g_hbFrames, g_hbEvents ? g_hbSumGpuJump / g_hbEvents : 0.0,
            g_hbSumGpu / g_hbFrames, g_hbLateJump, g_hbLate);
        g_hbWindowStart = now;
        g_hbFrames = g_hbEvents = g_hbFramesOn = g_hbEventsOn = g_hbLogged = g_hbLate = g_hbLateJump = 0;
        g_hbSumJump = g_hbSumIntJump = g_hbSumInt = g_hbSumGpuJump = g_hbSumGpu = g_hbMaxJump = 0;
    }
}

// for test code: events and frames counted in the current 10 s window
void HeartbeatTestCounts(int& events, int& frames) { events = g_hbEvents; frames = g_hbFrames; }
