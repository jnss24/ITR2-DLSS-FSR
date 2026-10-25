// Spoken (SAPI) or beep feedback for the hotkeys. COM, the voice and the beeps live on one background thread made
// once; callers only copy a short string and set an event.
// Audio output: [Announce] Device if set; else the output the game's own sound plays on (checked before each
// message), and until the game has sound, the first VR headset output by name, else the Windows default.
#include "common.h"
#include <cstdarg>
#include <sapi.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#ifndef DRV_QUERYFUNCTIONINSTANCEID   // mmddk.h (WDK): waveOut id -> MMDevice endpoint id
#define DRV_QUERYFUNCTIONINSTANCEID     (DRV_RESERVED + 17)
#define DRV_QUERYFUNCTIONINSTANCEIDSIZE (DRV_RESERVED + 18)
#endif

static SRWLOCK g_lock = SRWLOCK_INIT;
static wchar_t g_text[160];
static Tone g_tone = Tone::None;
static HANDLE g_event = nullptr;

static void Beeps(Tone t) {   // Beep() blocks, which is fine on this thread
    switch (t) {
    case Tone::On:     Beep(660, 90); Beep(990, 120); break;
    case Tone::Off:    Beep(990, 90); Beep(495, 160); break;
    case Tone::Change: Beep(800, 80); break;
    default: break;
    }
}

static bool ContainsNoCase(const wchar_t* s, const wchar_t* sub) {
    for (; *s; ++s) if (!_wcsnicmp(s, sub, wcslen(sub))) return true;
    return false;
}

// Points the voice at the first audio output whose name contains [Announce] Device (e.g. the Quest Link
// "Oculus Virtual Audio Device": the game sends its sound there even when it isn't the Windows default).
// Device empty (default): the first output that looks like a VR headset, in this order, else the Windows default.
// "Steam Streaming" is skipped on purpose: it exists on many PCs with Steam, VR or not.
static const wchar_t* const kVrAudioNames[] = {
    L"Oculus", L"Meta Quest", L"Quest", L"Rift",                      // Meta Link / Air Link
    L"Virtual Desktop",                                              // Virtual Desktop
    L"Valve Index", L"Index HMD", L"VIVE", L"HTC-VIVE", L"Bigscreen", L"Pimax", L"Varjo", L"PICO",
    L"Reverb", L"Mixed Reality" };
static void SelectOutput(ISpVoice* voice) {
    ISpObjectTokenCategory* cat = nullptr;
    IEnumSpObjectTokens* e = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory, (void**)&cat)) ||
        FAILED(cat->SetId(SPCAT_AUDIOOUT, FALSE)) || FAILED(cat->EnumTokens(nullptr, nullptr, &e))) {
        if (cat) cat->Release();
        Log("announce: cannot list audio outputs, using the Windows default");
        return;
    }
    const bool want = g_cfg.announceDevice[0] != 0;
    // list every output first, then pick: the ini's name, or the best VR match
    ISpObjectToken* toks[32] = {}; WCHAR* descs[32] = {}; int nt = 0;
    ISpObjectToken* t = nullptr; ULONG n = 0;
    while (nt < 32 && e->Next(1, &t, &n) == S_OK) { t->GetStringValue(nullptr, &descs[nt]); toks[nt++] = t; }
    int pick = -1;
    if (want) { for (int i = 0; i < nt && pick < 0; ++i) if (descs[i] && ContainsNoCase(descs[i], g_cfg.announceDevice)) pick = i; }
    else {
        for (const wchar_t* name : kVrAudioNames)
            for (int i = 0; i < nt && pick < 0; ++i)
                if (descs[i] && ContainsNoCase(descs[i], name) && !ContainsNoCase(descs[i], L"Steam Streaming")) pick = i;
    }
    const bool chosen = pick >= 0 && SUCCEEDED(voice->SetOutput(toks[pick], TRUE));
    for (int i = 0; i < nt; ++i) {
        char a[160] = "?"; if (descs[i]) WideCharToMultiByte(CP_UTF8, 0, descs[i], -1, a, sizeof(a) - 1, nullptr, nullptr);
        Log("announce: audio output '%s'%s", a, i == pick ? (chosen ? (want ? "  <- selected" : "  <- selected (VR headset, automatic)") : "  <- SetOutput failed") : "");
        if (descs[i]) CoTaskMemFree(descs[i]);
        toks[i]->Release();
    }
    e->Release(); cat->Release();
    if (!chosen) Log("announce: %s, using the Windows default output",
                     want ? "no audio output matches [Announce] Device" : "no VR headset output found (Device is empty)");
}

