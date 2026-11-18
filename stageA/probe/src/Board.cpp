#include "Board.hpp"
#include "Rigid.hpp"
#include "Transform.hpp"
#include <RED4ext/Scripting/Natives/entIPlacedComponent.hpp>

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Callback.hpp>
#include <RED4ext/Memory/SharedPtr.hpp>
#include <RED4ext/ResourceDepot.hpp>
#include <RED4ext/ResourcePath.hpp>
#include <RED4ext/Scripting/Natives/Generated/EulerAngles.hpp>
#include <RED4ext/Scripting/Natives/Generated/Quaternion.hpp>
#include <RED4ext/Scripting/Natives/Generated/Vector4.hpp>
#include <RED4ext/Scripting/Natives/Generated/WorldTransform.hpp>
#include <RED4ext/Scripting/Natives/entEntity.hpp>
#include <RED4ext/Scripting/Natives/entIComponent.hpp>
#include <RED4ext/Scripting/Natives/gameIDynamicEntityIDSystem.hpp>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace board
{
namespace
{
using namespace RED4ext;
using namespace rigid;

// Address hashes (the game's cyberpunk2077_addresses.json; the same names in Codeware's AddressLib).
constexpr uint32_t kSpawnEntity = 3068279668;  // EntitySpawner::SpawnEntity
constexpr uint32_t kTokenDtor = 552341017;     // EntitySpawnerToken::~EntitySpawnerToken
constexpr const char* kTemplate = "deckrunner\\board\\deckrunner_board.ent";
// A vanilla quest prop of the same shape (gameObject + one mesh), spawned once if the board fails: it tells a broken
// spawn apart from a board asset the game will not load.
constexpr const char* kFallback = "base\\quest\\side_quests\\sq024\\entities\\sq024_beer_bottle.ent";

template<class... A> void Logf(LogFn log, const char* fmt, A... a)
{
    char b[1024];
    snprintf(b, sizeof b, fmt, a...);
    log(b);
}

double NowMs()
{
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

uintptr_t Resolve(uint32_t hash)
{
    // RED4ext's own resolver returns 0 for an unknown hash (UniversalRelocBase::Resolve would end the process).
    using ResolveFn = uintptr_t (*)(uint32_t);
    static const auto fn = [] {
        HMODULE m = GetModuleHandleW(L"RED4ext.dll");
        return m ? reinterpret_cast<ResolveFn>(GetProcAddress(m, "RED4ext_ResolveAddress")) : nullptr;
    }();
    return fn ? fn(hash) : 0;
}

// The engine's spawn token and request, as Codeware defines them for 2.5 (src/Red/EntitySpawner.hpp).
struct Token
{
    using AllocatorType = Memory::EngineAllocator;
    ~Token()
    {
        if (auto dtor = reinterpret_cast<void (*)(Token*)>(Resolve(kTokenDtor))) dtor(this);
    }
    uint64_t unk00;              // 00
    uint64_t unk08;              // 08
    Handle<ent::Entity> entity;  // 10
    bool detached;               // 20
    bool failed;                 // 21
    bool aborted;                // 22
};
static_assert(sizeof(Token) == 0x28);
static_assert(offsetof(Token, entity) == 0x10 && offsetof(Token, failed) == 0x21);

using SpawnCallback = FlexCallback<void (*)(const SharedPtr<Token>&)>;
struct Request
{
    ResourcePath templatePath;     // 00
    WorldTransform transform;      // 10
    ent::EntityID entityID;        // 30
    CName appearanceName;          // 38
    uint64_t unk40;                // 40
    SpawnCallback callback;        // 48
    bool detached;                 // 78
    bool dissolve;                 // 79
    uint8_t unk7A[0xE1 - 0x7A];    // 7A
    uint8_t priority{1};           // E1
    uint8_t unkE2;                 // E2
};
static_assert(sizeof(Request) == 0xF0);
static_assert(offsetof(Request, transform) == 0x10 && offsetof(Request, entityID) == 0x30 && offsetof(Request, callback) == 0x48 &&
              offsetof(Request, detached) == 0x78 && offsetof(Request, priority) == 0xE1);

// The spawn result is handed over in the engine's callback, as Codeware does it: the entity handle is MOVED out of the
// token there, so the token holds nothing when it is destroyed. 0.12.0 copied the handle out of the token on the main
// thread and then released the token; the token's destructor (the engine's, then C++'s for the Handle member) released
// the entity once more than it was referenced, the entity was freed while the world still used it, and an engine worker
// thread made a virtual call through the freed object (C3b run 1, a DEP fault at 0x0 33 ms after the spawn).
// The callback may run on any thread. Never freed (no destructor runs at shutdown).
struct HandOff
{
    std::recursive_mutex m;          // recursive: the engine might call back synchronously, inside SpawnEntity
    bool waiting = false;            // a spawn of ours is in flight
    bool done = false, failed = false;
    Handle<ent::Entity> entity;      // the result, moved out of the token
    std::vector<Handle<ent::Entity>> strays;  // results that arrived after we stopped waiting: disposed on the main thread
};
HandOff* const g_handOff = new HandOff();
HandOff* const g_camHandOff = new HandOff();  // P2-10 (0.25.0): the view camera's spawn

void Receive(HandOff* h, const SharedPtr<Token>& token)
{
    std::lock_guard lock(h->m);
    Token* t = token.instance;
    const bool ok = t && t->entity.instance && !t->failed && !t->aborted;
    if (!h->waiting)
    {
        if (ok) h->strays.push_back(std::move(t->entity));
        return;
    }
    if (ok) h->entity = std::move(t->entity);
    h->failed = !ok;
    h->done = true;
    h->waiting = false;
}
void OnSpawned(const SharedPtr<Token>& token) { Receive(g_handOff, token); }
void OnCamSpawned(const SharedPtr<Token>& token) { Receive(g_camHandOff, token); }

CClass* Cls(const char* name)
{
    auto* r = CRTTISystem::Get();
    CClass* c = r->GetClass(name);
    return c ? c : r->GetClassByScriptName(name);
}

CBaseFunction* Func(CClass* c, const char* shortName)
{
    const CName n(shortName);
    for (; c; c = c->parent)
    {
        for (auto* f : c->funcs)
            if (f && f->shortName == n) return f;
        for (auto* f : c->staticFuncs)
            if (f && f->shortName == n) return f;
    }
    return nullptr;
}

bool Call(IScriptable* self, CBaseFunction* fn, void* ret, std::initializer_list<void*> given)
{
    if (!fn) return false;
    const std::vector<void*> in(given);
    StackArgs_t args;
    std::vector<std::pair<const rtti::IType*, std::unique_ptr<uint8_t[]>>> scratch;
    for (uint32_t i = 0; i < fn->params.Size(); ++i)
    {
        auto* t = fn->params[i]->type;
        if (i < in.size() && in[i]) { args.emplace_back(t, in[i]); continue; }
        auto buf = std::make_unique<uint8_t[]>(t->GetSize() + 16);
        t->Construct(buf.get());
        args.emplace_back(t, buf.get());
        scratch.emplace_back(t, std::move(buf));
    }
    std::unique_ptr<uint8_t[]> retBuf;
    if (fn->returnType && fn->returnType->type && !ret)
    {
        retBuf = std::make_unique<uint8_t[]>(fn->returnType->type->GetSize() + 16);
        fn->returnType->type->Construct(retBuf.get());
        ret = retBuf.get();
    }
    const bool ok = ExecuteFunction(self, fn, ret, args);
    if (retBuf) fn->returnType->type->Destruct(retBuf.get());
    for (auto& [t, b] : scratch) t->Destruct(b.get());
    return ok;
}

enum class Phase { None, Spawning, Live, Failed };
struct State
{
    Phase phase = Phase::None;
    SharedPtr<Token> token;
    Handle<ent::Entity> entity;
    Handle<ent::IComponent> mesh;
    const char* tmpl = kTemplate;
    bool fallbackTried = false;
    double requestedAt = 0, wantedAt = 0, nextDiag = 0;
    V3 sentAt{0, 0, 0};
    Q sentQ{0, 0, 0, 1};      // the orientation sent with the last teleport
    uint32_t teleports = 0, placed = 0, offFrames = 0, failedCalls = 0, respawns = 0;
    // P1-15 (0.24.0): placements by transform write (counted in `teleports` too); frames in a row the last write was read
    // back more than 0.5 m off, and whether the writes were given up for this board.
    uint32_t writes = 0, writeMiss = 0;
    bool lastWrote = false, writeFailed = false;
    // Orientation checks over the diagnostic window (degrees): the entity's orientation read back against what was sent
    // the frame before, and the Euler conversion's round trip through the game's own EulerAngles.ToQuat.
    float errMax = 0, errSum = 0, tripMax = 0;
    uint32_t errN = 0;
    bool viaGame = false;
    bool respawnNow = false;  // set by Place when the entity stopped following its teleports
    float yaw = 0;
    std::vector<std::pair<Handle<ent::Entity>, double>> disposed;  // held a few seconds after Dispose, as Codeware holds them
};
// Never freed: no engine handle is released while the game shuts down.
State* const g = new State();
bool g_byTransform = true;  // P1-15: SetByTransform

void* Spawner(LogFn log)
{
    auto* e = CGameEngine::Get();
    const uintptr_t scene = e && e->framework ? e->framework->unk18 : 0;
    if (!scene) { log("stage C3b: board: no runtime scene"); return nullptr; }
    // The scene's systems are handles in a fixed order; worldRuntimeSystemEntity is #10 (Codeware's mapping for 2.5).
    auto* h = reinterpret_cast<Handle<ISerializable>*>(scene + 10 * sizeof(Handle<ISerializable>));
    ISerializable* sys = h->instance;
    const char* type = sys ? sys->GetType()->GetName().ToString() : "nothing";
    if (!sys || sys->GetType()->GetName() != CName("worldRuntimeSystemEntity"))
    {
        Logf(log, "stage C3b: board: runtime system #10 is %s, not worldRuntimeSystemEntity; not spawning", type);
        return nullptr;
    }
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(sys) + 0x58);
}

// The outcome of a spawn request: on its way, refused by the engine (no token), not possible yet (try again later), or
// for a template the game's archives do not hold.
enum class Req { Pending, NoToken, NotReady, Missing };

Req RequestSpawn(const char* what, const char* tmpl, const Pose& at, HandOff* h, void (*cb)(const SharedPtr<Token>&), SharedPtr<Token>& token,
            LogFn log)
{
    using SpawnFn = void* (*)(void* spawner, SharedPtr<Token>& token, const Request& request);
    const auto spawn = reinterpret_cast<SpawnFn>(Resolve(kSpawnEntity));
    if (!spawn || !Resolve(kTokenDtor)) { Logf(log, "%s: the entity spawner's address is not in this game's address file", what); return Req::NotReady; }
    // As Codeware does before every spawn: a template the game's archives do not hold is never requested.
    if (auto* depot = ResourceDepot::Get(); !depot || !depot->ResourceExists(ResourcePath(tmpl)))
    {
        Logf(log, "%s: %s is not in the game's archives%s", what, tmpl,
             std::strcmp(tmpl, kTemplate) == 0 ? " (is deckrunner_board.archive in archive\\pc\\mod?)" : "");
        return Req::Missing;
    }
    void* spawner = Spawner(log);
    if (!spawner) return Req::NotReady;
    auto* e = CGameEngine::Get();
    auto* gi = e->framework->gameInstance;
    CClass* idc = Cls("gameIDynamicEntityIDSystem");
    auto* ids = gi && idc ? reinterpret_cast<game::IDynamicEntityIDSystem*>(gi->GetSystem(idc)) : nullptr;
    if (!ids) { Logf(log, "%s: no dynamic entity ID system", what); return Req::NotReady; }
    Request r{};
    r.templatePath = ResourcePath(tmpl);
    r.transform.Position.x.Bits = static_cast<int32_t>(std::lround(at.p.x * 131072.0));
    r.transform.Position.y.Bits = static_cast<int32_t>(std::lround(at.p.y * 131072.0));
    r.transform.Position.z.Bits = static_cast<int32_t>(std::lround(at.p.z * 131072.0));
    r.transform.Orientation.i = at.q.x, r.transform.Orientation.j = at.q.y, r.transform.Orientation.k = at.q.z, r.transform.Orientation.r = at.q.w;
    r.entityID = ids->GetNextTransientID();
    r.callback = SpawnCallback(cb);
    r.detached = false;
    r.priority = 1;
    if (!r.entityID.hash) { Logf(log, "%s: no entity ID", what); return Req::NotReady; }
    token = SharedPtr<Token>();
    {
        std::lock_guard lock(h->m);
        h->waiting = true;
        h->done = h->failed = false;
        h->entity = Handle<ent::Entity>();
    }
    spawn(spawner, token, r);
    Logf(log, "%s: spawn of %s requested at (%.2f, %.2f, %.2f), entity ID %llu: %s", what, tmpl, at.p.x, at.p.y, at.p.z,
         static_cast<unsigned long long>(r.entityID.hash), token.instance ? "pending" : "no token returned");
    return token.instance ? Req::Pending : Req::NoToken;
}

bool Spawn(const char* tmpl, const Pose& at, LogFn log)
{
    const Req r = RequestSpawn("stage C3b: board", tmpl, at, g_handOff, &OnSpawned, g->token, log);
    if (r == Req::NotReady) return false;
    if (r == Req::Missing) { g->phase = Phase::Failed; return false; }
    g->tmpl = tmpl;
    g->requestedAt = NowMs();
    g->phase = r == Req::Pending ? Phase::Spawning : Phase::Failed;
    return r == Req::Pending;
}

ent::IComponent* MeshOf(ent::Entity* e)
{
    CClass* mc = Cls("entMeshComponent");
    for (auto& c : e->components)
        if (c.instance && (c->name == CName("deckrunner_board") || (mc && c.instance->GetType()->IsA(mc)))) return c.instance;
    return nullptr;
}

void Dispose(LogFn log, const char* why)
{
    if (g->entity.instance)
    {
        // A loaded save tears the old world down first: an entity no longer attached is only released, not disposed.
        bool attached = false;
        Call(g->entity.instance, Func(g->entity.instance->GetType(), "IsAttached"), &attached, {});
        const bool ok = attached && Call(g->entity.instance, Func(g->entity.instance->GetType(), "Dispose"), nullptr, {});
        Logf(log, "stage C3b: board removed (%s): %s; %u placements, %u by teleport", why,
             !attached ? "no longer attached, released" : ok ? "Dispose called" : "Dispose FAILED", g->placed, g->teleports);
        if (attached) g->disposed.emplace_back(g->entity, NowMs());
    }
    else if (g->phase == Phase::Spawning)
    {
        std::lock_guard lock(g_handOff->m);
        g_handOff->waiting = false;
        if (g_handOff->done && g_handOff->entity.instance) g_handOff->strays.push_back(std::move(g_handOff->entity));
        g_handOff->done = false;
        Logf(log, "stage C3b: board: spawn abandoned (%s)", why);
    }
    g->entity = Handle<ent::Entity>();
    g->mesh = Handle<ent::IComponent>();
    g->token = SharedPtr<Token>();
    g->phase = Phase::None;
    g->placed = g->teleports = g->offFrames = g->failedCalls = 0;
    g->writes = g->writeMiss = 0, g->lastWrote = g->writeFailed = false;
    g->errMax = g->errSum = g->tripMax = 0, g->errN = 0;
}

// The game's own conversions between quaternions and Euler angles: Quaternion.ToEulerAngles and EulerAngles.ToQuat
// (both names are in 2.5's script bundle; Let There Be Flight uses EulerAngles.ToQuat). Static natives on the script
// structs, or global functions; looked up once.
CBaseFunction* FindConversion(const char* cls, const char* name, const char* param)
{
    auto matches = [&](CBaseFunction* f) {
        return f && f->shortName == CName(name) && f->params.Size() >= 1 && f->params[0]->type && f->params[0]->type->GetName() == CName(param);
    };
    if (CClass* c = Cls(cls))
    {
        for (auto* f : c->staticFuncs)
            if (matches(f)) return f;
        for (auto* f : c->funcs)
            if (matches(f)) return f;
    }
    auto* r = CRTTISystem::Get();
    for (const std::string full : {std::string(cls) + "::" + name, std::string(name)})
        if (CBaseFunction* f = r->GetFunction(CName(full.c_str()));
            f && f->params.Size() >= 1 && f->params[0]->type && f->params[0]->type->GetName() == CName(param))
            return f;
    return nullptr;
}

struct Conversions
{
    CBaseFunction* toEuler = nullptr;  // Quaternion -> EulerAngles
    CBaseFunction* toQuat = nullptr;   // EulerAngles -> Quaternion
};
const Conversions& Conv(LogFn log)
{
    static Conversions c;
    static bool looked = false;
    if (!looked)
    {
        looked = true;
        c.toEuler = FindConversion("Quaternion", "ToEulerAngles", "Quaternion");
        c.toQuat = FindConversion("EulerAngles", "ToQuat", "EulerAngles");
        Logf(log, "stage C3b: board: the game's Quaternion.ToEulerAngles %s, EulerAngles.ToQuat %s", c.toEuler ? "found" : "NOT found",
             c.toQuat ? "found" : "NOT found");
        // A record of the game's convention: each angle alone, 90 degrees.
        if (c.toQuat)
            for (int k = 0; k < 3; ++k)
            {
                EulerAngles e{};
                (k == 0 ? e.Roll : k == 1 ? e.Pitch : e.Yaw) = 90.f;
                Quaternion q{};
                Call(nullptr, c.toQuat, &q, {&e});
                Logf(log, "stage C3b: board: EulerAngles.ToQuat(%s 90) = (%.3f, %.3f, %.3f, %.3f)", k == 0 ? "Roll" : k == 1 ? "Pitch" : "Yaw", q.i, q.j, q.k, q.r);
            }
    }
    return c;
}

float AngleBetween(Q a, Q b)
{
    const float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.f * std::acos(std::min(1.f, d)) * 57.29578f;
}

// Euler angles for the board's full orientation. The game's ToEulerAngles when it exists; otherwise our own
// decomposition, which assumes q = Yaw(Z) * Pitch(X) * Roll(Y) (INFERRED; the ToQuat round trip in the log checks it).
EulerAngles ToEuler(const Q& q, LogFn log)
{
    const auto& c = Conv(log);
    EulerAngles e{};
    if (c.toEuler)
    {
        Quaternion rq{};
        rq.i = q.x, rq.j = q.y, rq.k = q.z, rq.r = q.w;
        if (Call(nullptr, c.toEuler, &e, {&rq})) { g->viaGame = true; return e; }
    }
    g->viaGame = false;
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w);
    const float r11 = 1 - 2 * (x * x + z * z);
    const float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
    (void)r00;
    e.Pitch = std::asin(std::clamp(r21, -1.f, 1.f)) * 57.29578f;
    e.Roll = std::atan2(-r20, r22) * 57.29578f;
    e.Yaw = std::atan2(-r01, r11) * 57.29578f;
    return e;
}

void Place(const Pose& w, LogFn log)
{
    ent::Entity* e = g->entity.instance;
    if (!e) return;
    if (!g->mesh.instance)
        if (ent::IComponent* m = MeshOf(e))
            for (auto& c : e->components)
                if (c.instance == m) g->mesh = c;
    // 0.12.2 (C3b run 3: V kickflipped but the board did not turn): the whole orientation goes on the entity, as Euler
    // angles with the teleport. 0.12.0-0.12.1 teleported with the heading only and put the rest (tilt, flips) in the mesh
    // component's local transform, which the game did not show.
    CClass* tc = Cls("gameTeleportationFacility");
    CBaseFunction* tp = Func(tc, "Teleport");
    auto* gi = CGameEngine::Get()->framework->gameInstance;
    // Systems register under their interface (gameITeleportationFacility): walk up from the class until one answers.
    IScriptable* facility = nullptr;
    for (CClass* k = tc; gi && k && !facility; k = k->parent)
    {
        if (k->GetName() == CName("IScriptable") || k->GetName() == CName("gameIGameSystem")) break;
        facility = gi->GetSystem(k);
    }
    // Did the last teleport's orientation arrive? (Read before this frame's teleport.)
    if (g->teleports > 0)
    {
        Quaternion now{};
        if (Call(e, Func(e->GetType(), "GetWorldOrientation"), &now, {}))
        {
            const float err = AngleBetween({now.i, now.j, now.k, now.r}, g->sentQ);
            g->errMax = std::max(g->errMax, err), g->errSum += err, ++g->errN;
        }
        // P1-15: did the last transform write hold until now?
        Vector4 was{};
        if (g->lastWrote && !g->writeFailed && Call(e, Func(e->GetType(), "GetWorldPosition"), &was, {}))
        {
            const float off = Len(V3{was.X, was.Y, was.Z} - g->sentAt);
            g->writeMiss = off > 0.5f ? g->writeMiss + 1 : 0;
            if (g->writeMiss > 15)
            {
                g->writeFailed = true;
                Logf(log, "stage C3b: board: its transform writes do not hold (read back %.2f m off, %u frames in a row); teleporting it again",
                     off, g->writeMiss);
            }
        }
    }
    const V3 fwd = Rot(w.q, {0, 1, 0});
    if (std::hypot(fwd.x, fwd.y) > 0.2f) g->yaw = std::atan2(-fwd.x, fwd.y) * 57.29578f;  // for the log
    Vector4 v{};
    v.X = w.p.x, v.Y = w.p.y, v.Z = w.p.z, v.W = 1.f;
    EulerAngles ea = ToEuler(w.q, log);
    if (const auto& c = Conv(log); c.toQuat)
    {
        Quaternion back{};
        if (Call(nullptr, c.toQuat, &back, {&ea})) g->tripMax = std::max(g->tripMax, AngleBetween({back.i, back.j, back.k, back.r}, w.q));
    }
    // P1-15 (0.24.0): the board's root transform is written directly, as V's is (teleports every frame stopped the world
    // streaming in), unless it is set to teleports or its writes did not hold.
    const bool wrote = g_byTransform && !g->writeFailed && xform::Place(e, w.p, w.q);
    const bool ok = wrote || (facility && Call(facility, tp, nullptr, {&g->entity, &v, &ea}));
    g->lastWrote = wrote;
    if (wrote) ++g->writes;
    if (ok) ++g->teleports, ++g->placed, g->sentAt = w.p, g->sentQ = w.q;
    else if (++g->failedCalls == 1) log("stage C3b: board: Teleport failed");
    // C3b run 3: after the game was paused, the entity no longer followed its teleports (it reported a position near the
    // map's origin, 5.4 km away) and the board vanished. If it stays more than 2 m away for 30 frames, spawn a new one.
    if (g->teleports > 30)
    {
        Vector4 at{};
        Call(e, Func(e->GetType(), "GetWorldPosition"), &at, {});
        const float off = Len(V3{at.X, at.Y, at.Z} - g->sentAt);
        g->offFrames = off > 2.f ? g->offFrames + 1 : 0;
        if (g->offFrames > 30 && g->respawns < 5)
        {
            ++g->respawns;
            Logf(log, "stage C3b: board: the entity is %.1f m from where it is teleported (it reports (%.1f, %.1f, %.1f)); spawning a new board (%u of 5)",
                 off, at.X, at.Y, at.Z, g->respawns);
            g->offFrames = 0;
            g->respawnNow = true;
        }
    }
}

// ----- P2-10 (0.25.0): a camera of our own, to view V through -----
// The engine's own scene camera, an entGameEntity with one gameCameraComponent and nothing else. Its component has no
// parent binding, so it is placed in the world directly (or, if the game binds it at runtime, through the entity).
constexpr const char* kCamTemplate = "engine\\scenesystem\\camera.ent";
struct CamState
{
    Phase phase = Phase::None;
    SharedPtr<Token> token;
    Handle<ent::Entity> entity;
    Handle<ent::IComponent> comp;
    bool viaRoot = false;  // the component is the entity's root, or bound to it: the entity is placed instead
    double requestedAt = 0, nextDiag = 0;
    V3 sentAt{0, 0, 0};
    bool sent = false;
    uint32_t placed = 0, failed = 0;
    float errMax = 0, errSum = 0;
    uint32_t errN = 0;
    std::vector<std::pair<Handle<ent::Entity>, double>> disposed;
};
CamState* const gc = new CamState();

void CamDispose(LogFn log, const char* why)
{
    if (gc->entity.instance)
    {
        bool attached = false;
        Call(gc->entity.instance, Func(gc->entity.instance->GetType(), "IsAttached"), &attached, {});
        const bool ok = attached && Call(gc->entity.instance, Func(gc->entity.instance->GetType(), "Dispose"), nullptr, {});
        Logf(log, "stage C4: view camera removed (%s): %s; %u placements", why, !attached ? "no longer attached, released" : ok ? "Dispose called" : "Dispose FAILED",
             gc->placed);
        if (attached) gc->disposed.emplace_back(gc->entity, NowMs());
    }
    else if (gc->phase == Phase::Spawning)
    {
        std::lock_guard lock(g_camHandOff->m);
        g_camHandOff->waiting = false;
        if (g_camHandOff->done && g_camHandOff->entity.instance) g_camHandOff->strays.push_back(std::move(g_camHandOff->entity));
        g_camHandOff->done = false;
        Logf(log, "stage C4: view camera: spawn abandoned (%s)", why);
    }
    gc->entity = Handle<ent::Entity>();
    gc->comp = Handle<ent::IComponent>();
    gc->token = SharedPtr<Token>();
    gc->phase = Phase::None;
    gc->sent = false, gc->placed = gc->failed = 0, gc->errMax = gc->errSum = 0, gc->errN = 0;
}

V3 WorldOf(const WorldTransform& t) { return {t.Position.x.Bits / 131072.f, t.Position.y.Bits / 131072.f, t.Position.z.Bits / 131072.f}; }
} // namespace

