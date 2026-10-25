// drs_logic.h - dynamic resolution decisions, no engine dependencies (drs.cpp; tests/drstest.cpp simulates them
// offline). Quick and deep when performance collapses, slow when it recovers.
//   Stage 1, render percentage (no hitch: the DLSS features are made for the largest size):
//     every 20 frames or 0.2 s: down by sqrt(budget / gpu) when ratio < 0.93, at most -25% per step;
//     up only after 3 intervals in a row with ratio > 1.10, by at most +3%.
//     emergency (every frame): if the last 6 frames average more than 1.15 x the frame time, step down at once.
//   Stage 2, output percentage (the game's resolution setting, in percent of its own value; each change is a
//            55-600 ms hitch and rebuilds the render targets). GPU time and frame pacing only: VRAM paging shows in
//            the GPU time, and VRAM over budget just releases unused models (upscaler.cpp / dlss.cpp).
//     fast:  render at its floor and p75 > 1.3 x budget for 0.5 s -> drop straight to scale x sqrt(0.9 budget / p75),
//            rounded down to 10% (e.g. 29 ms -> -40%), 1.5 s after any change.
//     down:  render at its floor and over budget for DownHold s -> -10%, 3 s after any change.
//     up (a probe): all of: cooldown since the last change; the wait after a drop is over (ProbeWait s, default 20,
//            doubled after every failed probe up to ProbeMaxWait, back to ProbeWait after a probe that held 15 s or
//            5 calm minutes); no big overload (> 1.3 x budget) within that wait; for UpHold s (counted alongside the
//            wait) the render at its ceiling with headroom, no frame over the frame time in 10 s, frames on time;
//            the 8 s p90, UpElasticity of it scaled by the area, below UpMargin x budget. Only 25-63% of the GPU
//            time follows the resolution: scaling the whole frame blocked raises with half the GPU free.
//     revert: a probe that pushes the render more than 10 points below its ceiling within 15 s is undone; any drop
//            within those 15 s counts as a failed probe.
#pragma once
#include <algorithm>
#include <cmath>

