// Deckrunner Stage A probe: a read-only RED4ext plugin for Cyberpunk 2077 2.5.
//
// What it does, all read-only:
//  - From the Running game state (main-thread update), finds V via cpPlayerSystem and reads V's world position
//    and yaw through the game's own RTTI functions.
//  - Once V exists (5 s after spawning), and on Ctrl+Shift+F8, it:
//      * enumerates the game's PhysX scenes from PxGetPhysics() and counts the actors and shapes near V;
//      * dumps nearby world collision to an OBJ file;
//      * finds the ground under V from the collected triangles (no PhysX query);
//      * lists V's entity components;
//      * reads controller state via XInput;
//      * logs main-thread frame timing.
//  - Output goes to %LOCALAPPDATA%\Deckrunner\probe and to RED4ext's log. It writes nothing to the game,
//    its physics or its save data.
#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Scripting/Natives/Generated/Vector4.hpp>
#include <RED4ext/Scripting/Natives/entEntity.hpp>
#include <RED4ext/Scripting/Natives/entIComponent.hpp>

#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "Audio.hpp"
#include "Data.hpp"
#include "Keys.hpp"
#include "Pad.hpp"
#include "PhysxProbe.hpp"
#include "StageB.hpp"
#include "StageC.hpp"
#include "Visibility.hpp"
#include "Wheel.hpp"

