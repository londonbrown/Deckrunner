#include "StageB.hpp"
#include "Data.hpp"

#include "Audio.hpp"
#include "PhysxProbe.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace stageb
{
namespace
{
using AbiFn = uint32_t (*)();
using CreateFn = void* (*)(const char*, const char*);
using StartFn = uint32_t (*)(void*, const float*, const uint8_t*, uint32_t, const float*, float, uint32_t);
using StepFn = uint32_t (*)(void*, float, const DkrPad*);
using StateFn = uint32_t (*)(const void*, DkrState*);
using WantsFn = uint32_t (*)(const void*, float*);
using InstallFn = uint32_t (*)(void*, const float*, const uint8_t*, uint32_t, const float*);
using DestroyFn = void (*)(void*);
using LoadRigFn = uint32_t (*)(void*, const char*);
using PoseFn = uint32_t (*)(const void*, DkrPose*);
using SnapshotFn = uint32_t (*)(const void*, DkrState*, DkrPose*);
using ParkFn = uint32_t (*)(void*);
using CaptureFn = uint32_t (*)(void*);
using SetBodiesFn = uint32_t (*)(void*, const DkrBody*, uint32_t, const float*, uint32_t);
using AudioConfigureFn = uint32_t (*)(void*, const char*, float);
using AudioPullFn = uint32_t (*)(const void*, uint32_t, float*, uint32_t, DkrAudioEmitter*);
using AudioPausedFn = uint32_t (*)(const void*, uint32_t);
using AudioStatusFn = uint32_t (*)(const void*, DkrAudioStatus*);

struct Api
{
    HMODULE dll = nullptr;
    AbiFn abi = nullptr;
    CreateFn create = nullptr;
    StartFn start = nullptr;
    StepFn step = nullptr;
    StateFn state = nullptr;
    WantsFn wants = nullptr;
    InstallFn install = nullptr;
    DestroyFn destroy = nullptr;
    LoadRigFn loadRig = nullptr;
    PoseFn pose = nullptr, reference = nullptr;
    SnapshotFn snapshot = nullptr;  // ABI 8
    ParkFn park = nullptr;
    CaptureFn capture = nullptr;  // ABI 9
    SetBodiesFn setBodies = nullptr;  // ABI 10
    skateaudio::Fns audio;  // ABI 7
} g_api;
bool g_parked = false;  // C5: the runtime keeps Skate loaded between rides
std::string g_rig;  // C3: rig JSON for the next session

void* g_rt = nullptr;
float g_shift[3] = {};
uint64_t g_capsule = 0;                  // V's capsule actor from the last FindShift (fast re-check)
float g_start[3] = {};
double g_logTimer = 0;
uint64_t g_lastTick = 0;
constexpr float kRadius = 40.f;          // collision gathered around the skater (m)
constexpr size_t kMaxTriangles = 400'000;
constexpr double kScanBudgetMs = 2.0;    // main-thread time per frame for a re-centre's scene walk

// P1-6 (0.18.0): this frame's view of the runtime, read once before the frame's step is posted.
struct Snapshot
{
    DkrState state{};
    DkrPose pose{};
    bool have = false;
    double showTick = NAN;
} g_snap;
uint64_t g_posted = 0;       // dkr_step calls the runtime accepted (its DkrState.steps counts the same jobs)
// P1-14 (0.22.0): cars near the skater, sent to the runtime every frame as moving geometry.
constexpr float kBodyRadius = 30.f;
constexpr uint32_t kMaxBodies = 24;
probe::Vehicles g_vehicles;
std::vector<probe::MovingBody> g_bodies;
std::vector<float> g_bodyTris;
std::vector<DkrBody> g_dkrBodies;
struct BodyLog
{
    double at = 0, msSum = 0, msMax = 0;
    uint32_t frames = 0, maxBodies = 0, found = 0, dropped = 0;
} g_bodyLog;
float g_postedDt[64] = {};   // their dt, by sequence number mod 64

// One re-centre in flight: scene walk on the main thread over several frames, then the copy on a worker thread.
struct Collected
{
    std::vector<float> tris;
    std::vector<uint8_t> flags;
    probe::Collector::Stats stats;
};
struct Recentre
{
    enum class Phase { Idle, Scanning, Extracting } phase = Phase::Idle;
    float center[3] = {}, v[3] = {};
    bool vOk = false, fullShift = false;
    double shiftMs = 0, requestedAt = 0;
    std::unique_ptr<probe::Collector> collector;
    std::future<Collected> extract;
} g_rc;

std::filesystem::path PluginDir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&PluginDir), &self);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::string Utf8(const std::filesystem::path& p)
{
    const std::wstring w = p.wstring();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

double NowMs()
{
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

template<class... A> void Logf(LogFn log, const char* fmt, A... a)
{
    char b[1024];
    snprintf(b, sizeof b, fmt, a...);
    log(b);
}

bool LoadApi(LogFn log)
{
    if (g_api.dll) return true;
    const auto dll = PluginDir() / L"runtime" / L"deckrunner_runtime.dll";
    // Loaded once and never unloaded: the runtime's worker thread may outlive a session.
    g_api.dll = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_api.dll) { Logf(log, "stage B: cannot load %s (error %lu)", Utf8(dll).c_str(), GetLastError()); return false; }
    auto get = [](const char* n) { return GetProcAddress(g_api.dll, n); };
    g_api.abi = reinterpret_cast<AbiFn>(get("dkr_abi_version"));
    g_api.create = reinterpret_cast<CreateFn>(get("dkr_create"));
    g_api.start = reinterpret_cast<StartFn>(get("dkr_start"));
    g_api.step = reinterpret_cast<StepFn>(get("dkr_step"));
    g_api.state = reinterpret_cast<StateFn>(get("dkr_get_state"));
    g_api.wants = reinterpret_cast<WantsFn>(get("dkr_wants_collision"));
    g_api.install = reinterpret_cast<InstallFn>(get("dkr_install_collision"));
    g_api.destroy = reinterpret_cast<DestroyFn>(get("dkr_destroy"));
    g_api.loadRig = reinterpret_cast<LoadRigFn>(get("dkr_load_rig"));
    g_api.pose = reinterpret_cast<PoseFn>(get("dkr_get_pose"));
    g_api.snapshot = reinterpret_cast<SnapshotFn>(get("dkr_get_snapshot"));
    g_api.reference = reinterpret_cast<PoseFn>(get("dkr_reference_pose"));
    g_api.park = reinterpret_cast<ParkFn>(get("dkr_park"));
    g_api.capture = reinterpret_cast<CaptureFn>(get("dkr_capture"));
    g_api.setBodies = reinterpret_cast<SetBodiesFn>(get("dkr_set_bodies"));
    g_api.audio.configure = reinterpret_cast<AudioConfigureFn>(get("dkr_audio_configure"));
    g_api.audio.pull = reinterpret_cast<AudioPullFn>(get("dkr_audio_pull"));
    g_api.audio.setPaused = reinterpret_cast<AudioPausedFn>(get("dkr_audio_set_paused"));
    g_api.audio.status = reinterpret_cast<AudioStatusFn>(get("dkr_audio_status"));
    if (!g_api.abi || !g_api.create || !g_api.start || !g_api.step || !g_api.state || !g_api.wants || !g_api.install || !g_api.destroy ||
        !g_api.loadRig || !g_api.pose || !g_api.reference || !g_api.park || !g_api.snapshot || !g_api.capture || !g_api.setBodies)
    {
        log("stage B: runtime DLL is missing exports");
        return false;
    }
    if (g_api.abi() != 10) { Logf(log, "stage B: runtime ABI %u, expected 10", g_api.abi()); return false; }
    return true;
}

std::string AssetsPath()
{
    return data::Utf8(data::SkateAssets());  // R1: from the data folder (Data.hpp)
}
} // namespace

