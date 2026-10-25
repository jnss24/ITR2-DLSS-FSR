// version.dll proxy: forwards the 17 version.dll exports to the system DLL and, only inside
// IntoTheRadius2-Win64-Shipping.exe, loads itr2_dlss.dll on a separate thread.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

extern "C" void* g_real[17];
void* g_real[17];

static const char* const kNames[17] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA", "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW", "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
    "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW" };

static HMODULE g_self = nullptr;

static DWORD WINAPI LoaderThread(LPVOID) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    if (wchar_t* s = wcsrchr(path, L'\\')) s[1] = 0;
    wcscat_s(path, L"itr2_dlss.dll");
    if (HMODULE core = LoadLibraryW(path)) {
        if (auto start = (void (*)())GetProcAddress(core, "ITR2DLSS_Start")) start();
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_self = h;
    DisableThreadLibraryCalls(h);

    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    wcscat_s(sys, L"\\version.dll");
    HMODULE real = LoadLibraryW(sys);
    if (!real) return FALSE;
    for (int i = 0; i < 17; ++i) g_real[i] = (void*)GetProcAddress(real, kNames[i]);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    if (_wcsicmp(name, L"IntoTheRadius2-Win64-Shipping.exe") == 0) {
        if (HANDLE t = CreateThread(nullptr, 0, LoaderThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