namespace
{
RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;
std::filesystem::path g_out;
uint64_t g_frames = 0, g_probes = 0;
LARGE_INTEGER g_freq{}, g_last{};
double g_dtSum = 0, g_dtMax = 0;
uint32_t g_dtCount = 0;
DWORD g_mainThread = 0;
ULONGLONG g_playerSeenAt = 0;
bool g_autoDone = false, g_hotkeyDown = false;

template<class... A> void Log(const char* fmt, A... a) { g_sdk->logger->InfoF(g_handle, fmt, a...); }

// 0.28.0: the players' two keys, from deckrunner.ini [keys] (Keys.hpp); a missing or unreadable binding keeps its default.
keys::Chord g_keySkate, g_keyCapture;
keys::Chord LoadKey(const wchar_t* name, const char* fallback)
{
    keys::Chord c;
    std::string err;
    keys::Parse(fallback, c, err);
    wchar_t buf[128] = {};
    GetPrivateProfileStringW(L"keys", name, L"", buf, 128, data::Ini().c_str());
    std::string spec;
    for (const wchar_t* p = buf; *p; ++p) spec += *p < 128 ? static_cast<char>(*p) : '?';
    if (spec.empty()) return c;
    keys::Chord read;
    if (keys::Parse(spec, read, err)) return read;
    Log("deckrunner.ini [keys] %ls=%s: %s; using %s", name, spec.c_str(), err.c_str(), c.text.c_str());
    return c;
}

struct PlayerInfo
{
    bool ok = false;
    RED4ext::Handle<RED4ext::IScriptable> handle;
    RED4ext::Vector4 pos{};
    float yaw = 0;
    std::string className;
};

PlayerInfo GetPlayer()
{
    PlayerInfo p;
    auto engine = RED4ext::CGameEngine::Get();
    if (!engine || !engine->framework || !engine->framework->gameInstance) return p;
    if (!RED4ext::ExecuteFunction("cpPlayerSystem", "GetLocalPlayerMainGameObject", &p.handle) || !p.handle.instance)
        return p;
    auto cls = p.handle.instance->GetType();
    p.className = cls->GetName().ToString();
    auto fPos = cls->GetFunction("GetWorldPosition");
    auto fYaw = cls->GetFunction("GetWorldYaw");
    if (!fPos || !RED4ext::ExecuteFunction(p.handle.instance, fPos, &p.pos)) return p;
    if (fYaw) RED4ext::ExecuteFunction(p.handle.instance, fYaw, &p.yaw);
    p.ok = std::isfinite(p.pos.X) && std::isfinite(p.pos.Y) && std::isfinite(p.pos.Z);
    return p;
}

std::string Components(const PlayerInfo& p)
{
    auto rtti = RED4ext::CRTTISystem::Get();
    auto entityCls = rtti->GetClass("entEntity");
    if (!entityCls || !p.handle.instance->GetType()->IsA(entityCls)) return "player is not an entEntity";
    auto entity = reinterpret_cast<RED4ext::ent::Entity*>(p.handle.instance);
    std::map<std::string, int> byType;
    std::string animated;
    for (auto& c : entity->components)
    {
        if (!c.instance) continue;
        const std::string type = c.instance->GetType()->GetName().ToString();
        byType[type]++;
        if (type == "entAnimatedComponent" || type == "entSkinnedMeshComponent" || type == "entMorphTargetSkinnedMeshComponent")
            if (animated.size() < 1800) animated += type + ":" + c->name.ToString() + " ";
    }
    std::string s = std::to_string(entity->components.Size()) + " components; ";
    for (auto& [t, n] : byType) s += t + "x" + std::to_string(n) + " ";
    return s + "| skeleton/mesh: " + animated;
}

void LogLine(const char* s) { g_sdk->logger->Info(g_handle, s); }

stageb::DkrPad ReadPad()
{
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static GetStateFn get = [] {
        HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll");
        if (!m) m = LoadLibraryW(L"xinput1_4.dll");
        return m ? reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
    }();
    stageb::DkrPad pad{};
    XINPUT_STATE st{};
    if (get && get(0, &st) == ERROR_SUCCESS)
        pad = {1, st.Gamepad.wButtons, st.Gamepad.bLeftTrigger, st.Gamepad.bRightTrigger, st.Gamepad.sThumbLX,
               st.Gamepad.sThumbLY, st.Gamepad.sThumbRX, st.Gamepad.sThumbRY};
    // No Xbox pad: a PlayStation pad over HID (0.14.1; ported from SK8V, which reads the user's DualSense this way).
    // The game uses these natively, so XInput never sees them.
    else if (pspad::Pad ps; pspad::Read(ps))
        pad = {1, ps.buttons, ps.leftTrigger, ps.rightTrigger, ps.lx, ps.ly, ps.rx, ps.ry};
    return pad;
}

std::string XInputState()
{
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static GetStateFn get = [] {
        HMODULE m = GetModuleHandleW(L"XINPUT9_1_0.dll");
        if (!m) m = LoadLibraryW(L"xinput1_4.dll");
        return m ? reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
    }();
    if (!get) return "XInputGetState unavailable";
    std::string s;
    for (DWORD i = 0; i < 4; ++i)
    {
        XINPUT_STATE st{};
        if (get(i, &st) == ERROR_SUCCESS)
        {
            char b[128];
            snprintf(b, sizeof b, "pad%lu buttons=%04x LT=%u RT=%u L=(%d,%d) R=(%d,%d) ", i, st.Gamepad.wButtons,
                     st.Gamepad.bLeftTrigger, st.Gamepad.bRightTrigger, st.Gamepad.sThumbLX, st.Gamepad.sThumbLY,
                     st.Gamepad.sThumbRX, st.Gamepad.sThumbRY);
            s += b;
        }
    }
    // PlayStation pads (HID, read-only); the reader starts on first use and finds a pad within moments.
    pspad::Pad ps;
    const char* name = nullptr;
    pspad::Read(ps, &name);
    for (int i = 0; !name && i < 20; ++i) Sleep(10), pspad::Read(ps, &name);
    if (name)
    {
        char b[160];
        snprintf(b, sizeof b, "%s%s buttons=%04x L2=%u R2=%u L=(%d,%d) R=(%d,%d)", s.empty() ? "" : "; ", name, ps.buttons, ps.leftTrigger,
                 ps.rightTrigger, ps.lx, ps.ly, ps.rx, ps.ry);
        s += b;
    }
    return s.empty() ? "no XInput or PlayStation pad connected" : s;
}

void RunProbe(const char* why)
{
    const PlayerInfo p = GetPlayer();
    if (!p.ok) { Log("probe (%s): player not available", why); return; }
    ++g_probes;
    const auto stem = g_out / ("probe-" + std::to_string(g_probes));
    FILE* obj = nullptr;
    _wfopen_s(&obj, (stem.wstring() + L".obj").c_str(), L"w");
    if (obj) fprintf(obj, "# Deckrunner probe %llu (%s): Cyberpunk world collision within 24 m of V at %.3f %.3f %.3f, Z up, metres\n",
                     g_probes, why, p.pos.X, p.pos.Y, p.pos.Z);
    // V's entity and component addresses: PhysX actors owned by V reference one of these through userData.
    std::vector<uint64_t> owners{reinterpret_cast<uint64_t>(p.handle.instance)};
    for (auto& c : reinterpret_cast<RED4ext::ent::Entity*>(p.handle.instance)->components)
        if (c.instance) owners.push_back(reinterpret_cast<uint64_t>(c.instance));
    static probe::Result r;  // large (16 scenes); keep it off the stack
    const ULONGLONG t0 = GetTickCount64();
    probe::Run({p.pos.X, p.pos.Y, p.pos.Z}, 64.f, 24.f, 3'000'000, obj, owners.data(), (uint32_t)owners.size(), r);
    const ULONGLONG ms = GetTickCount64() - t0;
    if (obj) fclose(obj);

    Log("probe %llu (%s): V class=%s pos=(%.3f, %.3f, %.3f) yaw=%.1f", g_probes, why, p.className.c_str(), p.pos.X,
        p.pos.Y, p.pos.Z, p.yaw);
    Log("  physx: abi=%d (%s) scenes=%u time=%llums owners=%zu", r.abiOk, r.abiWhy, r.nbScenes, ms, owners.size());
    for (uint32_t i = 0; i < r.nOwners; ++i)
    {
        const auto& h = r.owners[i];
        Log("  V-owned actor: scene %u %s depth=%u pos=(%.3f, %.3f, %.3f) geom=%u capsule r=%.3f hh=%.3f userData=0x%llx matched=0x%llx",
            h.scene, h.isStatic ? "static" : "dynamic", h.depth, h.pos[0], h.pos[1], h.pos[2], h.geom, h.capsule[0],
            h.capsule[1], h.userData, h.matched);
    }
    Log("  shift (world = physx + shift): (%.3f, %.3f, %.3f) from %s; census/dump around PhysX (%.3f, %.3f, %.3f); V moved %.2f m since last probe",
        r.shift[0], r.shift[1], r.shift[2], r.shiftFromOwner ? "V's capsule (motion match)" : "streaming centroid (rough, z unshifted)",
        r.probeAt[0], r.probeAt[1], r.probeAt[2], r.vDisplacement);
    for (uint32_t i = 0; i < r.nCapsules; ++i)
    {
        const auto& c = r.capsules[i];
        Log("    capsule%s actor=0x%llx physx=(%.3f, %.3f, %.3f) r=%.3f hh=%.3f dist-to-predicted=%.1f motion-error=%.3f",
            (int)i == r.matchedCapsule ? " [V]" : "", c.actor, c.pos[0], c.pos[1], c.pos[2], c.radius, c.halfHeight,
            c.distToPredicted, c.motionError);
    }
    for (uint32_t i = 0; i < r.nbScenes && i < 16; ++i)
    {
        const auto& s = r.scenes[i];
        if (!s.staticSeen) continue;
        Log("  scene %u static centres: min=(%.1f, %.1f, %.1f) max=(%.1f, %.1f, %.1f) mean=(%.1f, %.1f, %.1f)", i,
            s.centerMin[0], s.centerMin[1], s.centerMin[2], s.centerMax[0], s.centerMax[1], s.centerMax[2],
            s.centerMean[0], s.centerMean[1], s.centerMean[2]);
        for (int k = 0; k < 5; ++k)
            for (const auto* a : {&s.nearestStatic[k], &s.nearestDynamic[k]})
                if (a->used)
                    Log("    nearest %s #%d: dist=%.1f centre=(%.2f, %.2f, %.2f) extent=(%.2f, %.2f, %.2f) shapes=%u firstGeom=%u userData=0x%llx",
                        a->isStatic ? "static " : "dynamic", k, a->dist, a->center[0], a->center[1], a->center[2],
                        a->extent[0], a->extent[1], a->extent[2], a->shapes, a->geom, a->userData);
    }
    FILE* js = nullptr;
    _wfopen_s(&js, (stem.wstring() + L".json").c_str(), L"w");
    if (js) fprintf(js, "{\"probe\":%llu,\"why\":\"%s\",\"player\":{\"class\":\"%s\",\"pos\":[%.4f,%.4f,%.4f],\"yaw\":%.3f},\"physx\":{\"abi\":%d,\"abiWhy\":\"%s\",\"scenes\":%u,\"ms\":%llu,\"list\":[",
                    g_probes, why, p.className.c_str(), p.pos.X, p.pos.Y, p.pos.Z, p.yaw, r.abiOk, r.abiWhy, r.nbScenes, ms);
    for (uint32_t i = 0; i < r.nbScenes && i < 16; ++i)
    {
        const auto& s = r.scenes[i];
        const auto& c = s.census;
        Log("  scene %u: vtable=PhysX3_x64+0x%llx flags=0x%x gravity=(%.2f, %.2f, %.2f) actors static=%u dynamic=%u", i,
            s.vtableRva, s.flags, s.gravity[0], s.gravity[1], s.gravity[2], s.nbStatic, s.nbDynamic);
        Log("    within 64 m: static=%u dynamic=%u (skipped actors %u, shapes %u, meshes %u) shapes=%u (sim %u, query %u, trigger %u) "
            "sphere=%u plane=%u capsule=%u box=%u convex=%u trimesh=%u heightfield=%u; OBJ triangles (static, 24 m)=%llu",
            c.staticActors, c.dynamicActors, c.skippedActors, c.skippedShapes, c.skippedMeshes, c.shapes, c.simulationShapes,
            c.queryShapes, c.triggerShapes, c.byType[0], c.byType[1], c.byType[2], c.byType[3], c.byType[4], c.byType[5],
            c.byType[6], c.trianglesWritten);
        std::string filters;
        for (uint32_t k = 0; k < c.filterDistinct; ++k)
            filters += std::to_string(c.filterWord0Count[k]) + "x" + std::to_string(c.filterWord0[k]) + " ";
        Log("    query filter word0 (count x value): %s", filters.c_str());
        Log("    ground under V from collected static triangles: found=%d z=%.3f (V.z - ground = %.3f m)", s.groundFound,
            s.groundZ, s.groundDelta);
        if (js) fprintf(js, "%s{\"vtableRva\":%llu,\"flags\":%u,\"gravity\":[%.3f,%.3f,%.3f],\"static\":%u,\"dynamic\":%u,\"near\":{\"static\":%u,\"dynamic\":%u,\"skippedActors\":%u,\"skippedShapes\":%u,\"skippedMeshes\":%u,\"shapes\":%u,\"byType\":[%u,%u,%u,%u,%u,%u,%u],\"triangles\":%llu},\"ground\":{\"found\":%d,\"z\":%.4f,\"delta\":%.4f}}",
                        i ? "," : "", s.vtableRva, s.flags, s.gravity[0], s.gravity[1], s.gravity[2], s.nbStatic, s.nbDynamic,
                        c.staticActors, c.dynamicActors, c.skippedActors, c.skippedShapes, c.skippedMeshes, c.shapes,
                        c.byType[0], c.byType[1], c.byType[2], c.byType[3], c.byType[4], c.byType[5], c.byType[6],
                        c.trianglesWritten, s.groundFound, s.groundZ, s.groundDelta);
    }
    // P1-12 (0.21.0): what Stage B's collision gather leaves out within 3 m of V.
    if (r.skippedNear.empty()) Log("  left out of Stage B's collision within 3 m of V: nothing");
    else
    {
        Log("  dynamic actors and uncovered shapes within 3 m of V (%zu; [TAKEN as a prop] ones are in Stage B's collision since 0.21.1):", r.skippedNear.size());
        for (const auto& line : r.skippedNear) Log("    %s", line.c_str());
    }
    const std::string comps = Components(p);
    const std::string pad = XInputState();
    const double mean = g_dtCount ? 1000.0 * g_dtSum / g_dtCount : 0;
    Log("  V entity: %s", comps.c_str());
    Log("  input: %s", pad.c_str());
    Log("  main-thread Running update: thread=%lu frames=%llu mean dt=%.2fms max dt=%.2fms", g_mainThread, g_frames, mean, 1000.0 * g_dtMax);
    if (js)
    {
        std::string esc = comps;
        std::replace(esc.begin(), esc.end(), '"', '\'');
        fprintf(js, "]},\"entity\":\"%s\",\"input\":\"%s\",\"frame\":{\"thread\":%lu,\"frames\":%llu,\"meanMs\":%.3f,\"maxMs\":%.3f}}\n",
                esc.c_str(), pad.c_str(), g_mainThread, g_frames, mean, 1000.0 * g_dtMax);
        fclose(js);
    }
    g_dtSum = g_dtMax = 0;
    g_dtCount = 0;
}

// ----- C5: lifecycle (0.15.0) -----
// One-key skating: from normal play Ctrl+Shift+F10 enters third person with the restrictions and starts the skater;
// pressing it again stops the skater and leaves skating. With third person already on (Ctrl+Shift+F7), it starts and
// stops only the skater, as before. After the first ride Skate stays loaded (parked), so later starts take no load.
bool g_skateToggle = false;               // skating was entered by Ctrl+Shift+F10 (so it also leaves it)
uintptr_t g_stagebPlayer = 0;             // identity only (no reference held): a loaded save brings a new player object

bool StartSkater(const PlayerInfo& p)
{
    const float v[3] = {p.pos.X, p.pos.Y, p.pos.Z};
    const std::string rig = stagec::RigFor(p.handle, &LogLine);  // C3: V's own skeleton, female or male
    // R1: the data folder comes from the one-time setup; without it there is nothing to load.
    if (const std::string missing = data::Missing(rig); !missing.empty())
    {
        Log("stage B: Deckrunner's data folder is missing or incomplete; run the setup first (or set deckrunner.ini [data] dir=). "
            "Missing: %s", missing.c_str());
        return false;
    }
    stageb::SetRig(rig);
    if (!stageb::Start(v, p.yaw, &LogLine)) return false;
    g_stagebPlayer = reinterpret_cast<uintptr_t>(p.handle.instance);
    return true;
}

// #2: once the world has loaded, Skate loads in the background (about 8 s on the runtime's threads) and waits parked, so
// the first Ctrl+Shift+F10 is instant. deckrunner.ini [skate] preload=0 turns it off: Skate then loads on the first start.
void PreloadSkater(const PlayerInfo& p)
{
    if (!GetPrivateProfileIntW(L"skate", L"preload", 1, data::Ini().c_str()))
    {
        Log("stage B: no preload (deckrunner.ini [skate] preload=0); Skate loads on the first start");
        return;
    }
    const std::string rig = stagec::RigFor(p.handle, &LogLine);
    if (!data::Missing(rig).empty()) return;  // the first start says what's missing
    stageb::SetRig(rig);
    const float v[3] = {p.pos.X, p.pos.Y, p.pos.Z};
    stageb::Preload(v, p.yaw, &LogLine);
}

void LeaveSkating(const PlayerInfo& p, const char* why)
{
    if (stageb::Active()) stageb::Stop(&LogLine);
    stagec::ExitSkating(p.handle, &LogLine, why);
    g_skateToggle = false;
}

// C5 criterion (§7.2, Stage C criterion 3): 50 enter/exit cycles leave V as before. Ctrl+Shift+Alt+F10 runs them
// unattended: enter skating, ride 1 s, leave, wait for the cleanup to be confirmed, put V back where the test began, and
// compare the snapshot (skating restrictions on V, the FPP camera's local transform, the head slot) with the one taken
// before the first cycle. Ctrl+Shift+Alt+F10 again, or Ctrl+Shift+F6, stops it.
struct CycleTest
{
    bool on = false;
    int total = 50, done = 0, clean = 0, phase = 0;
    double at = 0, started = 0, enterSum = 0, firstEnter = 0;
    int enterN = 0;
    float home[3] = {}, homeYaw = 0;
    uintptr_t player = 0;
    std::string baseline;
    std::vector<std::string> failures;
} g_cycle;

double NowSeconds()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return double(t.QuadPart) / double(f.QuadPart);
}

