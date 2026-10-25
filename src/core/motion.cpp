// motion.cpp - the comparison's motion outputs, from two bursts of consecutive frames taken with the view still
// (capture.cpp, BurstRequest): the game's TAA and the mod (DLSS / FSR). A still frame hides what you notice most in
// the headset: shimmer and crawling on foliage, wires and fences from one frame to the next. Written next to the
// comparison's PNGs (worker thread):
//   <base>_flicker.png        per pixel, how much the brightness changes from frame to frame (temporal standard
//                             deviation of luma), as a heat map, TAA left, mod right, with the averages
//   <base>_motion_TAA.png,    every frame stacked vertically (lossless), for <base>_motion.html, which loops them
//   <base>_motion_MOD.png     side by side at the real frame rate (or slower), with frame stepping and 2x zoom
//   <base>_motion.mp4         the same loops side by side with labels, H.264 (Media Foundation), for sharing
// With the headset still, anything that changes between frames is flicker / shimmer (plus wind on foliage and the
// small tracking jitter of a resting headset, the same for both sides).
#include "common.h"
#include <cmath>
#include <vector>
#include <algorithm>
#include <string>
#include <cstdarg>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

namespace {

struct FlickerStats { double mean = 0, p95 = 0, over2 = 0; };

// temporal standard deviation of luma per pixel (w*h floats)
static std::vector<float> TemporalStd(const BurstFrames& f) {
    const size_t px = (size_t)f.w * f.h;
    std::vector<float> sum(px, 0.f), sq(px, 0.f), out(px, 0.f);
    for (int k = 0; k < f.n; ++k) {
        const uint8_t* p = f.data + px * 3 * k;
        for (size_t i = 0; i < px; ++i, p += 3) {
            const float l = 0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2];
            sum[i] += l; sq[i] += l * l;
        }
    }
    for (size_t i = 0; i < px; ++i) {
        const float m = sum[i] / f.n;
        const float v = sq[i] / f.n - m * m;
        out[i] = v > 0 ? sqrtf(v) : 0.f;
    }
    return out;
}

static FlickerStats Stats(const std::vector<float>& s) {
    FlickerStats r;
    if (s.empty()) return r;
    double sum = 0; size_t over = 0;
    for (float v : s) { sum += v; if (v > 2.f) ++over; }
    r.mean = sum / s.size(); r.over2 = (double)over / s.size();
    std::vector<float> c = s;
    const size_t k = (size_t)(c.size() * 0.95);
    std::nth_element(c.begin(), c.begin() + k, c.end());
    r.p95 = c[k];
    return r;
}

// Frames whose average brightness is far from the rest of the burst are left out: not the scene as it is, but e.g. the
// near-black first frame seen in DLSS bursts (average 1.2 of 255 against 148-149 for the other 31, which alone made a
// "shimmer" of 26 and threw the alignment off). The rest is copied to keep.
static BurstFrames DropOdd(const BurstFrames& in, std::vector<uint8_t>& keep, const char* name, int& dropped) {
    const size_t fb = (size_t)in.w * in.h * 3;
    std::vector<double> mean(in.n);
    for (int k = 0; k < in.n; ++k) {
        const uint8_t* p = in.data + fb * k; double s = 0; size_t c = 0;
        for (size_t i = 0; i < fb; i += 3 * 7, ++c) s += 0.114 * p[i] + 0.587 * p[i + 1] + 0.299 * p[i + 2];
        mean[k] = c ? s / c : 0;
    }
    std::vector<double> sorted = mean;
    std::nth_element(sorted.begin(), sorted.begin() + in.n / 2, sorted.end());
    const double med = sorted[in.n / 2], tol = std::max(12.0, med * 0.15);
    std::vector<int> ok;
    char list[256] = ""; int len = 0;
    for (int k = 0; k < in.n; ++k) {
        if (fabs(mean[k] - med) <= tol) ok.push_back(k);
        else if (len < 200) len += snprintf(list + len, sizeof(list) - len, " %d (%.1f)", k, mean[k]);
    }
    dropped = in.n - (int)ok.size();
    if (dropped == 0) return in;
    Log("motion: %s burst: left out %d of %d frames whose brightness is far from the rest (median %.1f):%s",
        name, dropped, in.n, med, list);
    if ((int)ok.size() < 4) return in;   // not enough left: keep everything, the log line says why it looks wrong
    keep.resize(fb * ok.size());
    for (size_t j = 0; j < ok.size(); ++j) memcpy(&keep[fb * j], in.data + fb * ok[j], fb);
    BurstFrames out = in;
    out.data = keep.data(); out.n = (int)ok.size();
    return out;
}

// heat colour (BGR) for a flicker value: black -> purple -> red -> orange -> pale yellow at 6 levels (of 255)
static void Heat(float v, uint8_t* o) {
    static const float stops[5][3] = { { 0, 0, 0 }, { 60, 10, 100 }, { 190, 40, 80 }, { 250, 140, 20 }, { 255, 255, 180 } };   // RGB
    float t = std::min(1.f, std::max(0.f, v / 6.f)) * 4.f;
    const int i = std::min(3, (int)t); const float f = t - i;
    for (int c = 0; c < 3; ++c) {
        const float x = stops[i][c] + (stops[i + 1][c] - stops[i][c]) * f;
        o[2 - c] = (uint8_t)(x + 0.5f);   // RGB -> BGR
    }
}

