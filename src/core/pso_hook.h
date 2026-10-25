// pso_hook.h - times pipeline-state creation on the game's D3D12 device (hitch diagnostics).
// Patches three vtable slots: ID3D12Device::CreateGraphicsPipelineState (10), ::CreateComputePipelineState (11) and
// ID3D12Device2::CreatePipelineState (47, the stream form UE 5.5 uses when the driver has it). The vtable belongs to
// D3D12Core.dll, so every device in the process goes through the timers; calls are passed on unchanged.
// Header only: tests/psotest.cpp runs the same code on its own device.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>

namespace psohook {

// kind: 0 graphics, 1 compute, 2 stream (ID3D12Device2)
using Sink = void (*)(double ms, int kind, HRESULT hr);

constexpr int kSlotGraphics = 10, kSlotCompute = 11, kSlotStream = 47;

using GraphicsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using ComputeFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using StreamFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);

inline Sink g_sink = nullptr;
inline GraphicsFn g_graphics = nullptr;
inline ComputeFn g_compute = nullptr;
inline StreamFn g_stream = nullptr;
inline double g_qpcToMs = 0.0;

inline double NowMs() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart * g_qpcToMs; }

inline HRESULT STDMETHODCALLTYPE Graphics(ID3D12Device* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID iid, void** out) {
    const double t = NowMs();
    const HRESULT hr = g_graphics(d, desc, iid, out);
    if (g_sink) g_sink(NowMs() - t, 0, hr);
    return hr;
}
inline HRESULT STDMETHODCALLTYPE Compute(ID3D12Device* d, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** out) {
    const double t = NowMs();
    const HRESULT hr = g_compute(d, desc, iid, out);
    if (g_sink) g_sink(NowMs() - t, 1, hr);
    return hr;
}
inline HRESULT STDMETHODCALLTYPE Stream(ID3D12Device2* d, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID iid, void** out) {
    const double t = NowMs();
    const HRESULT hr = g_stream(d, desc, iid, out);
    if (g_sink) g_sink(NowMs() - t, 2, hr);
    return hr;
}

// once per process; returns how many of the three entries were hooked (the stream one needs ID3D12Device2)
inline int Install(ID3D12Device* dev, Sink sink) {
    if (!dev || g_graphics) return 0;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcToMs = 1000.0 / (double)f.QuadPart;
    g_sink = sink;
    void** vt = *(void***)dev;
    bool has2 = false;
    ID3D12Device2* d2 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device2), (void**)&d2)) && d2) {
        has2 = *(void***)d2 == vt;   // one object, one vtable (D3D12Core): else leave the stream entry alone
        d2->Release();
    }
    const int last = has2 ? kSlotStream : kSlotCompute;
    DWORD old = 0;
    if (!VirtualProtect(&vt[kSlotGraphics], (SIZE_T)(last - kSlotGraphics + 1) * sizeof(void*), PAGE_READWRITE, &old)) return 0;
    g_graphics = (GraphicsFn)vt[kSlotGraphics];
    g_compute = (ComputeFn)vt[kSlotCompute];
    InterlockedExchangePointer(&vt[kSlotGraphics], (void*)&Graphics);
    InterlockedExchangePointer(&vt[kSlotCompute], (void*)&Compute);
    if (has2) { g_stream = (StreamFn)vt[kSlotStream]; InterlockedExchangePointer(&vt[kSlotStream], (void*)&Stream); }
    DWORD tmp = 0;
    VirtualProtect(&vt[kSlotGraphics], (SIZE_T)(last - kSlotGraphics + 1) * sizeof(void*), old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return has2 ? 3 : 2;
}

} // namespace psohook