void CycleFinish(const char* why)
{
    Log("stage C5: cycle test %s: %d of %d cycles clean, %d done in %.0f s; first start %.1f s (Skate loading), later starts %.0f ms on average",
        why, g_cycle.clean, g_cycle.total, g_cycle.done, NowSeconds() - g_cycle.started, g_cycle.firstEnter,
        g_cycle.enterN > 1 ? 1000.0 * (g_cycle.enterSum - g_cycle.firstEnter) / (g_cycle.enterN - 1) : 0.0);
    for (const auto& f : g_cycle.failures) Log("stage C5: cycle test difference: %s", f.c_str());
    g_cycle.on = false;
}

void CycleStep()
{
    if (!g_cycle.on) return;
    const PlayerInfo p = GetPlayer();
    if (!p.ok || reinterpret_cast<uintptr_t>(p.handle.instance) != g_cycle.player)
    {
        CycleFinish("stopped: V changed or is unavailable");
        return;
    }
    const double now = NowSeconds();
    switch (g_cycle.phase)
    {
    case 0:  // enter
        if (now - g_cycle.at < 0.3) return;
        g_skateToggle = stagec::EnterSkating(p.handle, &LogLine);
        if (!g_skateToggle || !StartSkater(p))
        {
            g_cycle.failures.push_back("cycle " + std::to_string(g_cycle.done + 1) + ": skating did not start");
            LeaveSkating(p, "cycle test");
            g_cycle.phase = 3, g_cycle.at = now;
            return;
        }
        g_cycle.phase = 1, g_cycle.at = now;
        return;
    case 1:  // wait for the skater to ride
    {
        float sp[3], sv[3], sh = 0;
        uint64_t st = 0;
        char state[48] = {};
        if (stageb::Skater(sp, sv, sh, st, state))
        {
            const double took = now - g_cycle.at;
            if (g_cycle.enterN == 0) g_cycle.firstEnter = took;
            g_cycle.enterSum += took, ++g_cycle.enterN;
            g_cycle.phase = 2, g_cycle.at = now;
        }
        else if (now - g_cycle.at > 20)
        {
            g_cycle.failures.push_back("cycle " + std::to_string(g_cycle.done + 1) + ": the skater did not ride within 20 s");
            g_cycle.phase = 2, g_cycle.at = now - 10;
        }
        return;
    }
    case 2:  // ride, then leave
        if (now - g_cycle.at < 1.0) return;
        LeaveSkating(p, "cycle test");
        g_cycle.phase = 3, g_cycle.at = now;
        return;
    case 3:  // cleanup confirmed: back home, compare
        if (!(stagec::Settled() && now - g_cycle.at > 0.5) && now - g_cycle.at < 10) return;
        if (!stagec::Settled()) g_cycle.failures.push_back("cycle " + std::to_string(g_cycle.done + 1) + ": cleanup not confirmed within 10 s");
        stagec::PlaceV(p.handle, g_cycle.home, g_cycle.homeYaw);
        g_cycle.phase = 4, g_cycle.at = now;
        return;
    case 4:
    {
        if (now - g_cycle.at < 0.3) return;
        const std::string snap = stagec::LifecycleSnapshot(p.handle);
        ++g_cycle.done;
        if (snap == g_cycle.baseline) ++g_cycle.clean;
        else g_cycle.failures.push_back("cycle " + std::to_string(g_cycle.done) + ": " + snap);
        if (g_cycle.done % 10 == 0 || snap != g_cycle.baseline)
            Log("stage C5: cycle %d of %d %s", g_cycle.done, g_cycle.total, snap == g_cycle.baseline ? "clean" : ("DIFFERS: " + snap).c_str());
        if (g_cycle.done >= g_cycle.total) CycleFinish("finished");
        else g_cycle.phase = 0, g_cycle.at = now;
        return;
    }
    }
}