// ---- follow the game's own sound (Device empty) ----
// Each process that plays sound has an audio session on one output. The voice runs inside the game, so outputs
// holding an active session of this process are the game's (XAudio2) and maybe the voice's own. Steam Link, Air Link,
// Virtual Desktop etc. route the game to outputs the name list may not guess (Steam Link = "Steam Streaming").
static wchar_t g_voiceDev[256] = L"";   // endpoint id the voice was last pointed at ("" = not by this code)

static void DeviceName(IMMDevice* d, char* out, size_t n) {
    static const PROPERTYKEY kFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
    snprintf(out, n, "?");
    IPropertyStore* ps = nullptr;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) return;
    PROPVARIANT v; PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, out, (int)n - 1, nullptr, nullptr);
    PropVariantClear(&v);
    ps->Release();
}

// The output with an active audio session of this process other than the voice's own (or the voice's, if that is
// the only one). False when the game has no active sound anywhere.
static bool FindGameOutput(wchar_t* id, size_t idLen, char* name, size_t nameLen, bool logAll) {
    IMMDeviceEnumerator* en = nullptr; IMMDeviceCollection* col = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) ||
        FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        if (en) en->Release();
        return false;
    }
    const DWORD pid = GetCurrentProcessId();
    bool found = false, foundIsVoice = false;
    UINT nd = 0; col->GetCount(&nd);
    for (UINT i = 0; i < nd; ++i) {
        IMMDevice* d = nullptr; if (FAILED(col->Item(i, &d))) continue;
        IAudioSessionManager2* mgr = nullptr; IAudioSessionEnumerator* se = nullptr;
        int ours = 0, active = 0;
        if (SUCCEEDED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) &&
            SUCCEEDED(mgr->GetSessionEnumerator(&se))) {
            int ns = 0; se->GetCount(&ns);
            for (int s = 0; s < ns; ++s) {
                IAudioSessionControl* c = nullptr; IAudioSessionControl2* c2 = nullptr;
                if (FAILED(se->GetSession(s, &c))) continue;
                DWORD p = 0; AudioSessionState st = AudioSessionStateInactive;
                if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2)) &&
                    c2->GetProcessId(&p) == S_OK && p == pid) {
                    ++ours;
                    if (SUCCEEDED(c->GetState(&st)) && st == AudioSessionStateActive) ++active;
                }
                if (c2) c2->Release();
                c->Release();
            }
        }
        if (se) se->Release();
        if (mgr) mgr->Release();
        LPWSTR did = nullptr;
        if (SUCCEEDED(d->GetId(&did)) && did) {
            const bool isVoice = !wcscmp(did, g_voiceDev);
            if (logAll && ours) {
                char nm[160]; DeviceName(d, nm, sizeof(nm));
                Log("announce:   game audio sessions on '%s': %d (%d active)%s", nm, ours, active, isVoice ? " [voice is here]" : "");
            }
            if (active && (!found || (foundIsVoice && !isVoice))) {
                wcsncpy_s(id, idLen, did, _TRUNCATE);
                DeviceName(d, name, nameLen);
                found = true; foundIsVoice = isVoice;
            }
            CoTaskMemFree(did);
        }
        d->Release();
    }
    col->Release(); en->Release();
    return found;
}

// SAPI plays through waveOut: find the waveOut device of that endpoint and point the voice at it
static bool SetVoiceOutput(ISpVoice* voice, const wchar_t* endpointId) {
    const UINT n = waveOutGetNumDevs();
    for (UINT w = 0; w < n; ++w) {
        ULONG size = 0;
        if (waveOutMessage((HWAVEOUT)(UINT_PTR)w, DRV_QUERYFUNCTIONINSTANCEIDSIZE, (DWORD_PTR)&size, 0) != MMSYSERR_NOERROR || !size || size > 2048) continue;
        wchar_t buf[1024] = L"";
        if (waveOutMessage((HWAVEOUT)(UINT_PTR)w, DRV_QUERYFUNCTIONINSTANCEID, (DWORD_PTR)buf, size) != MMSYSERR_NOERROR) continue;
        if (_wcsicmp(buf, endpointId)) continue;
        ISpMMSysAudio* out = nullptr;
        if (FAILED(CoCreateInstance(CLSID_SpMMAudioOut, nullptr, CLSCTX_ALL, IID_ISpMMSysAudio, (void**)&out))) return false;
        bool ok = SUCCEEDED(out->SetDeviceId(w));
        if (ok) {
            voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);   // SetOutput fails while it speaks
            ok = SUCCEEDED(voice->SetOutput(out, TRUE));
        }
        out->Release();
        return ok;
    }
    return false;
}

