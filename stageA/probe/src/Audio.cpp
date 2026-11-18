#include "Audio.hpp"
#include "Data.hpp"

#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

namespace skateaudio
{
namespace
{
constexpr uint32_t kRate = 48000;
constexpr uint32_t kChunk = 480;   // 10 ms per XAudio2 buffer
constexpr uint32_t kQueued = 4;    // buffers kept queued: 40 ms in front of the device
constexpr uint32_t kBuffers = 8;   // the ring the queued buffers come from (more than kQueued: XAudio2 reads in place)
constexpr uint32_t kEmitters = 3;  // deck, body, head ("sense of speed")
constexpr uint32_t kFlagSounding = 1u << 1, kFlagListener = 1u << 2;
// Placement: equal-power pan over a narrowed stage (a sound at the camera's side sits at 65 % of hard left or right),
// and a gentle roll-off past 4 m (the Skate camera rides 3-5 m from the skater, so a ride is heard at full level and a
// board that rolled away after a bail fades with distance: 8 m 0.67, 20 m 0.33, 40 m 0.18).
constexpr float kWidth = 0.65f, kRefDist = 4.f, kRolloff = 0.5f;
constexpr float kCeiling = 0.891f;  // limiter ceiling, -1 dBFS (as SK8V's)
constexpr double kReleaseS = 0.1;
constexpr ULONGLONG kStaleMs = 250;  // no game frame this long: pause (as SK8V's watchdog)

void* g_rt = nullptr;
Fns g_fns;
float g_volume = 1.f;
std::atomic<bool> g_started{false};

// Main thread -> feeder.
std::mutex g_listenerLock;
struct Listener
{
    bool have = false;
    float p[3] = {}, f[3] = {0, 1, 0}, r[3] = {1, 0, 0};
} g_listener;
std::atomic<bool> g_wantPaused{false};
std::atomic<ULONGLONG> g_heartbeat{0};

// Feeder -> main thread (the log).
std::atomic<int> g_outState{0};  // 0 not open yet, 1 playing, 2 open failed (retrying), 3 device lost (reopening)
std::atomic<long> g_outHr{0};
std::atomic<uint32_t> g_opens{0}, g_chunks{0}, g_soundingChunks{0}, g_peakBits{0}, g_minEnvBits{0x3f800000};
std::atomic<bool> g_feederPaused{false};
std::atomic<float> g_deckDist{-1.f};

void MaxBits(std::atomic<uint32_t>& a, float v)
{
    uint32_t b;
    std::memcpy(&b, &v, 4);
    uint32_t cur = a.load(std::memory_order_relaxed);
    float c;
    do std::memcpy(&c, &cur, 4);
    while (v > c && !a.compare_exchange_weak(cur, b, std::memory_order_relaxed));
}

void MinBits(std::atomic<uint32_t>& a, float v)
{
    uint32_t b;
    std::memcpy(&b, &v, 4);
    uint32_t cur = a.load(std::memory_order_relaxed);
    float c;
    do std::memcpy(&c, &cur, 4);
    while (v < c && !a.compare_exchange_weak(cur, b, std::memory_order_relaxed));
}

float TakeBits(std::atomic<uint32_t>& a, float reset)
{
    uint32_t r;
    std::memcpy(&r, &reset, 4);
    const uint32_t b = a.exchange(r, std::memory_order_relaxed);
    float v;
    std::memcpy(&v, &b, 4);
    return v;
}

template<class... A> void Logf(LogFn log, const char* fmt, A... a)
{
    char b[768];
    snprintf(b, sizeof b, fmt, a...);
    log(b);
}

std::filesystem::path PluginDir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&PluginDir), &self);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::string Utf8(const std::wstring& w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// ----- XAudio2 (feeder thread only) -----
HANDLE g_wake = nullptr;  // a buffer finished, or the device went away

struct VoiceCallback final : IXAudio2VoiceCallback
{
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { SetEvent(g_wake); }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override { SetEvent(g_wake); }
} g_voiceCb;

std::atomic<bool> g_lost{false};
struct EngineCallback final : IXAudio2EngineCallback
{
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    // The device was removed or changed (headphones unplugged, the default device switched): reopen on the new one.
    void STDMETHODCALLTYPE OnCriticalError(HRESULT hr) override
    {
        g_outHr = hr;
        g_lost = true;
        SetEvent(g_wake);
    }
} g_engineCb;

struct Output
{
    IXAudio2* x = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;

    HRESULT Open()
    {
        HRESULT hr = XAudio2Create(&x, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) return hr;
        x->RegisterForCallbacks(&g_engineCb);
        // Stereo in at 48 kHz on the default device; XAudio2 maps it to the device's own layout and rate.
        hr = x->CreateMasteringVoice(&master, 2, kRate, 0, nullptr, nullptr, AudioCategory_GameEffects);
        if (FAILED(hr)) return Close(), hr;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        hr = x->CreateSourceVoice(&voice, &wf, XAUDIO2_VOICE_NOPITCH, 1.0f, &g_voiceCb);
        if (FAILED(hr)) return Close(), hr;
        hr = voice->Start();
        if (FAILED(hr)) return Close(), hr;
        return S_OK;
    }

    void Close()
    {
        if (voice) voice->DestroyVoice(), voice = nullptr;
        if (master) master->DestroyVoice(), master = nullptr;
        if (x) x->UnregisterForCallbacks(&g_engineCb), x->StopEngine(), x->Release(), x = nullptr;
    }
};

// One emitter's placement, ramped across each chunk (no zipper noise as the camera swings).
struct Placement
{
    float l = 1.f, r = 1.f;
};

void Target(const stageb::DkrAudioEmitter& e, const Listener& L, float& tl, float& tr, float& dist)
{
    float g = std::isfinite(e.gain) ? std::clamp(e.gain, 0.f, 4.f) : 1.f;
    g *= g_volume;
    tl = tr = g;
    dist = -1.f;
    const bool finite = std::isfinite(e.position[0]) && std::isfinite(e.position[1]) && std::isfinite(e.position[2]);
    if ((e.flags & kFlagListener) || !L.have || !finite) return;  // the wind is heard at the listener, centred
    const float d[3] = {e.position[0] - L.p[0], e.position[1] - L.p[1], e.position[2] - L.p[2]};
    dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float att = 1.f / (1.f + kRolloff * std::max(dist - kRefDist, 0.f) / kRefDist);
    const float x = dist > 0.05f ? (d[0] * L.r[0] + d[1] * L.r[1] + d[2] * L.r[2]) / dist : 0.f;
    const float pan = std::clamp(x, -1.f, 1.f) * kWidth;
    const float a = (pan + 1.f) * 0.78539816f;  // 0 .. pi/2
    tl = g * att * std::cos(a) * 1.41421356f;     // centre: 1 in each ear
    tr = g * att * std::sin(a) * 1.41421356f;
}

struct Mixer
{
    Placement place[kEmitters];
    float env = 1.f, release = float(1.0 - std::exp(-1.0 / (kReleaseS * kRate)));
    float mono[kChunk];

    void Mix(float* out, bool paused)
    {
        std::fill(out, out + 2 * kChunk, 0.f);
        if (paused) return;
        Listener L;
        {
            std::lock_guard<std::mutex> lock(g_listenerLock);
            L = g_listener;
        }
        bool sounding = false;
        float peak = 0.f;
        for (uint32_t id = 0; id < kEmitters; ++id)
        {
            stageb::DkrAudioEmitter e{};
            e.size = sizeof e;
            const uint32_t n = g_fns.pull(g_rt, id, mono, kChunk, &e);
            if (e.flags & kFlagSounding) sounding = true;
            if (!n) continue;
            float tl, tr, dist;
            Target(e, L, tl, tr, dist);
            if (id == 0) g_deckDist.store(dist, std::memory_order_relaxed);
            Placement& p = place[id];
            const float sl = (tl - p.l) / kChunk, sr = (tr - p.r) / kChunk;
            float gl = p.l, gr = p.r;
            for (uint32_t k = 0; k < kChunk; ++k, gl += sl, gr += sr) out[2 * k] += mono[k] * gl, out[2 * k + 1] += mono[k] * gr;
            p.l = tl, p.r = tr;
        }
        // Limiter, both channels linked: instant attack, 100 ms release (SK8V's, on the stereo mix).
        float minEnv = env;
        for (uint32_t k = 0; k < kChunk; ++k)
        {
            float& l = out[2 * k];
            float& r = out[2 * k + 1];
            if (!(l == l)) l = 0.f;
            if (!(r == r)) r = 0.f;
            env += (1.f - env) * release;
            const float a = std::max(std::fabs(l), std::fabs(r));
            peak = std::max(peak, a);
            if (a * env > kCeiling) env = kCeiling / a;
            l *= env, r *= env;
            minEnv = std::min(minEnv, env);
        }
        g_chunks.fetch_add(1, std::memory_order_relaxed);
        if (sounding) g_soundingChunks.fetch_add(1, std::memory_order_relaxed);
        MaxBits(g_peakBits, peak);
        MinBits(g_minEnvBits, minEnv);
    }
};

DWORD WINAPI FeederMain(void*)
{
    SetThreadDescription(GetCurrentThread(), L"deckrunner-audio-out");
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static float buffers[kBuffers][2 * kChunk];
    static Mixer mixer;
    uint32_t next = 0;
    Output out;
    ULONGLONG retryAt = 0;
    bool paused = false;
    for (;;)
    {
        const ULONGLONG now = GetTickCount64();
        if (g_lost.exchange(false))
        {
            out.Close();
            g_outState = 3;
            retryAt = now + 1000;
        }
        if (!out.voice && now >= retryAt)
        {
            const HRESULT hr = out.Open();
            if (SUCCEEDED(hr)) g_outState = 1, g_opens.fetch_add(1);
            else g_outState = 2, g_outHr = hr, retryAt = now + 5000;
        }
        WaitForSingleObject(g_wake, out.voice ? 20 : 100);
        // The feeder is the only writer of the runtime's pause (as in SK8V): the game paused, or no game frame lately.
        const bool want = g_wantPaused.load() || GetTickCount64() - g_heartbeat.load() > kStaleMs;
        if (want != paused)
        {
            paused = want;
            g_fns.setPaused(g_rt, paused ? 1u : 0u);
            g_feederPaused = paused;
        }
        if (!out.voice) continue;
        XAUDIO2_VOICE_STATE st{};
        out.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        for (uint32_t q = st.BuffersQueued; q < kQueued; ++q)
        {
            float* b = buffers[next];
            next = (next + 1) % kBuffers;
            mixer.Mix(b, paused);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = 2 * kChunk * sizeof(float);
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            if (FAILED(out.voice->SubmitSourceBuffer(&xb))) break;
        }
    }
    out.Close();
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

// ----- the log (main thread) -----
uint32_t g_lastState = 0xffffffff, g_lastUnderruns = 0;
int g_lastOut = -1;
bool g_lastPaused = false;
ULONGLONG g_nextStatus = 0, g_nextSummary = 0;
const char* kStates[] = {"off", "loading", "ready", "error"};
} // namespace

void Begin(void* rt, const Fns& fns, LogFn log)
{
    if (g_started || !rt) return;
    if (!fns.configure || !fns.pull || !fns.setPaused || !fns.status) { log("skate audio: the runtime has no audio exports; off"); return; }
    const std::wstring ini = (PluginDir() / L"deckrunner.ini").wstring();
    wchar_t buf[1024]{};
    if (!GetPrivateProfileIntW(L"audio", L"enabled", 1, ini.c_str())) { log("skate audio: off (deckrunner.ini [audio] enabled=0)"); return; }
    GetPrivateProfileStringW(L"audio", L"volume", L"1.0", buf, 64, ini.c_str());
    const float vol = static_cast<float>(_wtof(buf));
    g_volume = std::isfinite(vol) ? std::clamp(vol, 0.f, 4.f) : 1.f;
    if (g_volume <= 0.f) { log("skate audio: off (deckrunner.ini [audio] volume=0)"); return; }
    const std::string cache = data::Utf8(data::SkateAudio());  // R1: from the data folder (Data.hpp)
    g_rt = rt;
    g_fns = fns;
    // The runtime checks the cache (raw\audiofiles) and logs why it refuses one; the engine loads on its own thread.
    if (!fns.configure(rt, cache.c_str(), 1.0f))
    {
        Logf(log, "skate audio: off: %s is not a prepared Skate audio cache (the setup makes it; or set deckrunner.ini "
                  "[data] skate_audio=)", cache.c_str());
        return;
    }
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_heartbeat = GetTickCount64();
    HANDLE t = CreateThread(nullptr, 0, &FeederMain, nullptr, 0, nullptr);
    if (!t) { log("skate audio: could not start the output thread; off"); return; }
    SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(t);
    g_started = true;
    Logf(log, "skate audio: loading Skate 3's sound banks from %s (volume %.2f); output through XAudio2, %u ms queued", cache.c_str(),
         g_volume, kQueued * kChunk * 1000 / kRate);
}

void Frame(const float* pos, const float* fwd, const float* right, bool paused, LogFn log)
{
    if (!g_started) return;
    const ULONGLONG now = GetTickCount64();
    g_heartbeat = now;
    g_wantPaused = paused;
    {
        std::lock_guard<std::mutex> lock(g_listenerLock);
        g_listener.have = pos != nullptr;
        if (pos) std::copy(pos, pos + 3, g_listener.p), std::copy(fwd, fwd + 3, g_listener.f), std::copy(right, right + 3, g_listener.r);
    }
    const int o = g_outState.load();
    if (o != g_lastOut)
    {
        if (o == 1) Logf(log, "skate audio: output open on the default device (open #%u)", g_opens.load());
        else if (o == 2) Logf(log, "skate audio: output could not open (HRESULT 0x%08lX); retrying every 5 s", (unsigned long)g_outHr.load());
        else if (o == 3) Logf(log, "skate audio: output device lost (HRESULT 0x%08lX); reopening on the default device", (unsigned long)g_outHr.load());
        g_lastOut = o;
    }
    const bool fp = g_feederPaused.load();
    if (fp != g_lastPaused) Logf(log, "skate audio: %s", fp ? "paused (the game paused, or no game frame for 250 ms)" : "resumed"), g_lastPaused = fp;
    if (now < g_nextStatus) return;
    g_nextStatus = now + 500;
    stageb::DkrAudioStatus st{};
    st.size = sizeof st;
    if (!g_fns.status(g_rt, &st)) return;
    st.message[sizeof st.message - 1] = 0;
    if (st.state != g_lastState)
    {
        Logf(log, "skate audio: engine %s: %s", st.state < 4 ? kStates[st.state] : "?", st.message);
        g_lastState = st.state;
    }
    if (now < g_nextSummary) return;
    const uint32_t sounding = g_soundingChunks.exchange(0), chunks = g_chunks.exchange(0);
    const float peak = TakeBits(g_peakBits, 0.f), minEnv = TakeBits(g_minEnvBits, 1.f);
    if (!sounding) return;  // quiet: nothing to report (the summary waits for the next sound)
    g_nextSummary = now + 10000;
    const uint32_t under = st.underruns - g_lastUnderruns;
    g_lastUnderruns = st.underruns;
    Logf(log, "skate audio: %u voices; sounding %.0f%% of the last %.1f s; mix peak %.1f dBFS, limiter down to %.1f dB; %u underruns; "
              "deck %.1f m from the camera",
         st.voices, chunks ? 100.0 * sounding / chunks : 0.0, chunks * double(kChunk) / kRate, 20.0 * std::log10(std::max(peak, 1e-6f)),
         20.0 * std::log10(std::max(minEnv, 1e-6f)), under, g_deckDist.load());
}
} // namespace skateaudio