// ----- #3: the skateboard in the weapon wheel (0.32.0) -----
// Picking the skateboard in the weapon wheel draws it, and a drawn skateboard starts skating (the restrictions then
// empty V's hands). While skating on foot the wheel opens on a held Y (Wheel.hpp keeps that press from Skate); whatever
// the wheel picked stops skating, and a weapon other than the skateboard is drawn once the restrictions are gone.
// With a keyboard the wheel (Alt) shows a pick the same way. Ctrl+Shift+F10 works as before throughout.
wheel::YSplit g_ysplit;
struct WheelState
{
    double footSince = -1;        // when the skater's on-foot state last changed (the restrictions follow it after a moment)
    bool foot = false;
    stagec::ItemId active;        // the Weapon area's active item, polled while the wheel can open
    bool haveActive = false;
    double pollAt = 0;
    double decideAt = -1;         // a pick to act on: leave skating then
    const char* how = "";
    stagec::ItemId draw;          // a weapon to draw once the skating restrictions are gone
    double drawFrom = -1;
    double quietUntil = 0;        // no start from a drawn skateboard right after skating ends
} g_wheel;
int g_boardDefined = -1;          // Items.Deckrunner_Skateboard in TweakDB (-1: not looked up yet)

void BeginSkating(const PlayerInfo& p)
{
    // C5: from normal play, straight into skating; with third person already on, only the skater starts.
    g_ysplit = wheel::YSplit{};
    g_wheel.decideAt = -1, g_wheel.footSince = -1, g_wheel.foot = false, g_wheel.haveActive = false;
    if (!stagec::Restricted()) g_skateToggle = stagec::EnterSkating(p.handle, &LogLine);
    if (stagec::Restricted() && !StartSkater(p) && g_skateToggle) LeaveSkating(p, "the skater could not start");
}