void Frame(bool want, const float pos[3], const float rot[4], LogFn log)
{
    const double now = NowMs();
    {
        std::vector<Handle<ent::Entity>> strays;
        {
            std::lock_guard lock(g_handOff->m);
            strays.swap(g_handOff->strays);
        }
        for (auto& e : strays)
        {
            bool attached = false;
            Call(e.instance, Func(e.instance->GetType(), "IsAttached"), &attached, {});
            if (attached) Call(e.instance, Func(e.instance->GetType(), "Dispose"), nullptr, {});
            Logf(log, "stage C3b: board: a spawn finished after it was abandoned; %s", attached ? "disposed" : "released");
            if (attached) g->disposed.emplace_back(std::move(e), now);
        }
    }
    for (size_t i = 0; i < g->disposed.size();)
        if (now - g->disposed[i].second > 5000) g->disposed.erase(g->disposed.begin() + i);
        else ++i;
    Pose w;
    const bool have = pos && rot;
    if (have)
    {
        w.p = {pos[0], pos[1], pos[2]};
        w.q = {rot[0], rot[1], rot[2], rot[3]};
    }
    if (want && have) g->wantedAt = now;
    if (!want || !have)
    {
        // Brief gaps (a skipped tick, the worker catching up) keep the board; a second without one removes it.
        if (g->phase != Phase::None && g->phase != Phase::Failed && now - g->wantedAt > 1000) Dispose(log, "no longer riding");
        if (!want && g->phase == Phase::Failed) g->phase = Phase::None, g->fallbackTried = false, g->tmpl = kTemplate;
        return;
    }
    switch (g->phase)
    {
    case Phase::None:
        if (!Spawn(g->tmpl, w, log) && g->phase == Phase::Failed && !g->fallbackTried && g->tmpl == kTemplate)
        {
            g->fallbackTried = true;
            log("stage C3b: board: spawning a vanilla prop (a beer bottle) in its place, to check the spawning itself");
            Spawn(kFallback, w, log);
        }
        break;
    case Phase::Spawning:
    {
        bool done = false, failed = false;
        {
            std::lock_guard lock(g_handOff->m);
            if (g_handOff->done)
            {
                done = true;
                failed = g_handOff->failed || !g_handOff->entity.instance;
                g->entity = std::move(g_handOff->entity);
                g_handOff->done = false;
            }
            else if (now - g->requestedAt > 10000) g_handOff->waiting = false;  // a late result becomes a stray
        }
        if (done && !failed)
        {
            g->token = SharedPtr<Token>();  // holds no entity any more: releasing it releases nothing of ours
            g->phase = Phase::Live;
            ent::IComponent* m = MeshOf(g->entity.instance);
            Logf(log, "stage C3b: board: %s spawned after %.0f ms as %s; mesh component %s", g->tmpl, now - g->requestedAt,
                 g->entity.instance->GetType()->GetName().ToString(), m ? m->name.ToString() : "NOT FOUND");
            Place(w, log);
        }
        else if (done || now - g->requestedAt > 10000)
        {
            Token* t = g->token.instance;
            Logf(log, "stage C3b: board: spawn of %s %s", g->tmpl, !done ? "timed out after 10 s" : !t ? "failed (no token)" : t->aborted ? "was aborted" : "failed");
            if (g->entity.instance)  // a failed spawn that still produced an entity: removed with the strays
            {
                std::lock_guard lock(g_handOff->m);
                g_handOff->strays.push_back(std::move(g->entity));
            }
            g->token = SharedPtr<Token>();
            g->phase = Phase::Failed;
            if (!g->fallbackTried && g->tmpl == kTemplate)
            {
                g->fallbackTried = true;
                log("stage C3b: board: spawning a vanilla prop (a beer bottle) in its place, to tell a broken spawn from a board the game will not load");
                Spawn(kFallback, w, log);
            }
        }
        break;
    }
    case Phase::Live:
        Place(w, log);
        if (g->respawnNow)
        {
            g->respawnNow = false;
            const uint32_t n = g->respawns;
            Dispose(log, "stopped following its teleports");
            g->respawns = n;
        }
        break;
    case Phase::Failed: break;
    }
    if (g->phase == Phase::Live && now >= g->nextDiag)
    {
        g->nextDiag = now + 2000;
        Vector4 at{};
        ent::Entity* e = g->entity.instance;
        Call(e, Func(e->GetType(), "GetWorldPosition"), &at, {});
        Logf(log, "stage C3b: board at (%.2f, %.2f, %.2f), entity at (%.2f, %.2f, %.2f), heading %.0f; orientation read back vs sent: max %.1f deg, "
                  "mean %.1f deg over %u frames; Euler angles via %s, round trip through ToQuat max %.2f deg; %u placements (%u by transform writes%s), %u failed calls",
             w.p.x, w.p.y, w.p.z, at.X, at.Y, at.Z, g->yaw, g->errMax, g->errN ? g->errSum / g->errN : 0.f, g->errN,
             g->viaGame ? "the game's ToEulerAngles" : "our decomposition", g->tripMax, g->teleports, g->writes,
             g->writeFailed ? ", given up" : !g_byTransform ? ", set to teleports" : "", g->failedCalls);
        g->errMax = g->errSum = g->tripMax = 0, g->errN = 0;
    }
}

