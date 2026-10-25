// Per-setup performance profiles, no engine dependencies (used by profile.cpp and tests/profiletest.cpp). What
// dynamic resolution learns about a setup's GPU costs is kept between sessions:
//   key:     the setup as "name=value" pairs joined by '|': GPU + driver, game build, the eye image the game asks for
//            (Oculus / runtime size x the game's resolution, without the mod's own output changes), refresh rate, the
//            mod's model / fovea / edges, the game's graphics menu (Settings.ini [Graphics], LightSourcesLimit,
//            spectator view). Only what changes GPU cost; the F6 quality isn't in it (size steps are keyed by their
//            own value, e.g. 66.7>75.0, so they carry across ceilings);
//   values:  name -> (value, samples): step:<a>><b> (GPU ms of the size step, whole frame, both eyes), factor (step
//            jump / DLSS costs' difference), dlss:<size> (DLSS ms per eye at that set-up size), slope (GPU ms per
//            unit of render area);
//   learn:   a new sample moves the value by 1 / (samples + 1), at least kMinWeight: fast at first, then slower,
//            never frozen (scenes differ; a driver keeps the key, so it must keep following);
//   switch:  a known setup takes its own profile; a new one starts as a copy of the current profile with every
//            sample count 0 ("uncertain": the first measurement replaces the copied value), then learns on its own;
//   cap:     at most kMaxProfiles, least recently used dropped (never the active one).
// File: plain text, one [profile] block each; deleting it starts from scratch. Another format version is ignored.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace prof {

constexpr int kVersion = 1;
constexpr int kMaxProfiles = 20;
constexpr float kMinWeight = 0.2f;

struct Value { float v = 0.f; int n = 0; };   // n = samples (0 = copied from another setup, not measured here)

struct Profile {
    std::string key;
    long long lastUsed = 0;   // seconds (unix time)
    int uses = 0;             // sessions / switches that selected it
    std::map<std::string, Value> vals;
};

inline float Weight(int n) { return std::max(kMinWeight, 1.f / (float)(n + 1)); }

// one new measurement; returns the value now kept
inline float Learn(Profile& p, const std::string& name, float v) {
    Value& x = p.vals[name];
    x.v = x.n <= 0 ? v : x.v + Weight(x.n) * (v - x.v);
    ++x.n;
    return x.v;
}

// replaces the value instead of averaging (the best DLSS size is a step, not a mean of steps)
inline float Set(Profile& p, const std::string& name, float v) {
    Value& x = p.vals[name];
    x.v = v;
    ++x.n;
    return v;
}

// the kept value (0 = none)
inline float Get(const Profile& p, const std::string& name, int* samples = nullptr) {
    auto it = p.vals.find(name);
    if (samples) *samples = it == p.vals.end() ? 0 : it->second.n;
    return it == p.vals.end() ? 0.f : it->second.v;
}

inline std::string StepName(float a, float b) { char s[48]; snprintf(s, sizeof(s), "step:%.1f>%.1f", a, b); return s; }
inline std::string RetryName(float a, float b) { char s[48]; snprintf(s, sizeof(s), "retry:%.1f>%.1f", a, b); return s; }
inline std::string DlssName(float size) { char s[32]; snprintf(s, sizeof(s), "dlss:%.1f", size); return s; }

inline int Find(const std::vector<Profile>& ps, const std::string& key) {
    for (int i = 0; i < (int)ps.size(); ++i) if (ps[i].key == key) return i;
    return -1;
}

// picks the profile for key: its own when known, else a new one copied from `from` (or empty) with every sample
// count at 0; then drops the least recently used beyond the cap (never the picked one). Returns its index.
inline int Select(std::vector<Profile>& ps, const std::string& key, int from, long long now, bool* created = nullptr) {
    int i = Find(ps, key);
    if (created) *created = i < 0;
    if (i < 0) {
        Profile p;
        p.key = key;
        if (from >= 0 && from < (int)ps.size())
            for (const auto& kv : ps[from].vals) if (kv.second.v > 0.f) p.vals[kv.first] = Value{ kv.second.v, 0 };
        ps.push_back(p);
        i = (int)ps.size() - 1;
    }
    ps[i].lastUsed = now; ++ps[i].uses;
    while ((int)ps.size() > kMaxProfiles) {
        int old = -1;
        for (int j = 0; j < (int)ps.size(); ++j)
            if (j != i && (old < 0 || ps[j].lastUsed < ps[old].lastUsed)) old = j;
        ps.erase(ps.begin() + old);
        if (old < i) --i;
    }
    return i;
}

inline std::string Serialize(const std::vector<Profile>& ps) {
    std::string o = "; itr2_dlss performance profiles: what dynamic resolution learned per setup (GPU, driver, game build,\n"
                    "; eye image size, refresh rate, the mod's model / fovea / edges, the game's graphics menu). Written by the\n"
                    "; mod; delete this file to learn everything from scratch.\n";
    char b[256];
    snprintf(b, sizeof(b), "version=%d\n", kVersion);
    o += b;
    for (const Profile& p : ps) {
        snprintf(b, sizeof(b), "\n[profile]\nlast_used=%lld\nuses=%d\n", p.lastUsed, p.uses);
        o += b;
        o += "key=" + p.key + "\n";
        for (const auto& kv : p.vals) {
            snprintf(b, sizeof(b), "%s=%.4f,%d\n", kv.first.c_str(), kv.second.v, kv.second.n);
            o += b;
        }
    }
    return o;
}

// a file of another (newer) format version, or none: no profiles
inline std::vector<Profile> Parse(const std::string& text, int* version = nullptr) {
    std::vector<Profile> ps;
    int ver = 0;
    size_t pos = 0;
    Profile* cur = nullptr;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line == "[profile]") { ps.emplace_back(); cur = &ps.back(); continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (!cur) { if (k == "version") ver = atoi(v.c_str()); continue; }
        if (k == "key") cur->key = v;
        else if (k == "last_used") cur->lastUsed = atoll(v.c_str());
        else if (k == "uses") cur->uses = atoi(v.c_str());
        else {
            const size_t c = v.find(',');
            Value x; x.v = (float)atof(v.c_str()); x.n = c == std::string::npos ? 0 : atoi(v.c_str() + c + 1);
            if (x.v > 0.f && x.v < 1000.f && x.n >= 0) cur->vals[k] = x;
        }
    }
    if (version) *version = ver;
    if (ver != kVersion) return {};
    ps.erase(std::remove_if(ps.begin(), ps.end(), [](const Profile& p) { return p.key.empty(); }), ps.end());
    return ps;
}

// "a=1|b=2" -> the names whose values differ between two keys (log: what changed)
inline std::string KeyDiff(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::map<std::string, std::string> m;
        size_t p = 0;
        while (p <= s.size()) {
            size_t e = s.find('|', p); if (e == std::string::npos) e = s.size();
            const std::string kv = s.substr(p, e - p);
            const size_t q = kv.find('=');
            if (q != std::string::npos) m[kv.substr(0, q)] = kv.substr(q + 1);
            p = e + 1;
        }
        return m;
    };
    const auto ma = split(a), mb = split(b);
    std::string o;
    for (const auto& kv : mb) {
        auto it = ma.find(kv.first);
        if (it == ma.end() || it->second != kv.second)
            o += (o.empty() ? "" : ", ") + kv.first + " " + (it == ma.end() ? "?" : it->second) + " -> " + kv.second;
    }
    for (const auto& kv : ma) if (!mb.count(kv.first)) o += (o.empty() ? "" : ", ") + kv.first + " removed";
    return o;
}

}  // namespace prof