bool BoardDefined()
{
    if (g_boardDefined < 0) g_boardDefined = stagec::BoardItemDefined() ? 1 : 0;
    return g_boardDefined == 1;
}

// Not skating: the skateboard given once the world is in, a drawn skateboard starting a ride, and a weapon picked in the
// wheel while skating drawn once the restrictions are gone.
void WheelIdle(double now)
{
    if (!g_autoDone) return;  // the world isn't loaded yet
    if (g_frames % 300 == 0)
        if (const PlayerInfo p = GetPlayer(); p.ok) stagec::EnsureBoardItem(p.handle, &LogLine);
    if (g_wheel.drawFrom > 0 && !stagec::Restricted())
    {
        const PlayerInfo p = GetPlayer();
        if (!stagec::Active() || now - g_wheel.drawFrom > 3.0)  // the restrictions are gone (or it's been long enough)
        {
            if (p.ok) stagec::DrawWeapon(p.handle, g_wheel.draw, &LogLine);
            g_wheel.drawFrom = -1;
        }
        return;
    }
    if (g_frames % 6 != 0 || now < g_wheel.quietUntil || stagec::Restricted() || stageb::Active() || g_cycle.on || !BoardDefined()) return;
    const PlayerInfo p = GetPlayer();
    if (p.ok && stagec::InRightHand(p.handle) == stagec::Held::Board)
    {
        Log("weapon wheel: the skateboard was drawn; skating");
        g_wheel.quietUntil = now + 3.0;  // if the start fails with the skateboard still in hand, it isn't retried at once
        BeginSkating(p);
    }
}