void Remove(LogFn log, const char* why)
{
    if (g->phase != Phase::None) Dispose(log, why);
    g->phase = Phase::None;
    g->fallbackTried = false;
    g->tmpl = kTemplate;
}
void SetByTransform(bool on) { g_byTransform = on; }

void CamFrame(bool want, const float pos[3], const float rot[4], LogFn log)
{
    const double now = NowMs();
    {
        std::vector<Handle<ent::Entity>> strays;
        {
            std::lock_guard lock(g_camHandOff->m);
            strays.swap(g_camHandOff->strays);
        }
        for (auto& e : strays)
        {
            bool attached = false;
            Call(e.instance, Func(e.instance->GetType(), "IsAttached"), &attached, {});
            if (attached) Call(e.instance, Func(e.instance->GetType(), "Dispose"), nullptr, {});
            Logf(log, "stage C4: view camera: a spawn finished after it was abandoned; %s", attached ? "disposed" : "released");
            if (attached) gc->disposed.emplace_back(std::move(e), now);
        }
    }
    for (size_t i = 0; i < gc->disposed.size();)
        if (now - gc->disposed[i].second > 5000) gc->disposed.erase(gc->disposed.begin() + i);
        else ++i;
    if (!want || !pos || !rot)
    {
        if (gc->phase == Phase::Live || gc->phase == Phase::Spawning) CamDispose(log, "no longer wanted");
        if (!want && gc->phase == Phase::Failed) gc->phase = Phase::None;
        return;
    }
    Pose w;
    w.p = {pos[0], pos[1], pos[2]};
    w.q = {rot[0], rot[1], rot[2], rot[3]};
    switch (gc->phase)
    {
    case Phase::None:
    {
        const Req r = RequestSpawn("stage C4: view camera", kCamTemplate, w, g_camHandOff, &OnCamSpawned, gc->token, log);
        if (r == Req::Pending) gc->phase = Phase::Spawning, gc->requestedAt = now;
        else if (r != Req::NotReady) gc->phase = Phase::Failed;
        break;
    }
    case Phase::Spawning:
    {
        bool done = false, failed = false;
        {
            std::lock_guard lock(g_camHandOff->m);
            if (g_camHandOff->done)
            {
                done = true;
                failed = g_camHandOff->failed || !g_camHandOff->entity.instance;
                gc->entity = std::move(g_camHandOff->entity);
                g_camHandOff->done = false;
            }
            else if (now - gc->requestedAt > 10000) g_camHandOff->waiting = false;
        }
        if (done && !failed)
        {
            gc->token = SharedPtr<Token>();
            CClass* cc = Cls("gameCameraComponent");
            for (auto& c : gc->entity->components)
                if (c.instance && cc && c.instance->GetType()->IsA(cc)) { gc->comp = c; break; }
            if (!gc->comp.instance)
            {
                CamDispose(log, "it has no camera component");
                gc->phase = Phase::Failed;
                break;
            }
            auto* pc = reinterpret_cast<ent::IPlacedComponent*>(gc->comp.instance);
            const bool isRoot = gc->entity->transformComponent == pc, bound = pc->parentTransform.instance != nullptr;
            gc->viaRoot = isRoot || bound;
            gc->phase = Phase::Live;
            Logf(log, "stage C4: view camera spawned after %.0f ms: %s \"%s\", %s", now - gc->requestedAt, gc->comp.instance->GetType()->GetName().ToString(),
                 gc->comp->name.ToString(), isRoot ? "the entity's root, so the entity is placed" : bound ? "bound to a parent, so the entity is placed"
                                                                                                   : "unbound, so placed in the world directly");
        }
        else if (done)
        {
            gc->token = SharedPtr<Token>();
            gc->phase = Phase::Failed;
            log("stage C4: view camera: the spawn failed");
        }
        else if (now - gc->requestedAt > 10000)
        {
            gc->phase = Phase::Failed;
            log("stage C4: view camera: no spawn result after 10 s");
        }
        break;
    }
    case Phase::Live:
    {
        auto* pc = reinterpret_cast<ent::IPlacedComponent*>(gc->comp.instance);
        if (gc->sent)
        {
            const float err = Len(WorldOf(pc->worldTransform) - gc->sentAt);
            gc->errMax = std::max(gc->errMax, err), gc->errSum += err, ++gc->errN;
        }
        const bool ok = gc->viaRoot ? xform::Place(gc->entity.instance, w.p, w.q) : xform::PlaceComponent(gc->comp.instance, w.p, w.q);
        if (ok) ++gc->placed, gc->sentAt = w.p, gc->sent = true;
        else ++gc->failed;
        if (now >= gc->nextDiag)
        {
            gc->nextDiag = now + 2000;
            Logf(log, "stage C4: view camera at (%.2f, %.2f, %.2f); its world transform read back %.1f cm off on average, %.1f cm at most over %u frames; "
                      "%u placements, %u failed",
                 w.p.x, w.p.y, w.p.z, gc->errN ? 100.f * gc->errSum / gc->errN : 0.f, 100.f * gc->errMax, gc->errN, gc->placed, gc->failed);
            gc->errMax = gc->errSum = 0, gc->errN = 0;
        }
        break;
    }
    case Phase::Failed: break;
    }
}

RED4ext::IScriptable* CamComponent() { return gc->phase == Phase::Live ? gc->comp.instance : nullptr; }

void CamRemove(LogFn log, const char* why)
{
    if (gc->phase != Phase::None) CamDispose(log, why);
    gc->phase = Phase::None;
}
} // namespace board