namespace drs {

struct RenderState { int upIntervals = 0; };

inline float RenderNext(RenderState& rs, float pct, float gpu, float budget, float lo, float hi) {
    const float ratio = budget / gpu;                      // > 1: headroom, < 1: over budget
    if (ratio < 0.93f) {   // hold band 0.93-1.10 (with 0.96 it hunted +-1-2 points)
        rs.upIntervals = 0;
        const float next = std::max(lo, std::round(pct * std::max(0.75f, std::sqrt(ratio))));
        return next < pct - 0.5f ? next : (pct > lo + 0.5f ? pct - 1.f : pct);
    }
    if (ratio > 1.10f) {
        if (++rs.upIntervals < 3) return pct;
        rs.upIntervals = 0;
        const float next = std::min(hi, std::round(pct * std::min(1.03f, std::sqrt(ratio))));
        return next > pct + 0.5f ? next : (pct < hi - 0.5f ? pct + 1.f : pct);
    }
    rs.upIntervals = 0;
    return pct;
}

// every frame: the mean GPU time of the last few frames against the headset's frame time; returns the new percentage.
// intervalMs = the real frame interval over the same frames (0 = unknown). Only when the GPU is the limit: drops after
// CPU / streaming hitches (GPU at ~48% of the frame) do not help. Not within 10 points of the floor (the normal stage
// still steps there).
inline float RenderEmergency(float pct, float meanMs, float frameMs, float budget, float lo, float intervalMs = 0.f) {
    if (meanMs <= 1.15f * frameMs || pct <= lo + 10.f) return pct;
    if (intervalMs > 0.f && meanMs < 0.5f * intervalMs) return pct;   // frames late for another reason (CPU, streaming)
    return std::max(lo, std::round(pct * std::max(0.75f, std::sqrt(budget / meanMs))));
}

struct OutputParams {
    float minScale, maxScale, step;
    float cooldownS;        // before an up, after any change
    float downCooldownS;    // before a normal down, after any change
    float downHoldS, upHoldS;
    float upMargin;         // predicted p90 after the raise must stay below upMargin x budget
    float probeWaitS = 20.f;      // after a drop: wait this long before probing up
    float probeMaxWaitS = 240.f;  // ... doubled after every failed probe, up to this
    float upElasticity = 0.6f;    // share of the GPU time that grows with the output area (measured 0.25-0.63)
};

struct OutputInputs {
    float pct, gpuP75, gpuP90Long, budget, lo, hi;
    double secondsSinceSpike;   // since the last frame over the headset's frame time
    double now;
    bool framesLate = false;    // the real frame interval is over 1.2 x the frame time: never raise
};

struct OutputState {
    double lowSince = -1.0, highSince = -1.0, fastSince = -1.0;
    double lastChange = -1e9, lastUp = -1e9;
    double upLockUntil = -1e9;
    float upLockLen = 0.f;         // the current wait after a drop (0 = ProbeWait)
    float raisedFrom = 0.f;
    bool probing = false;          // a raise less than 15 s old: a drop now means the probe failed
    double lastBigOverload = -1e9;
    // diagnostics only (drs.cpp log and CSV): what kept the last call from raising, as kBlock* bits, and the last
    // raise prediction
    unsigned block = 0;
    float predicted = 0.f, predictedLimit = 0.f;
};

enum : unsigned {
    kBlockAtMax = 1, kBlockCooldown = 2, kBlockWait = 4, kBlockOverload = 8, kBlockRender = 16, kBlockHeadroom = 32,
    kBlockSpike = 64, kBlockLate = 128, kBlockHold = 256, kBlockPredict = 512, kBlockChanged = 1024,
    kBlockFrozen = 2048, kBlockWarmup = 4096,   // set by drs.cpp: the controller did not run this interval
};
inline const char* BlockName(unsigned bit) {
    switch (bit) {
    case kBlockAtMax: return "at max"; case kBlockCooldown: return "cooldown"; case kBlockWait: return "wait after drop";
    case kBlockOverload: return "recent overload"; case kBlockRender: return "render below ceiling";
    case kBlockHeadroom: return "no GPU headroom"; case kBlockSpike: return "GPU spike <10 s"; case kBlockLate: return "frames late";
    case kBlockHold: return "holding"; case kBlockPredict: return "prediction too high"; case kBlockChanged: return "just changed";
    case kBlockFrozen: return "frozen (frames >33 ms)"; case kBlockWarmup: return "settling"; default: return "?";
    }
}

inline float OutputNext(OutputState& s, const OutputParams& p, float scale, const OutputInputs& in) {
    const double now = in.now;
    const float ratio = in.budget / in.gpuP75;
    s.block = kBlockChanged;   // overwritten below unless the call changes the output
    if (in.gpuP75 > 1.3f * in.budget) s.lastBigOverload = now;
    if (s.upLockLen <= 0.f || now - s.lastChange > 300.0) s.upLockLen = p.probeWaitS;   // calm for 5 min: forget failures
    if (s.probing && now - s.lastUp >= 15.0) { s.probing = false; s.upLockLen = p.probeWaitS; }   // the probe held
    auto down = [&](float to) {
        if (s.probing) s.upLockLen = std::min(p.probeMaxWaitS, s.upLockLen * 2.f);   // failed probe: back off
        s.probing = false;
        s.upLockUntil = now + s.upLockLen;
        s.lowSince = s.highSince = s.fastSince = -1.0; s.lastChange = now; s.lastUp = -1e9;
        return to;
    };
    const bool canDrop = scale > p.minScale + 0.5f;
    if (s.probing && in.pct < in.hi - 10.f && s.raisedFrom > 0.f && now - s.lastChange >= 1.5)
        return down(s.raisedFrom);
    const bool atFloor = in.pct <= in.lo + 0.5f;
    // big overload at the floor: go down several steps at once
    const bool big = atFloor && in.gpuP75 > 1.3f * in.budget;
    s.fastSince = big ? (s.fastSince < 0.0 ? now : s.fastSince) : -1.0;
    if (big && canDrop && now - s.fastSince >= 0.5 && now - s.lastChange >= 1.5) {
        const float target = std::floor(scale * std::sqrt(0.9f * in.budget / in.gpuP75) / p.step) * p.step;
        return down(std::max(p.minScale, std::min(scale - p.step, target)));
    }
    const bool atFloorOver = atFloor && ratio < 0.96f;
    s.lowSince = atFloorOver ? (s.lowSince < 0.0 ? now : s.lowSince) : -1.0;
    if (s.lowSince >= 0.0 && now - s.lowSince >= p.downHoldS && now - s.lastChange >= p.downCooldownS && canDrop)
        return down(std::max(p.minScale, scale - p.step));
    // up (a probe): the clean time counts alongside the wait after a drop, so a clean scene probes ProbeWait s after it
    const bool clean = in.pct >= in.hi - 0.5f && ratio > 1.06f && in.secondsSinceSpike >= 10.0 && !in.framesLate;
    s.highSince = clean ? (s.highSince < 0.0 ? now : s.highSince) : -1.0;
    const bool upAllowed = now - s.lastChange >= p.cooldownS && now >= s.upLockUntil && now - s.lastBigOverload >= s.upLockLen;
    {   // diagnostics: every condition that blocks a raise right now
        unsigned b = 0;
        if (scale >= p.maxScale - 0.5f) b |= kBlockAtMax;
        if (now - s.lastChange < p.cooldownS) b |= kBlockCooldown;
        if (now < s.upLockUntil) b |= kBlockWait;
        if (now - s.lastBigOverload < s.upLockLen) b |= kBlockOverload;
        if (in.pct < in.hi - 0.5f) b |= kBlockRender;
        if (ratio <= 1.06f) b |= kBlockHeadroom;
        if (in.secondsSinceSpike < 10.0) b |= kBlockSpike;
        if (in.framesLate) b |= kBlockLate;
        if (clean && now - s.highSince < p.upHoldS) b |= kBlockHold;
        const float nx = std::min(p.maxScale, scale + p.step), ar = (nx / scale) * (nx / scale);
        s.predicted = in.gpuP90Long * (1.f + p.upElasticity * (ar - 1.f)); s.predictedLimit = in.budget * p.upMargin;
        if (!(b & kBlockAtMax) && s.predicted >= s.predictedLimit) b |= kBlockPredict;
        s.block = b;
    }
    if (upAllowed && s.highSince >= 0.0 && now - s.highSince >= p.upHoldS && scale < p.maxScale - 0.5f) {
        const float next = std::min(p.maxScale, scale + p.step);
        const float area = (next / scale) * (next / scale);
        const float predicted = in.gpuP90Long * (1.f + p.upElasticity * (area - 1.f));   // the busiest frames, part scaled
        if (predicted < in.budget * p.upMargin) {
            s.block = 0;
            s.raisedFrom = scale; s.lastUp = now; s.probing = true;
            s.lowSince = s.highSince = s.fastSince = -1.0; s.lastChange = now;
            return next;
        }
    }
    return scale;
}

// ---------------------------------------------------------------- miss guard (key 3 in the debug build)
// Frames are missed with the GPU at 11.5-12.7 ms, inside the hold band above (the headset compositor and the Link
// encoder share the GPU), but only ~18 % of the late frames were clearly GPU-bound; the rest are CPU / streaming
// hitches that a lower resolution cannot fix.
// Every late frame (the headset skipped a refresh, from xrWaitFrame's predicted display times) is classified:
//   GPU-caused: the GPU time of the last 3 frames reached MissGpuFraction of the frame time, and the game thread's own
//               work (frame interval - wait for the headset - wait for the render thread) stayed under 0.9 of it;
//   CPU-caused: everything else. Only GPU-caused late frames count:
//   drop:    MissCount of them within MissWindow s -> render x0.9 (at least -2 points) at once;
//   memory:  after such a drop no raise above the failed level - 2 for MissLock s, doubled while the drops repeat
//            (up to MissLockMax), back to MissLock after 5 calm minutes; no raise within 3 s of a GPU-caused miss;
//   learned: the budget becomes 0.95 x the 25th percentile of the GPU time at the GPU-caused misses of the last 2
//            minutes (at least 4 of them), never above the ini budget nor more than budgetMaxCutFrac x the frame time
//            (0.6 ms at 90 Hz) below it; misses at the render floor are not learned.
// kLateEvent: a single GPU frame over 2.5 x the frame time is an event (a DLSS model rebuilt by F5/F6/F7/F9,
// streaming), not steady GPU load; counted with the CPU ones, never fed to the guard.
enum LateKind : int { kLateNone = 0, kLateGpu = 1, kLateCpu = 2, kLateEvent = 3 };

// A CPU overrun is checked first (else a 228 ms game-thread hitch reads as an "event"), and the render / RHI threads'
// own busy time counts as CPU too (else a 28.9 ms frame with 9.75 ms of GPU, the game thread waiting 27.5 ms for the
// render thread, reads as GPU-caused). renderBusyMs = max(GRenderThreadTime, GRHIThreadTime) of the frame (0 = unknown).
inline int ClassifyLate(bool late, float gpuMax3, float workMs, float frameMs, float gpuFrac, float renderBusyMs = 0.f) {
    if (!late) return kLateNone;
    if (workMs >= 0.9f * frameMs || renderBusyMs >= 0.9f * frameMs) return kLateCpu;
    if (gpuMax3 > 2.5f * frameMs) return kLateEvent;
    return gpuMax3 >= gpuFrac * frameMs ? kLateGpu : kLateCpu;
}

struct MissParams {
    int count = 2; float windowS = 1.f;          // GPU-caused late frames within the window that trigger a drop
    float lockS = 20.f, lockMaxS = 240.f;         // no raise above the failed level for this long (doubling)
    float episodeS = 5.f;                         // drops this close to the previous one are the same episode:
                                                  //   the lock does not double (else 4 drops in 9 s take it 20 -> 160 s)
    float quietS = 3.f;                           // no raise this soon after a GPU-caused late frame
    float calmS = 300.f;                          // a drop after this long without one starts the lock at lockS again
    float sampleLifeS = 120.f;                    // learned limit: GPU times at misses older than this are forgotten
    // budget up: with no GPU-caused late frame for upQuietS while the render is held by the budget
    // (below its ceiling, the GPU's 90th percentile near the budget), the budget grows by upStep x the frame time every
    // upEveryS, up to upMaxFrac x the frame time; any GPU-caused late frame takes the growth away at once
    float upQuietS = 30.f, upEveryS = 10.f, upStep = 0.01f, upMaxFrac = 0.865f;   // 0.865 = 12.0 ms at 72 Hz
    float upAfterDropS = 60.f;                    // no growth this soon after a guard drop (the climb back is not
                                                  //   'held by the budget'; it jumped 11.39 -> 12.36 ms)
    // the learned budget is never more than budgetMaxCutFrac x the frame time (0.6 ms at 90 Hz) under the ini's (cut
    // deeper, it fell to ~95 % of the GPU's normal time at the floor and the render could not climb again); a GPU frame
    // over spikeFrac x the frame time and the afterglowS after it are not counted (the half-rate stretch the runtime
    // holds after a spike ran at 43-55 % GPU use: no resolution fixes that)
    float budgetMaxCutFrac = 0.054f;
    float spikeFrac = 1.5f, afterglowS = 3.f;
};

struct MissState {
    double t[16] = {}; int n = 0, head = 0;                         // GPU-caused late frames since the last drop
    float gpu[32] = {}; double gpuT[32] = {}; int gn = 0, gh = 0;   // GPU time (max of 3) at each of them
    double lastGpuMiss = -1e9, lastDrop = -1e9;
    float cap = 0.f; double capUntil = -1e9; float lockLen = 0.f;
    int drops = 0;
    float bonus = 0.f; double lastBonusStep = -1e9;   // budget growth above the ini's, ms
    long long lastNoteFrame = -100;                   // frame of the last counted GPU-caused late frame
};

// One GPU spike counts once. GGPUFrameTime is not refreshed every frame (22.20/22.20/22.20 logged) and the
// classification looks at the largest of the last 3 frames, so one slow GPU frame made 2-3 "GPU-caused" late frames in
// a row (a single 22.6 ms rebuild spike caused a guard drop). A GPU-caused late frame counts only 3 or more frames after
// the last counted one. Returns true when it was counted.
// learn = false at the render floor: a GPU time measured there says nothing about a size the render can reach (the
// floor's own misses set a budget its normal GPU time could never meet)
inline void MissNote(MissState& s, double now, float gpuMax3, bool learn = true);
inline bool MissNoteOnce(MissState& s, double now, float gpuMax3, long long frame, bool learn = true) {
    if (frame - s.lastNoteFrame < 3) return false;
    s.lastNoteFrame = frame;
    MissNote(s, now, gpuMax3, learn);
    return true;
}

// true while a GPU spike (over spikeFrac x the frame time) or its afterglow is going on: not counted
inline bool MissAfterglow(double& until, double now, float gpuMax3, float frameMs, const MissParams& p) {
    if (gpuMax3 > p.spikeFrac * frameMs) until = now + p.afterglowS;
    return now < until;
}

// the new controller (re)starts after the old one ran (key 3 / key 4): no window, lock or quiet time carried over
// (else it drops within 0.5 s of a swap). The learned GPU times stay; they expire after sampleLifeS anyway.
inline void MissOnSwitch(MissState& s) {
    s.n = 0; s.cap = 0.f; s.capUntil = -1e9; s.lockLen = 0.f; s.lastGpuMiss = -1e9;
}

// a new refresh rate makes the budget growth (ms), the GPU times at misses and the recent window meaningless
// (+0.58 ms grown at a wrongly detected 120 Hz was carried into 72 Hz)
inline void MissOnHzChange(MissState& s, double now) {
    s.bonus = 0.f; s.lastBonusStep = now; s.gn = 0; s.gh = 0; s.n = 0;
}

inline void MissNote(MissState& s, double now, float gpuMax3, bool learn) {
    s.t[s.head] = now; s.head = (s.head + 1) % 16; if (s.n < 16) ++s.n;
    if (learn) { s.gpu[s.gh] = gpuMax3; s.gpuT[s.gh] = now; s.gh = (s.gh + 1) % 32; if (s.gn < 32) ++s.gn; }
    s.lastGpuMiss = now;
    s.bonus = 0.f; s.lastBonusStep = now;
}

// forget the recent GPU-caused late frames (not the learned GPU times): dynamic resolution (re)started, so frames from
// before (DLSS off, the warm-up, a comparison) must not trigger a drop
inline void MissResetWindow(MissState& s) { s.n = 0; }

// dynamic resolution (re)starts (A/B resume, F8): the budget growth starts again from the ini's
inline void MissOnResume(MissState& s, double now) { s.bonus = 0.f; s.lastBonusStep = now; }

// the GPU times at misses still in use (MissBudget learns from 4 or more)
inline int MissLearnedCount(const MissState& s, const MissParams& p, double now) {
    int m = 0;
    for (int i = 0; i < s.gn; ++i) if (now - s.gpuT[i] <= p.sampleLifeS) ++m;
    return m;
}

// once per interval: grow the budget while it is what holds the render back and nothing misses. Not while the
// learned budget is in force: growth judged against a learned 11.3 ms made the budget jump to 12.01 when the samples
// expired.
inline void MissBudgetUp(MissState& s, const MissParams& p, double now, float iniBudget, float frameMs, float pct, float hi,
                         float gpuP90Long, float budget, float lo = 0.f) {
    if (MissLearnedCount(s, p, now) >= 4) { s.lastBonusStep = now; return; }
    if (now - s.lastGpuMiss < p.upQuietS || now - s.lastBonusStep < p.upEveryS) return;
    if (now < s.capUntil || now - s.lastDrop < p.upAfterDropS) return;   // a guard lock / the climb after a drop
    if (pct >= hi - 0.5f || gpuP90Long < 0.9f * budget) return;   // not held back by the budget
    if (pct <= lo + 0.5f) return;   // at the floor the budget is not what holds the render (it cannot go lower)
    const float cap = p.upMaxFrac * frameMs - iniBudget;
    if (cap <= 0.f || s.bonus >= cap) return;
    s.bonus = std::min(cap, s.bonus + p.upStep * frameMs);
    s.lastBonusStep = now;
}

inline int MissRecent(const MissState& s, double now, float windowS) {   // the newest n entries of the ring
    int k = 0;
    for (int i = 0; i < s.n; ++i) if (now - s.t[(s.head - 1 - i + 16) % 16] <= windowS) ++k;
    return k;
}

// the budget to steer on: the ini's plus any growth, or lower when GPU-caused misses happened below it
inline float MissBudget(const MissState& s, const MissParams& p, double now, float budget, float frameMs) {
    const float up = std::min(p.upMaxFrac * frameMs, budget + s.bonus);
    float v[32]; int m = 0;
    for (int i = 0; i < s.gn; ++i) if (now - s.gpuT[i] <= p.sampleLifeS) v[m++] = s.gpu[i];   // as MissLearnedCount
    if (m < 4) return std::max(budget, up);
    std::sort(v, v + m);
    // never more than budgetMaxCutFrac x the frame time under the ini's, never under 0.6 x the frame time
    return std::max(std::max(0.6f * frameMs, budget - p.budgetMaxCutFrac * frameMs), std::min(std::max(budget, up), 0.95f * v[m / 4]));
}

// every frame: the new render percentage (below pct = drop now)
inline float MissDrop(MissState& s, const MissParams& p, double now, float pct, float lo) {
    if (pct <= lo + 0.5f || now - s.lastDrop < p.windowS || MissRecent(s, now, p.windowS) < p.count) return pct;
    if (s.lockLen <= 0.f || now - s.lastDrop > p.calmS) s.lockLen = p.lockS;
    else if (now - s.lastDrop <= p.episodeS) {}   // the same episode: keep the lock length
    else if (now < s.capUntil + p.lockS) s.lockLen = std::min(p.lockMaxS, s.lockLen * 2.f);   // failed again soon
    const float next = std::max(lo, std::min(pct - 2.f, std::round(pct * 0.9f)));
    s.cap = std::max(lo, pct - 2.f); s.capUntil = now + s.lockLen;
    s.lastDrop = now; ++s.drops; s.n = 0;   // the window starts over
    return next;
}

// the highest render percentage a raise may reach right now
inline float MissCeiling(const MissState& s, const MissParams& p, double now, float pct, float hi) {
    if (now - s.lastGpuMiss < p.quietS) return std::min(hi, pct);
    if (now < s.capUntil) return std::min(hi, std::max(pct, s.cap));
    return hi;
}

// ---------------------------------------------------------------- model-based render stage (key 3, with the guard)
// Cost model: GPU time = fixed + b x area, area = (render % / 100)^2. RenderNext's sqrt(budget / gpu) assumes the whole
// frame scales with the area, but only 25-63 % does, so its drops undershoot and take several steps. b is learned from
// the controller's own steps: the intervals before and after a step measure the same scene at two areas,
// (gpu1 - gpu0) / (area1 - area0); the median of the last 16 steps of at least 0.03 area is used, kept between 25 %
// (the low end of the measured range) and 100 % of the frame scaling with the area. Until 3 steps are known, 60 %
// (OutputUpElasticity's measured value) is assumed.
// Steps are learned only once dynamic resolution has run steadily for kModelSettleS (the caller passes `learn`):
// start-up steps whose GPU time did not follow the render pinned the model at its floor, so raises overshot and drops
// went deep (a 20-point sawtooth). Estimates older than kModelLifeS are forgotten, ModelReset on a new refresh rate,
// and raises use the more cautious of the learned and the prior slope (ModelSlopeRaise).
constexpr float kModelLifeS = 60.f, kModelSettleS = 5.f, kModelMinShare = 0.25f;
struct CostModel {
    float b[16] = {}; double t[16] = {}; int n = 0, head = 0;
    float prevGpu = 0.f, prevArea = 0.f;
};

inline void ModelReset(CostModel& m) { m = CostModel{}; }

// once per interval window: its p75 and its area; `learn` = false only remembers the window (no estimate from it)
inline void ModelObserve(CostModel& m, float gpu, float area, double now = 0.0, bool learn = true) {
    if (learn && m.prevArea > 0.f && std::fabs(area - m.prevArea) >= 0.03f) {
        const float b = (gpu - m.prevGpu) / (area - m.prevArea);
        if (b > 0.f && b < 100.f) { m.b[m.head] = b; m.t[m.head] = now; m.head = (m.head + 1) % 16; if (m.n < 16) ++m.n; }
    }
    m.prevGpu = gpu; m.prevArea = area;
}

inline int ModelCount(const CostModel& m, double now) {   // estimates still in use
    int k = 0;
    for (int i = 0; i < m.n; ++i) if (now - m.t[i] <= kModelLifeS) ++k;
    return k;
}

// ms of GPU time per unit of area at this point (clamped to 25-100 % of the frame scaling with the area)
inline float ModelSlope(const CostModel& m, float gpu, float area, double now = 0.0, float priorShare = 0.6f) {
    const float full = gpu / std::max(0.05f, area);   // everything scales
    float b = priorShare * full;
    float v[16]; int k = 0;
    for (int i = 0; i < m.n; ++i) if (now - m.t[i] <= kModelLifeS) v[k++] = m.b[i];
    if (k >= 3) { std::sort(v, v + k); b = v[k / 2]; }
    return std::min(full, std::max(kModelMinShare * full, b));
}

// raises: the steeper (more cautious) of the learned slope and the prior (60 % scaling): a raise that the model thinks is
// cheap but the prior does not stays small
inline float ModelSlopeRaise(const CostModel& m, float gpu, float area, double now = 0.0, float priorShare = 0.6f) {
    const float full = gpu / std::max(0.05f, area);
    return std::max(ModelSlope(m, gpu, area, now, priorShare), std::min(full, priorShare * full));
}

inline float ModelShare(const CostModel& m, float gpu, float area, double now = 0.0) {   // the share of the frame that scales (CSV)
    return gpu > 0.f ? ModelSlope(m, gpu, area, now) * area / gpu : 0.f;
}

// Drops go straight to the area the model says fits (95 % of the budget, at most -25 % per step). A raise needs 3 calm
// intervals (p75 with 1.10 x headroom) and the 90th percentile of the last ~2.5 s under 92 % of the budget (the
// 20-frame p75 alone started climbs that the next movement undid), and goes to the size whose predicted p90 is 92 % of
// the budget, at most maxUp points per step.
inline float RenderNextV5(RenderState& rs, const CostModel& m, float pct, float gpu, float gpuP90Long, float budget,
                          float lo, float hi, float maxUp = 10.f, double now = 0.0) {
    const float ratio = budget / gpu;
    const float area = (pct / 100.f) * (pct / 100.f);
    const float b = ModelSlope(m, gpu, area, now);
    if (ratio < 0.93f) {
        rs.upIntervals = 0;
        const float a = area - (gpu - 0.95f * budget) / b;
        float next = a > 0.f ? std::floor(100.f * std::sqrt(a)) : lo;
        next = std::max(lo, std::max(std::round(pct * 0.75f), std::min(pct - 1.f, next)));
        return next < pct ? next : pct;
    }
    if (ratio > 1.10f) {
        if (++rs.upIntervals < 3) return pct;
        rs.upIntervals = 0;
        const float target = 0.92f * budget;
        if (gpuP90Long >= target || pct >= hi - 0.5f) return pct;
        const float a = area + (target - gpuP90Long) / ModelSlopeRaise(m, gpu, area, now);   // the cautious slope
        float next = std::floor(100.f * std::sqrt(a));
        next = std::min(hi, std::min(pct + maxUp, next));
        return next >= pct + 1.f ? next : pct;
    }
    rs.upIntervals = 0;
    return pct;
}

// the headset's refresh rate from xrWaitFrame's predictedDisplayPeriod, only when it is a usual headset rate
// (a runtime holding half rate may report twice the period: 36 Hz would double the budget)
inline float SnapHeadsetHz(float hz) {
    for (float r : { 60.f, 72.f, 80.f, 90.f, 120.f, 144.f }) if (std::fabs(hz - r) < r * 0.02f) return r;
    return 0.f;
}

// camera motion between two frames from their view matrices (3x3 rotation part, any axis convention) and
// view origins (cm): the turn angle in degrees and the distance in metres
inline float TurnDegrees(const double* r0, const double* r1) {   // row-major 3x3 each (9 doubles)
    double tr = 0.0;   // trace(R1^T R0) = sum of the element-wise products
    for (int i = 0; i < 9; ++i) tr += r0[i] * r1[i];
    const double c = std::min(1.0, std::max(-1.0, (tr - 1.0) * 0.5));
    return (float)(std::acos(c) * 57.29577951308232);
}

struct MotionParams { float turnDps = 90.f, moveMps = 1.5f, holdS = 1.f; };
// ini MotionHold (off by default until the CSV shows GPU time following motion): no raise while the camera turns
// faster than turnDps or moves faster than moveMps, nor for holdS after
inline bool MotionBlocksRaise(const MotionParams& p, double now, double lastMoving) { return now - lastMoving < p.holdS; }

// ---------------------------------------------------------------- adaptive DLSS set-up size
// The DLSS feature is created for the largest input it may get, and model M's cost follows that size, not the input
// (DLAA-sized centre feature 2.34 ms/eye at 50-70 % input, Quality-sized 1.30). The ceiling stays the F6 quality's,
// but the feature is made for a step that the render actually uses:
//   steps:   the Quality size (66.7 %), 75, 83, 91, 100 %, those above the floor and up to the ceiling (plus the
//            ceiling itself when it is not a step): about +0.2 ms/eye of DLSS each (model M);
//   shrink:  the render stayed usedMargin points under a smaller step for downAfterS -> the smallest such step;
//   grow:    the render at the top of the step for upHoldS while the predicted GPU (p90 + the bigger step's cost + the
//            render climbing to the new step's top along the cost model slope) fits upMargin x the budget -> one step
//            up, as a probe: unless the render goes usedMargin points past the old step within probeEvalS, back down
//            and no growth for backoffS (doubling up to backoffMaxS); a used probe resets the back-off. No growth in
//            the first 2 s of frames after a (re)start (SizeResume);
//   pace:    at least minGapS between changes: each one rebuilds the features (a short hitch, DLSS history restarts);
//   cost:    a step's cost is the GPU jump measured around a change at an unchanged render (SizeMeasureBegin / End: the
//            caller holds the render and passes the median GPU before and after the rebuild); until a step is measured,
//            the DLSS timers' difference x stepFactor (learned from the measured steps, stepFactor0 before any). The
//            timers alone miss most of it: 66.7 -> 75 % at an unchanged render raised the GPU 8.45 -> 10.0 ms while
//            they rose 0.46 ms for both eyes;
//   undo:    a probe whose render reaches the bigger step with the GPU p90 over upMargin x the budget for overNowS, or
//            for overShare of its time there at the end of probeEvalS, or with probeMissMax GPU-caused late frames or
//            probeDropMax fast drops, is undone (and backs off like an unused one).
struct SizeParams {
    float downAfterS = 30.f, upHoldS = 12.f, minGapS = 45.f, probeEvalS = 45.f;
    float backoffS = 60.f, backoffMaxS = 600.f;
    float minSlope = 5.f;    // GPU ms per unit of render area at least this (menu frames gave a fallback of 2.0)
    float upMargin = 0.92f, costExp = 1.5f, usedMargin = 2.f;   // costExp: measured 1.65 / 1.96 / 2.31 ms/eye
                                                                 // at 66.7 / 75 / 83 %
    float stepFactor0 = 2.5f;      // estimate from one step (+1.5 ms measured / ~0.6 ms from the DLSS costs)
    float measureS = 2.5f;         // frames measured after the rebuild's 1.5 s, the render held
    float maxStepMs = 4.f;         // a measured jump above this is a scene change, not the step (dropped)
    float overNowS = 20.f, overMinS = 10.f, overShare = 0.5f;   // probe undo when the GPU stays over the limit
    int probeMissMax = 4;          // GPU-caused late frames during a probe that undo it (one guard drop already takes 2
                                   //   and is answered by the render alone)
    int probeDropMax = 2;          // fast drops (guard / emergency) during a probe that undo it (undoing at the first:
                                   //   the undo's rebuild cost 20 skipped refreshes, ~10x the miss it answered)
    // no growth while the upscaler's cost at the current size is unknown (else the step is estimated at 0.00 ms before
    // the first DLSS timing); if no timing arrives within costWaitS (a path without the GPU timer), the step is taken as
    // unknownStepMs (the largest measured: 66.7 -> 75 % at 140 % / 72 Hz)
    float costWaitS = 60.f, unknownStepMs = 1.5f;
    // a fast drop or a GPU-caused late frame counts against a probe only with the GPU p90 at probeRoomFrac x the budget
    // or more, and a fast drop only within gpuMissNearS of a GPU-caused late frame (hitches with the GPU p90 at ~5.3 ms
    // of a 9.44 ms budget undid probes; the emergency step also fires on CPU-labelled hitches)
    float probeRoomFrac = 0.8f, gpuMissNearS = 1.f;
    // the upHoldS hold before a growth survives conditions failing for up to holdGraceS (a hitch's spike frames lift
    // the 2.5 s p90 over the limit for a moment)
    float holdGraceS = 2.f;
    // after a growth undone for GPU load, the same step waits for a lighter scene: the GPU p90 at the smaller size
    // under the p90 before the failed try minus retryFrac x the rise the check predicted for it (self-scaling: the
    // step's own cost on this PC); kept in the profile; ends after retryLifeS or when that step passes
    float retryFrac = 0.5f, retryLifeS = 1800.f;
    // the ms values above as fractions of the frame time, from the 90 Hz values they were tuned at (11.11 ms:
    // minSlope 5.0, unknownStepMs 1.5, maxStepMs 4.0; SizeParamsFor); a measured jump is also accepted up to
    // maxStepDlss x the DLSS costs' own difference (a slower GPU's real step can pass 4 ms)
    float minSlopeFrac = 0.45f, unknownStepFrac = 0.135f, maxStepFrac = 0.36f, maxStepDlss = 8.f;
};

// the size parameters for this refresh rate (the defaults are the 90 Hz values)
inline SizeParams SizeParamsFor(float frameMs) {
    SizeParams p;
    p.minSlope = p.minSlopeFrac * frameMs;
    p.unknownStepMs = p.unknownStepFrac * frameMs;
    p.maxStepMs = p.maxStepFrac * frameMs;
    return p;
}
struct SizeState {
    float size = 0.f;                  // the feature's size in % of the output (0 = not started)
    float hiSeen = 0.f;                // the ceiling the steps were made for
    int n = 0; float tiers[6] = {};    // the steps, ascending
    double lastChange = -1e9, pinnedSince = -1.0;
    double lastNear[6] = {};           // per step: last time the render was within usedMargin of it, or above
    float probeFrom = 0.f; double probeAt = -1e9; float probePeak = 0.f;
    float backoff = 0.f; double noUpUntil = -1e9;
    float cost[6] = {};                // DLSS ms per eye measured at each step (0 = not yet)
    int changes = 0;
    double readyAt = -1e9;             // no growth before this (2 s of frames after a (re)start and its hold)
    float lastPredicted = 0.f;         // the GPU the last growth check predicted at the next step's top (log)
    // measured step costs and the step being measured
    float stepCost[6] = {};            // GPU ms (whole frame, both eyes) from step i to step i + 1 (0 = not measured)
    float stepFactor = 0.f;            // measured jump / the DLSS costs' difference (0 = stepFactor0)
    int measLo = -1; bool measUp = false; float measPct = 0.f, measBefore = 0.f, measDlss = 0.f;
    float lastJump = 0.f;              // the last measured jump (log)
    // the probe's time with the render at the bigger step, over the limit there, and the late-frame count at its start
    double lastCall = -1e9; float probeTopS = 0.f, probeOverS = 0.f; int probeMiss0 = -1;
    bool probePassed = false;          // set when a probe passes its trial (the caller logs it and clears it)
    int probeDrops = 0; double probeLastDrop = -1e9;   // fast drops seen during the probe
    float passedFrom = 0.f, passedPeak = 0.f, passedTopS = 0.f, passedOverS = 0.f;
    int probeMissSeen = -1;            // the GPU-caused late-frame count last seen during the probe
    // the best size this setup holds: the size of a probe that passed its trial, or the one an undone probe went
    // back to; bestChanged tells the caller to keep it in the profile (size:best)
    float best = 0.f; bool bestChanged = false;
    // the hold's grace (when its conditions started failing) and its restarts since the caller last logged them
    double holdFailSince = -1.0; int holdResets = 0, holdResetsOver = 0, holdResetsRender = 0;
    // the probe's GPU p90 before it and the rise its check predicted; per step i (i -> i + 1) the GPU p90 the
    // smaller size must get under before that step is tried again, until when; retryChanged = the step to keep (-1 none)
    float probeP90From = 0.f, probeRise = 0.f;
    float retryP90[6] = {}; double retryUntil[6] = {}; int retryChanged = -1;
};

// the size logic's "not gameplay" test: a menu / pause (velocity placeholder, or the game thread's smoothed work under
// 2 ms) or the 2 s after a load. Not GPU use: with real headroom it stays under 60 % in gameplay too (97 % of rows).
inline bool SizeIdle(bool velPlaceholder, float workEmaMs, bool afterLoad) {
    return velPlaceholder || (workEmaMs > 0.f && workEmaMs < 2.f) || afterLoad;
}

inline int SizeIndex(const SizeState& s) {
    int best = 0;
    for (int i = 0; i < s.n; ++i) if (std::fabs(s.tiers[i] - s.size) < std::fabs(s.tiers[best] - s.size)) best = i;
    return best;
}

inline void SizeApply(SizeState& s, double now, float size) {
    s.size = size; s.lastChange = now; s.pinnedSince = -1.0; ++s.changes;
    for (int i = 0; i < s.n; ++i) s.lastNear[i] = now;   // evidence for the next change starts now
}

// (re)builds the steps for this ceiling and floor; the size is kept when it is still a step, else the smallest step
// at or above pct (the render the controller resumes at)
inline void SizeStart(SizeState& s, const SizeParams& p, double now, float pct, float lo, float hi, float qualityPct) {
    s.n = 0;
    for (float t : { qualityPct, 75.f, 83.f, 91.f, 100.f })
        if (t > lo + 0.5f && t <= hi + 0.05f && s.n < 5) s.tiers[s.n++] = t;
    // the ceiling itself (e.g. Balanced 58), unless within a point of the last step (the Quality ceiling 67 would
    // add 67.0 after 66.7, a step that rebuilds DLSS for nothing)
    if (s.n == 0 || s.tiers[s.n - 1] < hi - 1.f) s.tiers[s.n++] = hi;
    s.hiSeen = hi;
    if (s.backoff <= 0.f) s.backoff = p.backoffS;
    bool still = false;
    for (int i = 0; i < s.n; ++i) if (std::fabs(s.tiers[i] - s.size) < 0.05f) still = true;
    if (still) return;
    float want = s.tiers[s.n - 1];
    for (int i = s.n - 1; i >= 0; --i) if (s.tiers[i] >= pct - 0.05f) want = s.tiers[i];
    for (int i = 0; i < 6; ++i) { s.cost[i] = 0.f; s.stepCost[i] = 0.f; s.retryP90[i] = 0.f; }   // other steps: their costs no longer apply
    s.probeFrom = 0.f; s.measLo = -1;
    SizeApply(s, now, want);
}

// dynamic resolution (re)starts (key-4 A/B resume, F8, DLSS back on): the timers start over, so nothing seen before
// the pause decides a change (a 'pinned at the top' timer from before a pause made a probe 5 s after the resume,
// judged on 0.2 s of frames); a probe still on trial gets a fresh window; growth waits until readyAt (the caller: the
// end of the hold + 2 s of frames)
inline void SizeResume(SizeState& s, double now, double readyAt) {
    s.pinnedSince = -1.0; s.holdFailSince = -1.0;
    for (int i = 0; i < s.n; ++i) s.lastNear[i] = now;
    if (s.probeFrom > 0.f) { s.probeAt = now; s.probePeak = 0.f; s.probeTopS = s.probeOverS = 0.f; s.probeMiss0 = -1; s.probeMissSeen = -1; s.probeDrops = 0; s.probeLastDrop = -1e9; }
    s.readyAt = readyAt;
    s.measLo = -1;   // a step being measured is not measured across a pause
}

// the render % dynamic resolution (re)starts at: an A/B swap (exact) resumes where it was, a restart 10 points under it;
// the session's first start (lastPct 0) at firstPct (the caller passes Quality, 66.7 %) at least, not the floor (with
// no profile yet the menu and the first gameplay seconds ran at the 50 % floor, seen as Ultra Performance)
inline float RenderStart(float lo, float hi, float firstPct, float lastPct, bool exact) {
    if (lastPct > 0.f) return std::min(hi, std::max(lo, exact ? lastPct : lastPct - 10.f));
    return std::min(hi, std::max(lo, firstPct));
}

// the size a session starts at, from the setup's best size (0 = unknown: unchanged): the step below it, so a probe
// checks the best one again (the smallest step when the best is the smallest), never below the size already chosen
inline float SizeStartStep(const SizeState& s, float best) {
    if (best <= 0.f || s.n == 0) return s.size;
    float want = s.tiers[0];
    for (int i = 0; i < s.n; ++i) if (s.tiers[i] < best - 0.05f) want = s.tiers[i];
    return std::max(s.size, want);
}

inline float SizeCost(const SizeState& s, const SizeParams& p, int i) {
    if (s.cost[i] > 0.f) return s.cost[i];
    int k = SizeIndex(s);
    if (s.cost[k] <= 0.f) {   // else the nearest step with a measured cost (a shrink right after a growth)
        k = -1;
        for (int j = 0; j < s.n; ++j) if (s.cost[j] > 0.f && (k < 0 || std::abs(j - i) < std::abs(k - i))) k = j;
        if (k < 0) return 0.f;   // unknown: only the GPU p90 is checked
    }
    return s.cost[k] * std::pow(s.tiers[i] / s.tiers[k], p.costExp);
}

// is the step's cost known (measured, or the upscaler's cost measured at some size)?
inline bool SizeStepKnown(const SizeState& s, const SizeParams& p, int i) {
    return i >= 0 && i + 1 < s.n && (s.stepCost[i] > 0.f || SizeCost(s, p, i) > 0.f);
}

// the GPU cost of going from step i to step i + 1 (whole frame, both eyes): measured when known, else the DLSS costs'
// difference x the learned factor (stepFactor0 before any step was measured)
inline float SizeStepExtra(const SizeState& s, const SizeParams& p, int i) {
    if (i < 0 || i + 1 >= s.n) return 0.f;
    if (s.stepCost[i] > 0.f) return s.stepCost[i];
    if (!SizeStepKnown(s, p, i)) return p.unknownStepMs;   // never 0
    const float dl = 2.f * std::max(0.f, SizeCost(s, p, i + 1) - SizeCost(s, p, i));
    return dl * (s.stepFactor > 0.f ? s.stepFactor : p.stepFactor0);
}

// the GPU predicted with the next step's feature and the render at that step's top (0 at the top step): the GPU p90
// now + the step's cost (measured, or the DLSS costs x the factor; both eyes) + the render climbing to the new top
// (at 83 % the render climbed to 79-81 with the GPU at 97-100 % of the budget; slope 0 = DLSS cost only)
inline float SizePredict(const SizeState& s, const SizeParams& p, float pct, float gpuP90, float slopePerArea) {
    const int k = SizeIndex(s);
    if (k + 1 >= s.n) return 0.f;
    const float aNow = (pct / 100.f) * (pct / 100.f), aTop = (s.tiers[k + 1] / 100.f) * (s.tiers[k + 1] / 100.f);
    const float slope = slopePerArea > 0.f ? std::max(p.minSlope, slopePerArea) : 0.f;
    return gpuP90 + SizeStepExtra(s, p, k) + slope * std::max(0.f, aTop - aNow);
}

// the caller changed the size from -> to with the render at pct (unchanged by the change) and gpuBefore = the
// median GPU of the frames before at that render (<= 0: not enough of them). Only neighbouring steps are measured.
inline void SizeMeasureBegin(SizeState& s, const SizeParams& p, float from, float to, float pct, float gpuBefore) {
    s.measLo = -1;
    if (gpuBefore <= 0.f) return;
    int a = -1, b = -1;
    for (int i = 0; i < s.n; ++i) {
        if (std::fabs(s.tiers[i] - from) < 0.05f) a = i;
        if (std::fabs(s.tiers[i] - to) < 0.05f) b = i;
    }
    if (a < 0 || b < 0 || std::abs(a - b) != 1) return;
    s.measLo = std::min(a, b); s.measUp = b > a; s.measPct = pct; s.measBefore = gpuBefore;
    s.measDlss = 2.f * std::max(0.f, SizeCost(s, p, s.measLo + 1) - SizeCost(s, p, s.measLo));
}

// gpuAfter = the median GPU of the frames after the rebuild at the same render (<= 0: not enough, or the render moved:
// nothing learned). A jump under half the DLSS costs' own difference or over the cap (maxStepMs, or maxStepDlss x that
// difference when larger) is the scene changing, not the step: dropped. Else it is averaged with the step's earlier
// measurement; the factor (jump / DLSS difference, 1-6) carries it to the steps not measured yet. Returns true when
// something was learned.
inline bool SizeMeasureEnd(SizeState& s, const SizeParams& p, float gpuAfter) {
    const int i = s.measLo;
    s.measLo = -1;
    if (i < 0 || gpuAfter <= 0.f) return false;
    const float jump = s.measUp ? gpuAfter - s.measBefore : s.measBefore - gpuAfter;
    s.lastJump = jump;
    if (jump <= 0.f || jump < 0.5f * s.measDlss || jump > std::max(p.maxStepMs, p.maxStepDlss * s.measDlss)) return false;
    s.stepCost[i] = s.stepCost[i] > 0.f ? 0.5f * (s.stepCost[i] + jump) : jump;
    if (s.measDlss > 0.05f) {
        const float f = std::min(6.f, std::max(1.f, jump / s.measDlss));
        s.stepFactor = s.stepFactor > 0.f ? 0.5f * (s.stepFactor + f) : f;
    }
    return true;
}

// step k -> k + 1 failed for GPU load recently and the GPU p90 is not yet under the lighter-scene line
inline bool SizeWaitsLighter(const SizeState& s, int k, double now, float gpuP90) {
    return k >= 0 && k < 6 && s.retryP90[k] > 0.f && now < s.retryUntil[k] && gpuP90 > s.retryP90[k];
}

// once per DRS interval: returns the size to use (== s.size when unchanged); why gets a short reason on a change.
// lastFastDrop: the time of the last miss-guard or emergency drop of the render; slopePerArea: the controller's
// cautious GPU ms per unit of render area (ModelSlopeRaise), 0 = only the DLSS cost is predicted; gpuMisses: the
// running count of GPU-caused late frames (-1 = unknown); lastGpuMiss: the time of the last one
inline float SizeNext(SizeState& s, const SizeParams& p, double now, float pct, float gpuP90, float budget, float dlssMs,
                      const char** why, double lastFastDrop = -1e9, float slopePerArea = 0.f, bool idle = false, int gpuMisses = -1,
                      double lastGpuMiss = -1e9) {
    *why = "";
    if (s.n == 0 || s.size <= 0.f) return s.size;
    const int k = SizeIndex(s);
    const float dt = (float)std::min(1.0, std::max(0.0, now - s.lastCall));
    s.lastCall = now;
    if (dlssMs > 0.f && now - s.lastChange > 5.0) s.cost[k] = s.cost[k] > 0.f ? 0.8f * s.cost[k] + 0.2f * dlssMs : dlssMs;
    for (int i = 0; i < s.n; ++i) if (pct >= s.tiers[i] - p.usedMargin || idle) s.lastNear[i] = now;   // a menu is no evidence
    const bool gpuRoom = gpuP90 < p.probeRoomFrac * budget;
    auto stepOf = [&](float size) { for (int i = 0; i < s.n; ++i) if (std::fabs(s.tiers[i] - size) < 0.05f) return i; return -1; };
    auto undo = [&](const char* reason, bool gpuLoad) {
        const float from = s.probeFrom;
        s.probeFrom = 0.f;
        s.noUpUntil = now + s.backoff; s.backoff = std::min(p.backoffMaxS, s.backoff * 2.f);
        // only GPU load says the bigger size does not fit: the best comes down only when the size that failed is
        // the best or above it (a heavy scene under the best keeps it), and the step waits for a lighter scene
        if (gpuLoad) {
            if (s.best <= 0.f || s.size >= s.best - 0.05f) { s.best = from; s.bestChanged = true; }
            const int i = stepOf(from);
            if (i >= 0 && s.probeP90From > 0.f) {
                s.retryP90[i] = std::max(0.01f, s.probeP90From - p.retryFrac * std::max(0.f, s.probeRise));
                s.retryUntil[i] = now + p.retryLifeS;
                s.retryChanged = i;
            }
        }
        *why = reason;
        return from;
    };
    // fast drops during the probe undo it at the probeDropMax-th (the first is answered by the render); only drops near
    // a GPU-caused late frame with the GPU p90 near the budget count
    if (s.probeFrom > 0.f && lastFastDrop > s.probeAt && lastFastDrop > s.probeLastDrop) {
        s.probeLastDrop = lastFastDrop;
        const bool gpuDrop = !gpuRoom && std::fabs(lastFastDrop - lastGpuMiss) <= p.gpuMissNearS;
        if (gpuDrop && ++s.probeDrops >= p.probeDropMax) return undo("the render had to drop fast twice during the probe", true);
    }
    if (s.probeFrom > 0.f) {   // a growth on trial
        s.probePeak = std::max(s.probePeak, pct);
        // judged where the render uses the bigger step, after the rebuild, without menus / loads
        if (s.probeMiss0 < 0 && gpuMisses >= 0) s.probeMiss0 = s.probeMissSeen = gpuMisses;
        if (s.probeMiss0 >= 0 && gpuMisses > s.probeMissSeen) {   // late frames with GPU room are spikes: not counted
            if (gpuRoom) s.probeMiss0 += gpuMisses - s.probeMissSeen;
            s.probeMissSeen = gpuMisses;
        }
        if (!idle && now - s.probeAt >= 2.0 && pct > s.probeFrom + p.usedMargin) {
            s.probeTopS += dt;
            if (gpuP90 > p.upMargin * budget) s.probeOverS += dt;
        }
        if (s.probeMiss0 >= 0 && gpuMisses - s.probeMiss0 >= p.probeMissMax) return undo("GPU-caused late frames at the bigger size", true);
        if (s.probeOverS >= p.overNowS) return undo("no GPU room at the bigger size (GPU p90 over the limit)", true);
        if (now - s.probeAt >= p.probeEvalS) {
            if (s.probeTopS >= p.overMinS && s.probeOverS >= p.overShare * s.probeTopS)
                return undo("no GPU room at the bigger size (GPU p90 over the limit)", true);
            if (s.probePeak <= s.probeFrom + p.usedMargin)   // the room was not used: back, and wait longer next time
                return undo("the bigger size was not used", false);
            s.probePassed = true;   // kept (logged by the caller)
            // a passed step raises the best, never lowers it; the step needs no lighter scene any more
            if (s.size > s.best + 0.05f) { s.best = s.size; s.bestChanged = true; }
            if (const int i = stepOf(s.probeFrom); i >= 0 && s.retryP90[i] > 0.f) { s.retryP90[i] = 0.f; s.retryChanged = i; }
            s.passedFrom = s.probeFrom; s.passedPeak = s.probePeak; s.passedTopS = s.probeTopS; s.passedOverS = s.probeOverS;
            s.probeFrom = 0.f;
            s.backoff = p.backoffS;
        }
    }
    if (now - s.lastChange < p.minGapS) return s.size;
    for (int i = 0; i < k; ++i)   // shrink to the smallest step the render stayed under
        if (now - s.lastNear[i] >= p.downAfterS) { *why = "the render stayed under a smaller size"; return s.tiers[i]; }
    // a menu / pause / loading moment restarts the wait: its GPU time says nothing about gameplay (a probe to 83 % was
    // decided on 14 s of a menu at 5.65 ms)
    if (idle) { s.pinnedSince = -1.0; return s.size; }
    if (k + 1 < s.n && now >= s.noUpUntil && now >= s.readyAt) {   // grow: pinned at the top with room for the bigger feature
        const float predicted = SizePredict(s, p, pct, gpuP90, slopePerArea);
        s.lastPredicted = predicted;
        // not before the upscaler's cost is known here (or costWaitS passed: then the unknownStepMs fallback)
        const bool costWait = !SizeStepKnown(s, p, k) && now - s.lastChange < p.costWaitS;
        const bool atTop = pct >= s.size - 0.75f, fits = predicted <= p.upMargin * budget;
        const bool heavier = SizeWaitsLighter(s, k, now, gpuP90);
        if (!costWait && atTop && fits && !heavier) {
            s.holdFailSince = -1.0;
            if (s.pinnedSince < 0.0) s.pinnedSince = now;
            else if (now - s.pinnedSince >= p.upHoldS) {
                s.probeFrom = s.size; s.probeAt = now; s.probePeak = pct;
                s.probeTopS = s.probeOverS = 0.f; s.probeMiss0 = s.probeMissSeen = gpuMisses;
                s.probeDrops = 0; s.probeLastDrop = -1e9;
                s.probeP90From = gpuP90; s.probeRise = std::max(0.f, predicted - gpuP90);
                *why = "at the top with GPU room (probe)";
                return s.tiers[k + 1];
            }
        } else if (s.pinnedSince >= 0.0) {
            // a hold survives a moment over the limit or under the top (a hitch); a cost not known yet or a scene
            // still too heavy for a step that failed restarts it at once
            if (s.holdFailSince < 0.0) s.holdFailSince = now;
            if (costWait || heavier || now - s.holdFailSince > p.holdGraceS) {
                s.pinnedSince = -1.0; s.holdFailSince = -1.0;
                ++s.holdResets; if (!fits) ++s.holdResetsOver; if (!atTop) ++s.holdResetsRender;
            }
        }
    }
    return s.size;
}

}  // namespace drs