// Skating: the restrictions follow the skater's on-foot state, Y is split on foot, and a pick in the wheel leaves.
uint16_t WheelRide(uint16_t buttons, double now)
{
    const PlayerInfo p = GetPlayer();
    if (!p.ok || !stagec::Restricted()) return buttons;
    float pos[3], vel[3], heading = 0;
    uint64_t tick = 0;
    char state[48] = {};
    if (stageb::Skater(pos, vel, heading, tick, state) && tick > 0)
    {
        const bool foot = std::strncmp(state, "Biped", 5) == 0;  // BipedGround and the rest of Skate's on-foot states
        if (foot != g_wheel.foot) g_wheel.foot = foot, g_wheel.footSince = now;
        if (g_wheel.footSince >= 0 && now - g_wheel.footSince > 0.15)
        {
            const bool was = stagec::OnFootRestrictions();
            stagec::SetOnFoot(p.handle, foot, &LogLine);
            if (!was && stagec::OnFootRestrictions()) g_wheel.haveActive = stagec::ActiveWeapon(p.handle, g_wheel.active);
            g_wheel.footSince = -1;
        }
    }
    wheel::Event ev;
    const uint16_t out = g_ysplit.Filter(buttons, now, stagec::OnFootRestrictions(), ev);
    if (ev == wheel::Event::HoldStart) Log("weapon wheel: Y held on foot: the game's weapon wheel (Skate doesn't get this press)");
    if (ev == wheel::Event::HoldEnd && g_wheel.decideAt < 0) g_wheel.decideAt = now + 0.3, g_wheel.how = "Y held on foot";
    // A pick without Y (the keyboard's Alt wheel) shows up as a new active item.
    if (stagec::OnFootRestrictions() && g_wheel.haveActive && g_wheel.decideAt < 0 && !g_ysplit.Holding() && now >= g_wheel.pollAt)
    {
        g_wheel.pollAt = now + 0.2;
        stagec::ItemId a;
        if (stagec::ActiveWeapon(p.handle, a) && std::memcmp(a.bytes, g_wheel.active.bytes, sizeof a.bytes) != 0)
            g_wheel.decideAt = now + 0.1, g_wheel.how = "a new pick in the weapon wheel";
    }
    if (g_wheel.decideAt > 0 && now >= g_wheel.decideAt)
    {
        g_wheel.decideAt = -1;
        stagec::ItemId pick;
        stagec::ActiveWeapon(p.handle, pick);
        const bool draw = pick.valid && !pick.board;
        Log("weapon wheel: %s, picked %s: leaving skating%s", g_wheel.how, !pick.valid ? "nothing" : pick.board ? "the skateboard" : "a weapon",
            draw ? "; it's drawn once the restrictions are off" : "");
        if (g_skateToggle) LeaveSkating(p, "weapon wheel");
        else stageb::Stop(&LogLine);
        if (draw) g_wheel.draw = pick, g_wheel.drawFrom = now;
        g_wheel.quietUntil = now + 1.5;
        return out & ~wheel::kY;
    }
    return out;
}

bool OnRunningUpdate(RED4ext::CGameApplication*)
{
    ++g_frames;
    if (!g_mainThread)
    {
        g_mainThread = GetCurrentThreadId();
        const char* why = "";
        const bool ok = probe::CheckAbi(why);
        Log("PhysX ABI check: %s (%s)", ok ? "OK" : "FAILED", why);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double frameDt = 0;
    if (g_last.QuadPart)
    {
        frameDt = double(now.QuadPart - g_last.QuadPart) / double(g_freq.QuadPart);
        g_dtSum += frameDt, g_dtMax = std::max(g_dtMax, frameDt), ++g_dtCount;
    }
    g_last = now;

    // Hotkeys (edge-triggered, only while the game window has focus): Ctrl+Shift+F8 probe, Ctrl+Shift+F10 toggles
    // Stage B. Only keys the game leaves unbound: it reads F5 (quick save) and F9 (quick load) whatever the modifiers,
    // so the 0.5 binding of Ctrl+Shift+F9 also quick-loaded the last save (in-game Stage B, 2026-11-14).
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool focused = pid == GetCurrentProcessId();
    const bool chord = focused && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    // Players get two keys, set in deckrunner.ini [keys] (Keys.hpp): skate (Ctrl+Shift+F10) and capture
    // (Ctrl+Shift+F12). The lab's keys (Ctrl+Shift+ F8 probe, F7 third-person cycling, F11 pose test / teleport mode,
    // F2 V's head, F6 emergency exit, Alt+F10 cycle test) exist only in a build configured with -DDECKRUNNER_LAB_KEYS=ON.
#ifdef DECKRUNNER_LAB_KEYS
    const bool down = chord && (GetAsyncKeyState(VK_F8) & 0x8000);
    if (down && !g_hotkeyDown) RunProbe("hotkey");
    g_hotkeyDown = down;
    const bool cycleKey = chord && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_F10) & 0x8000);
#else
    const bool cycleKey = false;