// draws one or two lines of white text on a dark strip (stripH rows at the top of a BGR image of width w)
static void DrawStrip(uint8_t* bgr, int w, int stripH, int x0, int colW, const wchar_t* line1, const wchar_t* line2) {
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = colW; bi.bmiHeader.biHeight = -stripH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) { if (bmp) DeleteObject(bmp); if (dc) DeleteDC(dc); return; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    uint32_t* pix = (uint32_t*)bits;
    for (int i = 0; i < colW * stripH; ++i) pix[i] = 0x181818;
    const int bigPx = std::max(14, stripH * 2 / 5), smallPx = std::max(11, stripH / 3);
    // a font of at most px pixels that fits the text in the column (labels on narrow crops are shrunk, not cut off)
    auto fitFont = [&](const wchar_t* text, int px, int weight) {
        for (;; --px) {
            HFONT f = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            if (!text || px <= 8) return f;
            HGDIOBJ o = SelectObject(dc, f);
            SIZE sz{}; GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
            SelectObject(dc, o);
            if (sz.cx <= colW - 20) return f;
            DeleteObject(f);
        }
    };
    HFONT f1 = fitFont(line1, bigPx, FW_SEMIBOLD);
    HFONT f2 = fitFont(line2, smallPx, FW_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, f1);
    SetTextColor(dc, RGB(240, 240, 240));
    TextOutW(dc, 10, line2 ? 3 : (stripH - bigPx) / 2 - 1, line1, (int)wcslen(line1));
    if (line2) { SelectObject(dc, f2); SetTextColor(dc, RGB(245, 179, 53)); TextOutW(dc, 10, 5 + bigPx, line2, (int)wcslen(line2)); }
    GdiFlush();
    for (int y = 0; y < stripH; ++y)
        for (int x = 0; x < colW && x0 + x < w; ++x) {
            const uint32_t v = pix[y * colW + x];
            uint8_t* o = bgr + ((size_t)y * w + x0 + x) * 3;
            o[0] = (uint8_t)(v & 0xFF); o[1] = (uint8_t)((v >> 8) & 0xFF); o[2] = (uint8_t)((v >> 16) & 0xFF);
        }
    SelectObject(dc, oldFont); SelectObject(dc, oldBmp);
    DeleteObject(f1); DeleteObject(f2); DeleteObject(bmp); DeleteDC(dc);
}

// side-by-side BGR image: strip on top, left and right images below with a gap
struct Canvas {
    int w, h, strip, gap, cw, ch; std::vector<uint8_t> px;
    Canvas(int colW, int colH, int stripH, int gapW, bool two = true) : w(two ? colW * 2 + gapW : colW), h(colH + stripH), strip(stripH),
        gap(gapW), cw(colW), ch(colH), px((size_t)(two ? colW * 2 + gapW : colW) * (colH + stripH) * 3, 24) {}
    uint8_t* At(int x, int y) { return &px[((size_t)y * w + x) * 3]; }
    void Blit(int side, const uint8_t* src) {   // src: cw*ch BGR
        const int x0 = side ? cw + gap : 0;
        for (int y = 0; y < ch; ++y) memcpy(At(x0, strip + y), src + (size_t)y * cw * 3, (size_t)cw * 3);
    }
};

