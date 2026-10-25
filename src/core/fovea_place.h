// fovea_place.h - input origin and jitter for the foveated window, so its history stays registered when the
// render size changes. No engine dependencies (tests/placetest.cpp checks it offline).
//
// If the window's output position follows a rounded render position, every render-size step moves it by up to
// 1 output pixel and leaves the input up to half a render pixel off. The centre model sees that as the picture
// jumping with no motion vectors and partly drops its history: a pulse on every dynamic-resolution step.
// So the output window (o, ow) is fixed in output pixels; each frame the input starts at the whole render pixel r
// nearest to where it should be, and the remainder goes into the jitter.
//
// The engine renders with jitter J (render px), passed to DLSS as is: render pixel c holds the scene at c - J.
// The window's model assumes its input pixel i (from r) holds window position (i + 0.5 - J') x k output pixels,
// k = ow / rw. The truth is (r + i + 0.5 - J) / s - o, s = render / output size. Equating the two at the window
// centre gives J'; with k = 1/s exactly, J' = J - (r - o s). Of the whole-pixel origins near o s + J, the one with
// the smallest |J'| is used (|J'| <= ~0.5). The residual is the scale difference between rw and ow s (rw is whole
// pixels): at most a quarter of a render pixel at the window's edges (up to ~0.5 output px at 50% render), where
// the window is already fading out (soft blend).
#pragma once
#include <cmath>

namespace fovea {

// one axis: s = render / output, o = window output origin, ow = window output size, rw = window render size,
// J = this frame's jitter (render px), renderMax = render size; out: r = input origin, Jp = jitter for the window
inline void PlaceAxis(double s, int o, int ow, int rw, double J, int renderMax, int& r, float& Jp) {
    const double k = (double)ow / rw, ic = rw * 0.5;
    auto jitterFor = [&](int rr) { return ic + 0.5 - ((rr + ic + 0.5 - J) / s - o) / k; };
    // of the whole-pixel origins around the ideal one, take the one needing the smallest jitter (<= ~0.5)
    const int r0 = (int)std::floor(o * s + J + 0.5);
    double best = 1e9; r = r0;
    for (int rr = r0 - 1; rr <= r0 + 1; ++rr) {
        if (rr < 0 || rr > renderMax - rw) continue;
        const double jp = jitterFor(rr);
        if (std::fabs(jp) < std::fabs(best)) { best = jp; r = rr; }
    }
    if (best > 1e8) { r = r0 < 0 ? 0 : (r0 > renderMax - rw ? renderMax - rw : r0); best = jitterFor(r); }   // at the image border
    Jp = (float)best;
}

}  // namespace fovea