#endif
    static bool f10Down = false, cycleDown = false;
    const bool f10 = focused && keys::Held(g_keySkate);
    if (cycleKey && !cycleDown)  // C5: the cycle test
    {
        const PlayerInfo p = GetPlayer();
        if (g_cycle.on) CycleFinish("stopped by Ctrl+Shift+Alt+F10");
        else if (!p.ok) Log("stage C5: cycle test: V not available");
        else if (stagec::Active() || stageb::Active()) Log("stage C5: cycle test: leave skating first (Ctrl+Shift+F10)");
        else
        {
            g_cycle = CycleTest{};
            g_cycle.on = true;
            g_cycle.player = reinterpret_cast<uintptr_t>(p.handle.instance);
            g_cycle.home[0] = p.pos.X, g_cycle.home[1] = p.pos.Y, g_cycle.home[2] = p.pos.Z, g_cycle.homeYaw = p.yaw;
            g_cycle.baseline = stagec::LifecycleSnapshot(p.handle);
            g_cycle.started = g_cycle.at = NowSeconds() - 1;
            Log("stage C5: cycle test started: %d enter/exit cycles from (%.1f, %.1f, %.1f); before: %s", g_cycle.total, p.pos.X, p.pos.Y,
                p.pos.Z, g_cycle.baseline.c_str());
        }
    }
    else if (f10 && !f10Down && !g_cycle.on)
    {
        const PlayerInfo p = GetPlayer();
        if (stageb::Active())
        {
            stageb::Stop(&LogLine);
            if (g_skateToggle && p.ok) LeaveSkating(p, g_keySkate.text.c_str());
        }
        else if (!p.ok) Log("stage B: player not available");
        else BeginSkating(p);
    }
    f10Down = f10, cycleDown = cycleKey;
    CycleStep();
    // C5: the game needs V back (she died, is in a vehicle, a scene or a workspot, or it moved her): leave skating.
    if (stagec::Restricted() && !g_cycle.on)
    {
        const PlayerInfo p = GetPlayer();
        if (p.ok)
            if (const char* why = stagec::ExitReason(p.handle))
            {
                Log("stage C5: leaving skating: %s", why);
                LeaveSkating(p, why);
            }
    }
    // P0-2: Skate's engine stopped mid-ride. Hand V back at once instead of leaving her frozen behind the board with the
    // restrictions on (first in-game ride: the d-pad gesture error). The next Ctrl+Shift+F10 reloads Skate.
    if (stageb::Active() && g_frames % 15 == 0)
        if (const char* err = stageb::Failed())
        {
            Log("stage C5: leaving skating: Skate's engine stopped (%s); the next %s reloads it", err, g_keySkate.text.c_str());
            if (g_cycle.on) CycleFinish("Skate's engine stopped");
            const PlayerInfo p = GetPlayer();
            if (p.ok && (g_skateToggle || stagec::Restricted())) LeaveSkating(p, "Skate's engine stopped");
            else stageb::Stop(&LogLine);
        }
    // Stop on a save or checkpoint load: V is gone, the player object is replaced, or (0.24.0) the skating restrictions
    // are gone from V. They are never saved, so a loaded save always comes back without them, even if the new player
    // object were to reuse the old one's address. The skater's collision would otherwise be re-gathered from a different
    // world state. When the player object is still V, skating is left in full, as on an engine error (P1-13, 0.23.1).
    // Up to 0.23.1, V jumping more than 100 m in half a second also counted as a load, and a session marker return of
    // 108 m stopped the ninth test's ride.
    if (stageb::Active() && g_frames % 30 == 0)
    {
        const PlayerInfo p = GetPlayer();
        const bool sameV = p.ok && reinterpret_cast<uintptr_t>(p.handle.instance) == g_stagebPlayer;
        const bool lost = sameV && stagec::RestrictionsLost(p.handle);
        if (!p.ok || !sameV || lost)
        {
            Log("stage B: V %s; stopping the session (a save or checkpoint was probably loaded)",
                !p.ok ? "is unavailable" : !sameV ? "was replaced by a new player object" : "no longer has the skating restrictions");
            if (sameV && (g_skateToggle || stagec::Restricted())) LeaveSkating(p, "the skating restrictions are gone from V");
            else stageb::Stop(&LogLine);
        }
    }
    // Stage C1 (§7.2o): Ctrl+Shift+F7 cycles off -> third person -> third person with restrictions -> off;
    // Ctrl+Shift+F6 is the emergency exit. Both keys are unbound in the game (r6/config/inputUserMappings.xml binds
    // only F1, F3, F4, F5 and F9 among the function keys).
    // P1-11 (0.19.0): Ctrl+Shift+F12 captures the last 5 s of the ride and the collision around it, for moments that feel
    // wrong; bails are captured by themselves. F12 is unbound in the game (F9, the first choice, is its quick load).
    static bool f12Down = false;
    const bool f12 = focused && keys::Held(g_keyCapture);
    if (f12 && !f12Down)
    {
        if (stageb::Active()) stageb::Capture(&LogLine), stagec::DumpFollowTrace(g_out, &LogLine);
        else Log("stage B: capture: not riding (%s works while skating)", g_keyCapture.text.c_str());
    }
    f12Down = f12;
    static bool f7Down = false, f6Down = false, f11Down = false, f2Down = false;
#ifdef DECKRUNNER_LAB_KEYS
    const bool f6 = chord && (GetAsyncKeyState(VK_F6) & 0x8000), f2 = chord && (GetAsyncKeyState(VK_F2) & 0x8000);
    const bool f7 = chord && (GetAsyncKeyState(VK_F7) & 0x8000), f11 = chord && (GetAsyncKeyState(VK_F11) & 0x8000);
#else
    const bool f6 = false, f2 = false, f7 = false, f11 = false;
