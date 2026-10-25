// Entry points. DllMain does nothing heavy (loader lock); the work runs in ITR2DLSS_Start,
// called once by the version.dll proxy on its own thread.
#include "common.h"

static HMODULE g_self = nullptr;

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void ITR2DLSS_Start() {
    static LONG started = 0;
    if (InterlockedExchange(&started, 1)) return;

    GetModuleFileNameW(g_self, g_dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(g_dir, L'\\')) slash[1] = 0;
    // the log and the performance CSVs go to their own folder; if it cannot be created (permissions),
    // they go next to the DLL
    swprintf_s(g_logDir, L"%sitr2_dlss_log\\", g_dir);
    if (!CreateDirectoryW(g_logDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) wcscpy_s(g_logDir, g_dir);
    wchar_t logPath[MAX_PATH]; wcscpy_s(logPath, g_logDir); wcscat_s(logPath, L"itr2_dlss.log");
    LogInit(logPath);
    LoadConfig();
#if defined(ITR2_AMDTEST)
    Log("itr2_dlss %s %s AMD test build 4 (release + F1 FSR exposure, F2 FSR version, F4 output cleanup, M mask, R history reset, "
        "G game-format output, C screenshot, N input cleanup, A automatic reset, image probe) - Mode=%d", __DATE__, __TIME__, g_cfg.mode);
#elif defined(ITR2_RELEASE)
    Log("itr2_dlss %s %s release build - Mode=%d", __DATE__, __TIME__, g_cfg.mode);
#else
    Log("itr2_dlss %s %s debug build - Mode=%d", __DATE__, __TIME__, g_cfg.mode);
#endif
    if (g_cfg.mode <= 0) { Log("Mode=0: disabled"); return; }

    if (!ResolveAll(g_cfg.mode >= 2 ? 2 : 1)) {
        Log("Resolver failed: nothing installed, the game runs unmodified.");
        return;
    }
    if (!StartEngineHooks()) { Log("Engine hooks not installed, the game runs unmodified."); return; }
    PerfRunLoop();   // keeps this thread as the performance sampler (returns at once if [Perf] Enabled=0)
}
