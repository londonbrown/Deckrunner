// Offline Stage B harness: drives deckrunner_runtime.dll through the same C ABI and structs as the lab plugin,
// with collision from Stage A probe dumps (OBJ, game world coordinates) instead of live PhysX.
// Usage: stageb_harness <runtime.dll> <assets> <out dir> <probe.json for spawn> <seconds> <probe.obj>...
// P1-6 (0.18.0): every run also compares V's per-frame movement as the plugin now computes it (the runtime's clock,
// read before the frame's step) with 0.17.0's (the wall time at which each tick was noticed, read after the step).
// DKR_FPS=<n> paces the loop at n frames a second (the game ran at about 60); otherwise it sleeps 16 ms a frame.
// DKR_JITTER=1 adds what the game adds: frame times varying by up to 1.5 ms either way, and 0-2.5 ms of main-thread work
// between posting the step and reading the state (the plugin's re-centre scan alone may take 2 ms there).
// C3: with DKR_RIG=<rig json> set, the rig is loaded as the plugin does, and every frame's pose is checked (finite, unit
// quaternions, the same tick as the state, hips at standing height) and summarised.
#include "../src/Audio.hpp"
#include "../src/StageB.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace stageb;

struct Tri { float p[9]; uint8_t physical; };

// P1-6: one way of placing V between the skater's ticks, and how evenly it moves her from frame to frame.
struct Interp
{
    uint64_t prevT = 0, curT = 0;
    float prev[3] = {}, cur[3] = {}, last[3] = {}, lastStep[3] = {};
    double curAt = 0;
    bool have = false, haveLast = false, haveStep = false;
    double sum = 0, sum2 = 0, mx = 0;
    uint32_t n = 0;
    double r2 = 0;          // sum of (step / (speed * dt) - 1)^2
    uint32_t rn = 0, stalls = 0, doubles = 0;
    void Sample(uint64_t tick, const float pos[3], double now)
    {
        if (!have || tick < curT) { prevT = curT = tick; std::copy(pos, pos + 3, prev); std::copy(pos, pos + 3, cur); curAt = now; have = true; haveLast = haveStep = false; }
        else if (tick != curT) { prevT = curT; std::copy(cur, cur + 3, prev); curT = tick; std::copy(pos, pos + 3, cur); curAt = now; }
    }
    // a: the weight of cur; measure: rolling on the ground, so a step is riding, not a respawn
    void Place(float a, bool measure, double expected = 0)
    {
        float at[3];
        for (int i = 0; i < 3; ++i) at[i] = prev[i] + (cur[i] - prev[i]) * a;
        if (haveLast)
        {
            const float st[3] = {at[0] - last[0], at[1] - last[1], at[2] - last[2]};
            const double len = std::sqrt(st[0] * st[0] + st[1] * st[1] + st[2] * st[2]);
            if (measure && haveStep && len < 1.0)
            {
                const double d = std::sqrt((st[0] - lastStep[0]) * (st[0] - lastStep[0]) + (st[1] - lastStep[1]) * (st[1] - lastStep[1]) +
                                           (st[2] - lastStep[2]) * (st[2] - lastStep[2]));
                sum += len, sum2 += d * d, mx = std::max(mx, d), ++n;
                if (expected > 0.02)
                {
                    const double r = len / expected;
                    r2 += (r - 1) * (r - 1), ++rn;
                    stalls += r < 0.5, doubles += r > 1.5;
                }
            }
            std::copy(st, st + 3, lastStep);
            haveStep = measure && len < 1.0;
        }
        std::copy(at, at + 3, last);
        haveLast = true;
    }
    void Print(const char* name) const
    {
        if (n) printf("P1-6 %s: %u frames rolling: mean step %.1f cm, frame-to-frame change rms %.2f cm, max %.2f cm; step / (speed x frame time): "
                      "rms deviation from 1 %.3f, %u frames under 0.5 (standing still), %u over 1.5 (double steps)\n", name, n, 100.0 * sum / n,
                      100.0 * std::sqrt(sum2 / n), 100.0 * mx, rn ? std::sqrt(r2 / rn) : 0.0, stalls, doubles);
        else printf("P1-6 %s: no frames measured\n", name);
    }
};