bool Active() { return g_rt != nullptr && !g_parked; }

// P0-2: the runtime drops a session whose tick failed, so the next start reloads Skate (a few seconds) instead of
// resuming. Kept here so the log says so; the first in-game ride logged "resuming (no reload)" over a reload.
bool g_reload = false;

const char* Failed()
{
    static DkrState s{};
    if (!g_rt || g_parked) return nullptr;
    s = DkrState{};
    s.size = sizeof s;
    if (!g_api.state(g_rt, &s) || s.status != 3) return nullptr;
    g_reload = true;
    s.error[sizeof s.error - 1] = 0;
    return s.error[0] ? s.error : "no message";
}

// P1-6: the snapshot, and the tick to show: one behind the runtime's clock, counting steps it hasn't handled yet (the
// read comes before this frame's step is posted, so normally none). Placing things one tick behind keeps both ticks
// they interpolate between already published.
void TakeSnapshot()
{
    g_snap = Snapshot{};
    g_snap.state.size = sizeof g_snap.state;
    g_snap.pose.size = sizeof g_snap.pose;
    g_snap.have = g_rt && g_api.snapshot(g_rt, &g_snap.state, &g_snap.pose);
    const DkrState& s = g_snap.state;
    if (!g_snap.have || s.status != 2 || s.tick == 0 || !(s.period > 0) || !std::isfinite(s.subTick)) return;
    double pending = 0;
    if (s.steps <= g_posted && g_posted - s.steps < 64)
        for (uint64_t i = s.steps; i < g_posted; ++i) pending += std::clamp(g_postedDt[i % 64], 0.f, 0.25f);
    g_snap.showTick = double(s.tick) + double(s.subTick) + pending / double(s.period) - 1.0;
}