#endif
    if ((f7 && !f7Down) || (f6 && !f6Down) || (f11 && !f11Down) || (f2 && !f2Down))
    {
        const PlayerInfo p = GetPlayer();
        if (f6 && !f6Down)
        {
            if (g_cycle.on) CycleFinish("stopped by the emergency exit");
            if (stageb::Active()) stageb::Stop(&LogLine);
            g_skateToggle = false;
            stagec::EmergencyExit(p.handle, &LogLine);
        }
        else if (f2 && !f2Down) stagec::HeadToggle(p.handle, &LogLine);  // P2-10 (0.26.0): V's head off/on while skating; F2 is unbound in the game
        else if (f7 && !f7Down) stagec::Cycle(p.handle, &LogLine);
        else if (stagec::Following()) stagec::CycleMove(&LogLine);  // P1-15 (0.24.1): sparse teleports or every frame, while riding
        else stagec::PoseTest(p.handle, &LogLine);  // C3 (0.11.0); the head experiment is unbound until C4
    }
    f7Down = f7, f6Down = f6, f11Down = f11, f2Down = f2;
    if (stagec::Active()) stagec::Frame(GetPlayer().handle, &LogLine);

    WheelIdle(NowSeconds());  // #3
    if (stageb::Active())
    {
        stageb::DkrPad pad = ReadPad();
        pad.buttons = WheelRide(pad.buttons, NowSeconds());  // #3: Y on foot, and a pick in the weapon wheel
        if (stageb::Active())  // a pick in the wheel may have just ended the ride
            stageb::Frame(stagec::GameClock(static_cast<float>(frameDt), &LogLine), pad,
                          [](float out[3]) {
                              const PlayerInfo p = GetPlayer();
                              if (p.ok) out[0] = p.pos.X, out[1] = p.pos.Y, out[2] = p.pos.Z;
                              return p.ok;
                          },
                          &LogLine);
    }
    // Stage C2: third person + restrictions on (Ctrl+Shift+F7 twice) and a Stage B session riding (Ctrl+Shift+F10):
    // the skater carries V.
    {
        float sp[3], sv[3], sh = 0;
        uint64_t st = 0;
        char state[48] = {};
        if (stagec::Restricted() && stageb::Skater(sp, sv, sh, st, state)) stagec::Follow(GetPlayer().handle, sp, sv, sh, st, state, &LogLine);
    }
    // Stage C3: the skater's pose (or the reference-pose test) on V's own skeleton, through the lab graph's variables.
    if (stagec::Active()) stagec::PoseFrame(GetPlayer().handle, &LogLine);
    // Skate audio: the listener is the camera as just placed; the output pauses with the game.
    {
        float lp[3], lf[3], lr[3];
        const bool listener = stagec::Listener(lp, lf, lr);
        skateaudio::Frame(listener ? lp : nullptr, lf, lr, stageb::Active() && stagec::GamePaused(), &LogLine);
    }

    // Automatic first probe only once the world has streamed in: V away from the origin (the loading stage puts V
    // near 0,0,0) and the physics scene holding at least 300 static actors, then 5 s more.
    if (!g_autoDone && g_frames % 60 == 0)
    {
        const PlayerInfo p = GetPlayer();
        const bool away = p.ok && (std::fabs(p.pos.X) + std::fabs(p.pos.Y)) > 20.f;
        const uint32_t statics = away ? probe::StaticActorCount() : 0;
        if (!g_playerSeenAt)
        {
            if (away && statics >= 300) { g_playerSeenAt = GetTickCount64(); Log("world loaded (%u static actors); first probe in 5 s", statics); }
        }
        else if (GetTickCount64() - g_playerSeenAt > 5000)
        {
            g_autoDone = true;
            RunProbe("auto");
            stagec::DumpRtti(g_out / L"c0-rtti.txt", GetPlayer().handle.instance, &LogLine);  // C0, read-only
            if (const PlayerInfo p = GetPlayer(); p.ok && !stageb::Active() && !stagec::Active()) PreloadSkater(p);
        }
    }
    return false;
}
} // namespace

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle, RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk)
{
    if (aReason == RED4ext::v1::EMainReason::Load)
    {
        g_handle = aHandle;
        g_sdk = aSdk;
        QueryPerformanceFrequency(&g_freq);
        wchar_t local[MAX_PATH]{};
        GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        g_out = std::filesystem::path(local) / L"Deckrunner" / L"probe";
        std::error_code ec;
        std::filesystem::create_directories(g_out, ec);
        static RED4ext::v1::GameState running{nullptr, &OnRunningUpdate, nullptr};
        const bool added = aSdk->gameStates->Add(aHandle, RED4ext::EGameStateType::Running, &running);
        // P2-10 (0.25.7): the head shield's hooks on the engine's two hide setters, pass-through until a shield is set.
        const bool shield = vis::InstallShield(aSdk, aHandle);
        // stagec::InstallCameraHook(aSdk, aHandle, &LogLine);  // not attached in 0.9.5: with the first-person body the
        // camera is steady without it, and it runs on a worker thread anyway (C1 run 4). Kept for Stage C4.
        // 0.29.0: the plugin file ships deckrunner.default.ini, never deckrunner.ini, so an update (by hand, Vortex or
        // MO2) can't overwrite the player's settings; the first start makes deckrunner.ini from it.
        {
            const auto ini = data::Ini(), defaults = data::PluginDir() / L"deckrunner.default.ini";
            std::error_code e;
            if (!std::filesystem::exists(ini, e) && std::filesystem::exists(defaults, e))
                Log(CopyFileW(defaults.c_str(), ini.c_str(), TRUE) ? "deckrunner.ini created from deckrunner.default.ini"
                                                                   : "deckrunner.ini could not be created from deckrunner.default.ini; using the defaults");
        }
        g_keySkate = LoadKey(L"skate", "Ctrl+Shift+F10");
        g_keyCapture = LoadKey(L"capture", "Ctrl+Shift+F12");
#ifdef DECKRUNNER_LAB_KEYS
        Log("Deckrunner 0.32.0 (lab keys) loaded (skate on/off: %s; capture the last 5 s of a ride: %s; lab: probe read-only Ctrl+Shift+F8, cycle test Ctrl+Shift+Alt+F10, sparse teleports or every frame while riding Ctrl+Shift+F11, Stage C third person Ctrl+Shift+F7, C3 reference-pose test Ctrl+Shift+F11, V's head off/on while skating Ctrl+Shift+F2, emergency exit Ctrl+Shift+F6). Game product version %u.%u.%u. Running-state hook %s. Head shield hooks %s. Output: %ls",
#else
        Log("Deckrunner 0.32.0 loaded (skate on/off: %s; capture the last 5 s of a ride: %s; keys in deckrunner.ini [keys]). Game product version %u.%u.%u. Running-state hook %s. Head shield hooks %s. Output: %ls",
#endif
            g_keySkate.text.c_str(), g_keyCapture.text.c_str(), aSdk->runtime->major, aSdk->runtime->minor, aSdk->runtime->patch, added ? "added" : "FAILED",
            shield ? "attached" : "NOT attached (the engine's hide setters were not found or their code is not 2.5's)", g_out.c_str());
    }
    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo)
{
    aInfo->name = L"Deckrunner";
    aInfo->author = L"Deckrunner";
    aInfo->version = RED4EXT_V1_SEMVER(0, 26, 3);
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_2_50;  // refuse to load on anything but 2.5 (3.0.88.46235)
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports()
{
    return RED4EXT_API_VERSION_1;
}