// H.264 MP4 via Media Foundation; frame(i, BGRA buffer) fills frame i (top-down)
template <class F>
static bool WriteMp4(const wchar_t* path, int cw, int ch, int fps, int count, F frame) {
    // the video is padded (dark, right and bottom) to multiples of 16: the encoder and the decoders work in 16x16 blocks
    // and some players show a skewed picture or a green edge on other sizes
    const int w = (cw + 15) & ~15, h = (ch + 15) & ~15;
    std::vector<BYTE> img((size_t)cw * ch * 4);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    bool ok = false;
    IMFAttributes* attr = nullptr; IMFSinkWriter* wr = nullptr; IMFMediaType* outT = nullptr; IMFMediaType* inT = nullptr;
    DWORD stream = 0;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(path, nullptr, attr, &wr);
    const UINT32 bitrate = (UINT32)std::min(60.0e6, std::max(10.0e6, (double)w * h * fps * 0.3));
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&outT);
    if (SUCCEEDED(hr)) hr = outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = outT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = outT->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (SUCCEEDED(hr)) hr = outT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = outT->SetUINT32(MF_MT_MPEG2_PROFILE, 100 /* eAVEncH264VProfile_High */);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(outT, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(outT, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(outT, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = wr->AddStream(outT, &stream);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&inT);
    if (SUCCEEDED(hr)) hr = inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = inT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = inT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = inT->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(w * 4));   // positive = top-down rows
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(inT, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(inT, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(inT, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = wr->SetInputMediaType(stream, inT, nullptr);
    if (SUCCEEDED(hr)) hr = wr->BeginWriting();
    const LONGLONG dur = 10000000LL / fps;
    for (int i = 0; SUCCEEDED(hr) && i < count; ++i) {
        IMFMediaBuffer* buf = nullptr; IMFSample* s = nullptr; BYTE* p = nullptr;
        hr = MFCreateMemoryBuffer((DWORD)(w * h * 4), &buf);
        if (SUCCEEDED(hr)) hr = buf->Lock(&p, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            frame(i, img.data());
            for (int y = 0; y < h; ++y) {
                uint32_t* row = (uint32_t*)(p + (size_t)y * w * 4);
                if (y < ch) memcpy(row, &img[(size_t)y * cw * 4], (size_t)cw * 4);
                for (int x = y < ch ? cw : 0; x < w; ++x) row[x] = 0xFF181818u;
            }
            buf->Unlock();
        }
        if (SUCCEEDED(hr)) hr = buf->SetCurrentLength((DWORD)(w * h * 4));
        if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
        if (SUCCEEDED(hr)) hr = s->AddBuffer(buf);
        if (SUCCEEDED(hr)) hr = s->SetSampleTime(dur * i);
        if (SUCCEEDED(hr)) hr = s->SetSampleDuration(dur);
        if (SUCCEEDED(hr)) hr = wr->WriteSample(stream, s);
        if (s) s->Release(); if (buf) buf->Release();
    }
    if (SUCCEEDED(hr)) hr = wr->Finalize();
    ok = SUCCEEDED(hr);
    if (!ok) Log("motion: MP4 writer failed (hr=0x%08lX)", (unsigned long)hr);
    if (inT) inT->Release(); if (outT) outT->Release(); if (wr) wr->Release(); if (attr) attr->Release();
    MFShutdown();
    return ok;
}

// ---- steadying. In the headset, timewarp moves each frame to where the head really is before it is shown, so a
// one-frame camera jump of the render (measured 0.5-0.9 px in DLSS bursts, following the engine's camera with
// correlation -0.99) is mostly cancelled; the raw frames show it in full, magnified, and it dominated the
// shimmer number. Each frame's overall shift against the burst's middle frame is measured (sum of squared luma
// differences over the central 60 %, whole pixels +-6, then a parabola through the neighbours for the fraction) and
// taken out with a Lanczos-3 resample; frames that moved less than 0.05 px are left as they are.
static std::vector<float> LumaF(const uint8_t* bgr, int w, int h) {
    std::vector<float> l((size_t)w * h);
    for (size_t i = 0; i < l.size(); ++i, bgr += 3) l[i] = 0.114f * bgr[0] + 0.587f * bgr[1] + 0.299f * bgr[2];
    return l;
}
// mean squared difference of b[y+oy][x+ox] against a[y][x] over the central region
static double SsdAt(const std::vector<float>& a, const std::vector<float>& b, int w, int h, int ox, int oy, int step) {
    const int x0 = w / 5, x1 = w - w / 5, y0 = h / 5, y1 = h - h / 5;
    double s = 0; size_t n = 0;
    for (int y = y0; y < y1; y += step) {
        const float* ra = &a[(size_t)y * w]; const float* rb = &b[(size_t)(y + oy) * w + ox];
        for (int x = x0; x < x1; x += step) { const float d = ra[x] - rb[x]; s += d * d; ++n; }
    }
    return n ? s / n : 1e30;
}
static float Parabola(double em, double e0, double ep) {   // offset of the minimum through three points at -1, 0, +1
    const double d = em - 2 * e0 + ep;
    return d > 1e-12 ? (float)std::max(-0.5, std::min(0.5, 0.5 * (em - ep) / d)) : 0.f;
}
static void FrameShift(const std::vector<float>& ref, const std::vector<float>& f, int w, int h, float& sx, float& sy) {
    const int r = std::min(6, std::min(w, h) / 5 - 1);
    double best = 1e30; int bx = 0, by = 0;
    for (int oy = -r; oy <= r; ++oy) for (int ox = -r; ox <= r; ++ox) {
        const double e = SsdAt(ref, f, w, h, ox, oy, 3);
        if (e < best) { best = e; bx = ox; by = oy; }
    }
    if (abs(bx) == r || abs(by) == r) { sx = (float)bx; sy = (float)by; return; }   // at the edge of the search: no fraction
    const double e0 = SsdAt(ref, f, w, h, bx, by, 1);
    sx = bx + Parabola(SsdAt(ref, f, w, h, bx - 1, by, 1), e0, SsdAt(ref, f, w, h, bx + 1, by, 1));
    sy = by + Parabola(SsdAt(ref, f, w, h, bx, by - 1, 1), e0, SsdAt(ref, f, w, h, bx, by + 1, 1));
}
static void Lanczos3(float t, float* wgt) {   // 6 weights for the taps at floor - 2 .. floor + 3, t = fraction
    float s = 0;
    for (int i = 0; i < 6; ++i) {
        const double x = (i - 2) - t;
        double v = 1.0;
        if (fabs(x) > 1e-6) { const double px = 3.14159265358979 * x; v = fabs(x) < 3 ? 3.0 * sin(px) * sin(px / 3.0) / (px * px) : 0.0; }
        wgt[i] = (float)v; s += (float)v;
    }
    for (int i = 0; i < 6; ++i) wgt[i] /= s;
}
// out[y][x] = in[y + sy][x + sx] (BGR, edges clamped)
static void ShiftFrame(const uint8_t* in, uint8_t* out, int w, int h, float sx, float sy) {
    const int ix = (int)floor(sx), iy = (int)floor(sy);
    float wx[6], wy[6]; Lanczos3(sx - ix, wx); Lanczos3(sy - iy, wy);
    std::vector<float> tmp((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                float v = 0;
                for (int i = 0; i < 6; ++i) { const int xx = std::min(w - 1, std::max(0, x + ix + i - 2)); v += wx[i] * in[((size_t)y * w + xx) * 3 + c]; }
                tmp[((size_t)y * w + x) * 3 + c] = v;
            }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                float v = 0;
                for (int i = 0; i < 6; ++i) { const int yy = std::min(h - 1, std::max(0, y + iy + i - 2)); v += wy[i] * tmp[((size_t)yy * w + x) * 3 + c]; }
                out[((size_t)y * w + x) * 3 + c] = (uint8_t)std::min(255.f, std::max(0.f, v + 0.5f));
            }
}
static BurstFrames Steady(const BurstFrames& in, std::vector<uint8_t>& keep, const char* name, double& rms) {
    const size_t fb = (size_t)in.w * in.h * 3;
    const int refK = in.n / 2;
    const std::vector<float> ref = LumaF(in.data + fb * refK, in.w, in.h);
    std::vector<float> sx(in.n, 0.f), sy(in.n, 0.f);
    for (int k = 0; k < in.n; ++k) if (k != refK) FrameShift(ref, LumaF(in.data + fb * k, in.w, in.h), in.w, in.h, sx[k], sy[k]);
    // the shift from one frame to the next is the camera jitter; the burst may also drift slowly as a whole, which
    // is taken out too (the comparison is about the image, not the head)
    double s2 = 0, mx = 0; int mk = 0;
    for (int k = 1; k < in.n; ++k) {
        const double d = hypot(sx[k] - sx[k - 1], sy[k] - sy[k - 1]);
        s2 += d * d; if (d > mx) { mx = d; mk = k; }
    }
    rms = in.n > 1 ? sqrt(s2 / (in.n - 1)) : 0.0;
    keep.resize(fb * in.n);
    int moved = 0;
    for (int k = 0; k < in.n; ++k) {
        if (fabs(sx[k]) < 0.05f && fabs(sy[k]) < 0.05f) memcpy(&keep[fb * k], in.data + fb * k, fb);
        else { ShiftFrame(in.data + fb * k, &keep[fb * k], in.w, in.h, sx[k], sy[k]); ++moved; }
    }
    char list[64 * 14] = ""; int len = 0;
    for (int k = 0; k < in.n && len < (int)sizeof(list) - 16; ++k) len += snprintf(list + len, sizeof(list) - len, " %+.2f,%+.2f", sx[k], sy[k]);
    Log("motion: %s burst steadied (like the headset's timewarp): frame-to-frame camera shift rms %.2f px, largest %.2f px "
        "(frame %d), %d of %d frames moved back; shift per frame vs frame %d:%s", name, rms, mx, mk, moved, in.n, refK, list);
    BurstFrames out = in;
    out.data = keep.data();
    return out;
}

// head movement between the two bursts: (dx, dy) with MOD[y+dy][x+dx] ~ TAA[y][x], on the middle frame of each
// (an odd first frame can throw it off), middle region
static void AlignBursts(const BurstFrames& a, const BurstFrames& b, int& dx, int& dy) {
    const int w = a.w, h = a.h;
    const uint8_t* fa = a.data + (size_t)w * h * 3 * (a.n / 2);
    const uint8_t* fbm = b.data + (size_t)w * h * 3 * (b.n / 2);
    std::vector<uint8_t> ga((size_t)w * h), gb((size_t)w * h);
    for (size_t i = 0; i < ga.size(); ++i) {
        const uint8_t* p = fa + i * 3; const uint8_t* q = fbm + i * 3;
        ga[i] = (uint8_t)((p[0] * 29 + p[1] * 150 + p[2] * 77) >> 8); gb[i] = (uint8_t)((q[0] * 29 + q[1] * 150 + q[2] * 77) >> 8);
    }
    // coarse to fine: every offset on 1/4-size box-filtered copies, then +-3 px at full size around the best ones.
    // A plain grid search with offsets 4 px apart is not enough: with fine detail an offset 2 px off looks as bad as a
    // random one (the offline benchmark test got (8, -4) at mean difference 45 instead of the true (5, -3) at 1.7).
    // mean absolute difference with each region's mean brightness taken out: the two sides may differ in brightness
    // (exposure, or the motion test's tint), and a plain difference then comes out about the same at every offset
    auto sadOn = [](const std::vector<uint8_t>& A, const std::vector<uint8_t>& Bv, int W2, int H2, int ox, int oy, int step) -> double {
        const int x0 = W2 / 5, x1 = W2 * 4 / 5, y0 = H2 / 5, y1 = H2 * 4 / 5;
        if (x0 + ox < 0 || y0 + oy < 0 || x1 + ox > W2 || y1 + oy > H2) return 1e9;
        int64_t sa = 0, sb = 0, n = 0;
        for (int y = y0; y < y1; y += step) for (int x = x0; x < x1; x += step) { sa += A[(size_t)y * W2 + x]; sb += Bv[(size_t)(y + oy) * W2 + x + ox]; ++n; }
        if (!n) return 1e9;
        const int off = (int)((sb - sa) / n);
        uint64_t s = 0;
        for (int y = y0; y < y1; y += step) for (int x = x0; x < x1; x += step) s += (uint32_t)abs(A[(size_t)y * W2 + x] + off - Bv[(size_t)(y + oy) * W2 + x + ox]);
        return (double)s / n;
    };
    const int w4 = w / 4, h4 = h / 4;
    std::vector<uint8_t> qa((size_t)w4 * h4), qb((size_t)w4 * h4);
    for (int y = 0; y < h4; ++y) for (int x = 0; x < w4; ++x) {
        uint32_t sa = 0, sb = 0;
        for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) { const size_t o = (size_t)(y * 4 + j) * w + x * 4 + i; sa += ga[o]; sb += gb[o]; }
        qa[(size_t)y * w4 + x] = (uint8_t)(sa / 16); qb[(size_t)y * w4 + x] = (uint8_t)(sb / 16);
    }
    // the 6 best 1/4-size offsets (repeating patterns can make a wrong one look best at that size: the motion test's
    // synthetic lines repeat under a shift of (11, -24)); each is refined at full size and the best full-size match wins
    struct Cand { double e; int x, y; };
    std::vector<Cand> cands;
    const int r4 = std::max(1, std::min(64, std::min(w, h) / 6) / 4);
    for (int oy = -r4; oy <= r4; ++oy) for (int ox = -r4; ox <= r4; ++ox) cands.push_back({ sadOn(qa, qb, w4, h4, ox, oy, 1), ox, oy });
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.e < b.e; });
    std::vector<Cand> top;
    for (const Cand& c : cands) {
        bool dup = false;   // (not "near": a windows.h macro)
        for (const Cand& t : top) if (abs(t.x - c.x) <= 1 && abs(t.y - c.y) <= 1) dup = true;
        if (!dup) top.push_back(c);
        if (top.size() >= 6) break;
    }
    // two more full-size starting points: no shift, and a grid search (full size sampled every 6 px, offsets 4 px apart,
    // centred on 0), which finds the motion test's (7, -5) where the 1/4-size pass alone does not
    std::vector<std::pair<int, int>> starts;
    for (const Cand& t : top) starts.push_back({ t.x * 4, t.y * 4 });
    starts.push_back({ 0, 0 });
    {
        const int r = (std::min(64, std::min(w, h) / 6) / 4) * 4;
        double gbst = 1e18; int gx = 0, gy = 0;
        for (int oy = -r; oy <= r; oy += 4) for (int ox = -r; ox <= r; ox += 4) {
            const double e = sadOn(ga, gb, w, h, ox, oy, 6);
            if (e < gbst) { gbst = e; gx = ox; gy = oy; }
        }
        starts.push_back({ gx, gy });
    }
    double best = 1e18; int bx = 0, by = 0;
    for (const auto& st : starts)
        for (int oy = st.second - 3; oy <= st.second + 3; ++oy) for (int ox = st.first - 3; ox <= st.first + 3; ++ox) {
            const double e = sadOn(ga, gb, w, h, ox, oy, 2);
            if (e < best) { best = e; bx = ox; by = oy; }
        }
    double fb = 1e18; int fx = bx, fy = by;   // every pixel at the end, +-1 around the best
    for (int oy = by - 1; oy <= by + 1; ++oy) for (int ox = bx - 1; ox <= bx + 1; ++ox) {
        const double e = sadOn(ga, gb, w, h, ox, oy, 1);
        if (e < fb) { fb = e; fx = ox; fy = oy; }
    }
    dx = fx; dy = fy;
}