double ShowTick() { return g_snap.showTick; }

bool CapsuleOffset(const float v[3], float& metres)
{
    metres = 0;
    if (!g_capsule) return false;
    float err = 1e30f;
    probe::ConfirmShift({v[0], v[1], v[2]}, g_shift, g_capsule, err);
    if (!(err < 1e29f)) return false;
    metres = err;
    return true;
}

void ActorCounts(uint32_t& statics, uint32_t& dynamics) { probe::ActorCounts(statics, dynamics); }

float Blend(uint64_t prev, uint64_t cur)
{
    const double t = g_snap.showTick;
    if (!std::isfinite(t) || cur <= prev) return 1.f;
    return float(std::clamp((t - double(prev)) / double(cur - prev), 0.0, 1.0));
}

bool Skater(float pos[3], float vel[3], float& heading, uint64_t& tick, char state[48])
{
    if (!g_rt || g_parked || !g_snap.have) return false;
    const DkrState& s = g_snap.state;
    // Riding and at least one tick published: the runtime reports status 2 at "session ready", a step before the first
    // tick sets the position, and C2 run 1 teleported V to the zeroed position, the map's origin.
    if (s.status != 2 || s.tick == 0) return false;
    std::copy(s.pos, s.pos + 3, pos);
    std::copy(s.velocity, s.velocity + 3, vel);
    heading = s.heading;
    tick = s.tick;
    std::copy(s.state, s.state + 48, state);
    state[47] = 0;
    return true;
}

void SetRig(const std::string& path) { g_rig = path; }

bool Pose(DkrPose& out)
{
    out = DkrPose{};
    out.size = sizeof out;
    if (!g_rt || g_parked || !g_snap.have) return false;
    out = g_snap.pose;
    return out.valid || out.boardValid || out.camValid;
}

bool ReferencePose(DkrPose& out)
{
    out = DkrPose{};
    out.size = sizeof out;
    return g_rt && g_api.reference(g_rt, &out) && out.valid;
}

bool Capture(LogFn log)
{
    if (!g_rt || g_parked) { log("stage B: capture: not riding"); return false; }
    const bool ok = g_api.capture(g_rt) != 0;
    log(ok ? "stage B: capture requested: the skater's last 5 s and the collision around them, written beside the runtime log (its log names the files)"
           : "stage B: capture request failed");
    return ok;
}