static void LoadObj(const char* path, std::vector<Tri>& out)
{
    std::ifstream f(path);
    std::string line;
    std::vector<float> v;
    uint8_t physical = 1;
    while (std::getline(f, line))
    {
        if (line.rfind("v ", 0) == 0) { float x, y, z; sscanf_s(line.c_str() + 2, "%f %f %f", &x, &y, &z); v.insert(v.end(), {x, y, z}); }
        else if (line.rfind("f ", 0) == 0)
        {
            unsigned a, b, c;
            if (sscanf_s(line.c_str() + 2, "%u %u %u", &a, &b, &c) == 3 && a && b && c && 3 * std::max({a, b, c}) <= v.size())
            {
                Tri t{};
                for (int k = 0; k < 3; ++k) { const unsigned i = (k == 0 ? a : k == 1 ? b : c) - 1; for (int j = 0; j < 3; ++j) t.p[k * 3 + j] = v[i * 3 + j]; }
                t.physical = physical;
                out.push_back(t);
            }
        }
        else if (line.rfind("# shapeFlags=", 0) == 0)
        {
            const auto s = line.find("sim=");
            physical = s != std::string::npos && std::stoul(line.substr(s + 4, 8), nullptr, 16) != 0;
        }
    }
}

static void Near(const std::vector<Tri>& all, const float c[3], float r, std::vector<float>& tris, std::vector<uint8_t>& flags)
{
    for (const auto& t : all)
    {
        const float cx = (t.p[0] + t.p[3] + t.p[6]) / 3, cy = (t.p[1] + t.p[4] + t.p[7]) / 3;
        if (std::hypot(cx - c[0], cy - c[1]) > r) continue;
        tris.insert(tris.end(), t.p, t.p + 9);
        flags.push_back(t.physical);
    }
}