// wipe frame: mod to the left of the line, TAA to the right; an orange line; labels in a strip on top
static void WipeFrame(Canvas& c, const uint8_t* left, const uint8_t* right, int cw, int ch, int lineX) {
    for (int y = 0; y < ch; ++y) {
        uint8_t* o = c.At(0, c.strip + y);
        const size_t row = (size_t)y * cw * 3;
        if (lineX > 0) memcpy(o, left + row, (size_t)std::min(lineX, cw) * 3);
        if (lineX < cw) memcpy(o + (size_t)std::max(lineX, 0) * 3, right + row + (size_t)std::max(lineX, 0) * 3, (size_t)(cw - std::max(lineX, 0)) * 3);
        for (int x = lineX - 1; x <= lineX + 1; ++x) if (x >= 0 && x < cw) { uint8_t* p = o + (size_t)x * 3; p[0] = 53; p[1] = 179; p[2] = 245; }
    }
}

static void Utf8(const wchar_t* w, char* out, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, nullptr, nullptr); }
static const wchar_t* FileName(const wchar_t* p) { const wchar_t* s = wcsrchr(p, L'\\'); return s ? s + 1 : p; }

}   // namespace

MotionResult WriteMotionOutputs(const wchar_t* base, const BurstFrames& taaIn, const BurstFrames& modIn,
                                const char* taaTag, const char* modTag) {
    MotionResult r;
    if (!taaIn.data || !modIn.data || taaIn.w != modIn.w || taaIn.h != modIn.h || taaIn.n < 2 || modIn.n < 2) {
        Log("motion: bursts missing or of different size, no motion outputs");
        return r;
    }
    std::vector<uint8_t> keepTaa, keepMod, steadyTaa, steadyMod;
    BurstFrames taa = DropOdd(taaIn, keepTaa, "TAA", r.dropTaa), mod = DropOdd(modIn, keepMod, "mod", r.dropMod);
    if (g_cfg.shotSteady) {
        taa = Steady(taa, steadyTaa, "TAA", r.shiftTaa);
        mod = Steady(mod, steadyMod, "mod", r.shiftMod);
    }
    const int w = taa.w, h = taa.h, n = std::min(taa.n, mod.n);
    wchar_t taaTagW[64], modTagW[64];
    MultiByteToWideChar(CP_UTF8, 0, taaTag, -1, taaTagW, 64); MultiByteToWideChar(CP_UTF8, 0, modTag, -1, modTagW, 64);

    // 1. flicker map
    const std::vector<float> sa = TemporalStd(taa), sb = TemporalStd(mod);
    const FlickerStats fa = Stats(sa), fb = Stats(sb);
    r.flickerTaa = fa.mean; r.flickerMod = fb.mean;
    const double change = fa.mean > 0 ? (fb.mean / fa.mean - 1.0) * 100.0 : 0.0;
    {
        Canvas c(w, h, 64, 12);
        std::vector<uint8_t> heat((size_t)w * h * 3);
        for (size_t i = 0; i < sa.size(); ++i) Heat(sa[i], &heat[i * 3]);
        c.Blit(0, heat.data());
        for (size_t i = 0; i < sb.size(); ++i) Heat(sb[i], &heat[i * 3]);
        c.Blit(1, heat.data());
        wchar_t l1[160], l2[200], r1[160], r2[200];
        swprintf_s(l1, L"Game: %s  -  shimmer map", taaTagW);
        swprintf_s(l2, L"average %.2f / frame, %.1f %% of pixels above 2  (bright = flickers)", fa.mean, fa.over2 * 100.0);
        swprintf_s(r1, L"Mod: %s  -  shimmer map", modTagW);
        swprintf_s(r2, L"average %.2f / frame (%+.0f %%), %.1f %% of pixels above 2", fb.mean, change, fb.over2 * 100.0);
        DrawStrip(c.px.data(), c.w, c.strip, 0, w, l1, l2);
        DrawStrip(c.px.data(), c.w, c.strip, w + c.gap, w, r1, r2);
        wchar_t path[MAX_PATH]; swprintf_s(path, L"%s_flicker.png", base);
        r.map = WritePngBgr(path, c.px.data(), c.w, c.h);
    }
    Log("motion: flicker TAA mean %.2f p95 %.2f >2: %.1f %% | mod mean %.2f p95 %.2f >2: %.1f %% (%+.0f %%)",
        fa.mean, fa.p95, fa.over2 * 100.0, fb.mean, fb.p95, fb.over2 * 100.0, change);

    // 2. lossless frame stacks + page
    wchar_t stackA[MAX_PATH], stackB[MAX_PATH], video[MAX_PATH], page[MAX_PATH], flick[MAX_PATH];
    swprintf_s(stackA, L"%s_motion_TAA.png", base); swprintf_s(stackB, L"%s_motion_MOD.png", base);
    swprintf_s(video, L"%s_motion.mp4", base); swprintf_s(page, L"%s_motion.html", base); swprintf_s(flick, L"%s_flicker.png", base);
    const bool stacks = WritePngBgr(stackA, taa.data, w, h * n) && WritePngBgr(stackB, mod.data, w, h * n);

    // 3. MP4: both loops side by side, labelled, repeated to at least 4 s
    const double fpsReal = std::min(120.0, std::max(24.0, (taa.fps + mod.fps) / 2.0));
    const int fps = (int)std::lround(fpsReal);
    {
        Canvas c(w, h, 40, 12);
        wchar_t l1[160], r1[160];
        swprintf_s(l1, L"Game: %s", taaTagW); swprintf_s(r1, L"Mod: %s", modTagW);
        DrawStrip(c.px.data(), c.w, c.strip, 0, w, l1, nullptr);
        DrawStrip(c.px.data(), c.w, c.strip, w + c.gap, w, r1, nullptr);
        const int loops = std::max(1, (int)std::ceil(4.0 * fps / n));
        const size_t fb3 = (size_t)w * h * 3;
        r.video = WriteMp4(video, c.w, c.h, fps, n * loops, [&](int i, BYTE* dst) {
            const int k = i % n;
            c.Blit(0, taa.data + fb3 * k); c.Blit(1, mod.data + fb3 * k);
            const uint8_t* s = c.px.data();
            for (size_t p = 0; p < (size_t)c.w * c.h; ++p, s += 3, dst += 4) { dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2]; dst[3] = 255; }
        });
    }

    // 4. wipes: the two loops (and the two heat maps) overlaid and lined up, an orange line sweeping left to right:
    //    the mod to the left of the line, the game's TAA to the right. 0.5 s TAA, 3 s sweep, 1 s mod
    int adx = 0, ady = 0;
    AlignBursts(taa, mod, adx, ady);
    const int ox0 = std::max(0, -adx), oy0 = std::max(0, -ady);
    const int ow = (w - abs(adx)) & ~1, oh = (h - abs(ady)) & ~1;
    Log("motion: bursts lined up, mod shifted by (%d, %d) px, overlap %dx%d", adx, ady, ow, oh);
    r.dx = adx; r.dy = ady; r.ow = ow; r.oh = oh;
    wchar_t heatA[MAX_PATH], heatB[MAX_PATH], wipeV[MAX_PATH], wipeH[MAX_PATH];
    swprintf_s(heatA, L"%s_flicker_TAA.png", base); swprintf_s(heatB, L"%s_flicker_MOD.png", base);
    swprintf_s(wipeV, L"%s_wipe.mp4", base); swprintf_s(wipeH, L"%s_flicker_wipe.mp4", base);
    bool heats = false, wipe1 = false, wipe2 = false;
    if (ow >= 64 && oh >= 64) {
        auto crop = [&](const uint8_t* src, int x0, int y0, std::vector<uint8_t>& out) {   // ow x oh from a w x h BGR image
            out.resize((size_t)ow * oh * 3);
            for (int y = 0; y < oh; ++y) memcpy(&out[(size_t)y * ow * 3], src + ((size_t)(y0 + y) * w + x0) * 3, (size_t)ow * 3);
        };
        std::vector<uint8_t> hA((size_t)w * h * 3), hB((size_t)w * h * 3);
        for (size_t i = 0; i < sa.size(); ++i) Heat(sa[i], &hA[i * 3]);
        for (size_t i = 0; i < sb.size(); ++i) Heat(sb[i], &hB[i * 3]);
        heats = WritePngBgr(heatA, hA.data(), w, h) && WritePngBgr(heatB, hB.data(), w, h);
        std::vector<uint8_t> cA, cB;
        crop(hA.data(), ox0, oy0, cA); crop(hB.data(), ox0 + adx, oy0 + ady, cB);
        const int total = (int)std::lround(fps * 4.5), hold0 = (int)std::lround(fps * 0.5), sweep = (int)std::lround(fps * 3.0);
        auto lineAt = [&](int i) { return i < hold0 ? 0 : i >= hold0 + sweep ? ow : (int)((double)(i - hold0) / sweep * ow); };
        wchar_t lab[200];
        {   // heat-map wipe
            Canvas c(ow, oh, 40, 0, false);
            swprintf_s(lab, L"Shimmer map:  left of the line = Mod (%s, %.2f),  right = Game (%s, %.2f)", modTagW, fb.mean, taaTagW, fa.mean);
            DrawStrip(c.px.data(), c.w, c.strip, 0, ow, lab, nullptr);
            wipe2 = WriteMp4(wipeH, c.w, c.h, fps, total, [&](int i, BYTE* dst) {
                WipeFrame(c, cB.data(), cA.data(), ow, oh, lineAt(i));
                const uint8_t* s = c.px.data();
                for (size_t p = 0; p < (size_t)c.w * c.h; ++p, s += 3, dst += 4) { dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2]; dst[3] = 255; }
            });
        }
        {   // footage wipe (the loops keep playing while the line moves)
            const size_t fb3 = (size_t)w * h * 3;
            std::vector<std::vector<uint8_t>> fA(n), fB(n);
            for (int k = 0; k < n; ++k) { crop(taa.data + fb3 * k, ox0, oy0, fA[k]); crop(mod.data + fb3 * k, ox0 + adx, oy0 + ady, fB[k]); }
            Canvas c(ow, oh, 40, 0, false);
            swprintf_s(lab, L"Left of the line: Mod (%s)      Right of the line: Game (%s)", modTagW, taaTagW);
            DrawStrip(c.px.data(), c.w, c.strip, 0, ow, lab, nullptr);
            wipe1 = WriteMp4(wipeV, c.w, c.h, fps, total, [&](int i, BYTE* dst) {
                WipeFrame(c, fB[i % n].data(), fA[i % n].data(), ow, oh, lineAt(i));
                const uint8_t* s = c.px.data();
                for (size_t p = 0; p < (size_t)c.w * c.h; ++p, s += 3, dst += 4) { dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2]; dst[3] = 255; }
            });
        }
    }
    r.video = r.video && wipe1 && wipe2;

    // 5. page: lossless loops (frames stacked in one PNG per side, stepped with CSS), the wipes, flicker map, videos
    if (stacks) {
        char a8[MAX_PATH * 3], b8[MAX_PATH * 3], v8[MAX_PATH * 3], f8[MAX_PATH * 3], t8[MAX_PATH * 3];
        char ha8[MAX_PATH * 3], hb8[MAX_PATH * 3], wv8[MAX_PATH * 3], wh8[MAX_PATH * 3];
        Utf8(FileName(stackA), a8, sizeof(a8)); Utf8(FileName(stackB), b8, sizeof(b8)); Utf8(FileName(video), v8, sizeof(v8));
        Utf8(FileName(flick), f8, sizeof(f8)); Utf8(FileName(base), t8, sizeof(t8));
        Utf8(FileName(heatA), ha8, sizeof(ha8)); Utf8(FileName(heatB), hb8, sizeof(hb8));
        Utf8(FileName(wipeV), wv8, sizeof(wv8)); Utf8(FileName(wipeH), wh8, sizeof(wh8));
        std::string s;
        char buf[4096];
        auto add = [&](const char* fmt, ...) {
            va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap); s += buf;
        };
        add("<!doctype html><html><head><meta charset=\"utf-8\"><title>%s motion</title><style>\n", t8);
        s += "html,body{margin:0;background:#111;color:#ddd;font:14px system-ui,sans-serif}\n"
             ".wrap{padding:12px 16px;display:flex;flex-direction:column;gap:12px}\n"
             ".row{display:flex;gap:12px;flex-wrap:wrap}\n"
             ".panel{display:flex;flex-direction:column;gap:6px}\n"
             ".lab{font-weight:600}.lab span{color:#f5b335;font-weight:400;margin-left:8px}\n"
             "h2{font-size:16px;margin:10px 0 0}\n"
             ".anim{background-repeat:no-repeat;border:1px solid #333;position:relative;overflow:hidden}\n"
             ".wipe{position:relative;overflow:hidden;border:1px solid #333;cursor:ew-resize}\n"
             ".wipe .layer{position:absolute;inset:0;background-repeat:no-repeat}\n"
             ".wipe .line{position:absolute;top:0;bottom:0;width:3px;margin-left:-1px;background:#f5b335;pointer-events:none}\n"
             ".wipe .tag{position:absolute;top:8px;padding:3px 8px;background:rgba(0,0,0,.65);border-radius:4px;font-weight:600;pointer-events:none}\n"
             ".bar{display:flex;gap:8px;align-items:center;flex-wrap:wrap}\n"
             "button{background:#2a2a2a;color:#eee;border:1px solid #444;border-radius:4px;padding:5px 12px;cursor:pointer;font:inherit}\n"
             "button.on{background:#f5b335;color:#111;border-color:#f5b335}\n"
             "img.map{max-width:100%;border:1px solid #333}\n"
             "p{margin:0;max-width:110ch;color:#aaa}a{color:#f5b335}\n"
             "</style></head><body><div class=\"wrap\">\n";
        add("<p>The same spot with the view held still: %d consecutive frames at %.0f fps, looped. What moves here is "
            "shimmer and flicker (and wind). Space: pause &middot; Left/Right: step one frame &middot; Z: zoom 2x</p>\n", n, fpsReal);
        s += "<div class=\"bar\"><button id=\"pp\">Pause</button><button data-s=\"1\" class=\"on\">Real speed</button>"
             "<button data-s=\"0.5\">1/2</button><button data-s=\"0.25\">1/4</button><button data-s=\"0.1\">1/10</button>"
             "<button id=\"zm\">Zoom 2x</button><span id=\"fr\"></span></div>\n";
        add("<h2>Side by side</h2><div class=\"row\">\n"
            "<div class=\"panel\"><div class=\"lab\">Game: %s<span>average change %.2f / frame</span></div><div class=\"anim\" id=\"a\"></div></div>\n"
            "<div class=\"panel\"><div class=\"lab\">Mod: %s<span>average change %.2f / frame (%+.0f %%)</span></div><div class=\"anim\" id=\"b\"></div></div>\n"
            "</div>\n", taaTag, fa.mean, modTag, fb.mean, change);
        add("<h2>Wipe: the line reveals the mod (move the mouse over it to hold the line)</h2>\n"
            "<div class=\"wipe\" id=\"wv\"><div class=\"layer\" id=\"wva\"></div><div class=\"layer\" id=\"wvb\"></div><div class=\"line\"></div>"
            "<div class=\"tag\" style=\"left:8px\">Mod: %s</div><div class=\"tag\" style=\"right:8px\">Game: %s</div></div>\n", modTag, taaTag);
        add("<h2>Shimmer map wipe<span style=\"color:#aaa;font-weight:400;font-size:14px\"> &nbsp;bright = the pixel changes from frame to frame</span></h2>\n"
            "<div class=\"wipe\" id=\"wh\"><div class=\"layer\" id=\"wha\"></div><div class=\"layer\" id=\"whb\"></div><div class=\"line\"></div>"
            "<div class=\"tag\" style=\"left:8px\">Mod: %.2f</div><div class=\"tag\" style=\"right:8px\">Game: %.2f</div></div>\n", fb.mean, fa.mean);
        add("<h2>Shimmer maps side by side</h2><img class=\"map\" src=\"%s\">\n", f8);
        add("<p>Videos for sharing: <a href=\"%s\">%s</a> (side by side) &middot; <a href=\"%s\">%s</a> (wipe) &middot; "
            "<a href=\"%s\">%s</a> (shimmer map wipe)</p>\n", v8, v8, wv8, wv8, wh8, wh8);
        add("</div><script>\nvar W=%d,H=%d,N=%d,FPS=%.3f,A='%s',B='%s',HA='%s',HB='%s',OX=%d,OY=%d,DX=%d,DY=%d,OW=%d,OH=%d;\n",
            w, h, n, fpsReal, a8, b8, ha8, hb8, ox0, oy0, adx, ady, ow, oh);
        s += "var k=0,speed=1,zoom=1,run=true,last=0,acc=0,wt=0;\n"
             "function $(i){return document.getElementById(i);}\n"
             "var sbs=[$('a'),$('b')],fr=$('fr');\n"
             "var wipes=[{el:$('wv'),a:$('wva'),b:$('wvb'),ia:A,ib:B,anim:true,hold:-1},{el:$('wh'),a:$('wha'),b:$('whb'),ia:HA,ib:HB,anim:false,hold:-1}];\n"
             "function sc(cols,ww){var avail=(document.body.clientWidth-60)/cols;return zoom>1?zoom:Math.min(1,avail/ww);}\n"
             "function layout(){var s=sc(2,W);\n"
             " sbs.forEach(function(e,i){e.style.width=(W*s)+'px';e.style.height=(H*s)+'px';e.style.backgroundImage='url('+(i?B:A)+')';\n"
             "  e.style.backgroundSize=(W*s)+'px '+(H*(N)*s)+'px';e.style.imageRendering=zoom>1?'pixelated':'auto';e.dataset.s=s;});\n"
             " var t=sc(1,OW);wipes.forEach(function(wp){wp.s=t;wp.el.style.width=(OW*t)+'px';wp.el.style.height=(OH*t)+'px';\n"
             "  [wp.a,wp.b].forEach(function(l,i){l.style.backgroundImage='url('+(i?wp.ib:wp.ia)+')';\n"
             "   l.style.backgroundSize=(W*t)+'px '+(H*(wp.anim?N:1)*t)+'px';l.style.imageRendering=zoom>1?'pixelated':'auto';});});draw();}\n"
             "function draw(){sbs.forEach(function(e){var s=+e.dataset.s;e.style.backgroundPosition='0 '+(-k*H*s)+'px';});\n"
             " var cyc=4.5,p=wt%cyc,pos=p<0.5?0:p<3.5?(p-0.5)/3:1;\n"
             " wipes.forEach(function(wp){var t=wp.s,kk=wp.anim?k:0,q=wp.hold>=0?wp.hold:pos;\n"
             "  wp.a.style.backgroundPosition=(-OX*t)+'px '+(-(OY+kk*H)*t)+'px';\n"
             "  wp.b.style.backgroundPosition=(-(OX+DX)*t)+'px '+(-(OY+DY+kk*H)*t)+'px';\n"
             "  wp.b.style.clipPath='inset(0 '+((1-q)*100)+'% 0 0)';wp.el.querySelector('.line').style.left=(q*100)+'%';});\n"
             " fr.textContent='frame '+(k+1)+' / '+N;}\n"
             "function tick(t){if(last&&run){var dt=(t-last);acc+=dt*speed;wt+=dt/1000;var d=1000/FPS;while(acc>=d){acc-=d;k=(k+1)%N;}draw();}last=t;requestAnimationFrame(tick);}\n"
             "wipes.forEach(function(wp){wp.el.onmousemove=function(e){var r=wp.el.getBoundingClientRect();wp.hold=Math.max(0,Math.min(1,(e.clientX-r.left)/r.width));draw();};\n"
             " wp.el.onmouseleave=function(){wp.hold=-1;};});\n"
             "document.querySelectorAll('button[data-s]').forEach(function(b){b.onclick=function(){speed=+b.dataset.s;\n"
             " document.querySelectorAll('button[data-s]').forEach(function(x){x.classList.toggle('on',x===b);});};});\n"
             "var pp=$('pp'),zm=$('zm');\n"
             "pp.onclick=function(){run=!run;pp.textContent=run?'Pause':'Play';};\n"
             "zm.onclick=function(){zoom=zoom>1?1:2;zm.classList.toggle('on',zoom>1);layout();};\n"
             "window.onkeydown=function(e){if(e.key==' '){e.preventDefault();pp.onclick();}\n"
             " else if(e.key=='ArrowRight'){run=false;pp.textContent='Play';k=(k+1)%N;draw();}\n"
             " else if(e.key=='ArrowLeft'){run=false;pp.textContent='Play';k=(k+N-1)%N;draw();}\n"
             " else if(e.key=='z'||e.key=='Z'){zm.onclick();}};\n"
             "window.onresize=layout;layout();requestAnimationFrame(tick);\n"
             "</script></body></html>\n";
        FILE* f = _wfopen(page, L"wb");
        if (f) { fwrite(s.data(), 1, s.size(), f); fclose(f); r.page = true; }
        r.page = r.page && heats;
    }
    char p8[MAX_PATH * 3]; Utf8(page, p8, sizeof(p8));
    Log("motion: %d frames %dx%d at %.1f fps -> map %s, page %s (%s), video %s", n, w, h, fpsReal, r.map ? "ok" : "FAILED",
        r.page ? "ok" : "FAILED", p8, r.video ? "ok" : "FAILED");
    return r;
}