// before each message: move the voice to the game's output if it changed
static void FollowGameOutput(ISpVoice* voice) {
    static int checks = 0, notFoundLogs = 3;
    wchar_t id[256]; char name[160];
    const bool first = checks++ == 0;
    if (first) Log("announce: following the game's audio output ([Announce] Device is empty)");
    if (!FindGameOutput(id, _countof(id), name, sizeof(name), first)) {
        if (notFoundLogs-- > 0) Log("announce: the game has no active sound yet, voice stays where it is");
        return;
    }
    if (!wcscmp(id, g_voiceDev)) return;
    if (SetVoiceOutput(voice, id)) {
        wcsncpy_s(g_voiceDev, id, _TRUNCATE);
        Log("announce: voice follows the game's audio output '%s'", name);
    } else {
        static int failLogs = 3;
        if (failLogs-- > 0) Log("announce: the game plays on '%s' but the voice could not be moved there", name);
    }
}

// messages are English: on a non-English Windows use an installed English voice if there is one
static void SelectEnglishVoice(ISpVoice* voice) {
    ISpObjectTokenCategory* cat = nullptr; IEnumSpObjectTokens* e = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory, (void**)&cat)) ||
        FAILED(cat->SetId(SPCAT_VOICES, FALSE)) || FAILED(cat->EnumTokens(L"Language=409", nullptr, &e))) {
        if (cat) cat->Release();
        return;
    }
    ISpObjectToken* t = nullptr; ULONG n = 0;
    if (e->Next(1, &t, &n) == S_OK && t) {
        WCHAR* desc = nullptr; t->GetStringValue(nullptr, &desc);
        char a[160] = "?"; if (desc) WideCharToMultiByte(CP_UTF8, 0, desc, -1, a, sizeof(a) - 1, nullptr, nullptr);
        Log("announce: English voice '%s'%s", a, SUCCEEDED(voice->SetVoice(t)) ? "" : " (SetVoice failed)");
        if (desc) CoTaskMemFree(desc);
        t->Release();
    } else Log("announce: no English voice installed, using the Windows default voice");
    e->Release(); cat->Release();
}

static DWORD WINAPI AnnounceThread(void*) {
    ISpVoice* voice = nullptr;
    if (g_cfg.announce == 2) {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(co) && SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, (void**)&voice)) && voice) {
            SelectEnglishVoice(voice);
            SelectOutput(voice);
            voice->SetVolume((USHORT)g_cfg.voiceVolume);
            voice->SetRate(g_cfg.voiceRate);
            Log("announce: voice ready (volume %d, rate %d)", g_cfg.voiceVolume, g_cfg.voiceRate);
        } else {
            voice = nullptr;
            Log("announce: SAPI voice not available (0x%08X), using beeps", (unsigned)co);
        }
    } else Log("announce: beeps");

    for (;;) {
        // pump messages too: an STA thread holding a COM object must not block them forever
        const DWORD w = MsgWaitForMultipleObjects(1, &g_event, FALSE, INFINITE, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0 + 1) {
            MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            continue;
        }
        if (w != WAIT_OBJECT_0) { Sleep(50); continue; }
        wchar_t text[160]; Tone tone;
        AcquireSRWLockExclusive(&g_lock);
        wcscpy_s(text, g_text); tone = g_tone; g_text[0] = 0; g_tone = Tone::None;
        ReleaseSRWLockExclusive(&g_lock);
        if (voice && text[0]) {
            if (!g_cfg.announceDevice[0]) FollowGameOutput(voice);
            const HRESULT hr = voice->Speak(text, SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr);
            static int failLogs = 8;
            if (FAILED(hr) && failLogs-- > 0) Log("announce: Speak failed 0x%08X (beeping instead)", (unsigned)hr);
            if (FAILED(hr)) Beeps(tone);
        } else Beeps(tone);
    }
}

void AnnounceStart() {
    if (g_cfg.announce <= 0 || g_event) return;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_event) return;
    HANDLE t = CreateThread(nullptr, 0, AnnounceThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t); else { CloseHandle(g_event); g_event = nullptr; }
}

void Announce(Tone tone, const char* fmt, ...) {
    if (!g_event) return;
    char a[160];
    va_list ap; va_start(ap, fmt);
    vsnprintf(a, sizeof(a), fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&g_lock);
    MultiByteToWideChar(CP_UTF8, 0, a, -1, g_text, _countof(g_text));
    g_tone = tone;
    ReleaseSRWLockExclusive(&g_lock);
    SetEvent(g_event);
}