int main(int argc, char** argv)
{
    if (argc < 7) { puts("usage: stageb_harness <runtime.dll> <assets> <out dir> <probe.json> <seconds> <probe.obj>..."); return 2; }
    std::vector<Tri> all;
    for (int i = 6; i < argc; ++i) LoadObj(argv[i], all);
    std::ifstream js(argv[4]);
    std::stringstream ss;
    ss << js.rdbuf();
    const std::string j = ss.str();
    float v[3], yaw = 0;
    const auto pp = j.find("\"pos\":[");
    sscanf_s(j.c_str() + pp + 7, "%f,%f,%f", &v[0], &v[1], &v[2]);
    sscanf_s(j.c_str() + j.find("\"yaw\":") + 6, "%f", &yaw);
    printf("loaded %zu dump triangles; spawn (%.2f, %.2f, %.2f) yaw %.1f\n", all.size(), v[0], v[1], v[2], yaw);

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { printf("cannot load %s (%lu)\n", argv[1], GetLastError()); return 1; }
    auto create = (void* (*)(const char*, const char*))GetProcAddress(dll, "dkr_create");
    auto start = (uint32_t (*)(void*, const float*, const uint8_t*, uint32_t, const float*, float, uint32_t))GetProcAddress(dll, "dkr_start");
    auto step = (uint32_t (*)(void*, float, const DkrPad*))GetProcAddress(dll, "dkr_step");
    auto state = (uint32_t (*)(const void*, DkrState*))GetProcAddress(dll, "dkr_get_state");
    auto wants = (uint32_t (*)(const void*, float*))GetProcAddress(dll, "dkr_wants_collision");
    auto install = (uint32_t (*)(void*, const float*, const uint8_t*, uint32_t, const float*))GetProcAddress(dll, "dkr_install_collision");
    auto destroy = (void (*)(void*))GetProcAddress(dll, "dkr_destroy");
    void* rt = create(argv[2], argv[3]);
    auto loadRig = (uint32_t (*)(void*, const char*))GetProcAddress(dll, "dkr_load_rig");
    auto getPose = (uint32_t (*)(const void*, DkrPose*))GetProcAddress(dll, "dkr_get_pose");
    auto refPose = (uint32_t (*)(const void*, DkrPose*))GetProcAddress(dll, "dkr_reference_pose");
    auto snapshot = (uint32_t (*)(const void*, DkrState*, DkrPose*))GetProcAddress(dll, "dkr_get_snapshot");  // ABI 8
    if (!snapshot) { puts("the runtime has no dkr_get_snapshot (ABI 8 needed)"); return 1; }
    Interp byClock, byNoticing;
    uint64_t posted = 0;
    float postedDt[64] = {};
    double dtSum = 0, dtMax = 0;
    uint32_t dtN = 0;
    char rig[1024] = {};
    const bool c3 = GetEnvironmentVariableA("DKR_RIG", rig, sizeof rig) > 0 && loadRig && getPose && refPose;
    // DKR_AUDIO=<prepared Skate audio cache> (Skate audio, ABI 7): pull the three emitters through the ride, report their
    // levels and write the mix to <out>/skate_audio.wav to listen to.
    auto audioConfigure = (uint32_t (*)(void*, const char*, float))GetProcAddress(dll, "dkr_audio_configure");
    auto audioPull = (uint32_t (*)(const void*, uint32_t, float*, uint32_t, DkrAudioEmitter*))GetProcAddress(dll, "dkr_audio_pull");
    auto audioStatus = (uint32_t (*)(const void*, DkrAudioStatus*))GetProcAddress(dll, "dkr_audio_status");
    char audioDir[1024] = {};
    // DKR_AUDIO_OUT=1 (with DKR_RIG): the plugin's own player (src/Audio.cpp, XAudio2) instead of the pulls below, at the
    // volume in deckrunner.ini beside this exe ([audio] volume=), the listener on Skate's camera.
    char ao[8] = {};
    const bool audioOut = GetEnvironmentVariableA("DKR_AUDIO_OUT", ao, sizeof ao) > 0 && audioConfigure && audioPull && audioStatus;
    const bool audio = !audioOut && GetEnvironmentVariableA("DKR_AUDIO", audioDir, sizeof audioDir) > 0 && audioConfigure && audioPull && audioStatus;
    const auto outLog = [](const char* m) { printf("  [audio out] %s\n", m); };
    if (audioOut)
    {
        skateaudio::Fns f;
        f.configure = audioConfigure;
        f.pull = audioPull;
        f.setPaused = (uint32_t (*)(const void*, uint32_t))GetProcAddress(dll, "dkr_audio_set_paused");
        f.status = audioStatus;
        skateaudio::Begin(rt, f, outLog);
    }
    if (audio) printf("audio: configure %s: %u\n", audioDir, audioConfigure(rt, audioDir, 1.0f));
    std::vector<int16_t> wav;
    double audioSq[3] = {}, audioPeak[3] = {};
    uint64_t audioFrames[3] = {};
    uint32_t audioSounding[3] = {}, audioPulls = 0;
    char ps[8] = {};
    const bool padScript = GetEnvironmentVariableA("DKR_PAD_SCRIPT", ps, sizeof ps) > 0;
    // DKR_PAD_SCRIPT=marker (P1-13, 0.21.0): push along from 1 s, LB + d-pad down at 3 s (a session marker), keep pushing
    // until 18 s, then hold LB + d-pad up from 19 s to 21 s: the skater must come back to the marker.
    const bool markerScript = padScript && strcmp(ps, "marker") == 0;
    // DKR_PAD_SCRIPT=walk (P2-8, 0.23.0): push from 1 s to 3 s, Y (off the board) at 4 s, walk forward from 5 s to 10 s,
    // Y again (back on) at 11 s.
    const bool walkScript = padScript && strcmp(ps, "walk") == 0;
    // DKR_BODY_SCRIPT=parked|oncoming (P1-14, ABI 10): pushes as the marker script does (without the marker), and a
    // 4.4 x 1.8 x 1.4 m box sent each frame through dkr_set_bodies as a 1,500 kg car: parked across the road 30 m along,
    // or driving down the road at the skater at 8 m/s from 60 m along, from 2 s.
    char bs[16] = {};
    const bool bodyScript = GetEnvironmentVariableA("DKR_BODY_SCRIPT", bs, sizeof bs) > 0;
    const bool oncoming = bodyScript && strcmp(bs, "oncoming") == 0;
    auto setBodies = (uint32_t (*)(void*, const DkrBody*, uint32_t, const float*, uint32_t))GetProcAddress(dll, "dkr_set_bodies");
    // DKR_RESUME=1 (C5): park after 10 s of riding, then start again at the spawn: the session must resume without
    // reloading Skate and ride from there.
    auto park = (uint32_t (*)(void*))GetProcAddress(dll, "dkr_park");
    char rs[8] = {};
    const bool resumeTest = GetEnvironmentVariableA("DKR_RESUME", rs, sizeof rs) > 0 && park;
    const bool resumeHere = resumeTest && strcmp(rs, "here") == 0;  // P1-13 (0.23.1): resume where the skater is, as V's spot in game
    bool parkedOnce = false, resumed = false;
    double resumeAt = 0;
    // DKR_CAPTURE_AT=<tick> (P1-11, ABI 9): ask for a capture once the session reaches that tick, as Ctrl+Shift+F12 does.
    auto capture = (uint32_t (*)(void*))GetProcAddress(dll, "dkr_capture");
    char capText[16] = {};
    const uint64_t captureAt = capture && GetEnvironmentVariableA("DKR_CAPTURE_AT", capText, sizeof capText) > 0 ? strtoull(capText, nullptr, 10) : 0;
    bool captured = false;
    if (c3) printf("C3: rig %s posted: %u\n", rig, loadRig(rt, rig));
    uint32_t poses = 0, poseBad = 0, poseOtherTick = 0, unreach = 0;
    float hipsMin = 1e9f, hipsMax = -1e9f, qErr = 0;
    // P2-7/P2-8 (0.23.0): poses while the skater is down (Wipeout*) or on foot (Biped*), which V now wears too.
    uint32_t downPoses = 0, downBad = 0, downUnreach = 0;
    float downDzMin = 1e9f, downDzMax = -1e9f, downFar = 0;
    uint32_t boards = 0, boardBad = 0, cams = 0, camBad = 0;
    float camDistMin = 1e9f, camDistMax = 0, camUpMin = 1e9f, camUpMax = -1e9f;
    float boardUpMin = 1, boardRootMax = 0;  // while rolling: the board's up axis, and the skater root's distance from it
    std::vector<float> tris;
    std::vector<uint8_t> flags;
    Near(all, v, 40.f, tris, flags);
    printf("start: %zu triangles within 40 m -> %u\n", flags.size(), start(rt, tris.data(), flags.data(), (uint32_t)flags.size(), v, yaw, 1));
    const float seconds = (float)atof(argv[5]);
    DkrPad pad{};
    const auto t0 = GetTickCount64();
    double logAt = 0;
    LARGE_INTEGER freq, last, now;
    char fpsText[16] = {};
    const double fps = GetEnvironmentVariableA("DKR_FPS", fpsText, sizeof fpsText) > 0 ? atof(fpsText) : 0.0;  // P1-6: frame pacing
    const bool jitter = GetEnvironmentVariableA("DKR_JITTER", fpsText, sizeof fpsText) > 0;
    uint32_t rng = 12345;
    auto uniform = [&rng](double lo, double hi) { rng = rng * 1664525u + 1013904223u; return lo + (hi - lo) * (rng >> 8) / double(1 << 24); };
    auto spin = [&freq](double ms) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        do { QueryPerformanceCounter(&b); } while (1000.0 * double(b.QuadPart - a.QuadPart) / double(freq.QuadPart) < ms);
    };
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    for (int frame = 0;; ++frame)
    {
        DkrState s{};
        s.size = sizeof s;
        DkrPose snapPose{};
        snapPose.size = sizeof snapPose;
        snapshot(rt, &s, &snapPose);  // P1-6: as the plugin now does, before this frame's step
        const double wall = (GetTickCount64() - t0) / 1000.0;
        const double nowMs = 1000.0 * double(GetTickCount64() - t0);
        if (s.status == 3) { printf("ERROR: %s\n", s.error); break; }
        if (s.status == 2 && s.tick / 60.0 > seconds) break;
        if (wall > seconds + 60) { puts("timeout"); break; }
        QueryPerformanceCounter(&now);
        const float dt = float(double(now.QuadPart - last.QuadPart) / double(freq.QuadPart));  // measured, as in the game
        last = now;
        // DKR_PAD_SCRIPT=1: a synthetic controller from 8 s (the in-game handover): steer right and push with A for
        // 3 s, steer left for 3 s, then pull the right stick down and flick it up (an ollie) at 14 s.
        if (walkScript)
        {
            const double ride = s.tick / 60.0;
            pad = DkrPad{};
            pad.connected = 1;
            if (ride >= 1 && ride < 3 && int(ride * 60) % 30 < 4) pad.buttons |= 0x1000;
            if ((ride >= 4 && ride < 4.1) || (ride >= 11 && ride < 11.1)) pad.buttons |= 0x8000;
            if (ride >= 5 && ride < 10) pad.ly = 32767;
            static double lastShown = 0;
            if (ride >= lastShown + 1) { lastShown = ride; printf("  walk script t=%.0fs pos=(%.1f, %.1f, %.2f) %s %.1f m/s\n", ride, s.pos[0], s.pos[1], s.pos[2], s.state, s.speed); }
        }
        else if (bodyScript && setBodies)
        {
            const double ride = s.tick / 60.0;
            pad = DkrPad{};
            pad.connected = 1;
            if (ride >= 1 && ride < 18 && int(ride * 60) % 30 < 4) pad.buttons |= 0x1000;
            const float hx = oncoming ? 0.9f : 2.2f, hy = oncoming ? 2.2f : 0.9f, hz = 0.7f, mass = 1500.f;
            const float speed = oncoming && ride >= 2 ? 8.f : 0.f;
            const float cy = oncoming ? 60.f - (ride >= 2 ? 8.f * float(ride - 2) : 0.f) : 30.f;
            const float c[3] = {v[0], v[1] + cy, v[2] + hz - 0.04f};
            // The box's 8 corners and 12 outward triangles (as Box() in PhysxProbe.cpp).
            float corner[8][3];
            for (int i = 0; i < 8; ++i)
                corner[i][0] = c[0] + (i & 1 ? hx : -hx), corner[i][1] = c[1] + (i & 2 ? hy : -hy), corner[i][2] = c[2] + (i & 4 ? hz : -hz);
            static const int idx[36] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
            float bt[36 * 3];
            for (int i = 0; i < 36; ++i) for (int k = 0; k < 3; ++k) bt[i * 3 + k] = corner[idx[i]][k];
            DkrBody b{};
            b.size = sizeof b, b.id = 7, b.kind = 0, b.firstTri = 0, b.triCount = 12;
            std::copy(c, c + 3, b.com);
            b.comRot[3] = 1.f;
            b.linVel[1] = -speed;
            b.invMass = 1.f / mass;
            const float ix = mass / 3.f * (hy * hy + hz * hz), iy = mass / 3.f * (hx * hx + hz * hz), iz = mass / 3.f * (hx * hx + hy * hy);
            b.invInertia[0] = 1.f / ix, b.invInertia[1] = 1.f / iy, b.invInertia[2] = 1.f / iz;
            setBodies(rt, &b, 1, bt, 12);
            static double lastShown = 0;
            if (ride >= lastShown + 0.5) { lastShown = ride; printf("  body script t=%.1fs skater (%.1f, %.1f, %.2f) %s %.1f m/s; car at y %.1f\n", ride, s.pos[0], s.pos[1], s.pos[2], s.state, s.speed, c[1]); }
        }
        else if (markerScript)
        {
            const double ride = s.tick / 60.0;
            pad = DkrPad{};
            pad.connected = 1;
            if (ride >= 1 && ride < 18 && int(ride * 60) % 30 < 4) pad.buttons |= 0x1000;
            if (ride >= 3 && ride < 3.2) pad.buttons |= 0x0102;
            if (ride >= 19 && ride < 21) pad.buttons |= 0x0101;
            static double lastShown = 0;
            if (ride >= lastShown + 1) { lastShown = ride; printf("  marker script t=%.0fs pos=(%.1f, %.1f, %.2f) %s\n", ride, s.pos[0], s.pos[1], s.pos[2], s.state); }
        }
        else if (padScript)
        {
            const double ride = s.tick / 60.0;  // riding time, from the skater's ticks (the load takes ~8 s)
            pad = DkrPad{};
            pad.connected = 1;
            if (ride >= 8 && ride < 11) pad.lx = 24000, pad.buttons = (int(ride * 4) % 2) ? 0x1000 : 0;
            else if (ride >= 11 && ride < 14) pad.lx = -24000;
            else if (ride >= 14 && ride < 14.13) pad.ry = -32767;
            else if (ride >= 14.13 && ride < 14.23) pad.ry = 32767;
        }
        if (audio)
        {
            // Pulled at the wall-clock rate, as an audio device would (the runtime counts a short pull as an underrun).
            static float buf[3][2048];
            static double owed = 0;
            owed = std::min(owed + dt * 48000.0, 2048.0);
            const uint32_t want = uint32_t(owed);
            owed -= want;
            uint32_t n[3] = {};
            for (uint32_t id = 0; id < 3 && want; ++id)
            {
                DkrAudioEmitter e{};
                e.size = sizeof e;
                n[id] = audioPull(rt, id, buf[id], want, &e);
                if (e.flags & kAudioEmitterSounding) ++audioSounding[id];
                for (uint32_t k = 0; k < n[id]; ++k)
                {
                    const double v = buf[id][k];
                    audioSq[id] += v * v, audioPeak[id] = std::max(audioPeak[id], std::fabs(v));
                }
                audioFrames[id] += n[id];
            }
            ++audioPulls;
            const uint32_t m = want;  // a short pull is silence in a device's stream too
            for (uint32_t k = 0; k < m; ++k)  // deck a little left, head a little right, body centred: a rough stereo picture
            {
                const float l = (k < n[0] ? buf[0][k] * 0.8f : 0) + (k < n[1] ? buf[1][k] * 0.7f : 0) + (k < n[2] ? buf[2][k] * 0.5f : 0);
                const float r = (k < n[0] ? buf[0][k] * 0.6f : 0) + (k < n[1] ? buf[1][k] * 0.7f : 0) + (k < n[2] ? buf[2][k] * 0.7f : 0);
                wav.push_back(int16_t(std::clamp(l, -1.f, 1.f) * 32767)), wav.push_back(int16_t(std::clamp(r, -1.f, 1.f) * 32767));
            }
        }
        if (captureAt && !captured && s.status == 2 && s.tick >= captureAt)
        {
            captured = true;
            printf("  capture requested at tick %llu: %u\n", s.tick, capture(rt));
        }
        if (resumeTest && !parkedOnce && s.status == 2 && s.tick > 600)
        {
            parkedOnce = true;
            printf("  park at tick %llu (%s, %.1f m from the spawn): %u\n", s.tick, s.state, std::hypot(s.pos[0] - v[0], s.pos[1] - v[1]), park(rt));
            Sleep(1000);
            DkrState p2{};
            p2.size = sizeof p2;
            state(rt, &p2);
            printf("  parked: status %u\n", p2.status);
            resumeAt = GetTickCount64();
            if (resumeHere)
            {
                const float here[3] = {s.pos[0], s.pos[1], s.pos[2]};
                std::vector<float> t2;
                std::vector<uint8_t> f2;
                Near(all, here, 40.f, t2, f2);
                printf("  resume where the skater is (%.1f, %.1f, %.2f): start %u\n", here[0], here[1], here[2], start(rt, t2.data(), f2.data(), (uint32_t)f2.size(), here, 0.f, 1));
            }
            else printf("  resume at the spawn: start %u\n", start(rt, tris.data(), flags.data(), (uint32_t)flags.size(), v, yaw + 90.f, 1));
        }
        if (resumeTest && parkedOnce && !resumed && s.status == 2 && GetTickCount64() > resumeAt + 50)
        {
            resumed = true;
            printf("  resumed: riding %.0f ms after the start call, at (%.1f, %.1f, %.2f), %.1f m from the spawn\n", double(GetTickCount64() - resumeAt),
                   s.pos[0], s.pos[1], s.pos[2], std::hypot(s.pos[0] - v[0], s.pos[1] - v[1]));
        }
        // P1-6: the plugin's clock (StageB.cpp TakeSnapshot and Blend), one tick behind the runtime's.
        const bool rolling = s.status == 2 && s.tick > 0 && std::string(s.state).rfind("PhysicsGround", 0) == 0 && s.speed > 2.f;
        if (s.status == 2 && s.tick > 0 && s.period > 0)
        {
            double pending = 0;
            if (s.steps <= posted && posted - s.steps < 64)
                for (uint64_t i = s.steps; i < posted; ++i) pending += std::clamp(postedDt[i % 64], 0.f, 0.25f);
            const double show = double(s.tick) + s.subTick + pending / s.period - 1.0;
            byClock.Sample(s.tick, s.pos, nowMs);
            const double a = byClock.curT > byClock.prevT ? std::clamp((show - double(byClock.prevT)) / double(byClock.curT - byClock.prevT), 0.0, 1.0) : 1.0;
            byClock.Place(float(a), rolling, double(s.speed) * dt);
        }
        if (step(rt, dt, &pad)) postedDt[posted++ % 64] = dt;
        if (frame > 0) dtSum += dt, dtMax = std::max<double>(dtMax, dt), ++dtN;
        if (jitter) spin(uniform(0.0, 2.5));
        {
            // 0.17.0: the state read after posting, interpolated by when each tick was noticed (QPC wall time).
            DkrState s2{};
            s2.size = sizeof s2;
            state(rt, &s2);
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            const double qms = 1000.0 * double(q.QuadPart) / double(freq.QuadPart);
            if (s2.status == 2 && s2.tick > 0)
            {
                byNoticing.Sample(s2.tick, s2.pos, qms);
                byNoticing.Place(std::clamp(float((qms - byNoticing.curAt) / (1000.0 / 60.0)), 0.f, 1.f), rolling, double(s.speed) * dt);
            }
        }
        if (audioOut)
        {
            DkrPose p{};
            p.size = sizeof p;
            const bool cam = getPose && getPose(rt, &p) && p.camValid;
            float r[3] = {p.camFwd[1], -p.camFwd[0], 0.f};
            const float rl = std::hypot(r[0], r[1]);
            if (rl > 1e-4f) r[0] /= rl, r[1] /= rl;
            skateaudio::Frame(cam && rl > 1e-4f ? p.camPos : nullptr, p.camFwd, r, false, outLog);
        }
        if (c3 && s.status == 2 && s.tick > 0)
        {
            DkrPose p{};
            p.size = sizeof p;
            if (getPose(rt, &p))
            {
                ++poses;
                bool ok = std::isfinite(p.hips[0]) && std::isfinite(p.hips[1]) && std::isfinite(p.hips[2]);
                for (auto& q : p.rot)
                {
                    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                    ok = ok && std::isfinite(n);
                    qErr = std::max(qErr, std::fabs(n - 1.f));
                }
                if (!ok) ++poseBad;
                if (p.tick != s.tick) ++poseOtherTick;
                if (p.unreachable) ++unreach;
                const float dz = p.hips[2] - p.root[2];
                if (p.camValid)
                {
                    ++cams;
                    const float dx = p.camPos[0] - p.root[0], dy = p.camPos[1] - p.root[1], dz2 = p.camPos[2] - p.root[2];
                    const float d = std::sqrt(dx * dx + dy * dy + dz2 * dz2);
                    const float fl = std::sqrt(p.camFwd[0] * p.camFwd[0] + p.camFwd[1] * p.camFwd[1] + p.camFwd[2] * p.camFwd[2]);
                    if (!std::isfinite(d) || std::fabs(fl - 1.f) > 1e-3f) ++camBad;
                    camDistMin = std::min(camDistMin, d), camDistMax = std::max(camDistMax, d);
                    camUpMin = std::min(camUpMin, dz2), camUpMax = std::max(camUpMax, dz2);
                }
                if (p.boardValid)
                {
                    ++boards;
                    const float* q = p.boardRot;
                    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                    if (!std::isfinite(n) || std::fabs(n - 1.f) > 1e-3f || !std::isfinite(p.boardPos[0])) ++boardBad;
                    if (std::string(s.state).rfind("PhysicsGround", 0) == 0)
                    {
                        const float upz = 1.f - 2.f * (q[0] * q[0] + q[1] * q[1]);  // z of the board's rotated +Z
                        boardUpMin = std::min(boardUpMin, upz);
                        const float d = std::sqrt((p.boardPos[0] - p.root[0]) * (p.boardPos[0] - p.root[0]) + (p.boardPos[1] - p.root[1]) * (p.boardPos[1] - p.root[1]) +
                                                  (p.boardPos[2] - p.root[2]) * (p.boardPos[2] - p.root[2]));
                        boardRootMax = std::max(boardRootMax, d);
                    }
                }
                if (std::string(s.state).rfind("PhysicsGround", 0) == 0) hipsMin = std::min(hipsMin, dz), hipsMax = std::max(hipsMax, dz);
                const std::string st(s.state);
                if (st.rfind("Wipeout", 0) == 0 || st.rfind("Biped", 0) == 0)
                {
                    ++downPoses;
                    if (!ok) ++downBad;
                    if (p.unreachable) ++downUnreach;
                    downDzMin = std::min(downDzMin, dz), downDzMax = std::max(downDzMax, dz);
                    downFar = std::max(downFar, std::hypot(p.hips[0] - p.root[0], p.hips[1] - p.root[1]));
                }
            }
        }
        float c[3];
        if (wants(rt, c))
        {
            std::vector<float> t2;
            std::vector<uint8_t> f2;
            Near(all, c, 40.f, t2, f2);
            install(rt, t2.data(), f2.data(), (uint32_t)f2.size(), c);
            printf("  re-centre (%.1f, %.1f, %.1f): %zu triangles\n", c[0], c[1], c[2], f2.size());
        }
        if (wall >= logAt)
        {
            logAt = wall + 2;
            printf("  t=%5.1fs status=%u tick=%llu %-18s pos=(%.1f, %.1f, %.2f) speed=%.2f ticks/s=%.1f step=%.3f/%.3f ms tris=%u rails=%u rebuilds=%u grind=%u air=%u trick=%s\n",
                   wall, s.status, s.tick, s.state, s.pos[0], s.pos[1], s.pos[2], s.speed, s.ticksPerS, s.stepMsAvg, s.stepMsMax,
                   s.triangles, s.rails, s.collisionBuilds, s.grindTicks, s.airTicks, s.trick);
        }
        if (fps > 0)
        {
            LARGE_INTEGER q;
            const double frameS = 1.0 / fps + (jitter ? uniform(-0.0015, 0.0015) : 0.0);
            do { std::this_thread::yield(); QueryPerformanceCounter(&q); } while (double(q.QuadPart - now.QuadPart) / double(freq.QuadPart) < frameS);
        }
        else std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    if (c3)
    {
        DkrPose r{};
        r.size = sizeof r;
        const uint32_t rv = refPose(rt, &r);
        printf("C3: %u poses read, %u not finite, %u read with another tick than the state, %u with a limb out of reach; quaternion length error %.2e; "
               "hips %.3f..%.3f m above the skater root while rolling; reference pose %s: hips (%.3f, %.3f, %.3f), %u bones\n",
               poses, poseBad, poseOtherTick, unreach, qErr, hipsMin, hipsMax, rv ? "valid" : "MISSING", r.hips[0], r.hips[1], r.hips[2], r.bones);
        printf("C3b: board in %u poses (%u bad); while rolling its up axis z >= %.3f and it sits within %.3f m of the skater root\n", boards, boardBad, boardUpMin, boardRootMax);
        if (downPoses)
            printf("C3 down: %u poses while down or on foot, %u not finite, %u with a limb out of reach; hips %.3f..%.3f m above the skater root, at most %.3f m from it across\n",
                   downPoses, downBad, downUnreach, downDzMin, downDzMax, downFar);
        else printf("C3 down: no poses while down or on foot\n");
        printf("C4: camera in %u poses (%u bad); %.2f..%.2f m from the skater root, %.2f..%.2f m above it\n", cams, camBad, camDistMin, camDistMax, camUpMin, camUpMax);
    }
    if (audio)
    {
        DkrAudioStatus st{};
        st.size = sizeof st;
        audioStatus(rt, &st);
        const char* names[3] = {"deck", "body", "head"};
        printf("audio: state %u, %u voices, %u underruns, %llu frames rendered (%s); %u pulls\n", st.state, st.voices, st.underruns, st.frames, st.message, audioPulls);
        for (int id = 0; id < 3; ++id)
            printf("audio: %s: %.1f s pulled, RMS %.4f, peak %.3f, sounding in %u of %u pulls\n", names[id], audioFrames[id] / 48000.0,
                   audioFrames[id] ? std::sqrt(audioSq[id] / audioFrames[id]) : 0.0, audioPeak[id], audioSounding[id], audioPulls);
        std::string path = std::string(argv[3]) + "/skate_audio.wav";
        if (FILE* f = fopen(path.c_str(), "wb"))
        {
            const uint32_t data = uint32_t(wav.size() * 2), rate = 48000;
            const uint16_t ch = 2, bits = 16, fmt = 1, align = 4;
            const uint32_t riff = 36 + data, fmtLen = 16, byteRate = rate * 4;
            fwrite("RIFF", 1, 4, f), fwrite(&riff, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f), fwrite(&fmtLen, 4, 1, f), fwrite(&fmt, 2, 1, f);
            fwrite(&ch, 2, 1, f), fwrite(&rate, 4, 1, f), fwrite(&byteRate, 4, 1, f), fwrite(&align, 2, 1, f), fwrite(&bits, 2, 1, f);
            fwrite("data", 1, 4, f), fwrite(&data, 4, 1, f), fwrite(wav.data(), 2, wav.size(), f);
            fclose(f);
            printf("audio: wrote %s (%.1f s)\n", path.c_str(), wav.size() / 2 / 48000.0);
        }
    }
    printf("P1-6: %u frames, mean frame time %.1f ms, max %.1f ms\n", dtN, dtN ? 1000.0 * dtSum / dtN : 0.0, 1000.0 * dtMax);
    byClock.Print("runtime's clock (0.18.0)");
    byNoticing.Print("time noticed (0.17.0)");
    DkrState s{};
    s.size = sizeof s;
    state(rt, &s);
    printf("final: tick=%llu state=%s pos=(%.2f, %.2f, %.2f) grind ticks=%u air ticks=%u rebuilds=%u\n", s.tick, s.state, s.pos[0],
           s.pos[1], s.pos[2], s.grindTicks, s.airTicks, s.collisionBuilds);
    destroy(rt);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return 0;
}