bool Start(const float v[3], float yaw, LogFn log)
{
    if (g_rt && !g_parked) { log("stage B: already running (Ctrl+Shift+F10 stops it)"); return false; }
    const bool resume = g_rt != nullptr;  // C5: Skate is still loaded from the last ride
    if (!resume && !LoadApi(log)) return false;
    float err = 0;
    uint32_t caps = 0;
    if (!probe::FindShift({v[0], v[1], v[2]}, g_shift, err, caps, &g_capsule))
    {
        Logf(log, "stage B: V's capsule not found among %u capsules (best error %.3f m); not starting", caps, err);
        return false;
    }
    Logf(log, "stage B: PhysX shift (world = physx + shift) = (%.0f, %.0f, %.0f) from V's capsule (error %.3f m, %u capsules)",
         g_shift[0], g_shift[1], g_shift[2], err, caps);
    std::vector<float> tris;
    std::vector<uint8_t> flags;
    probe::Collector::Stats st;
    // The first set is gathered in one go: Skate is about to spend ~8 s loading anyway.
    const uint32_t n = probe::CollectTriangles({v[0], v[1], v[2]}, g_shift, kRadius, kMaxTriangles, tris, flags, &st);
    uint32_t physical = 0;
    for (auto f : flags) physical += f;
    Logf(log, "stage B: collected %u triangles within %.0f m (%u physical, %u detail) on the main thread: scene walk %.1f ms "
              "(%u of %u actors near, %u of them kinematic props with %u shapes), copy %.1f ms (%u meshes and height fields, %u skipped as freed)",
         n, kRadius, physical, n - physical, st.scanMs, st.actorsNear, st.actorsVisited, st.props, st.propShapes, st.extractMs, st.meshJobs, st.stale);
    wchar_t local[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const auto out = std::filesystem::path(local) / L"Deckrunner" / L"stageb";
    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    const std::string assets = AssetsPath();
    if (!resume)
    {
        g_rt = g_api.create(assets.c_str(), Utf8(out).c_str());
        if (!g_rt) { log("stage B: dkr_create failed"); return false; }
        g_posted = 0;  // the new runtime counts its steps from zero
        // C3: the rig is fitted first (milliseconds), so V's reference pose is available while Skate loads (about 8 s).
        if (!g_rig.empty()) Logf(log, "stage C3: rig %s %s", g_rig.c_str(), g_api.loadRig(g_rt, g_rig.c_str()) ? "sent to the runtime (its log says whether it fitted)" : "could not be sent");
        // Skate audio: the sound banks load on the runtime's audio thread beside Skate (about 6 s).
        skateaudio::Begin(g_rt, g_api.audio, log);
    }
    if (!g_api.start(g_rt, tris.data(), flags.data(), n, v, yaw, 1))
    {
        log("stage B: dkr_start failed");
        return false;
    }
    g_parked = false;
    g_snap = Snapshot{};
    if (resume && g_reload) log("stage B: reloading Skate after the engine error (a few seconds)");
    else if (resume) log("stage B: resuming the loaded session (no reload)");
    g_reload = false;
    g_start[0] = v[0], g_start[1] = v[1], g_start[2] = v[2];
    g_logTimer = 0;
    g_lastTick = 0;
    Logf(log, "stage B: started at (%.2f, %.2f, %.2f) yaw %.1f with autopilot; assets %s; runtime log in %s", v[0], v[1], v[2], yaw,
         assets.c_str(), Utf8(out).c_str());
    return true;
}

// P1-14: the cars near the skater this frame, to the runtime (every frame, an empty set when none are near). Summarised
// in the log every 5 s.
void SendBodies(LogFn log)
{
    if (!g_snap.have || g_snap.state.status != 2 || g_snap.state.tick == 0) return;
    const float* p = g_snap.state.pos;
    probe::Vehicles::Stats st;
    g_vehicles.Update({p[0], p[1], p[2]}, g_shift, kBodyRadius, kMaxBodies, g_bodies, g_bodyTris, st);
    g_dkrBodies.clear();
    for (const auto& b : g_bodies)
    {
        DkrBody d{};
        d.size = sizeof d, d.id = b.id, d.kind = b.kind, d.firstTri = b.firstTri, d.triCount = b.triCount;
        std::copy(b.com, b.com + 3, d.com);
        std::copy(b.comRot, b.comRot + 4, d.comRot);
        std::copy(b.linVel, b.linVel + 3, d.linVel);
        std::copy(b.angVel, b.angVel + 3, d.angVel);
        d.invMass = b.invMass;
        std::copy(b.invInertia, b.invInertia + 3, d.invInertia);
        g_dkrBodies.push_back(d);
    }
    g_api.setBodies(g_rt, g_dkrBodies.data(), uint32_t(g_dkrBodies.size()), g_bodyTris.data(), uint32_t(g_bodyTris.size() / 9));
    BodyLog& l = g_bodyLog;
    ++l.frames, l.msSum += st.ms, l.msMax = std::max(l.msMax, st.ms);
    l.maxBodies = std::max<uint32_t>(l.maxBodies, uint32_t(g_bodies.size())), l.found += st.found, l.dropped += st.dropped;
    const double now = NowMs();
    if (l.at == 0) l.at = now + 5000;
    if (now >= l.at)
    {
        if (l.maxBodies > 0 || l.found > 0)
            Logf(log, "stage B: cars over 5 s: up to %u sent to Skate (%u tracked now; %u found, %u dropped); tracking %.2f ms a frame on average, %.2f ms at most",
                 l.maxBodies, st.tracked, l.found, l.dropped, l.msSum / std::max(1u, l.frames), l.msMax);
        l = BodyLog{};
        l.at = now + 5000;
    }
}

void Frame(float dt, const DkrPad& pad, PlayerPosFn playerPos, LogFn log)
{
    if (!g_rt || g_parked) return;
    TakeSnapshot();  // P1-6: before this frame's step, so the clock and the samples are the worker's settled ones
    SendBodies(log);  // P1-14: before the step, so its ticks see this frame's cars
    if (g_api.step(g_rt, dt, &pad)) g_postedDt[g_posted++ % 64] = dt;
    Recentre& rc = g_rc;
    double frameBudget = kScanBudgetMs;
    if (rc.phase == Recentre::Phase::Idle && g_api.wants(g_rt, rc.center))
    {
        const double t0 = NowMs();
        rc.requestedAt = t0;
        // The game moves its PhysX origin on a 1,024 m grid as V travels: re-check it against V's capsule first,
        // cheaply against the capsule found last time, else by walking every scene (3-4 ms).
        float shift[3] = {}, err = 0;
        uint32_t caps = 0;
        rc.vOk = playerPos && playerPos(rc.v);
        rc.fullShift = false;
        const probe::Vec3 v{rc.v[0], rc.v[1], rc.v[2]};
        bool ok = rc.vOk && probe::ConfirmShift(v, g_shift, g_capsule, err);
        if (!ok && rc.vOk)
        {
            rc.fullShift = true;
            ok = probe::FindShift(v, shift, err, caps, &g_capsule);
            if (ok && (shift[0] != g_shift[0] || shift[1] != g_shift[1] || shift[2] != g_shift[2]))
                Logf(log, "stage B: PhysX origin moved: shift (%.0f, %.0f, %.0f) -> (%.0f, %.0f, %.0f)", g_shift[0], g_shift[1], g_shift[2],
                     shift[0], shift[1], shift[2]);
            if (ok) std::copy(shift, shift + 3, g_shift);
        }
        if (!ok) Logf(log, "stage B: could not re-check the PhysX shift (V %s, best capsule error %.3f m); keeping it", rc.vOk ? "found" : "missing", err);
        rc.shiftMs = NowMs() - t0;
        frameBudget -= rc.shiftMs;
        rc.collector = std::make_unique<probe::Collector>();
        rc.collector->Begin({rc.center[0], rc.center[1], rc.center[2]}, g_shift, kRadius, kMaxTriangles);
        rc.phase = Recentre::Phase::Scanning;
    }
    if (rc.phase == Recentre::Phase::Scanning && frameBudget > 0.25 && rc.collector->Scan(frameBudget))
    {
        // Scene walk finished on this thread; copying the meshes needs no PhysX calls, so it moves off the main thread.
        rc.extract = std::async(std::launch::async, [c = std::move(rc.collector)]() {
            Collected r;
            c->Extract(r.tris, r.flags);
            r.stats = c->stats;
            return r;
        });
        rc.phase = Recentre::Phase::Extracting;
    }
    if (rc.phase == Recentre::Phase::Extracting && rc.extract.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        Collected r = rc.extract.get();
        const uint32_t n = (uint32_t)r.flags.size();
        g_api.install(g_rt, r.tris.data(), r.flags.data(), n, rc.center);
        const auto& st = r.stats;
        const float dvx = rc.v[0] - rc.center[0], dvy = rc.v[1] - rc.center[1];
        Logf(log, "stage B: re-centre at (%.1f, %.1f, %.1f): %u triangles in %.0f ms; main thread: shift check %.2f ms (%s), scene walk "
                  "%.1f ms over %u frames (max %.1f ms in one frame, %u of %u actors near, %u kinematic props); copy %.1f ms off the main thread (%u meshes "
                  "and height fields, %u skipped as freed); V at (%.1f, %.1f, %.1f), %.0f m away",
             rc.center[0], rc.center[1], rc.center[2], n, NowMs() - rc.requestedAt, rc.shiftMs, rc.fullShift ? "full search" : "known capsule",
             st.scanMs, st.frames, st.scanMaxMs, st.actorsNear, st.actorsVisited, st.props, st.extractMs, st.meshJobs, st.stale, rc.v[0], rc.v[1],
             rc.v[2], std::sqrt(dvx * dvx + dvy * dvy));
        rc.phase = Recentre::Phase::Idle;
    }
    g_logTimer += dt;
    if (g_logTimer < 2.0) return;
    g_logTimer = 0;
    DkrState s{};
    s.size = sizeof s;
    if (!g_api.state(g_rt, &s)) return;
    if (s.status != 2)
    {
        Logf(log, "stage B: status=%u (%s) %s", s.status, s.status == 1 ? "loading Skate" : s.status == 3 ? "error" : "idle", s.error);
        return;
    }
    const float dx = s.pos[0] - g_start[0], dy = s.pos[1] - g_start[1];
    Logf(log, "stage B: status=%u tick=%llu (+%llu) state=%s trick=%s pos=(%.2f, %.2f, %.2f) %.1f m from start speed=%.2f m/s "
              "ticks/s=%.1f step avg=%.3f ms max=%.3f ms tris=%u rails=%u rebuilds=%u grind=%u air=%u %s %.0f %s",
         s.status, s.tick, s.tick - g_lastTick, s.state, s.trick, s.pos[0], s.pos[1], s.pos[2], std::sqrt(dx * dx + dy * dy), s.speed,
         s.ticksPerS, s.stepMsAvg, s.stepMsMax, s.triangles, s.rails, s.collisionBuilds, s.grindTicks, s.airTicks,
         std::isfinite(s.heading) ? "autopilot heading" : "controller driving; pad", std::isfinite(s.heading) ? s.heading : float(pad.connected),
         s.error);
    g_lastTick = s.tick;
}

void Stop(LogFn log)
{
    if (!g_rt || g_parked) return;
    DkrState s{};
    s.size = sizeof s;
    g_api.state(g_rt, &s);
    Logf(log, "stage B: stopping at tick %llu, state %s, grind ticks %u, air ticks %u, rebuilds %u", s.tick, s.state, s.grindTicks,
         s.airTicks, s.collisionBuilds);
    // A copy still running on its worker owns only its own buffers: wait for it (milliseconds), then drop the result.
    if (g_rc.extract.valid()) g_rc.extract.wait();
    g_rc = Recentre{};
    g_vehicles.Clear();  // P1-14: the runtime drops its cars when parked
    g_bodyLog = BodyLog{};
    // C5: parked, not destroyed: Skate stays loaded and the next start resumes at once.
    g_api.park(g_rt);
    g_parked = true;
    g_snap = Snapshot{};
    log("stage B: parked (Skate stays loaded; the next start resumes without reloading)");
}
} // namespace stageb