// ---- benchmark (bench.cpp): one checkpoint's burst. Same treatment as the comparison's bursts (odd frames left out,
// steadied like the headset's timewarp), then: the frames stacked vertically in one lossless PNG (the page loops them),
// the shimmer heat map (temporal standard deviation of luma per pixel), its averages, and the shift against the
// benchmark's first burst (the reference, normally the game's TAA), so the page can line any two checkpoints up.
bool BenchProcessBurst(const BurstFrames& in, const wchar_t* stackPath, const wchar_t* heatPath,
                       std::vector<uint8_t>& refMid, int& refW, int& refH, BenchBurstOut& out) {
    out = BenchBurstOut{};
    if (!in.data || in.n < 2 || in.w < 64 || in.h < 64) { Log("bench: burst missing or too small"); return false; }
    std::vector<uint8_t> keep, steady;
    BurstFrames b = DropOdd(in, keep, "bench", out.dropped);
    if (g_cfg.shotSteady) b = Steady(b, steady, "bench", out.shiftRms);
    const int w = b.w, h = b.h;
    const std::vector<float> sd = TemporalStd(b);
    const FlickerStats st = Stats(sd);
    out.flicker = st.mean; out.p95 = st.p95; out.over2 = st.over2;
    out.n = b.n; out.w = w; out.h = h; out.fps = in.fps;
    std::vector<uint8_t> heat((size_t)w * h * 3);
    for (size_t i = 0; i < sd.size(); ++i) Heat(sd[i], &heat[i * 3]);
    out.heat = WritePngBgr(heatPath, heat.data(), w, h);
    out.stack = WritePngBgr(stackPath, b.data, w, h * b.n);
    const size_t fb = (size_t)w * h * 3;
    if (refMid.empty()) {   // the first burst is the reference
        refMid.assign(b.data + fb * (b.n / 2), b.data + fb * (b.n / 2 + 1));
        refW = w; refH = h; out.dx = out.dy = 0;
    } else if (refW == w && refH == h) {
        BurstFrames ref; ref.data = refMid.data(); ref.n = 1; ref.w = w; ref.h = h;
        AlignBursts(ref, b, out.dx, out.dy);   // b[y + dy][x + dx] ~ ref[y][x]
    }
    Log("bench: burst %d frames %dx%d (%d left out, steadied %.2f px rms): shimmer mean %.2f p95 %.2f >2 %.1f %%, shift vs reference (%d, %d), stack %s, map %s",
        out.n, w, h, out.dropped, out.shiftRms, out.flicker, out.p95, out.over2 * 100.0, out.dx, out.dy,
        out.stack ? "ok" : "FAILED", out.heat ? "ok" : "FAILED");
    return out.stack && out.heat;
}
