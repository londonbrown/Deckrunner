#include "StageC.hpp"
#include <cctype>
#include "Data.hpp"
#include "Rigid.hpp"
#include "StageB.hpp"
#include "Board.hpp"
#include "Visibility.hpp"
#include "StatusEffectNames.hpp"

#include <RED4ext/Scripting/Natives/Generated/EulerAngles.hpp>
#include <RED4ext/Scripting/Natives/Generated/Quaternion.hpp>
#include <RED4ext/Scripting/Natives/Generated/Vector4.hpp>
#include <RED4ext/Scripting/Natives/Generated/WorldTransform.hpp>
#include <RED4ext/Scripting/Natives/Generated/Transform.hpp>
#include <RED4ext/Scripting/Natives/entEntity.hpp>
#include <RED4ext/Scripting/Natives/Generated/ent/SlotComponent.hpp>
#include <RED4ext/TweakDB.hpp>

#include <windows.h>

#include <algorithm>
#include <map>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace stagec
{
namespace
{
using namespace RED4ext;

template<class... A> void Logf(LogFn log, const char* fmt, A... a)
{
    char b[2048];
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

CClass* Cls(const char* name)
{
    auto* r = CRTTISystem::Get();
    CClass* c = r->GetClass(name);
    return c ? c : r->GetClassByScriptName(name);
}

// Member or static function by short name, in the class or its parents.
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

bool ParamIs(const CBaseFunction* f, uint32_t i, const char* type)
{
    return f && i < f->params.Size() && f->params[i]->type && f->params[i]->type->GetName() == CName(type);
}

// The object `fn` runs on: none for a static function, otherwise the game system registered for class `c` or one of
// its parents (systems register under their interface, e.g. gameIStatusEffectSystem). False when a member function
// has no system to run on: calling it with no object would crash.
bool SelfFor(CClass* c, CBaseFunction* fn, IScriptable*& self)
{
    self = nullptr;
    if (!fn) return false;
    if (fn->flags.isStatic) return true;
    auto* e = CGameEngine::Get();
    auto* gi = e && e->framework ? e->framework->gameInstance : nullptr;
    for (CClass* k = c; gi && k && !self; k = k->parent)
    {
        const std::string n = k->GetName().ToString();
        if (n == "IScriptable" || n == "ISerializable" || n == "gameIGameSystem") break;
        self = gi->GetSystem(k);
    }
    return self != nullptr;
}

// Calls a script or native function through RTTI. Parameters beyond `given` (or given as nullptr) get a
// default-constructed value of their own type; a return value nobody asked for goes to a scratch buffer.
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

const char* TypeName(const rtti::IType* t) { return t ? t->GetName().ToString() : "?"; }

std::string Signature(const CBaseFunction* f)
{
    std::string s = f->shortName.ToString();
    s += "(";
    for (uint32_t i = 0; i < f->params.Size(); ++i)
    {
        const auto* p = f->params[i];
        if (i) s += ", ";
        s += p->name.ToString();
        s += ": ";
        s += TypeName(p->type);
        if (p->flags.isOut) s += " out";
        if (p->flags.isOptional) s += " opt";
    }
    s += ")";
    if (f->returnType && f->returnType->type) s += std::string(" -> ") + TypeName(f->returnType->type);
    const auto& fl = f->flags;
    s += std::string(" [") + (fl.isNative ? "native " : "script ") + (fl.isStatic ? "static " : "") + (fl.isFinal ? "final " : "") +
         (fl.isEvent ? "event " : "") + (fl.isExec ? "exec " : "") + (fl.isPrivate ? "private" : fl.isProtected ? "protected" : "public") + "]";
    return s;
}

void DumpClass(FILE* f, const char* name)
{
    CClass* c = Cls(name);
    if (!c) { fprintf(f, "\nclass %s: NOT FOUND\n", name); return; }
    auto* r = CRTTISystem::Get();
    fprintf(f, "\nclass %s (script name %s), size 0x%x, parents:", c->GetName().ToString(),
            r->ConvertNativeToScriptName(c->GetName()).ToString(), c->size);
    for (CClass* p = c->parent; p; p = p->parent) fprintf(f, " %s", p->GetName().ToString());
    fprintf(f, "\n");
    for (auto* p : c->props)
        if (p) fprintf(f, "  prop %s: %s @0x%x\n", p->name.ToString(), TypeName(p->type), p->valueOffset);
    for (auto* p : c->unk118)  // native properties (the SDK notes this list holds more than `props`)
        if (p) fprintf(f, "  native prop %s: %s @0x%x\n", p->name.ToString(), TypeName(p->type), p->valueOffset);
    for (auto* fn : c->funcs)
        if (fn) fprintf(f, "  func %s\n", Signature(fn).c_str());
    for (auto* fn : c->staticFuncs)
        if (fn) fprintf(f, "  static %s\n", Signature(fn).c_str());
}

bool RecordExists(const char* id, std::string* type = nullptr)
{
    auto* db = TweakDB::Get();
    Handle<IScriptable> rec;
    if (!db || !db->TryGetRecord(TweakDBID(id), rec) || !rec.instance) return false;
    if (type) *type = rec.instance->GetType()->GetName().ToString();
    return true;
}

using namespace rigid;

// RTTI "Matrix": four Vector4s X, Y, Z, W.
struct alignas(16) Mat
{
    Vector4 r[4];
};
// rows = true: X, Y, Z are the local axes and W the translation (row vectors); false: the transpose.
Pose Decode(const Mat& m, bool rows)
{
    const auto& a = m.r;
    if (rows) return {{a[3].X, a[3].Y, a[3].Z}, FromBasis({a[0].X, a[0].Y, a[0].Z}, {a[1].X, a[1].Y, a[1].Z}, {a[2].X, a[2].Y, a[2].Z})};
    return {{a[0].W, a[1].W, a[2].W}, FromBasis({a[0].X, a[1].X, a[2].X}, {a[0].Y, a[1].Y, a[2].Y}, {a[0].Z, a[1].Z, a[2].Z})};
}

// ----- C1 -----
// 0.14.0 (controller skating): also NoCameraControl (the right stick is Skate's Flick-It, not the game's look),
// NoRadialMenus (weapon and quick wheels) and NoScanning. All of them are applied, tracked and removed the same way.
// 0.17.0 (P1-3, first in-game ride): the game still acted on R1 (grenade), the d-pad (vehicle, pocket radio, healing)
// and L2 (zoom). Each has its own restriction record in the 2.5 TweakDB (wkprobe tweak), by the gameplay tag it adds:
// NoCombat (NoCombat, NoQuickHacks, NoGrenadeOrGadget), NoHealing (NoInhalerOrInjector), VehicleNoSummoning,
// VehicleBlockRadioInput (also VehicleBlockPocketRadio, which NoRadialMenus lacks), NoZooming and NoQuickMelee.
constexpr const char* kRestrictions[] = {
    "GameplayRestriction.NoMovement", "GameplayRestriction.NoJump", "GameplayRestriction.NoWeapons",
    "GameplayRestriction.NoWorldInteractions", "GameplayRestriction.NoPhone", "GameplayRestriction.NoCameraControl",
    "GameplayRestriction.NoRadialMenus", "GameplayRestriction.NoScanning", "GameplayRestriction.NoCombat",
    "GameplayRestriction.NoHealing", "GameplayRestriction.VehicleNoSummoning", "GameplayRestriction.VehicleBlockRadioInput",
    "GameplayRestriction.NoZooming", "GameplayRestriction.NoQuickMelee"};
constexpr size_t kRestrictionCount = sizeof kRestrictions / sizeof kRestrictions[0];
// The gameplay tags the game's own checks look for (grenades and gadgets, healing items, vehicle summoning, the pocket
// radio, zoom, quick melee), read back about a second after entry so the log shows which actions are actually blocked.
constexpr const char* kBlockedTags[] = {"NoCombat", "NoGrenadeOrGadget", "NoInhalerOrInjector", "VehicleNoSummoning",
                                        "VehicleBlockPocketRadio", "NoZooming", "NoQuickMelee", "NoPhone", "NoRadialMenus"};
double g_tagCheckAt = 0;  // when to read kBlockedTags back (0: not pending)
constexpr const char* kHeadSlot = "AttachmentSlots.TppHead";
constexpr float kChaseBack = 2.8f, kChaseHeight = 1.75f;  // camera behind V's feet and above them, level

// C1 cycle (0.9.4, an experiment build): FirstBody = camera behind V and the photo-mode head, V keeps the first-person
// body (as JB TPP did); ThirdBody = plus the game's third-person body (the TPP representation); Restricted = plus the
// skating restrictions. Each step is logged with V's crouch state and status effects, to find what forces the crouch
// (C1 run 4: steady camera standing, shaking when crouched, and the game kept putting V back into a crouch).
enum class Mode { Off, FirstBody, ThirdBody, Restricted };
struct State
{
    Mode mode = Mode::Off;
    uintptr_t player = 0;  // identity of the player object changed (a loaded save brings a new one)
    double enteredAt = 0;
    bool prepared = false, activated = false, settled = false;
    double tppAt = 0;           // when the TPP representation was requested (ThirdBody)
    std::string lastEffects;    // last logged snapshot (logged again only when it changes)
    // Head: the item in AttachmentSlots.TppHead swapped for the photo-mode head while third person is on.
    alignas(16) uint8_t oldHead[16] = {}, tppHead[16] = {};  // gameItemID values: the slot's own item, and the photo-mode head
    // Camera
    Handle<IScriptable> camera;  // V's FPP camera component
    bool haveCamera = false, chase = false, rows = true;
    Vector4 cameraPos{};
    Quaternion cameraRot{};
    Pose local;  // the camera's local transform as last set
    Pose wanted;
    Handle<IScriptable> playerHandle;      // V, held while third person is on (the hook needs it)
    Handle<ent::IComponent> slots;          // V's "slots" component: the camera hangs off its "camera" slot
    Pose parent;                            // P1-15 (0.24.1): the camera's parent at the last placement
    bool haveParent = false;
    uint32_t teleportPlacements = 0;        // P1-15 (0.24.1): placements again after a teleport
    V3 vp{0, 0, 0};                         // V's feet at the last placement
    uint32_t hookPlacedSeen = 0;
    uint32_t frames = 0;
    double nextDiag = 0;
    // P1-3 (0.18.0): the camera's zoom at entry, held while the camera is ours; and what moved it, per 2 s window.
    float zoom0 = 1.f;
    bool zoomKnown = false;
    uint32_t zoomResets = 0, zoomResetsTotal = 0;
    float zoomMax = 0, overrideW = 0, overrideV = 0, weaponW = 0, weaponV = 0;
    uint32_t overrideWrites = 0, overrideWritesTotal = 0;  // P1-3 (0.19.0): override weights written back to 0
    // P1-3 (0.20.0): the camera's two zoom-override animation parameter names, as they were, while ours point at None.
    uint64_t zoomParams[2] = {};
    bool zoomParamsSwapped = false;
} g;

// Restrictions this plugin applied, kept apart from `g` so they outlive an exit until the game confirms they are gone.
// C1 run 2: the game adds a status effect a frame after ApplyStatusEffect returns, so the immediate check said "not on
// V", nothing was recorded, and the exit left all five on V.
struct Cleanup
{
    uintptr_t player = 0;
    std::vector<std::string> ids;
    double appliedAt = 0;  // 0.24.0: when the last of them was applied (RestrictionsLost waits for them to land)
    double nextTry = 0;
    uint32_t tries = 0;
} g_ours;

bool QueueEvent(IScriptable* player, const char* eventClass, LogFn log)
{
    CClass* ec = Cls(eventClass);
    CBaseFunction* qe = Func(Cls("entEntity"), "QueueEvent");
    if (!ec || !qe) { Logf(log, "stage C1: cannot queue %s (%s missing)", eventClass, ec ? "entEntity.QueueEvent" : "event class"); return false; }
    Handle<IScriptable> evt(static_cast<IScriptable*>(ec->CreateInstance(true)));
    const bool ok = Call(player, qe, nullptr, {&evt});
    Logf(log, "stage C1: queued %s on V: %s", eventClass, ok ? "ok" : "FAILED");
    return ok;
}

bool HasEffect(const Handle<IScriptable>& player, const char* id)
{
    TweakDBID tdb(id);
    bool has = false;
    CClass* c = Cls("gameStatusEffectSystem");
    CBaseFunction* f = Func(c, "ObjectHasStatusEffect");
    IScriptable* self = nullptr;
    if (!SelfFor(c, f, self)) return false;
    Call(self, f, &has, {const_cast<Handle<IScriptable>*>(&player), &tdb});
    return has;
}

bool HasTag(const Handle<IScriptable>& player, const char* tag)
{
    CName name(tag);
    bool has = false;
    CClass* c = Cls("gameStatusEffectSystem");
    CBaseFunction* f = Func(c, "ObjectHasStatusEffectWithTag");
    IScriptable* self = nullptr;
    if (!SelfFor(c, f, self)) return false;
    Call(self, f, &has, {const_cast<Handle<IScriptable>*>(&player), &name});
    return has;
}

// P1-3: which of the blocking tags the game sees on V, once the restrictions have landed.
void LogBlockedTags(const Handle<IScriptable>& player, LogFn log)
{
    std::string on, off;
    for (const char* t : kBlockedTags) (HasTag(player, t) ? on : off) += std::string(" ") + t;
    Logf(log, "stage C1: blocking tags on V:%s; missing:%s", on.empty() ? " none" : on.c_str(), off.empty() ? " none" : off.c_str());
}

bool EntityIdOf(IScriptable* v, uint8_t (&eid)[16]) { return Call(v, Func(Cls("entEntity"), "GetEntityID"), eid, {}); }

// One removal (count 1). The game drops the effect on a later frame, so success is confirmed by StepCleanup.
bool RemoveEffect(const Handle<IScriptable>& player, const char* id)
{
    CClass* sc = Cls("gameStatusEffectSystem");
    CBaseFunction* remove = Func(sc, "RemoveStatusEffect");
    IScriptable* self = nullptr;
    alignas(16) uint8_t eid[16] = {};
    if (!SelfFor(sc, remove, self) || !ParamIs(remove, 0, "entEntityID") || !EntityIdOf(player.instance, eid)) return false;
    TweakDBID tdb(id);
    uint32_t one = 1;
    bool result = false;
    Call(self, remove, &result, {eid, &tdb, ParamIs(remove, 2, "Uint32") ? &one : nullptr});
    return result;
}

// The game's own status-effect system (2.5: StatusEffectHelper has no functions). Applied once each, not savable.
void ApplyRestrictions(const Handle<IScriptable>& player, LogFn log)
{
    CClass* sc = Cls("gameStatusEffectSystem");
    CBaseFunction* apply = Func(sc, "ApplyStatusEffect");
    IScriptable* self = nullptr;
    if (!SelfFor(sc, apply, self) || !ParamIs(apply, 0, "entEntityID") || !ParamIs(apply, 1, "TweakDBID"))
    {
        log("stage C1: StatusEffectSystem.ApplyStatusEffect(objID: EntityID, statusEffectID: TweakDBID, ...) not callable; no restrictions applied");
        return;
    }
    alignas(16) uint8_t eid[16] = {};
    if (!EntityIdOf(player.instance, eid)) { log("stage C1: V's EntityID unavailable; no restrictions applied"); return; }
    if (g_ours.player != reinterpret_cast<uintptr_t>(player.instance)) g_ours = Cleanup{};
    g_ours.player = reinterpret_cast<uintptr_t>(player.instance);
    for (const char* id : kRestrictions)
    {
        if (!RecordExists(id)) { Logf(log, "stage C1: %s does not exist; skipped", id); continue; }
        if (HasEffect(player, id)) { Logf(log, "stage C1: %s already on V before entry; left alone", id); continue; }
        TweakDBID tdb(id);
        uint32_t one = 1;
        bool savable = false, result = false;
        // objID, statusEffectID, instigatorID, instigatorEntityID, applyCount, direction, isSavable, proxyEntityID
        const bool ok = Call(self, apply, &result, {eid, &tdb, nullptr, nullptr, ParamIs(apply, 4, "Uint32") ? &one : nullptr, nullptr,
                                                    ParamIs(apply, 6, "Bool") ? &savable : nullptr});
        Logf(log, "stage C1: applied %s: call %s, returned %s (the game adds it on a later frame)", id, ok ? "ok" : "FAILED",
             result ? "true" : "false");
        if (ok && result && std::find(g_ours.ids.begin(), g_ours.ids.end(), id) == g_ours.ids.end()) g_ours.ids.push_back(id);
    }
    g_tagCheckAt = NowMs() + 1000;
    g_ours.appliedAt = NowMs();
}

void BeginCleanup(const Handle<IScriptable>& player, LogFn log)
{
    for (const auto& id : g_ours.ids)
        Logf(log, "stage C1: removing %s: %s", id.c_str(), RemoveEffect(player, id.c_str()) ? "accepted" : "NOT accepted");
    g_ours.nextTry = NowMs() + 250;
    g_ours.tries = 0;
}

// Confirms our restrictions are gone, retrying the removal every 250 ms (8 tries).
void StepCleanup(const Handle<IScriptable>& player, LogFn log)
{
    if (g_ours.ids.empty() || NowMs() < g_ours.nextTry) return;
    if (!player.instance || reinterpret_cast<uintptr_t>(player.instance) != g_ours.player)
    {
        log("stage C1: V was replaced (a save or checkpoint loaded); our restrictions went with the old V");
        g_ours = Cleanup{};
        return;
    }
    std::vector<std::string> left;
    for (const auto& id : g_ours.ids)
        if (HasEffect(player, id.c_str())) left.push_back(id);
    if (left.empty())
    {
        Logf(log, "stage C1: confirmed: every restriction we applied is off V (after %u checks)", g_ours.tries + 1);
        g_ours = Cleanup{};
        return;
    }
    if (++g_ours.tries >= 8)
    {
        for (const auto& id : left) Logf(log, "stage C1: %s is STILL ON V after 8 removals; Ctrl+Shift+F6 clears all of them", id.c_str());
        g_ours.ids = left;
        g_ours.nextTry = 1e300;  // stop retrying; the emergency key starts again
        return;
    }
    for (const auto& id : left) RemoveEffect(player, id.c_str());
    g_ours.ids = left;
    g_ours.nextTry = NowMs() + 250;
}

CProperty* Prop(CClass* c, const char* name)
{
    const CName n(name);
    for (; c; c = c->parent)
    {
        for (auto* p : c->props)
            if (p && p->name == n) return p;
        for (auto* p : c->unk118)
            if (p && p->name == n) return p;
    }
    return nullptr;
}

// The FPP camera's parent binding (IPlacedComponent.parentTransform): class, bindName, slotName, enabled.
std::string CameraBinding(IScriptable* cam, ISerializable** obj = nullptr, bool** enabled = nullptr)
{
    CProperty* pt = Prop(cam->GetType(), "parentTransform");
    if (!pt || std::string(TypeName(pt->type)).rfind("handle:", 0) != 0) return "no parentTransform handle";
    auto* h = reinterpret_cast<Handle<ISerializable>*>(reinterpret_cast<uint8_t*>(cam) + pt->valueOffset);
    if (!h->instance) return "no parent binding";
    ISerializable* b = h->instance;
    CClass* bc = b->GetType();
    std::string s = bc->GetName().ToString();
    for (const char* f : {"bindName", "slotName"})
        if (CProperty* p = Prop(bc, f); p && TypeName(p->type) == std::string("CName"))
            s += std::string(" ") + f + "=" + reinterpret_cast<CName*>(reinterpret_cast<uint8_t*>(b) + p->valueOffset)->ToString();
    if (CProperty* p = Prop(bc, "enabled"); p && TypeName(p->type) == std::string("Bool"))
    {
        bool* e = reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(b) + p->valueOffset);
        s += std::string(" enabled=") + (*e ? "1" : "0");
        if (enabled) *enabled = e;
    }
    if (obj) *obj = b;
    return s;
}

const char* HeadName(const TweakDBID& rec)
{
    for (const char* n : {"Items.PlayerFppHead", "Items.CharacterCustomizationWaHead", "Items.CharacterCustomizationMaHead",
                          "Items.PlayerWaTppHead", "Items.PlayerMaTppHead", "Items.PlayerWaPhotomodeHead", "Items.PlayerMaPhotomodeHead"})
        if (TweakDBID(n) == rec) return n;
    return nullptr;
}

// ----- V's head, the way AMM does it on 2.x (AppearanceMenuMod, Modules/tools.lua, Tools:ToggleHead) -----
// In first person AttachmentSlots.TppHead holds Items.PlayerFppHead, which renders only in shadows. AMM:
//  - on: gives V Items.CharacterCustomization{Wa,Ma}Head if missing, removes the slot's item (destroy, keep world
//    transform, skip anim equip events), and from 0.1 s later attaches its head once the slot is empty, retrying;
//  - off: gives that head the "default&FPP" appearance, removes it the same way, then re-attaches Items.PlayerFppHead
//    with "default&FPP".
// 0.9.4 instead swapped in the same frame with default flags, and the game crashed about a second later (a DEP fault
// on a worker thread, C1 run 5). 0.9.0-0.9.3's photo-mode and TPP heads never showed.
const char* HeadSlotItem(const Handle<IScriptable>& player);  // defined with the diagnostics below

struct HeadJob
{
    uintptr_t player = 0;
    bool on = false;       // our head is attached or being attached
    bool pending = false;  // waiting for the slot to empty, then attaching
    bool given = false;    // this plugin put the customization head into V's inventory
    double removedAt = 0, nextTry = 0;
    uint32_t tries = 0;
    bool added = false;    // AddItemToSlot made for this attempt (0.9.7: once per attempt)
    double addedAt = 0;
    uint32_t cycles = 0;   // removals after the game put another item back
    alignas(16) uint8_t item[16] = {};  // gameItemID of Items.CharacterCustomization{Wa,Ma}Head
    std::string record;
} g_head;

struct Transactions
{
    IScriptable* self = nullptr;
    CBaseFunction *has = nullptr, *give = nullptr, *remove = nullptr, *fromSlot = nullptr, *add = nullptr, *isEmpty = nullptr, *appearance = nullptr;
    bool ok = false;
    Transactions()
    {
        CClass* tc = Cls("gameTransactionSystem");
        has = Func(tc, "HasItem"), give = Func(tc, "GiveItem"), remove = Func(tc, "RemoveItem"), fromSlot = Func(tc, "RemoveItemFromSlot");
        add = Func(tc, "AddItemToSlot"), isEmpty = Func(tc, "IsSlotEmpty"), appearance = Func(tc, "ChangeItemAppearanceByName");
        ok = SelfFor(tc, fromSlot, self) && has && give && remove && add && isEmpty && appearance;
    }
};

bool ItemOf(const char* record, uint8_t (&out)[16])
{
    CBaseFunction* from = Func(Cls("gameItemID"), "FromTDBID");
    auto* type = CRTTISystem::Get()->GetType("gameItemID");
    if (!from || !from->flags.isStatic || !type || type->GetSize() > sizeof out) return false;
    type->Construct(out);
    TweakDBID t(record);
    return Call(nullptr, from, out, {&t});
}


// ----- C4: V's head (0.13.0), a careful experiment on Ctrl+Shift+F2 -----
// In first person V has no visible head: AttachmentSlots.TppHead holds Items.PlayerFppHead, and her entity carries a
// shadow-only head mesh ("head_shadowmesh"), as the game does for every worn head item (…\shadow_meshes\). What C1
// learned: destroying PlayerFppHead (AMM's swap, shouldDestroyEntity) emptied the slot and the game crashed 2 s later
// (0.9.4-0.9.5), in the same engine path as the C3b board's use-after-free (r12/r14 identical); 0.9.7's swap never
// emptied the slot. Never tested: the appearance change alone with a registered name (0.9.6 sent "").
// So nothing is destroyed here. Press 1: the TppHead item gets the appearance "default&TPP" (AMM gives heads that in
// vehicles). Press 2: V's character-creator head is attached to an empty slot (ignoring the slot's restrictions), next
// to the first-person head, not in place of it. Press 3: both undone (our head item removed, appearance "default&FPP").
// Every step logs what the slots hold 1 s later.
// P2-10: the head experiment (Ctrl+Shift+F2 while skating). The game draws V's head for every camera except her own
// first-person one (True First Person Camera 2.0, 9.0.0), and our skating view is that camera moved behind her; her face is
// built only in third-person mode (the TPP representation, as in a camera takeover). Step 1 views her through a camera of
// our own (board::CamFrame, placed where the skating camera is); step 2 adds the TPP representation; a third press, or the
// exit, undoes both.
// 0.25.2 (after 0.25.0, where nothing showed whether our camera ever took the view): our camera's field of view is 1
// degree wider than the game camera's, and CameraSystem.GetActiveCameraFOV says which one the game renders from. If ours
// has not taken the view 1 s after Activate, V's own third-person camera (her "tppCamera" component, as JB TPP used it)
// is tried instead.
struct ViewExp
{
    enum class Cam { None, Entity, Tpp };
    int step = 0;
    Cam which = Cam::None;            // the camera we activated
    Handle<IScriptable> tpp;          // V's tppCamera, once tried
    bool entityGaveUp = false, tppGaveUp = false, haveBaseline = false;
    float baselineFov = 0;            // the active camera's field of view before ours was activated
    double activatedAt = 0, notOursSince = 0, nextDiag = 0;
    const char* lastWho = "";
    bool featureOn = false;           // 0.25.3: the TPPRepresentation animation feature was applied
    double dumpAt[2] = {};            // when to log V's head parts after the reload (0.25.3: after step 2)
    // 0.25.4: the head reloaded once the TPP representation has settled (see HeadReloadStep).
    enum class Reload { Waiting, Removed, Added, Done };
    Reload reload = Reload::Waiting;
    double removedAt = 0, addedAt = 0, reloadExpire = 0;
    uintptr_t removedItem = 0;        // the ItemObject taken out, compared by address only (never touched again)
    alignas(16) uint8_t reloadId[16] = {};
    // 0.25.7: the head shield (ShieldStep).
    bool shieldReported = false;
    uint32_t shieldParts = 0, shieldCleared = 0;
    double shieldLogAt = 0;
    // 0.25.8: other parts of V's the engine hides the same way (hats, glasses).
    std::vector<uintptr_t> extraParts;
    // 0.26.0: the head runs by itself while skating (HeadAuto).
    double step1At = 0;
    bool oursActive = false, noCamReported = false;
} g_vx;

bool g_headOff = false;  // 0.26.0: V's head turned off with Ctrl+Shift+F2 for this ride

struct HeadExp
{
    int step = 0;
    uintptr_t player = 0;
    alignas(16) uint8_t cc[16] = {};  // the character-creator head's gameItemID
    bool given = false;
    const char* slot = nullptr;       // where press 2 attached it
    double checkAt = 0;
} g_hx;

CName Registered(const char* s) { return CNamePool::Add(s); }

// The item in a slot: its gameItemID, record and appearance, for the log.
std::string SlotReport(const Handle<IScriptable>& player, const char* slotName, uint8_t (*idOut)[16] = nullptr)
{
    CClass* tc = Cls("gameTransactionSystem");
    CBaseFunction* inSlot = Func(tc, "GetItemInSlot");
    CBaseFunction* getApp = Func(tc, "GetItemAppearance");
    CBaseFunction* getTdb = Func(Cls("gameItemID"), "GetTDBID");
    IScriptable* ts = nullptr;
    if (!SelfFor(tc, inSlot, ts) || !getTdb) return "(unavailable)";
    TweakDBID slot(slotName);
    Handle<IScriptable> item;
    Call(ts, inSlot, &item, {const_cast<Handle<IScriptable>*>(&player), &slot});
    if (!item.instance) return "empty";
    alignas(16) uint8_t id[16] = {};
    Call(item.instance, Func(item.instance->GetType(), "GetItemID"), id, {});
    if (idOut) std::memcpy(*idOut, id, 16);
    TweakDBID rec;
    Call(nullptr, getTdb, &rec, {id});
    CName app;
    if (getApp) Call(ts, getApp, &app, {const_cast<Handle<IScriptable>*>(&player), id});
    const char* n = HeadName(rec);
    char b[256];
    snprintf(b, sizeof b, "%s (appearance \"%s\")", n ? n : "an item", app.ToString());
    return b;
}

bool SetSlotItemAppearance(const Transactions& t, Handle<IScriptable>* pl, const char* slotName, const char* appearance)
{
    alignas(16) uint8_t id[16] = {};
    if (SlotReport(*pl, slotName, &id) == "empty") return false;
    CName a = Registered(appearance);
    return Call(t.self, t.appearance, nullptr, {pl, id, &a});
}

void HeadExpUndo(const Handle<IScriptable>& player, LogFn log, const char* why)
{
    if (g_hx.step == 0) return;
    const Transactions t;
    auto* pl = const_cast<Handle<IScriptable>*>(&player);
    if (!t.ok || !player.instance || reinterpret_cast<uintptr_t>(player.instance) != g_hx.player)
    {
        Logf(log, "stage C4: head experiment dropped (%s): V is a new object or the transaction system is unavailable", why);
        g_hx = HeadExp{};
        return;
    }
    if (g_hx.slot)
    {
        TweakDBID slot(g_hx.slot);
        bool yes = true, r = false;
        Call(t.self, t.fromSlot, &r, {pl, &slot, &yes, &yes, &yes});
        Logf(log, "stage C4: head experiment: our head removed from %s: %s", g_hx.slot, r ? "yes" : "NO");
    }
    if (g_hx.given)
    {
        int32_t one = 1;
        bool removed = false;
        Call(t.self, t.remove, &removed, {pl, g_hx.cc, ParamIs(t.remove, 2, "Int32") ? &one : nullptr});
    }
    const bool app = SetSlotItemAppearance(t, pl, kHeadSlot, "default&FPP");
    Logf(log, "stage C4: head experiment undone (%s); TppHead appearance back to default&FPP: %s; TppHead holds %s", why, app ? "set" : "NOT set",
         SlotReport(player, kHeadSlot).c_str());
    g_hx = HeadExp{};
}

// P2-10 (0.25.0): CameraComponent.Activate / Deactivate(blendTime, shouldOverrideAudioListeners) with no blend.
bool CamSwitch(IScriptable* cam, bool on)
{
    CBaseFunction* f = cam ? Func(cam->GetType(), on ? "Activate" : "Deactivate") : nullptr;
    if (!f || (f->params.Size() > 0 && !ParamIs(f, 0, "Float"))) return false;
    float blend = 0.f;
    return Call(cam, f, nullptr, {&blend});
}

bool QueueEvent(IScriptable* player, const char* eventClass, LogFn log);

// ----- P2-10 (0.25.1, used again by 0.25.3's step 2): V's head the way AMM shows it -----
// AMM's third person (MaximiliumM/appearancemenumod, Modules/tools.lua: Tools:ToggleTPPCamera, Tools:ToggleHead) is V's
// first-person camera moved behind her, as ours is. It shows her head by putting the character-creator head,
// Items.CharacterCustomization{Wa,Ma}Head, in AttachmentSlots.TppHead itself; DriveAerialVehicle (game 2.1x) copies it.
// Its sequence, here frame by frame (CET's Cron at 0.001 s fires once a frame; Util:CalculateDelay(5) is 5 at any rate):
//  - on: the head given if V has none (GiveItem, then an EquipRequest with only its owner, as Util:AddToInventory does);
//    RemoveItemFromSlot(TppHead, destroy, keep transform, skip anim events) at once; then a tick every frame from tick 2:
//    a removal on the next frame and, 0.1 s later, the head added if the slot is empty. Once a tick passes 5, the ticks
//    stop 0.1 s later (the removals and adds already due still happen).
//  - off: the same with Items.PlayerFppHead (added, then set to default&FPP); each tick also sets the character-creator
//    head to default&FPP; the ticks stop at the first one past 5.
// C1 runs 5-6 (0.9.4-0.9.5) crashed about 2 s after destroying PlayerFppHead with their own timing (an add every 0.1 s
// while the slot looked empty). This follows AMM's timing exactly and logs every removal and add.
constexpr int kAmmDelay = 5;
struct AmmHead
{
    enum class Dir { None, On, Off };
    Dir dir = Dir::None;
    uintptr_t player = 0;
    alignas(16) uint8_t cc[16] = {}, fpp[16] = {};  // gameItemIDs: the character-creator head, PlayerFppHead
    const char* record = nullptr;
    bool on = false;            // our head was put in TppHead (and not taken back out)
    int tick = 0;
    double haltAt = 0;          // the ticks stop then (0: not set yet)
    int removesDue = 0;         // removals due on the next frame
    std::vector<double> addAt;  // add checks due at these times
    double reportAt[2] = {};    // slot reports after the swap
    double startedAt = 0;
    uint32_t removes = 0, removed = 0, adds = 0, added = 0, full = 0;
} g_amm;

bool AmmPending() { return g_amm.dir != AmmHead::Dir::None || g_amm.reportAt[0] > 0 || g_amm.reportAt[1] > 0; }

// AMM's Util:AddToInventory, after GiveItem: an EquipRequest with only its owner set, queued on the EquipmentSystem.
bool QueueEquipRequest(const Handle<IScriptable>& player)
{
    CClass* sc = Cls("gameScriptableSystemsContainer");
    CBaseFunction* get = Func(sc, "Get");
    CClass* rc = Cls("EquipRequest");
    IScriptable* container = nullptr;
    if (!rc || !SelfFor(sc, get, container) || !ParamIs(get, 0, "CName")) return false;
    Handle<IScriptable> sys;
    CName name("EquipmentSystem");
    if (!Call(container, get, &sys, {&name}) || !sys.instance) return false;
    CBaseFunction* queue = Func(sys.instance->GetType(), "QueueRequest");
    CProperty* owner = Prop(rc, "owner");
    if (!queue || !owner) return false;
    Handle<IScriptable> req(static_cast<IScriptable*>(rc->CreateInstance(true)));
    const std::string type = TypeName(owner->type);
    auto* at = reinterpret_cast<uint8_t*>(req.instance) + owner->valueOffset;
    if (type.rfind("whandle:", 0) == 0)
    {
        WeakHandle<IScriptable> w(player);
        owner->type->Assign(at, &w);
    }
    else if (type.rfind("handle:", 0) == 0) owner->type->Assign(at, &player);
    else return false;
    return Call(sys.instance, queue, nullptr, {&req});
}

bool HeadSlotEmpty(const Handle<IScriptable>& player)
{
    CClass* tc = Cls("gameTransactionSystem");
    CBaseFunction* inSlot = Func(tc, "GetItemInSlot");
    IScriptable* ts = nullptr;
    if (!SelfFor(tc, inSlot, ts)) return false;
    TweakDBID slot(kHeadSlot);
    Handle<IScriptable> item;
    Call(ts, inSlot, &item, {const_cast<Handle<IScriptable>*>(&player), &slot});
    return item.instance == nullptr;
}

void AmmStart(const Handle<IScriptable>& player, bool on, LogFn log)
{
    if (g_amm.dir != AmmHead::Dir::None) { log("stage C4: head (AMM's way): a swap is still running; wait a moment"); return; }
    const Transactions t;
    if (!t.ok || !player.instance) { log("stage C4: head (AMM's way): TransactionSystem functions unavailable"); return; }
    auto* pl = const_cast<Handle<IScriptable>*>(&player);
    CName gender;
    Call(player.instance, Func(player.instance->GetType(), "GetResolvedGenderName"), &gender, {});
    const char* record = gender == CName("Female") ? "Items.CharacterCustomizationWaHead" : "Items.CharacterCustomizationMaHead";
    alignas(16) uint8_t cc[16] = {}, fpp[16] = {};
    if (!RecordExists(record) || !ItemOf(record, cc) || !ItemOf("Items.PlayerFppHead", fpp))
    {
        Logf(log, "stage C4: head (AMM's way): %s or Items.PlayerFppHead unavailable", record);
        return;
    }
    const bool wasOn = g_amm.on;
    g_amm = AmmHead{};
    g_amm.on = wasOn;
    g_amm.player = reinterpret_cast<uintptr_t>(player.instance);
    std::memcpy(g_amm.cc, cc, 16), std::memcpy(g_amm.fpp, fpp, 16);
    g_amm.record = record;
    g_amm.dir = on ? AmmHead::Dir::On : AmmHead::Dir::Off;
    g_amm.tick = 1;
    g_amm.startedAt = NowMs();
    const std::string before = SlotReport(player, kHeadSlot);
    const char* gift = "already in V's inventory";
    if (on)
    {
        bool has = false;
        Call(t.self, t.has, &has, {pl, g_amm.cc});
        if (!has)
        {
            int32_t one = 1;
            bool given = false;
            Call(t.self, t.give, &given, {pl, g_amm.cc, ParamIs(t.give, 2, "Int32") ? &one : nullptr});
            gift = !given ? "NOT given" : QueueEquipRequest(player) ? "given (EquipRequest queued)" : "given (EquipRequest NOT queued)";
        }
    }
    TweakDBID slot(kHeadSlot);
    bool yes = true, r = false;
    Call(t.self, t.fromSlot, &r, {pl, &slot, &yes, &yes, &yes});
    ++g_amm.removes, g_amm.removed += r;
    Logf(log, "stage C4: head (AMM's way) %s: TppHead held %s; %s %s; removed from TppHead at once: %s", on ? "ON" : "OFF", before.c_str(),
         on ? record : "Items.PlayerFppHead goes back;", on ? gift : "", r ? "true" : "false");
}

void AmmStep(const Handle<IScriptable>& player, LogFn log)
{
    if (!AmmPending()) return;
    if (!player.instance || reinterpret_cast<uintptr_t>(player.instance) != g_amm.player)
    {
        log("stage C4: head (AMM's way): V is a new object; the swap is dropped");
        g_amm = AmmHead{};
        return;
    }
    const double now = NowMs();
    const Transactions t;
    if (!t.ok) return;
    auto* pl = const_cast<Handle<IScriptable>*>(&player);
    TweakDBID slot(kHeadSlot);
    bool yes = true;
    if (g_amm.dir != AmmHead::Dir::None)
    {
        const bool on = g_amm.dir == AmmHead::Dir::On;
        uint8_t* id = on ? g_amm.cc : g_amm.fpp;
        // Removals scheduled on the last frame (Cron.After(0.001)).
        for (; g_amm.removesDue > 0; --g_amm.removesDue)
        {
            bool r = false;
            Call(t.self, t.fromSlot, &r, {pl, &slot, &yes, &yes, &yes});
            ++g_amm.removes, g_amm.removed += r;
        }
        // Add checks that are due (Cron.After(0.1)): the head goes in only while the slot is empty.
        for (auto it = g_amm.addAt.begin(); it != g_amm.addAt.end();)
        {
            if (*it > now) { ++it; continue; }
            it = g_amm.addAt.erase(it);
            if (!HeadSlotEmpty(player)) { ++g_amm.full; continue; }
            bool ok = false;
            Call(t.self, t.add, &ok, {pl, &slot, id});
            ++g_amm.adds, g_amm.added += ok;
            if (!on)
            {
                CName a = Registered("default&FPP");
                Call(t.self, t.appearance, nullptr, {pl, g_amm.fpp, &a});
            }
            Logf(log, "stage C4: head (AMM's way): %.0f ms in, the slot was empty: AddItemToSlot(%s) %s", now - g_amm.startedAt,
                 on ? g_amm.record : "Items.PlayerFppHead", ok ? "true" : "false");
        }
        // This frame's tick (Cron.Every(0.001)).
        if (g_amm.haltAt == 0 || now < g_amm.haltAt)
        {
            ++g_amm.tick;
            if (!on)
            {
                CName a = Registered("default&FPP");
                Call(t.self, t.appearance, nullptr, {pl, g_amm.cc, &a});
            }
            if (g_amm.tick > kAmmDelay && g_amm.haltAt == 0) g_amm.haltAt = on ? now + 100 : now;
            ++g_amm.removesDue;
            g_amm.addAt.push_back(now + 100);
        }
        else if (g_amm.removesDue == 0 && g_amm.addAt.empty())
        {
            Logf(log, "stage C4: head (AMM's way) %s done in %.0f ms: %u ticks, %u removals (%u true), %u adds (%u true), %u add checks found the "
                      "slot full; TppHead holds %s", on ? "ON" : "OFF", now - g_amm.startedAt, g_amm.tick - 1, g_amm.removes, g_amm.removed,
                 g_amm.adds, g_amm.added, g_amm.full, SlotReport(player, kHeadSlot).c_str());
            g_amm.on = on;
            g_amm.dir = AmmHead::Dir::None;
            g_amm.reportAt[0] = now + 500, g_amm.reportAt[1] = now + 2000;
        }
    }
    for (int k = 0; k < 2; ++k)
        if (g_amm.reportAt[k] > 0 && now >= g_amm.reportAt[k])
        {
            g_amm.reportAt[k] = 0;
            Logf(log, "stage C4: head (AMM's way), %s later: TppHead holds %s", k == 0 ? "0.5 s" : "2 s", SlotReport(player, kHeadSlot).c_str());
        }
}

bool ApplyTppFeature(const Handle<IScriptable>& player, bool active, LogFn log);
void ShieldEnd(LogFn log, const char* why);

// P2-10: back to V's own camera and first-person body.
void ViewExpUndo(const Handle<IScriptable>& player, LogFn log, const char* why)
{
    if (g_vx.step == 0) return;
    const bool same = player.instance && reinterpret_cast<uintptr_t>(player.instance) == g.player;
    if (g_vx.which != ViewExp::Cam::None)
    {
        IScriptable* cam = g_vx.which == ViewExp::Cam::Tpp ? g_vx.tpp.instance : board::CamComponent();
        const bool off = cam && CamSwitch(cam, false);
        const bool on = same && g.camera.instance && CamSwitch(g.camera.instance, true);
        Logf(log, "stage C4: head experiment (%s): %s deactivated: %s; V's own camera activated: %s", why,
             g_vx.which == ViewExp::Cam::Tpp ? "V's tppCamera" : "our camera", off ? "yes" : "NO", on ? "yes" : "NO");
    }
    board::CamRemove(log, why);
    ShieldEnd(log, why);  // 0.25.7: before the representation goes and the first-person head comes back
    if (same && g_vx.featureOn) ApplyTppFeature(player, false, log);
    if (same && (g.prepared || g.activated))
    {
        const bool ok = QueueEvent(player.instance, "gameDeactivateTPPRepresentationEvent", log);
        Logf(log, "stage C4: head experiment (%s): the third-person body off: %s", why, ok ? "queued" : "FAILED");
    }
    // 0.25.3: V's own first-person head back in TppHead, the AMM way (finished by AmmStep).
    if (same && (g_amm.on || g_amm.dir == AmmHead::Dir::On))
    {
        g_amm.dir = AmmHead::Dir::None;
        AmmStart(player, false, log);
    }
    g.prepared = g.activated = g.settled = false;
    g_vx = ViewExp{};
}

// RemoveItemFromSlot(obj, slot, shouldDestroyEntity, keepWorldTransform, skipSendAnimEquipEvents), all true as AMM does.
void ClearHeadSlot(const Transactions& t, Handle<IScriptable>* pl)
{
    TweakDBID slot(kHeadSlot);
    bool yes = true, r = false;
    Call(t.self, t.fromSlot, &r, {pl, &slot, ParamIs(t.fromSlot, 2, "Bool") ? &yes : nullptr, ParamIs(t.fromSlot, 3, "Bool") ? &yes : nullptr,
                                  ParamIs(t.fromSlot, 4, "Bool") ? &yes : nullptr});
}

void HeadBegin(const Handle<IScriptable>& player, bool on, LogFn log)
{
    const Transactions t;
    if (!t.ok) { log("stage C1: head: TransactionSystem functions unavailable; head left as it is"); return; }
    auto* pl = const_cast<Handle<IScriptable>*>(&player);
    if (on)
    {
        CName gender;
        Call(player.instance, Func(player.instance->GetType(), "GetResolvedGenderName"), &gender, {});
        g_head = HeadJob{};
        g_head.record = gender == CName("Female") ? "Items.CharacterCustomizationWaHead" : "Items.CharacterCustomizationMaHead";
        if (!RecordExists(g_head.record.c_str()) || !ItemOf(g_head.record.c_str(), g_head.item))
        {
            Logf(log, "stage C1: head: %s unavailable; head left as it is", g_head.record.c_str());
            g_head = HeadJob{};
            return;
        }
        bool has = false;
        Call(t.self, t.has, &has, {pl, g_head.item});
        if (!has)
        {
            int32_t one = 1;
            Call(t.self, t.give, &g_head.given, {pl, g_head.item, ParamIs(t.give, 2, "Int32") ? &one : nullptr});
        }
        Logf(log, "stage C1: head: the TppHead slot holds %s; %s %s", HeadSlotItem(player), g_head.record.c_str(),
             has ? "already in V's inventory" : g_head.given ? "given" : "NOT given");
    }
    else
    {
        CName fpp = CNamePool::Add("default&FPP");
        Call(t.self, t.appearance, nullptr, {pl, g_head.item, &fpp});
    }
    g_head.player = reinterpret_cast<uintptr_t>(player.instance);
    g_head.on = on;
    ClearHeadSlot(t, pl);
    g_head.pending = true;
    g_head.removedAt = NowMs();
    g_head.nextTry = g_head.removedAt + 100;
    g_head.tries = 0;
    g_head.added = false;  // a new attempt: one add for it (0.9.7)
    g_head.cycles = 0;
    Logf(log, "stage C1: head: swap %s started: TppHead slot cleared (destroy, keep world transform, skip anim events)",
         on ? "to V's own head" : "back to the first-person head");
}

// Per frame, every 0.1 s. 0.9.7: one AddItemToSlot per attempt. 0.9.5 re-added every 0.1 s while the slot looked empty,
// 20 spawn requests in 2 s; the head never finished spawning and the game crashed right after. Now the add is made once,
// from 0.1 s after the removal, and the head gets 5 s to spawn. If the game puts another item back meanwhile, that item
// is removed once and the add repeats, at most 3 times. Every step is logged with its time since the swap began.
void HeadStep(const Handle<IScriptable>& player, LogFn log)
{
    if (!g_head.pending || NowMs() < g_head.nextTry) return;
    if (!player.instance || reinterpret_cast<uintptr_t>(player.instance) != g_head.player)
    {
        log("stage C1: head: V was replaced (a save or checkpoint loaded); head job dropped");
        g_head = HeadJob{};
        return;
    }
    const Transactions t;
    if (!t.ok) { g_head.pending = false; return; }
    auto* pl = const_cast<Handle<IScriptable>*>(&player);
    TweakDBID slot(kHeadSlot);
    const char* want = g_head.on ? g_head.record.c_str() : "Items.PlayerFppHead";
    alignas(16) uint8_t fpp[16] = {};
    if (!g_head.on && !ItemOf("Items.PlayerFppHead", fpp)) { log("stage C1: head: Items.PlayerFppHead unavailable"); g_head = HeadJob{}; return; }
    const double since = (NowMs() - g_head.removedAt) / 1000.0;
    const char* now = HeadSlotItem(player);
    if (strcmp(now, want) == 0)
    {
        Logf(log, "stage C1: head: %s attached and spawned %.2f s after the swap began", want, since);
        g_head.pending = false;
        if (!g_head.on)
        {
            if (g_head.given)
            {
                int32_t one = 1;
                bool removed = false;
                Call(t.self, t.remove, &removed, {pl, g_head.item, ParamIs(t.remove, 2, "Int32") ? &one : nullptr});
                Logf(log, "stage C1: head: %s taken back out of V's inventory: %s", g_head.record.c_str(), removed ? "yes" : "NO");
            }
            g_head = HeadJob{};
        }
        g_head.nextTry = NowMs() + 100;
        return;
    }
    bool empty = false;
    Call(t.self, t.isEmpty, &empty, {pl, &slot});
    auto giveUp = [&](const char* why) {
        Logf(log, "stage C1: head: giving up on %s after %.2f s: %s (slot holds %s, IsSlotEmpty %s)", want, since, why, now, empty ? "true" : "false");
        g_head.pending = false;
        if (!g_head.on) g_head = HeadJob{};
    };
    if (!g_head.added)
    {
        if (empty)
        {
            bool ok = false;
            Call(t.self, t.add, &ok, {pl, &slot, g_head.on ? g_head.item : fpp});
            if (!g_head.on)
            {
                CName a = CNamePool::Add("default&FPP");
                Call(t.self, t.appearance, nullptr, {pl, fpp, &a});
            }
            g_head.added = true;
            g_head.addedAt = NowMs();
            Logf(log, "stage C1: head: slot empty at %.2f s; AddItemToSlot(%s) returned %s", since, want, ok ? "true" : "false");
        }
        else if (since > 1.0)
        {
            if (++g_head.cycles > 3) { giveUp("the slot never emptied"); return; }
            Logf(log, "stage C1: head: slot still holds %s at %.2f s; removing it again", now, since);
            ClearHeadSlot(t, pl);
            g_head.removedAt = NowMs();
        }
    }
    else if (strcmp(now, "empty") != 0)
    {
        // Another item appeared in the slot (the game re-equipped something): remove it once and add again.
        if (++g_head.cycles > 3) { giveUp("the game kept putting another item back"); return; }
        Logf(log, "stage C1: head: the game put %s in the slot at %.2f s; removing it and adding again", now, since);
        ClearHeadSlot(t, pl);
        g_head.added = false;
        g_head.removedAt = NowMs();
    }
    else if (NowMs() - g_head.addedAt > 5000)
    {
        giveUp("the added item did not spawn within 5 s");
        return;
    }
    g_head.nextTry = NowMs() + 100;
}

// ----- head experiment (0.9.6, Ctrl+Shift+F11): appearance only -----
// The least invasive way to show V's head: give the item already in AttachmentSlots.TppHead (Items.PlayerFppHead in
// first person) the "default&TPP" appearance, which AMM uses for the head in vehicles, and "default&FPP" to undo.
// Nothing is removed, destroyed or attached: runs 5 and 6 crashed about 2 s after the head was removed and replaced.
struct HeadLookState
{
    bool changed = false;
    alignas(16) uint8_t item[16] = {};  // gameItemID of the slot's item when it was changed
} g_look;

void HeadLook(const Handle<IScriptable>& player, bool tpp, LogFn log)
{
    CClass* tc = Cls("gameTransactionSystem");
    CBaseFunction *inSlot = Func(tc, "GetItemInSlot"), *appearance = Func(tc, "ChangeItemAppearanceByName");
    IScriptable* ts = nullptr;
    if (!SelfFor(tc, inSlot, ts) || !appearance || !ParamIs(appearance, 2, "CName"))
    {
        log("stage C1: head experiment: TransactionSystem functions unavailable");
        return;
    }
    auto* pl = const_cast<Handle<IScriptable>*>(&player);
    if (tpp)
    {
        TweakDBID slot(kHeadSlot);
        Handle<IScriptable> item;
        Call(ts, inSlot, &item, {pl, &slot});
        if (!item.instance) { log("stage C1: head experiment: the TppHead slot is empty; nothing to change"); return; }
        Call(item.instance, Func(item.instance->GetType(), "GetItemID"), g_look.item, {});
    }
    CName look = CNamePool::Add(tpp ? "default&TPP" : "default&FPP");
    Call(ts, appearance, nullptr, {pl, g_look.item, &look});
    g_look.changed = tpp;
    Logf(log, "stage C1: head experiment: %s appearance of the slot's item (%s) set to \"%s\"", tpp ? "third-person" : "first-person",
         HeadSlotItem(player), look.ToString());
}

// ----- C4: Skate's camera (0.13.0) -----
// While V rides, the game camera goes where Skate's camera is (runtime ABI 5: position, look direction, field of
// view), interpolated between the skater's ticks like V. It blends in from the chase camera over 0.5 s and back out
// the same way. It is placed right after V's teleport (from PoseFrame), not at the start of the next frame.
struct SkateCam
{
    stageb::DkrPose prev{}, cur{};
    double curAt = 0, lastAt = 0, nextDiag = 0;
    bool have = false;  // this frame's Skate camera is in `world`
    Pose world;
    float fov = 0, weight = 0;
    float gameFov = -1;  // the game camera's own FOV, for the log
} g_cam;
constexpr float kCamBlendS = 0.5f;

bool CameraWorld(Pose& out)
{
    Mat m{};
    if (!Call(g.camera.instance, Func(g.camera.instance->GetType(), "GetLocalToWorld"), &m, {})) return false;
    out = Decode(m, g.rows);
    return Finite(out.p);
}

bool VWorld(IScriptable* v, V3& pos, float& yaw)
{
    Vector4 p{};
    if (!Call(v, Func(v->GetType(), "GetWorldPosition"), &p, {}) || !Call(v, Func(v->GetType(), "GetWorldYaw"), &yaw, {})) return false;
    pos = V3{p.X, p.Y, p.Z};
    return Finite(pos) && std::isfinite(yaw);
}

void SetCameraLocal(const Pose& l)
{
    Vector4 p{};
    p.X = l.p.x, p.Y = l.p.y, p.Z = l.p.z, p.W = 1.f;
    Quaternion q{};
    q.i = l.q.x, q.j = l.q.y, q.k = l.q.z, q.r = l.q.w;
    Call(g.camera.instance, Func(g.camera.instance->GetType(), "SetLocalTransform"), nullptr, {&p, &q});
    g.local = l;
}

// The FPP camera hangs off the "camera" slot of V's "slots" component (entHardTransformBinding bindName=slots
// slotName=camera, C1 run 3). That slot's world transform, read straight from the slot component.
bool SlotParent(Pose& out)
{
    if (!g.slots.instance) return false;
    CBaseFunction* f = Func(g.slots.instance->GetType(), "GetSlotTransform");
    if (!ParamIs(f, 0, "CName") || !ParamIs(f, 1, "WorldTransform")) return false;
    CName slot("camera");
    WorldTransform wt{};
    bool ok = false;
    if (!Call(g.slots.instance, f, f->returnType ? &ok : nullptr, {&slot, &wt}) || (f->returnType && !ok)) return false;
    out.p = {wt.Position.x.Bits / 131072.f, wt.Position.y.Bits / 131072.f, wt.Position.z.Bits / 131072.f};
    out.q = {wt.Orientation.i, wt.Orientation.j, wt.Orientation.k, wt.Orientation.r};
    const float n = std::sqrt(out.q.x * out.q.x + out.q.y * out.q.y + out.q.z * out.q.z + out.q.w * out.q.w);
    return Finite(out.p) && n > 0.9f && n < 1.1f;
}

// Places the camera 2.8 m behind V's feet at 1.75 m, level and facing V's heading, given the world transform of
// whatever the camera hangs off.
bool PlaceAt(const Pose& parent, V3 vp, float yaw);

bool Place(const Pose& parent, IScriptable* v)
{
    V3 vp;
    float yaw = 0;
    if (!VWorld(v, vp, yaw)) return false;
    return PlaceAt(parent, vp, yaw);
}

// P1-15 (0.24.1): V's root goes from `fromP`/`fromYaw` to `toP`/`toYaw` by a teleport later this frame, after the camera was
// placed against it: place the camera again against where its parent will then be.
void CameraAfterTeleport(V3 fromP, float fromYaw, V3 toP, float toYaw)
{
    if (g.mode == Mode::Off || !g.camera.instance || !g.haveParent) return;
    const Q dq = Mul(YawQ(toYaw), Conj(YawQ(fromYaw)));
    Pose parent;
    parent.q = Mul(dq, g.parent.q);
    parent.p = toP + Rot(dq, g.parent.p - fromP);
    if (PlaceAt(parent, toP, toYaw)) ++g.teleportPlacements;
}

// The given slot's world transform from V's slots component (its bone's, after the last animation).
bool SlotWorld(CName slot, Pose& out)
{
    if (!g.slots.instance) return false;
    CBaseFunction* f = Func(g.slots.instance->GetType(), "GetSlotTransform");
    if (!ParamIs(f, 0, "CName") || !ParamIs(f, 1, "WorldTransform")) return false;
    WorldTransform wt{};
    bool ok = false;
    if (!Call(g.slots.instance, f, f->returnType ? &ok : nullptr, {&slot, &wt}) || (f->returnType && !ok)) return false;
    out.p = {wt.Position.x.Bits / 131072.f, wt.Position.y.Bits / 131072.f, wt.Position.z.Bits / 131072.f};
    out.q = {wt.Orientation.i, wt.Orientation.j, wt.Orientation.k, wt.Orientation.r};
    return Finite(out.p);
}

bool PlaceAt(const Pose& parent, V3 vp, float yaw)
{
    g.parent = parent, g.haveParent = true;
    g.wanted.q = YawQ(yaw);
    g.wanted.p = vp + Rot(g.wanted.q, {0, -kChaseBack, kChaseHeight});
    if (g_cam.weight > 0)  // C4: towards Skate's camera
    {
        const float w = g_cam.weight;
        Q a = g.wanted.q, b = g_cam.world.q;
        if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
        Q q{a.x + (b.x - a.x) * w, a.y + (b.y - a.y) * w, a.z + (b.z - a.z) * w, a.w + (b.w - a.w) * w};
        const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (n > 1e-6f) g.wanted.q = {q.x / n, q.y / n, q.z / n, q.w / n};
        g.wanted.p = g.wanted.p + (g_cam.world.p - g.wanted.p) * w;
    }
    Pose l;
    l.q = Mul(Conj(parent.q), g.wanted.q);
    l.p = Rot(Conj(parent.q), g.wanted.p - parent.p);
    if (!Finite(l.p) || Len(l.p) > 50.f) return false;
    SetCameraLocal(l);
    g.vp = vp;
    return true;
}

// ----- the in-frame hook on the FPP camera's rotation (hash 2531201123, as Let There Be Flight hooks it) -----
// Let There Be Flight's prototype, except that the 7th argument and the result are carried as full 64-bit values:
// whatever their real width, every bit is forwarded unchanged.
using RotateFn = uint64_t (*)(void* fpp, float dt, float dYaw, float dPitch, float dYawExternal, float dPitchExternal, uint64_t a7);
RotateFn g_rotate = nullptr;
std::atomic<uint32_t> g_hookCalls{0}, g_hookOffThread{0};
uint32_t g_hookPlaced = 0;  // main thread only
std::atomic<uint32_t> g_hookOurs{0};
std::atomic<DWORD> g_mainThread{0};

uint64_t RotateDetour(void* fpp, float dt, float dYaw, float dPitch, float dYawExternal, float dPitchExternal, uint64_t a7)
{
    const uint64_t r = g_rotate(fpp, dt, dYaw, dPitch, dYawExternal, dPitchExternal, a7);
    g_hookCalls.fetch_add(1, std::memory_order_relaxed);
    auto* cam = reinterpret_cast<uint8_t*>(g.camera.instance);  // pointer read only; acted on below on the main thread
    const bool ours = cam && (fpp == cam || reinterpret_cast<uint8_t*>(fpp) - 0x120 == cam);  // LTBF notes a shifted pointer
    if (ours) g_hookOurs.fetch_add(1, std::memory_order_relaxed);
    if (GetCurrentThreadId() != g_mainThread.load(std::memory_order_relaxed)) { g_hookOffThread.fetch_add(1, std::memory_order_relaxed); return r; }
    if (!ours || g.mode == Mode::Off || !g.chase || !g.playerHandle.instance) return r;
    Pose parent;
    if (SlotParent(parent) && Place(parent, g.playerHandle.instance)) ++g_hookPlaced;
    return r;
}

float AngleDeg(Q a, Q b)
{
    const float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.f * std::acos(std::min(1.f, d)) * 57.29578f;
}

// A Float property's value by name (RTTI, read only; NaN if there is none).
float PropFloat(IScriptable* o, const char* name)
{
    CProperty* p = o ? Prop(o->GetType(), name) : nullptr;
    if (!p || TypeName(p->type) != std::string("Float")) return NAN;
    return *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(o) + p->valueOffset);
}

// Writes a Float property through the game's RTTI (the same description PropFloat reads it by). False if `o` has no
// such Float property.
bool SetPropFloat(IScriptable* o, const char* name, float v)
{
    CProperty* p = o ? Prop(o->GetType(), name) : nullptr;
    if (!p || TypeName(p->type) != std::string("Float")) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(o) + p->valueOffset) = v;
    return true;
}

// A CName property's value by name (its hash; RTTI). False if `o` has no such CName property.
bool PropCName(IScriptable* o, const char* name, uint64_t& out)
{
    CProperty* p = o ? Prop(o->GetType(), name) : nullptr;
    if (!p || TypeName(p->type) != std::string("CName")) return false;
    out = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(o) + p->valueOffset);
    return true;
}

bool SetPropCName(IScriptable* o, const char* name, uint64_t v)
{
    CProperty* p = o ? Prop(o->GetType(), name) : nullptr;
    if (!p || TypeName(p->type) != std::string("CName")) return false;
    *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(o) + p->valueOffset) = v;
    return true;
}

// P1-3 (0.20.0): the zoom-override weights come from V's animation graph, read each frame through the parameter names
// in the camera's animParamZoomOverrideWeight and animParamZoomWeaponOverrideWeight. 0.19.0 wrote the weights back
// to 0 after the frame, which the frame had already drawn with: in the fourth ride L2 "starts to zoom and then
// resets". While the camera is ours both names point at None, so the camera has no weight to read; they are put back
// on exit. Writing the weights to 0 stays, for a camera that resolved the names once and kept them.
const char* const kZoomParams[2] = {"animParamZoomOverrideWeight", "animParamZoomWeaponOverrideWeight"};

void SwapZoomParams(IScriptable* cam, LogFn log)
{
    uint64_t was[2] = {};
    if (!PropCName(cam, kZoomParams[0], was[0]) || !PropCName(cam, kZoomParams[1], was[1]))
    {
        log("stage C1: the camera has no zoom-override parameter names; L2's zoom is only written back to 0 after each frame");
        return;
    }
    g.zoomParams[0] = was[0], g.zoomParams[1] = was[1];
    SetPropCName(cam, kZoomParams[0], 0);
    SetPropCName(cam, kZoomParams[1], 0);
    g.zoomParamsSwapped = true;
    Logf(log, "stage C1: camera zoom-override parameter names (%s and %s) pointed at None while skating", CName(was[0]).ToString(),
         CName(was[1]).ToString());
}

void RestoreZoomParams(LogFn log)
{
    if (!g.zoomParamsSwapped || !g.camera.instance) return;
    const bool ok = SetPropCName(g.camera.instance, kZoomParams[0], g.zoomParams[0]) && SetPropCName(g.camera.instance, kZoomParams[1], g.zoomParams[1]);
    g.zoomParamsSwapped = false;
    Logf(log, "stage C1: camera zoom-override parameter names %s", ok ? "restored" : "could not be restored");
}

// P1-3 (0.18.0): L2 still zoomed in the 0.17.0 test with NoZooming on V, and the camera's field of view never moved
// (68.2 in every sample), so the zoom is the FPP camera's own (CameraComponent.zoom, or the zoom overrides its
// animation parameters drive). While the camera is ours it is held at its value from entry with the public SetZoom,
// and the 2 s camera line says what moved: the zoom itself, or an override (which SetZoom would not reach).
void HoldZoom(LogFn log)
{
    IScriptable* cam = g.camera.instance;
    if (!cam) return;
    CClass* t = cam->GetType();
    float z = 0;
    if (!Call(cam, Func(t, "GetZoom"), &z, {}) || !std::isfinite(z)) return;
    if (!g.zoomKnown)
    {
        g.zoom0 = z > 0.f ? z : 1.f;
        g.zoomKnown = true;
        Logf(log, "stage C1: camera zoom at entry %.2f; held there while skating", g.zoom0);
        SwapZoomParams(cam, log);
    }
    auto keepMax = [](float& m, float v) { if (std::isfinite(v) && std::fabs(v) > std::fabs(m)) m = v; };
    keepMax(g.zoomMax, z);
    const float ow = PropFloat(cam, "zoomOverrideWeight"), ww = PropFloat(cam, "zoomWeaponOverrideWeight");
    if (std::isfinite(ow) && ow > g.overrideW) g.overrideW = ow, g.overrideV = PropFloat(cam, "zoomOverrideValue");
    if (std::isfinite(ww) && ww > g.weaponW) g.weaponW = ww, g.weaponV = PropFloat(cam, "zoomWeaponOverrideValue");
    // P1-3 (0.19.0): the 0.18.0 test showed L2's zoom is these overrides (weight 1.00, values up to 1.50), which replace
    // the zoom outright, so SetZoom alone never reached the screen. Their weights are written back to 0 through the
    // camera component's RTTI properties (no setter exists). The game sets them again every frame it wants the zoom;
    // the 2 s line says how often they had to be written.
    const bool wrote = (std::isfinite(ow) && ow > 0.f && SetPropFloat(cam, "zoomOverrideWeight", 0.f)) |
                       (std::isfinite(ww) && ww > 0.f && SetPropFloat(cam, "zoomWeaponOverrideWeight", 0.f));
    if (wrote)
    {
        ++g.overrideWrites;
        if (++g.overrideWritesTotal == 1)
            Logf(log, "stage C1: camera zoom override weights %.2f and %.2f (values %.2f and %.2f) written back to 0", ow, ww,
                 PropFloat(cam, "zoomOverrideValue"), PropFloat(cam, "zoomWeaponOverrideValue"));
    }
    if (std::fabs(z - g.zoom0) > 1e-3f)
    {
        float z0 = g.zoom0;
        Call(cam, Func(t, "SetZoom"), nullptr, {&z0});
        ++g.zoomResets;
        if (++g.zoomResetsTotal <= 3) Logf(log, "stage C1: camera zoom moved to %.2f; set back to %.2f", z, g.zoom0);
    }
}

// After the game's frame (RED4ext runs plugin state updates after it). If the hook placed the camera this frame, only
// check it; otherwise place it from here, a frame late (the 0.9.1-0.9.2 behaviour, which shook).
// P1-6 (camera jitter on rides after the first, cause unknown): where the camera ended up this frame against where it
// was placed for it. Placing after the frame hangs the camera off V's slot as it was, so V's movement since shows up
// here; jitter is that error changing from frame to frame. Summarised every 2 s, so rides can be compared in the log.
struct CamError
{
    V3 last{0, 0, 0};
    bool have = false;
    double sum = 0, sum2 = 0, stepMax = 0, errSum = 0;
    uint32_t n = 0;
} g_camErr;

void UpdateChase(IScriptable* v, LogFn log)
{
    Pose world;
    if (!CameraWorld(world)) return;
    Pose fromMatrix;
    fromMatrix.q = Mul(world.q, Conj(g.local.q));
    fromMatrix.p = world.p - Rot(fromMatrix.q, g.local.p);
    const bool hook = g_hookPlaced != g.hookPlacedSeen;
    g.hookPlacedSeen = g_hookPlaced;
    {
        const V3 e = world.p - g.wanted.p;
        if (Finite(e) && Len(e) < 5.f)
        {
            if (g_camErr.have)
            {
                const double s = Len(e - g_camErr.last);
                g_camErr.sum += s, g_camErr.sum2 += s * s, g_camErr.stepMax = std::max(g_camErr.stepMax, s), g_camErr.errSum += Len(e), ++g_camErr.n;
            }
            g_camErr.last = e, g_camErr.have = true;
        }
        else g_camErr.have = false;
    }
    if (!hook) Place(fromMatrix, v);
    HoldZoom(log);
    const double now = NowMs();
    if (now >= g.nextDiag)
    {
        if (g_camErr.n > 0)
            Logf(log, "stage C1: camera placement error over %u frames: mean %.1f cm from where it was placed; frame-to-frame change rms %.1f cm, "
                      "max %.1f cm (jitter)", g_camErr.n, 100.0 * g_camErr.errSum / g_camErr.n, 100.0 * std::sqrt(g_camErr.sum2 / g_camErr.n),
                 100.0 * g_camErr.stepMax);
        g_camErr = CamError{};
        if (g.zoomKnown && (g.zoomResets > 0 || g.overrideW > 0.f || g.weaponW > 0.f))
            Logf(log, "stage C1: camera zoom over 2 s: set back to %.2f %u times (furthest %.2f); zoom override weight up to %.2f (value %.2f), "
                      "weapon zoom override weight up to %.2f (value %.2f); override weights written back to 0 on %u frames",
                 g.zoom0, g.zoomResets, g.zoomMax, g.overrideW, g.overrideV, g.weaponW, g.weaponV, g.overrideWrites);
        g.zoomResets = 0, g.zoomMax = 0, g.overrideW = 0, g.overrideV = 0, g.weaponW = 0, g.weaponV = 0, g.overrideWrites = 0;
        g.nextDiag = now + 2000;
        Pose slot;
        const bool haveSlot = SlotParent(slot);
        const V3 up = Rot(world.q, {0, 0, 1}), pup = Rot(fromMatrix.q, {0, 0, 1});
        Logf(log, "stage C1: camera %s; up.z %.2f, %.2f m from where it was placed; attachment up.z %.2f, %.2f m above V's feet; slot query %s (%.3f m, %.1f deg from the matrix); hook calls %u, on V's camera %u, placed %u, off-thread %u",
             hook ? "placed in-frame by the hook" : "placed after the frame (fallback)", up.z, Len(world.p - g.wanted.p), pup.z,
             fromMatrix.p.z - g.vp.z, haveSlot ? "ok" : "unavailable", haveSlot ? Len(slot.p - fromMatrix.p) : 0.f,
             haveSlot ? AngleDeg(slot.q, fromMatrix.q) : 0.f, g_hookCalls.load(), g_hookOurs.load(), g_hookPlaced, g_hookOffThread.load());
    }
}

const char* EffectName(const TweakDBID& id)
{
    static const std::unordered_map<uint64_t, const char*> names = [] {
        std::unordered_map<uint64_t, const char*> m;
        for (const char* n : kStatusEffectNames)
        {
            const TweakDBID t(n);
            uint64_t k = 0;
            memcpy(&k, &t, sizeof k);
            m.emplace(k, n);
        }
        return m;
    }();
    uint64_t k = 0;
    memcpy(&k, &id, sizeof k);
    const auto it = names.find(k);
    return it == names.end() ? nullptr : it->second;
}

// Status effects on V by name (gameStatusEffect.GetRecord().GetID()); names outside the table show as "?" plus the ID.
std::string EffectsOn(const Handle<IScriptable>& player, const char* tag = nullptr, uint32_t* count = nullptr)
{
    CClass* sc = Cls("gameStatusEffectSystem");
    CBaseFunction* f = Func(sc, tag ? "GetAppliedEffectsWithTag" : "GetAppliedEffects");
    IScriptable* self = nullptr;
    alignas(16) uint8_t eid[16] = {};
    if (!SelfFor(sc, f, self) || !ParamIs(f, 0, "entEntityID") || !EntityIdOf(player.instance, eid)) return "(effect list unavailable)";
    DynArray<Handle<IScriptable>> arr;
    CName t(tag ? tag : "");
    const bool ok = tag ? Call(self, f, nullptr, {eid, &t, &arr}) : Call(self, f, nullptr, {eid, &arr});
    if (!ok) return "(effect list call failed)";
    if (count) *count = arr.Size();
    std::string s;
    for (auto& h : arr)
    {
        if (!h.instance) continue;
        Handle<IScriptable> rec;
        TweakDBID id;
        Call(h.instance, Func(h.instance->GetType(), "GetRecord"), &rec, {});
        if (rec.instance) Call(rec.instance, Func(rec.instance->GetType(), "GetID"), &id, {});
        const char* n = EffectName(id);
        uint64_t raw = 0;
        memcpy(&raw, &id, sizeof raw);
        char b[96];
        snprintf(b, sizeof b, "%s%s", s.empty() ? "" : ", ", n ? n : "?");
        s += b;
        if (!n)
        {
            snprintf(b, sizeof b, "(0x%llx)", raw);
            s += b;
        }
    }
    return s.empty() ? "none" : s;
}

const char* HeadSlotItem(const Handle<IScriptable>& player)
{
    CClass* tc = Cls("gameTransactionSystem");
    CBaseFunction* inSlot = Func(tc, "GetItemInSlot");
    CBaseFunction* getTdb = Func(Cls("gameItemID"), "GetTDBID");
    IScriptable* ts = nullptr;
    if (!SelfFor(tc, inSlot, ts) || !getTdb) return "(unavailable)";
    TweakDBID slot(kHeadSlot);
    Handle<IScriptable> item;
    Call(ts, inSlot, &item, {const_cast<Handle<IScriptable>*>(&player), &slot});
    if (!item.instance) return "empty";
    alignas(16) uint8_t id[16] = {};
    Call(item.instance, Func(item.instance->GetType(), "GetItemID"), id, {});
    TweakDBID rec;
    Call(nullptr, getTdb, &rec, {id});
    const char* n = HeadName(rec);
    return n ? n : "an unknown item";
}

// V's crouch flag (PlayerPuppet.inCrouch), force-crouch effects, the head slot's item, and every status effect.
void Snapshot(const Handle<IScriptable>& player, LogFn log, const char* when, bool always)
{
    CProperty* pc = Prop(player.instance->GetType(), "inCrouch");
    const bool readable = pc && TypeName(pc->type) == std::string("Bool");
    const bool crouch = readable && *(reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(player.instance) + pc->valueOffset));
    // 0.9.6: the status-effect lists (GetAppliedEffects*) are no longer read here. Runs 5 and 6 crashed with them in
    // and run 4 did not; they already showed that no effect forces the crouch.
    const std::string line = std::string("inCrouch ") + (readable ? (crouch ? "TRUE" : "false") : "?") + "; head slot: " + HeadSlotItem(player);
    if (!always && line == g.lastEffects) return;
    g.lastEffects = line;
    Logf(log, "stage C1: [%s] %s", when, line.c_str());
}

// 1 s after the TPP representation was activated: log the camera's binding and V's state.
void Settle(const Handle<IScriptable>& player, LogFn log)
{
    if (g.haveCamera && g.camera.instance) Logf(log, "stage C1: FPP camera binding with the third-person body: %s", CameraBinding(g.camera.instance).c_str());
    Snapshot(player, log, "third-person body + 1 s", true);
}

void EnterThird(const Handle<IScriptable>& player, LogFn log)
{
    IScriptable* v = player.instance;
    g = State{};
    g.player = reinterpret_cast<uintptr_t>(v);
    g.playerHandle = player;
    g_mainThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_hookCalls = 0, g_hookOffThread = 0, g_hookOurs = 0, g_hookPlaced = 0;
    g.enteredAt = NowMs();
    for (const char* id : kRestrictions)
        if (HasEffect(player, id)) Logf(log, "stage C1: before entry V already has %s", id);
    // V's "slots" component, which owns the "camera" slot the FPP camera hangs off.
    CClass* entityCls = Cls("entEntity");
    if (entityCls && v->GetType()->IsA(entityCls))
        for (auto& c : reinterpret_cast<ent::Entity*>(v)->components)
            if (c.instance && c->name == CName("slots")) { g.slots = c; break; }
    Logf(log, "stage C1: V's slots component: %s", g.slots.instance ? g.slots.instance->GetType()->GetName().ToString() : "not found");
    // Camera first, while V is still in first person: its world matrix then sits at V's eyes facing V's yaw, which
    // tells which matrix layout this build uses (run 2: rows, W = translation).
    CBaseFunction* getCam = Func(v->GetType(), "GetFPPCameraComponent");
    g.haveCamera = getCam && Call(v, getCam, &g.camera, {}) && g.camera.instance &&
                   Call(g.camera.instance, Func(g.camera.instance->GetType(), "GetLocalPosition"), &g.cameraPos, {}) &&
                   Call(g.camera.instance, Func(g.camera.instance->GetType(), "GetLocalOrientation"), &g.cameraRot, {});
    if (g.haveCamera)
    {
        g.local.p = {g.cameraPos.X, g.cameraPos.Y, g.cameraPos.Z};
        g.local.q = {g.cameraRot.i, g.cameraRot.j, g.cameraRot.k, g.cameraRot.r};
        Logf(log, "stage C1: FPP camera binding in first person: %s", CameraBinding(g.camera.instance).c_str());
        Mat m{};
        V3 vp;
        float yaw = 0;
        if (Call(g.camera.instance, Func(g.camera.instance->GetType(), "GetLocalToWorld"), &m, {}) && VWorld(v, vp, yaw))
        {
            const Pose a = Decode(m, true), b = Decode(m, false);
            const V3 eyes = vp + V3{0, 0, 1.6f};
            const float da = Len(a.p - eyes), db = Len(b.p - eyes);
            g.rows = da <= db;
            const Pose& c = g.rows ? a : b;
            const V3 fwd = Rot(c.q, {0, 1, 0}), want = Rot(YawQ(yaw), {0, 1, 0});
            g.chase = std::min(da, db) < 3.f;
            Logf(log, "stage C1: FPP camera world matrix: %s layout (camera %.2f m from V's eyes; other layout %.2f m); camera forward (%.2f, %.2f, %.2f) vs V's facing (%.2f, %.2f); chase camera %s",
                 g.rows ? "row" : "column", std::min(da, db), std::max(da, db), fwd.x, fwd.y, fwd.z, want.x, want.y, g.chase ? "ON" : "OFF (layout not recognised)");
            Pose slot;
            if (SlotParent(slot))
                Logf(log, "stage C1: slot query in first person: \"camera\" slot %.3f m and %.1f deg from the camera's parent per its matrix (should be about 0)",
                     Len(slot.p - c.p), AngleDeg(slot.q, Mul(c.q, Conj(g.local.q))));
            else log("stage C1: slot query unavailable (SlotComponent.GetSlotTransform); the camera is placed after the frame instead");
        }
    }
    else log("stage C1: V's FPP camera or its local transform is not available; the camera stays at V's eyes");
    Snapshot(player, log, "before", true);
    g.mode = Mode::FirstBody;
    log("stage C1: third person ON: camera behind V, first-person body, head unchanged. Ctrl+Shift+F11: head appearance experiment. Ctrl+Shift+F7: skating restrictions. Ctrl+Shift+F6 exits.");
}

void PoseOff(const Handle<IScriptable>& player, LogFn log);  // C3, below
void ResetRideState();  // C2 to C4, below

void ExitAll(const Handle<IScriptable>& player, LogFn log, const char* why)
{
    const bool same = player.instance && reinterpret_cast<uintptr_t>(player.instance) == g.player;
    if (!same)
    {
        Logf(log, "stage C1: exit (%s): V is a new object (a save or checkpoint loaded), so there is nothing to undo on it", why);
        board::Remove(log, "a new player object");
        board::CamRemove(log, "a new player object");
        g_vx = ViewExp{};
        g_amm = AmmHead{};  // nothing to undo on a new object
        vis::ShieldOff();  // 0.25.7: the old object's parts are gone
        // P0-13 (0.19.0): the ride's follow, pose, board and camera state go too. Up to 0.18.0 they survived a save
        // load, and the next ride opened on Skate's camera 5.7 km away and ended at once with "the game moved V".
        ResetRideState();
        g = State{};
        return;
    }
    ViewExpUndo(player, log, why);  // P2-10: V's own camera active again before it is restored
    BeginCleanup(player, log);  // confirmed over the next frames by StepCleanup
    if (g_head.on) HeadBegin(player, false, log);  // finished over the next frames by HeadStep
    if (g_look.changed) HeadLook(player, false, log);
    RestoreZoomParams(log);
    if (g.haveCamera && g.camera.instance)
    {
        Call(g.camera.instance, Func(g.camera.instance->GetType(), "SetLocalTransform"), nullptr, {&g.cameraPos, &g.cameraRot});
        Vector4 now{};
        Call(g.camera.instance, Func(g.camera.instance->GetType(), "GetLocalPosition"), &now, {});
        Logf(log, "stage C1: FPP camera local transform restored, now (%.3f, %.3f, %.3f)", now.X, now.Y, now.Z);
    }
    if (g.activated) QueueEvent(player.instance, "gameDeactivateTPPRepresentationEvent", log);
    PoseOff(player, log);
    board::Remove(log, why);
    HeadExpUndo(player, log, why);
    ResetRideState();
    Logf(log, "stage C1: third person OFF (%s)", why);
    g = State{};
}

// ----- C2: V follows the skater (0.10.0) -----
// With third person and the skating restrictions on, and a Stage B session riding, V is teleported to the skater
// every frame with the game's own TeleportationFacility.Teleport, as noclip mods move V (EasyTrainers NoClip). The
// skater ticks at 60 Hz, so V is interpolated between the last two ticks, one tick behind the runtime's own clock
// (P1-6, 0.18.0: stageb::Blend; 0.17.0 timed it by when the plugin noticed each tick, and at 60 fps V stood still for a
// frame, then moved two ticks). V faces the skater's direction of travel. V's own animation keeps playing (the pose is C3).
// P1-15: how V is moved to the skater. The tenth play test found the world stops streaming in (no traffic, no enemies,
// props without collision) while V is teleported every frame, until the game is paused. In the eleventh (0.24.0), writing
// V's root transform did not hold: the game put her back on her character controller within a second. Sparse teleports
// kept the world streaming in. So (0.24.1):
//  - Sparse (the default): V is teleported only once the skater is more than kSparseLeash from her. Between teleports she
//    stands where she was put, and Skate's pose places her body at the skater (the hips offset in her model space, C3).
//    The camera, placed against her root earlier in the frame, is placed again against where the teleport puts it
//    (CameraAfterTeleport): in 0.24.0 it sat 2.5 m off for that one frame, and V showed twice.
//  - Teleport: every frame, as up to 0.23.1 (the board too), to compare. Ctrl+Shift+F11 while riding switches.
// A jump (a respawn or a session marker return) is teleported at once.
enum class Move { Sparse, Teleport };
Move g_move = Move::Sparse;
// The follow has started in this ride (cleared on exit), and the height offset it found then (see FollowState::dz).
bool g_rideFollowed = false;
float g_rideDz = 0;
constexpr float kSparseLeash = 2.5f;

const char* MoveName(Move m) { return m == Move::Sparse ? "sparse teleports" : "teleports every frame"; }

// P1-15 (0.24.1): V's drawn body against the skater, frame to frame. A slot of V's slots component bound to one of the
// pose's bones (the hips first) is read each frame; it reflects the last frame's animation. Its step is compared with
// the step of the skater's hips, split by whether V's root was teleported in that frame, in the frame before (a frame of
// animation lag would show there), or neither. A body that keeps up shows a few centimetres in all three.
struct BodyCheck
{
    CName slot;
    std::string bone;
    bool chosen = false, have = false, haveLast = false;
    V3 lastS{0, 0, 0}, lastH{0, 0, 0};
    bool lastTp = false, prevTp = false;
    double sum[3] = {}, mx[3] = {};
    uint32_t n[3] = {};
    V3 curS{0, 0, 0};    // 0.26.1: this frame's reading (the last frame's drawn hips), for the follow trace
    bool haveCur = false;
} g_body;

// ----- 0.26.1: the follow trace -----
// The twenty-third ride: during backflips "every so many frames I see an onion layer of V at a seemingly different
// height". Its body check found the frame after a teleport off by up to 2.3 m when V was moved in the air, against under
// 1 cm on flat ground. Frame by frame, the last 15 s: where V's root was, where she was teleported, the root and facing
// her pose was built against, the skater's hips, her drawn hips (the hips slot, one frame behind) and the pose weight.
// Written beside the probe log by Ctrl+Shift+F12, with Skate's own capture.
struct TraceRow
{
    double t = 0;
    uint64_t tick = 0;
    char state[24] = {};
    bool tried = false, tp = false, haveV = false, haveSlot = false;
    V3 vNow{0, 0, 0}, at{0, 0, 0}, root{0, 0, 0}, hipsW{0, 0, 0}, hipsMs{0, 0, 0}, slot{0, 0, 0};
    float vYaw = 0, rootYaw = 0, weight = 0;
};
constexpr size_t kTraceRows = 900;
TraceRow g_trace[kTraceRows];
size_t g_traceNext = 0, g_traceCount = 0;

struct FollowState
{
    bool have = false;
    uint64_t tick = 0, prevTick = 0;  // the ticks of `cur` and `prev`
    V3 prev{0, 0, 0}, cur{0, 0, 0}, sent{0, 0, 0};
    double curAt = 0, nextDiag = 0, startedAt = 0;
    float yaw = 0, yawTarget = 0, dz = 0;
    // P1-16 (0.26.2): how far above the skater's height (less dz) the game rests V, on foot [0] and on the board [1],
    // learned from its own correction after each sparse teleport; and that teleport.
    float restDz[2] = {0, 0};
    float tpZ = 0, tpAtZ = 0, lastVz = 0;
    uint32_t sinceTp = 0, stillFrames = 0, restAdjusts = 0;
    bool tpMeasured = true;
    double lastAt = 0, jumpSince = 0;
    bool holding = false, resync = false;
    // P2-7/P2-8 (0.23.0): when the skater went down or off the board (0: it's on the board), and V's height then.
    double downSince = 0;
    float downZ = 0;
    uint32_t frames = 0, teleports = 0, failed = 0, skipped = 0;
    // C5: since when V has stood more than kFarM across from where she was last put (0: she has not); a long teleport of
    // ours is waited out until she arrives (`arriving`, for at most kArriveMaxMs).
    double farSince = 0, arriveSince = 0;
    bool arriving = false;
    // P1-15 (0.24.0): `sent` is the follow's target each frame; `put` is where V's root was last teleported, and when. `moves` counts frames followed, whatever the method.
    V3 put{0, 0, 0};
    double putAt = 0;
    uint32_t moves = 0, winTeleports = 0;
    uint32_t dynMin = 0, dynMax = 0;  // dynamic actors in the physics scenes over the 2 s window
    bool sync = false;                // teleport on the next frame (a new method, or a jump)
    double pausedAt = 0;     // P1-5: when the game paused (0: not paused)
    // P1-6 (0.18.0): how evenly V moves, per 2 s window: the frame-to-frame change of the step sent to the game, and of
    // V's own step as the game applied it (read back each frame before the new teleport).
    V3 stepSent{0, 0, 0}, stepV{0, 0, 0}, lastV{0, 0, 0};
    double lastSentAt = 0, lastVAt = 0;
    bool haveStepSent = false, haveStepV = false;
    double sentSum = 0, sentSum2 = 0, sentMax = 0, vSum2 = 0, vMax = 0;
    uint32_t sentN = 0, vN = 0;
    // P0-14 (0.19.0): the current hold (the skater down): when it began, V's lowest point against the held spot, and
    // whether she dropped and is now kept at the held spot until the skater is back on the board.
    double holdAt = 0, holdDiag = 0;
    float holdLow = 0;
    bool pinned = false;
    uint32_t holdPuts = 0;
} g_follow;

// P0-14: V is put back at the held spot once she drops this far below it while the skater is down.
constexpr float kHoldDrop = 1.0f;
// P2-7/P2-8 (0.23.0): how far below the spot where the skater went down V still follows its body.
constexpr float kDownFollowDrop = 6.0f;
// C5 (0.24.0): the game itself moved V (fast travel, a scene, a quest teleport) when she stands more than kFarM across from
// where she was last put for kFarMs. Up to 0.23.1: 8 m in 3D for 30 frames, counted against the follow's target, which a
// fall between sparse teleports or the game still applying a long teleport of ours could reach.
constexpr float kFarM = 8.0f;
constexpr double kFarMs = 2000, kArriveMaxMs = 10000;

// One frame's step into a regularity window: the change from the last step (both within 1 m: no respawn jumps).
void StepInto(V3 step, V3& last, bool& have, double& sum2, double& mx, uint32_t& n, double* sum = nullptr)
{
    if (Len(step) > 1.f) { have = false; return; }
    if (have)
    {
        const double d = Len(step - last);
        sum2 += d * d, mx = std::max(mx, d), ++n;
        if (sum) *sum += Len(step);
    }
    last = step, have = true;
}

bool TeleportV(const Handle<IScriptable>& player, V3 p, float yawDeg)
{
    CClass* tc = Cls("gameTeleportationFacility");
    CBaseFunction* f = Func(tc, "Teleport");
    IScriptable* self = nullptr;
    if (!SelfFor(tc, f, self) || !ParamIs(f, 1, "Vector4") || !ParamIs(f, 2, "EulerAngles")) return false;
    Vector4 v{};
    v.X = p.x, v.Y = p.y, v.Z = p.z, v.W = 1.f;
    EulerAngles e{};
    e.Roll = 0, e.Pitch = 0, e.Yaw = yawDeg;
    return Call(self, f, nullptr, {const_cast<Handle<IScriptable>*>(&player), &v, &e});
}

// ----- C3: Skate's pose on V's own skeleton (0.11.0) -----
// The lab archive deckrunner_c3.archive (tools/wkprobe patch-skate) replaces V's animation graph,
// player_base.animgraph, with a copy whose output is Blend2(weight dkr_weight; V's own animation; the same pose with
// SetBonePosition(Hips, dkr_p_Hips) and SetBoneOrientation(<bone>, dkr_q_<bone>) applied, model space, parents
// first). Every new variable defaults to "no change" (weight 0). Here those variables are set on V's
// AnimationControllerComponent: the runtime's retargeted pose (game world) is interpolated between the skater's 60 Hz
// ticks like V's position, and turned into V's model space with the transform V was just teleported to. The weight
// fades in and out over 0.25 s.
constexpr const char* kPoseBones[24] = {
    "Hips", "Spine", "Spine1", "Spine2", "Spine3", "Neck", "Neck1", "Head",
    "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "RightShoulder", "RightArm", "RightForeArm", "RightHand",
    "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase",
};
constexpr float kPoseFadeS = 0.25f;

struct PoseState
{
    uintptr_t player = 0;
    Handle<ent::IComponent> anim;  // V's AnimationControllerComponent
    CBaseFunction *setQ = nullptr, *setV = nullptr, *setF = nullptr;
    CName q[24], hipsVar, weightVar;
    bool bound = false, bindTried = false;
    bool test = false;            // Ctrl+Shift+F11: the rig's reference pose instead of the skater's
    bool testUntilRide = false;   // the test was started before the skater rode: it ends when the skater does
    stageb::DkrPose prev{}, cur{};  // the last two retargeted poses (game world), by tick
    double curAt = 0;
    Q ms[24]{};
    V3 hipsMs{0, 0, 0};
    V3 hipsWorld{0, 0, 0};  // P1-15 (0.24.1): where the skater's hips are this frame (the body check)
    bool haveMs = false, holding = false, haveHipsWorld = false;
    double drivenAt = -1e9;       // when Follow last produced a pose
    float weight = 0, sentWeight = 0;
    double lastAt = 0, nextDiag = 0, testSince = 0;
    uint32_t sends = 0, failed = 0, interpolated = 0;
} g_pose;

Q QOf(const float r[4]) { return {r[0], r[1], r[2], r[3]}; }

// ----- C3b: the board (Board.cpp), interpolated between the skater's ticks like V -----
struct BoardTrack
{
    stageb::DkrPose prev{}, cur{};
    double curAt = 0;
} g_board;

// P0-13: everything one ride leaves behind for V's follow, her pose, the board and Skate's camera, cleared on every
// exit (a save load included) so the next ride starts from nothing.
void ResetRideState()
{
    g_follow = FollowState{};
    g_rideFollowed = false, g_rideDz = 0;
    g_pose = PoseState{};
    g_board = BoardTrack{};
    g_cam = SkateCam{};
}

// C4: this frame's Skate camera (interpolated between ticks), and the weight it has over the chase camera.
void CamFrame(LogFn log)
{
    const double now = NowMs();
    const float dt = g_cam.lastAt > 0 ? std::clamp(float(now - g_cam.lastAt) / 1000.f, 0.f, 0.1f) : 0.f;
    g_cam.lastAt = now;
    stageb::DkrPose p;
    const bool riding = g.mode == Mode::Restricted && stageb::Pose(p) && p.camValid;
    g_cam.have = false;
    if (riding)
    {
        if (!g_cam.cur.camValid || p.tick < g_cam.cur.tick || now - g_cam.curAt > 500) g_cam.prev = g_cam.cur = p, g_cam.curAt = now;
        else if (p.tick != g_cam.cur.tick) g_cam.prev = g_cam.cur, g_cam.cur = p, g_cam.curAt = now;
        const float a = stageb::Blend(g_cam.prev.tick, g_cam.cur.tick);  // P1-6: from the runtime's clock
        const V3 p0{g_cam.prev.camPos[0], g_cam.prev.camPos[1], g_cam.prev.camPos[2]}, p1{g_cam.cur.camPos[0], g_cam.cur.camPos[1], g_cam.cur.camPos[2]};
        const V3 f0{g_cam.prev.camFwd[0], g_cam.prev.camFwd[1], g_cam.prev.camFwd[2]}, f1{g_cam.cur.camFwd[0], g_cam.cur.camFwd[1], g_cam.cur.camFwd[2]};
        const V3 f = Norm(f0 + (f1 - f0) * a);
        if (Len(f) > 0.5f && Finite(p1))
        {
            // No roll, as SK8V uses Skate's camera: heading from the look direction, then pitch about the camera's +X.
            const float yaw = std::atan2(-f.x, f.y) * 57.29578f, pitch = std::asin(std::clamp(f.z, -1.f, 1.f));
            g_cam.world.q = Mul(YawQ(yaw), Q{std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)});
            g_cam.world.p = p0 + (p1 - p0) * a;
            g_cam.fov = g_cam.prev.camFov + (g_cam.cur.camFov - g_cam.prev.camFov) * a;
            g_cam.have = true;
        }
    }
    else g_cam.prev = g_cam.cur = stageb::DkrPose{};
    const float target = g_cam.have ? 1.f : 0.f, step = dt / kCamBlendS;
    g_cam.weight = target > g_cam.weight ? std::min(target, g_cam.weight + step) : std::max(target, g_cam.weight - step);
    if (g_cam.weight > 0 && now >= g_cam.nextDiag && g.camera.instance)
    {
        g_cam.nextDiag = now + 2000;
        float gameFov = 0;
        Call(g.camera.instance, Func(g.camera.instance->GetType(), "GetFOV"), &gameFov, {});
        V3 vp;
        float vyaw = 0;
        const bool haveV = g.playerHandle.instance && VWorld(g.playerHandle.instance, vp, vyaw);
        const V3 fwd = Rot(g_cam.world.q, {0, 1, 0});
        Logf(log, "stage C4: Skate camera weight %.2f at (%.2f, %.2f, %.2f), looking (%.2f, %.2f, %.2f), %.2f m from V's feet and %.2f m above them; Skate fov %.1f, game camera fov %.1f",
             g_cam.weight, g_cam.world.p.x, g_cam.world.p.y, g_cam.world.p.z, fwd.x, fwd.y, fwd.z, haveV ? Len(g_cam.world.p - vp) : -1.f,
             haveV ? g_cam.world.p.z - vp.z : 0.f, g_cam.fov, gameFov);
    }
}

void BoardFrame(LogFn log)
{
    stageb::DkrPose p;
    const double now = NowMs();
    const bool riding = g.mode == Mode::Restricted && stageb::Pose(p) && p.boardValid;
    if (!riding)
    {
        g_board = BoardTrack{};
        board::Frame(false, nullptr, nullptr, log);
        return;
    }
    if (!g_board.cur.boardValid || p.tick < g_board.cur.tick || now - g_board.curAt > 500) g_board.prev = g_board.cur = p, g_board.curAt = now;
    else if (p.tick != g_board.cur.tick) g_board.prev = g_board.cur, g_board.cur = p, g_board.curAt = now;
    const float a = stageb::Blend(g_board.prev.tick, g_board.cur.tick);  // P1-6: from the runtime's clock
    float pos[3], rot[4];
    for (int i = 0; i < 3; ++i) pos[i] = g_board.prev.boardPos[i] + (g_board.cur.boardPos[i] - g_board.prev.boardPos[i]) * a;
    const Q q0 = QOf(g_board.prev.boardRot);
    Q q1 = QOf(g_board.cur.boardRot);
    if (q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w < 0) q1 = {-q1.x, -q1.y, -q1.z, -q1.w};
    Q q{q0.x + (q1.x - q0.x) * a, q0.y + (q1.y - q0.y) * a, q0.z + (q1.z - q0.z) * a, q0.w + (q1.w - q0.w) * a};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-6f)) return;
    rot[0] = q.x / n, rot[1] = q.y / n, rot[2] = q.z / n, rot[3] = q.w / n;
    board::Frame(true, pos, rot, log);
}

// V's AnimationControllerComponent and its SetInput* functions, once per player object.
bool PoseBind(const Handle<IScriptable>& player, LogFn log)
{
    const uintptr_t id = reinterpret_cast<uintptr_t>(player.instance);
    if (g_pose.player == id && g_pose.bindTried) return g_pose.bound;
    const bool test = g_pose.test && g_pose.player == id;
    g_pose = PoseState{};
    g_pose.player = id;
    g_pose.test = test;
    g_pose.bindTried = true;
    if (!player.instance) return false;
    CClass* ac = Cls("entAnimationControllerComponent");
    CClass* entityCls = Cls("entEntity");
    std::string all;
    if (ac && entityCls && player.instance->GetType()->IsA(entityCls))
        for (auto& c : reinterpret_cast<ent::Entity*>(player.instance)->components)
            if (c.instance && c.instance->GetType()->IsA(ac))
            {
                all += std::string(all.empty() ? "" : ", ") + c->name.ToString();
                if (!g_pose.anim.instance) g_pose.anim = c;
            }
    if (!g_pose.anim.instance) { log("stage C3: V has no AnimationControllerComponent; the pose cannot be set"); return false; }
    CClass* cc = g_pose.anim.instance->GetType();
    g_pose.setQ = Func(cc, "SetInputQuaternion");
    g_pose.setV = Func(cc, "SetInputVector");
    g_pose.setF = Func(cc, "SetInputFloat");
    const bool sig = ParamIs(g_pose.setQ, 0, "CName") && ParamIs(g_pose.setQ, 1, "Quaternion") && ParamIs(g_pose.setV, 0, "CName") &&
                     ParamIs(g_pose.setV, 1, "Vector4") && ParamIs(g_pose.setF, 0, "CName") && ParamIs(g_pose.setF, 1, "Float") &&
                     !g_pose.setQ->flags.isStatic;
    for (int i = 0; i < 24; ++i) g_pose.q[i] = CNamePool::Add((std::string("dkr_q_") + kPoseBones[i]).c_str());
    g_pose.hipsVar = CNamePool::Add("dkr_p_Hips");
    g_pose.weightVar = CNamePool::Add("dkr_weight");
    Logf(log, "stage C3: V's animation controller: %s (%s; all: %s); SetInputQuaternion/Vector/Float %s", g_pose.anim->name.ToString(),
         cc->GetName().ToString(), all.c_str(), sig ? "found" : "NOT found with the expected parameters");
    g_pose.bound = sig;
    return sig;
}

// Sets the graph's variables: the model-space pose (when there is one) and the weight.
bool PoseSend(float weight)
{
    IScriptable* a = g_pose.anim.instance;
    bool ok = a != nullptr;
    if (ok && g_pose.haveMs)
    {
        for (int i = 0; i < 24; ++i)
        {
            Quaternion q{};
            q.i = g_pose.ms[i].x, q.j = g_pose.ms[i].y, q.k = g_pose.ms[i].z, q.r = g_pose.ms[i].w;
            ok = Call(a, g_pose.setQ, nullptr, {&g_pose.q[i], &q}) && ok;
        }
        Vector4 h{};
        h.X = g_pose.hipsMs.x, h.Y = g_pose.hipsMs.y, h.Z = g_pose.hipsMs.z, h.W = 1.f;
        ok = Call(a, g_pose.setV, nullptr, {&g_pose.hipsVar, &h}) && ok;
    }
    float w = weight;
    ok = ok && Call(a, g_pose.setF, nullptr, {&g_pose.weightVar, &w});
    ++(ok ? g_pose.sends : g_pose.failed);
    g_pose.sentWeight = weight;
    return ok;
}

// From Follow, after V was teleported to `at` facing `yawDeg`: the skater's pose in V's model space.
void PoseFollow(V3 at, float yawDeg)
{
    stageb::DkrPose p;
    if (!stageb::Pose(p) || !p.valid) return;
    const double now = NowMs();
    if (!g_pose.cur.valid || p.tick < g_pose.cur.tick || now - g_pose.curAt > 500) g_pose.prev = g_pose.cur = p, g_pose.curAt = now;
    else if (p.tick != g_pose.cur.tick) g_pose.prev = g_pose.cur, g_pose.cur = p, g_pose.curAt = now;
    const float a = stageb::Blend(g_pose.prev.tick, g_pose.cur.tick);  // P1-6: from the runtime's clock
    const Q inv = Conj(YawQ(yawDeg));
    for (int i = 0; i < 24; ++i)
    {
        const Q q0 = QOf(g_pose.prev.rot[i]);
        Q q1 = QOf(g_pose.cur.rot[i]);
        if (q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w < 0) q1 = {-q1.x, -q1.y, -q1.z, -q1.w};
        Q q{q0.x + (q1.x - q0.x) * a, q0.y + (q1.y - q0.y) * a, q0.z + (q1.z - q0.z) * a, q0.w + (q1.w - q0.w) * a};
        const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (!(n > 1e-6f)) return;
        q = {q.x / n, q.y / n, q.z / n, q.w / n};
        g_pose.ms[i] = Mul(inv, q);
    }
    const V3 h0{g_pose.prev.hips[0], g_pose.prev.hips[1], g_pose.prev.hips[2]}, h1{g_pose.cur.hips[0], g_pose.cur.hips[1], g_pose.cur.hips[2]};
    g_pose.hipsWorld = h0 + (h1 - h0) * a, g_pose.haveHipsWorld = Finite(g_pose.hipsWorld);
    g_pose.hipsMs = Rot(inv, g_pose.hipsWorld - at);
    if (g_pose.prev.tick != g_pose.cur.tick) ++g_pose.interpolated;
    g_pose.haveMs = Finite(g_pose.hipsMs) && Len(g_pose.hipsMs) < 5.f;
    g_pose.drivenAt = now;
    g_pose.holding = false;
}

// P1-15 (0.24.1): the body check's slot: the first of V's slots bound to one of the pose's bones, hips first. Logged once.
void ChooseBodySlot(LogFn log)
{
    if (g_body.chosen || !g.slots.instance) return;
    g_body.chosen = true;
    static const char* const order[] = {"Hips", "Spine", "Spine1", "Spine2", "Spine3", "Neck", "Neck1", "Head"};
    auto* sc = reinterpret_cast<ent::SlotComponent*>(g.slots.instance);
    std::string all;
    int best = 99;
    for (const auto& sl : sc->slots)
    {
        const std::string bone = sl.boneName.ToString();
        if (all.size() < 600) all += std::string(all.empty() ? "" : ", ") + sl.slotName.ToString() + "->" + (bone.empty() ? "-" : bone);
        for (int k = 0; k < 8 && k < best; ++k)
            if (bone == order[k]) best = k, g_body.slot = sl.slotName, g_body.bone = bone;
    }
    g_body.have = best < 99;
    Logf(log, "stage C2: body check: %s; V's slots (%u): %s", g_body.have ? ("slot " + std::string(g_body.slot.ToString()) + " on " + g_body.bone).c_str()
                                                                          : "no slot on a pose bone, so no check",
         sc->slots.Size(), all.c_str());
}

// P1-15 (0.24.1): one frame of the body check (read before this frame's teleport: the slot shows the last frame).
void BodySample(LogFn log)
{
    ChooseBodySlot(log);
    Pose sp;
    g_body.haveCur = false;
    if (!g_body.have || !g_pose.haveHipsWorld || !SlotWorld(g_body.slot, sp)) { g_body.haveLast = false; return; }
    g_body.curS = sp.p, g_body.haveCur = true;
    const V3 h = g_pose.hipsWorld;
    if (g_body.haveLast)
    {
        const double d = Len((sp.p - g_body.lastS) - (h - g_body.lastH));
        const int k = g_body.lastTp ? 0 : g_body.prevTp ? 1 : 2;
        if (d < 10.0) g_body.sum[k] += d, g_body.mx[k] = std::max(g_body.mx[k], d), ++g_body.n[k];
    }
    g_body.lastS = sp.p, g_body.lastH = h, g_body.haveLast = true;
    g_body.prevTp = g_body.lastTp;
}

// Undo at once (exit, emergency exit): the weight back to 0 on V's graph, then forget everything.
void PoseOff(const Handle<IScriptable>& player, LogFn log)
{
    if (g_pose.bound && g_pose.sentWeight > 0 && player.instance && reinterpret_cast<uintptr_t>(player.instance) == g_pose.player)
    {
        g_pose.haveMs = false;
        Logf(log, "stage C3: pose off (weight %.2f -> 0): %s", g_pose.sentWeight, PoseSend(0.f) ? "done" : "FAILED");
    }
    g_pose = PoseState{};
}
} // namespace

bool Restricted() { return g.mode == Mode::Restricted; }

bool RestrictionsLost(const Handle<IScriptable>& player)
{
    if (g.mode != Mode::Restricted || !player.instance || g_ours.ids.empty() || NowMs() - g_ours.appliedAt < 2000) return false;
    for (const auto& id : g_ours.ids)
        if (HasEffect(player, id.c_str())) return false;
    return true;
}

bool Following() { return g.mode == Mode::Restricted && stageb::Active(); }

void CycleMove(LogFn log)
{
    g_move = g_move == Move::Sparse ? Move::Teleport : Move::Sparse;
    g_follow.sync = true;
    board::SetByTransform(g_move != Move::Teleport);
    uint32_t st = 0, dyn = 0;
    stageb::ActorCounts(st, dyn);
    Logf(log, "stage C2: Ctrl+Shift+F11: V is now moved by %s, the board by %s%s; the physics scenes hold %u static and %u dynamic actors",
         MoveName(g_move), g_move == Move::Teleport ? "teleports" : "transform writes",
         g_move == Move::Teleport ? " (0.23.1's way: the world should stop streaming in)" : "", st, dyn);
}

void Follow(const Handle<IScriptable>& player, const float pos[3], const float vel[3], float heading, uint64_t tick, const char* state, LogFn log)
{
    if (g.mode != Mode::Restricted || !player.instance || reinterpret_cast<uintptr_t>(player.instance) != g.player)
    {
        g_follow = FollowState{};
        return;
    }
    const double now = NowMs();
    // P1-5: while the game is paused nothing moves, so V is left alone and the clock is held. The first in-game ride
    // took each 0.5 s of pause for a lost skater and restarted the follow every 0.5 s (530 times), re-measuring V.
    if (GamePaused())
    {
        if (g_follow.pausedAt == 0) g_follow.pausedAt = now;
        return;
    }
    if (g_follow.pausedAt > 0)
    {
        const double held = now - g_follow.pausedAt;
        g_follow.curAt += held, g_follow.startedAt += held, g_follow.nextDiag += held;
        if (g_follow.lastAt > 0) g_follow.lastAt += held;
        if (g_follow.jumpSince > 0) g_follow.jumpSince += held;
        if (g_follow.farSince > 0) g_follow.farSince += held;
        if (g_follow.arriveSince > 0) g_follow.arriveSince += held;
        g_follow.pausedAt = 0;
    }
    const V3 p{pos[0], pos[1], pos[2]};
    // Guards (C2 run 1 sent V to the map's origin): the skater starts at V's feet, so a start more than 5 m from V is
    // refused; while following, a target more than 3 m from the last one (360 m/s at 120 fps) is skipped.
    V3 vNow;
    float vYawNow = 0;
    const bool haveV = VWorld(player.instance, vNow, vYawNow);
    // C5: the game itself moved V (fast travel, a scene, a quest teleport): she stays far across from where we last put her.
    if (haveV && g_follow.moves > 0)
    {
        const bool away = std::hypot(vNow.x - g_follow.put.x, vNow.y - g_follow.put.y) > kFarM;
        if (g_follow.arriving && (!away || now - g_follow.arriveSince > kArriveMaxMs)) g_follow.arriving = false;
        g_follow.farSince = away && !g_follow.arriving ? (g_follow.farSince > 0 ? g_follow.farSince : now) : 0;
    }
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return;
    // A new session (tick went back) or a gap of more than 0.5 s: start interpolating afresh.
    if (!g_follow.have || tick < g_follow.tick || now - g_follow.curAt > 500)
    {
        // 0.24.0: within a ride (after its first start) a restart may find V further away: moved by sparse teleports or not
        // at all, or after a gap in Skate's ticks. She is then teleported to the skater (Still leaves her), keeping the
        // first start's height offset.
        const bool rejoin = g_rideFollowed && haveV && Len(p - vNow) > 5.f;
        if (!haveV || (!g_rideFollowed && Len(p - vNow) > 5.f))
        {
            if (now >= g_follow.nextDiag)
            {
                g_follow.nextDiag = now + 2000;
                Logf(log, "stage C2: not following: the skater is at (%.2f, %.2f, %.2f), %.1f m from V at (%.2f, %.2f, %.2f)", p.x, p.y, p.z,
                     haveV ? Len(p - vNow) : -1.f, vNow.x, vNow.y, vNow.z);
            }
            g_follow.have = false;
            return;
        }
        g_follow = FollowState{};
        g_follow.have = true;
        g_follow.prev = g_follow.cur = p;
        g_follow.tick = g_follow.prevTick = tick;
        g_follow.curAt = g_follow.startedAt = now;
        // The runtime's heading is the autopilot's line; NaN once the controller drives: then V keeps her own.
        g_follow.yaw = g_follow.yawTarget = std::isfinite(heading) ? heading : vYawNow;
        g_follow.nextDiag = now + 2000;
        g_follow.dz = rejoin ? g_rideDz : p.z - vNow.z;  // the skater's root sits a little above V's feet: keep V's feet where they were
        g_follow.put = vNow, g_follow.putAt = now;
        g_follow.sync = rejoin;
        if (!g_rideFollowed) g_rideFollowed = true, g_rideDz = g_follow.dz;
        Logf(log, "stage C2: V follows the skater from (%.2f, %.2f, %.2f), %s %.0f; V's feet %.2f m below the skater's root", p.x, p.y, p.z,
             std::isfinite(heading) ? "autopilot heading" : "controller driving, V's yaw", g_follow.yaw, g_follow.dz);
    }
    else if (tick != g_follow.tick)
    {
        g_follow.prev = g_follow.cur;
        g_follow.prevTick = g_follow.tick;
        g_follow.cur = p;
        g_follow.tick = tick;
        g_follow.curAt = now;
    }
    // P1-6: V's own movement since last frame, as the game applied the earlier teleports.
    if (haveV)
    {
        if (g_follow.lastVAt > 0 && now - g_follow.lastVAt < 50)
            StepInto(vNow - g_follow.lastV, g_follow.stepV, g_follow.haveStepV, g_follow.vSum2, g_follow.vMax, g_follow.vN);
        else g_follow.haveStepV = false;
        g_follow.lastV = vNow, g_follow.lastVAt = now;
    }
    BodySample(log);
    {
        uint32_t st = 0, dyn = 0;
        stageb::ActorCounts(st, dyn);
        g_follow.dynMin = g_follow.dynMin == 0 ? dyn : std::min(g_follow.dynMin, dyn), g_follow.dynMax = std::max(g_follow.dynMax, dyn);
    }
    // P2-7/P2-8 (0.23.0): while the skater is down (Wipeout*) or on foot (Biped*), V follows it and wears Skate's pose, as
    // while riding: the bail and the walk play on V. Up to 0.22.0 she held still on her own animation (C2 run 3: her
    // standing animation at the fallen body's root was half in the road; with Skate's pose on her, the body lies where
    // Skate's does; offline, those poses are as sound as the riding ones). A body that drops more than kDownFollowDrop
    // below where it went down (over a wall, into a closed courtyard) is not followed further: V holds as before, and the
    // runtime puts the skater back (P0-15).
    const bool down = state && (strncmp(state, "Wipeout", 7) == 0 || strncmp(state, "Biped", 5) == 0);
    if (down && g_follow.downSince == 0)
    {
        g_follow.downSince = now, g_follow.downZ = g_follow.sent.z;
        Logf(log, "stage C2: the skater is %s (%s); V follows it with Skate's pose", strncmp(state, "Biped", 5) == 0 ? "on foot" : "down", state);
    }
    else if (!down && g_follow.downSince > 0)
    {
        Logf(log, "stage C2: the skater is back on the board (%s) after %.1f s", state, (now - g_follow.downSince) / 1000.0);
        g_follow.downSince = 0;
        g_follow.resync = true;  // Skate's respawn may put it several metres away
    }
    const float downBlend = stageb::Blend(g_follow.prevTick, g_follow.tick);
    const V3 downAt = g_follow.prev + (g_follow.cur - g_follow.prev) * downBlend - V3{0, 0, g_follow.dz};
    if (down && downAt.z < g_follow.downZ - kDownFollowDrop)
    {
        if (!g_follow.holding)
        {
            Logf(log, "stage C2: the skater fell %.1f m below where it went down (%s); V holds at (%.2f, %.2f, %.2f)", g_follow.downZ - downAt.z, state,
                 g_follow.sent.x, g_follow.sent.y, g_follow.sent.z);
            g_follow.holdAt = now, g_follow.holdDiag = now + 1000, g_follow.holdLow = 0, g_follow.pinned = false, g_follow.holdPuts = 0;
        }
        g_follow.holding = true;
        g_pose.holding = true;  // C3: V's own animation while she holds (the fallen skater's pose is elsewhere)
        g_follow.lastAt = now;
        // P0-14 (0.19.0): while she holds, the game's own physics has V from the last spot she was put. After a handrail
        // grind that is the top of the rail, and on the H10 stairs V died 1.6 s into a hold (about a 12 m fall). She
        // may settle onto the ground as before; once she drops more than 1 m below the held spot she is put back there,
        // and kept there until the skater is up.
        if (haveV && g_follow.teleports > 0)
        {
            const float drop = vNow.z - g_follow.sent.z;
            g_follow.holdLow = std::min(g_follow.holdLow, drop);
            if (!g_follow.pinned && drop < -kHoldDrop)
            {
                g_follow.pinned = true;
                Logf(log, "stage C2: V dropped %.2f m below where she holds, to (%.2f, %.2f, %.2f), %.1f s into the hold; put back and kept "
                          "there until the skater is up", -drop, vNow.x, vNow.y, vNow.z, (now - g_follow.holdAt) / 1000.0);
            }
            if (g_follow.pinned && TeleportV(player, g_follow.sent, g_follow.yaw)) ++g_follow.holdPuts;
            if (now >= g_follow.holdDiag)
            {
                g_follow.holdDiag = now + 1000;
                Logf(log, "stage C2: holding %.1f s: V at (%.2f, %.2f, %.2f), %+.2f m from the held spot%s", (now - g_follow.holdAt) / 1000.0,
                     vNow.x, vNow.y, vNow.z, drop, g_follow.pinned ? " (kept there)" : "");
            }
        }
        return;
    }
    if (g_follow.holding && !(down && downAt.z < g_follow.downZ - kDownFollowDrop))
    {
        if (g_follow.pinned)
            Logf(log, "stage C2: hold over after %.1f s: V's lowest %+.2f m from the held spot; put back after dropping, then kept there for %u frames",
                 (now - g_follow.holdAt) / 1000.0, g_follow.holdLow, g_follow.holdPuts);
        else Logf(log, "stage C2: hold over after %.1f s: V's lowest %+.2f m from the held spot", (now - g_follow.holdAt) / 1000.0, g_follow.holdLow);
        g_follow.holding = false;
        g_follow.prev = g_follow.cur = p;  // no interpolation from the fallen body
        g_follow.prevTick = g_follow.tick;
        g_follow.curAt = now;
        g_follow.resync = true;
        Logf(log, "stage C2: following the skater again (%s), %.1f m from V", state, Len(p - V3{0, 0, g_follow.dz} - g_follow.sent));
    }
    const float a = stageb::Blend(g_follow.prevTick, g_follow.tick);  // P1-6: from the runtime's clock
    V3 at = g_follow.prev + (g_follow.cur - g_follow.prev) * a - V3{0, 0, g_follow.dz};
    // Jumps: more than 3 m in one frame is not riding. Up to 50 m is accepted at once after a wipeout, and any jump once
    // the skater has stayed at the new place for 0.25 s (a respawn, or a session marker return); anything shorter-lived is
    // skipped. 0.24.0: up to 0.23.1 jumps of 1,000 m or more were never followed.
    const float jump = g_follow.moves > 0 ? Len(at - g_follow.sent) : 0.f;
    if (jump > 3.f)
    {
        if (g_follow.jumpSince == 0) g_follow.jumpSince = now;
        if ((g_follow.resync && jump < 50.f) || now - g_follow.jumpSince >= 250)
        {
            Logf(log, "stage C2: the skater moved %.1f m at once (%s); V follows it there", jump,
                 g_follow.resync && jump < 50.f ? "respawn after a wipeout" : "held 0.25 s");
            g_follow.prev = g_follow.cur = p;
            g_follow.prevTick = g_follow.tick;
            g_follow.haveStepSent = g_follow.haveStepV = false;
            g_follow.sync = true;  // P1-15: a jump is always teleported
            if (jump > kFarM) g_follow.arriving = true, g_follow.arriveSince = now;
            at = p - V3{0, 0, g_follow.dz};
        }
        else
        {
            if (++g_follow.skipped <= 3) Logf(log, "stage C2: skipped a teleport of %.1f m in one frame, to (%.2f, %.2f, %.2f)", jump, at.x, at.y, at.z);
            return;
        }
    }
    g_follow.jumpSince = 0;
    g_follow.resync = false;
    // Facing (C2 run 2: the yaw jittered during pops and landings, e.g. 50 -> 37 -> 44 -> 41 deg around a boneless). The
    // travel direction is taken only when riding (above 1 m/s, vertical speed under 0.5 m/s), and V turns towards it
    // with a 0.15 s time constant. C3 replaces this with the skater's own root rotation.
    const float speed = std::hypot(vel[0], vel[1]);
    const float dt = g_follow.lastAt > 0 ? float(now - g_follow.lastAt) / 1000.f : 0.f;
    g_follow.lastAt = now;
    if (speed > 1.0f && std::fabs(vel[2]) < 0.5f) g_follow.yawTarget = std::atan2(-vel[0], vel[1]) * 57.29578f;  // 0 = +Y, CCW
    float dyaw = std::fmod(g_follow.yawTarget - g_follow.yaw + 540.f, 360.f) - 180.f;
    g_follow.yaw += dyaw * (1.f - std::exp(-std::clamp(dt, 0.f, 0.1f) / 0.15f));
    ++g_follow.frames;
    // Never hand the game a NaN position or facing (P1-5): keep the last good one.
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z) || !std::isfinite(g_follow.yaw))
    {
        if (++g_follow.skipped <= 3) Logf(log, "stage C2: skipped a non-finite target (%.2f, %.2f, %.2f) yaw %.1f", at.x, at.y, at.z, g_follow.yaw);
        if (!std::isfinite(g_follow.yaw)) g_follow.yaw = g_follow.yawTarget = vYawNow;
        return;
    }
    // P1-16 (0.26.2): the twenty-fourth test's ghost. dz is measured once, when V starts following, but the skater's
    // root sits higher on the board than on foot, so after a change of stance (or a marker return) each sparse teleport
    // put V up to 30 cm into the ground; the game pushed her capsule back up during the next frame's physics, after her
    // pose had been built against the lower root, and her body was drawn 30 cm high for that frame (follow trace,
    // frames 490-492: teleported to z 10.73, read 10.73, then 11.03). So the game's own correction is measured once she
    // has settled after a teleport (2 still frames, the skater on the ground at the same height), per stance, and added
    // to the following teleports: she then lands where her capsule rests, and nothing moves her between the pose and
    // the drawing.
    const int stance = state && std::strncmp(state, "Biped", 5) == 0 ? 0 : 1;
    const bool grounded = state && !std::strstr(state, "Air") && std::strncmp(state, "Wipeout", 7) != 0 && std::strncmp(state, "Teleport", 8) != 0;
    if (haveV)
    {
        g_follow.stillFrames = std::fabs(vNow.z - g_follow.lastVz) < 0.005f ? g_follow.stillFrames + 1 : 0;
        g_follow.lastVz = vNow.z;
        if (!g_follow.tpMeasured && grounded && g_follow.sinceTp >= 2 && g_follow.stillFrames >= 2 && std::fabs(at.z - g_follow.tpAtZ) < 0.05f)
        {
            g_follow.tpMeasured = true;
            const float pop = vNow.z - g_follow.tpZ;
            if (std::fabs(pop) > 0.02f && std::fabs(pop) < 0.8f)
            {
                float& r = g_follow.restDz[stance];
                r = std::clamp(r + pop, -1.f, 1.f);
                if (++g_follow.restAdjusts <= 8)
                    Logf(log, "stage C2: the game moved V %+.2f m after a teleport (%s); her teleports %s now go %+.2f m from the skater's height",
                         pop, state, stance ? "on the board" : "on foot", r);
            }
        }
    }
    if (++g_follow.sinceTp > 30) g_follow.tpMeasured = true;
    const V3 target = at + V3{0, 0, g_follow.restDz[stance]};
    // P1-15: V moved by the current method; Skate's pose is placed against wherever her root is this frame.
    const bool sync = g_follow.moves == 0 || g_follow.sync;
    V3 root = target;
    float rootYaw = g_follow.yaw;
    bool teleported = false;
    const bool tried = g_move == Move::Teleport || sync || !haveV || Len(target - vNow) > kSparseLeash;
    if (tried) teleported = TeleportV(player, target, g_follow.yaw);
    else root = vNow, rootYaw = vYawNow;
    if (teleported)
        g_follow.tpZ = target.z, g_follow.tpAtZ = at.z, g_follow.sinceTp = 0, g_follow.stillFrames = 0, g_follow.tpMeasured = false;
    g_body.lastTp = teleported;
    if (tried && !teleported)
    {
        if (++g_follow.failed == 1) log("stage C2: TeleportationFacility.Teleport unavailable; V cannot follow");
    }
    else
    {
        if (teleported)
        {
            // P1-15 (0.24.1): the camera was placed this frame against V's root where it stood; the teleport moves the root,
            // and the camera's parent with it, before the frame is drawn.
            if (haveV) CameraAfterTeleport(vNow, vYawNow, target, g_follow.yaw);
            g_follow.put = target, g_follow.putAt = now;
            ++g_follow.teleports, ++g_follow.winTeleports;
        }
        g_follow.sync = false;
        if (g_follow.moves > 0 && now - g_follow.lastSentAt < 50)
            StepInto(at - g_follow.sent, g_follow.stepSent, g_follow.haveStepSent, g_follow.sentSum2, g_follow.sentMax, g_follow.sentN, &g_follow.sentSum);
        else g_follow.haveStepSent = false;
        g_follow.lastSentAt = now;
        ++g_follow.moves, g_follow.sent = at;
        PoseFollow(root, rootYaw);
    }
    {
        TraceRow& r = g_trace[g_traceNext];
        r = TraceRow{};
        r.t = now, r.tick = g_follow.tick, r.tried = tried, r.tp = teleported, r.haveV = haveV, r.haveSlot = g_body.haveCur;
        snprintf(r.state, sizeof r.state, "%s", state ? state : "");
        r.vNow = vNow, r.vYaw = vYawNow, r.at = at, r.root = root, r.rootYaw = rootYaw;
        r.hipsW = g_pose.hipsWorld, r.hipsMs = g_pose.hipsMs, r.slot = g_body.curS, r.weight = g_pose.weight;
        g_traceNext = (g_traceNext + 1) % kTraceRows;
        g_traceCount = std::min(g_traceCount + 1, kTraceRows);
    }
    if (now >= g_follow.nextDiag)
    {
        g_follow.nextDiag = now + 2000;
        V3 vp;
        float vyaw = 0;
        const bool ok = VWorld(player.instance, vp, vyaw);
        Logf(log, "stage C2: %.0f s: skater (%.2f, %.2f, %.2f) %.1f m/s; V %s (%.2f, %.2f, %.2f) yaw %.0f, %.2f m from where it was last sent (dz %+.2f); %u frames, %u teleports, %u failed",
             (now - g_follow.startedAt) / 1000.0, p.x, p.y, p.z, speed, ok ? "at" : "unknown", vp.x, vp.y, vp.z, vyaw, ok ? Len(vp - g_follow.sent) : 0.f,
             ok ? vp.z - g_follow.sent.z : 0.f, g_follow.frames, g_follow.teleports, g_follow.failed);
        // P1-15: the method, whether the game carries V's capsule with her, and how much of the world is in the physics
        // scenes (it grows as traffic, people and props stream in); and (0.24.1) V's drawn body against the skater.
        char capsule[64] = "unknown";
        const float vf[3] = {vp.x, vp.y, vp.z};
        float cap = 0;
        if (ok && stageb::CapsuleOffset(vf, cap)) snprintf(capsule, sizeof capsule, "%.2f m from where it belongs", cap);
        uint32_t st = 0, dyn = 0;
        stageb::ActorCounts(st, dyn);
        Logf(log, "stage C2: V moved by %s: %u teleports in 2 s; %.2f m from her root to the skater; V's PhysX capsule %s; physics "
                  "scenes: %u static and %u dynamic actors (dynamic %u-%u over 2 s)",
             MoveName(g_move), g_follow.winTeleports, ok ? Len(vp - g_follow.sent) : -1.f, capsule, st, dyn, g_follow.dynMin, g_follow.dynMax);
        g_follow.winTeleports = 0, g_follow.dynMin = g_follow.dynMax = 0;
        if (g_body.have)
        {
            auto part = [](int k) {
                char b[64];
                if (g_body.n[k] == 0) snprintf(b, sizeof b, "none");
                else snprintf(b, sizeof b, "mean %.1f cm, max %.1f cm over %u", 100.0 * g_body.sum[k] / g_body.n[k], 100.0 * g_body.mx[k], g_body.n[k]);
                return std::string(b);
            };
            Logf(log, "stage C2: V's body (slot %s on %s) against the skater's hips, frame to frame: on teleport frames %s; the frame after %s; "
                      "other frames %s", g_body.slot.ToString(), g_body.bone.c_str(), part(0).c_str(), part(1).c_str(), part(2).c_str());
            for (int k = 0; k < 3; ++k) g_body.sum[k] = g_body.mx[k] = 0, g_body.n[k] = 0;
        }
        if (g_follow.sentN > 0 && g_follow.vN > 0)
            Logf(log, "stage C2: V's per-frame movement over 2 s: sent %.1f cm a frame on average, its frame-to-frame change rms %.1f cm, max %.1f cm; "
                      "as applied by the game rms %.1f cm, max %.1f cm (show tick %.2f)",
                 100.0 * g_follow.sentSum / g_follow.sentN, 100.0 * std::sqrt(g_follow.sentSum2 / g_follow.sentN), 100.0 * g_follow.sentMax,
                 100.0 * std::sqrt(g_follow.vSum2 / g_follow.vN), 100.0 * g_follow.vMax, stageb::ShowTick() - double(g_follow.tick));
        g_follow.sentSum = g_follow.sentSum2 = g_follow.sentMax = g_follow.vSum2 = g_follow.vMax = 0;
        g_follow.sentN = g_follow.vN = 0;
    }
}

namespace
{

} // namespace

void DumpFollowTrace(const std::filesystem::path& dir, LogFn log)
{
    if (g_traceCount == 0) { log("stage C2: follow trace: nothing recorded yet"); return; }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"follow-%04u%02u%02u-%02u%02u%02u.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const auto file = dir / name;
    FILE* f = nullptr;
    _wfopen_s(&f, file.c_str(), L"w");
    if (!f) { log("stage C2: follow trace: cannot open the file"); return; }
    fprintf(f, "t_ms,tick,state,tried,teleported,haveV,vx,vy,vz,vyaw,atx,aty,atz,rootx,rooty,rootz,rootyaw,"
               "hipsx,hipsy,hipsz,msx,msy,msz,haveSlot,slotx,sloty,slotz,weight\n");
    const size_t first = (g_traceNext + kTraceRows - g_traceCount) % kTraceRows;
    const double t0 = g_trace[(g_traceNext + kTraceRows - 1) % kTraceRows].t;
    for (size_t i = 0; i < g_traceCount; ++i)
    {
        const TraceRow& r = g_trace[(first + i) % kTraceRows];
        fprintf(f, "%.1f,%llu,%s,%d,%d,%d,%.3f,%.3f,%.3f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.2f\n",
                r.t - t0, static_cast<unsigned long long>(r.tick), r.state, r.tried, r.tp, r.haveV, r.vNow.x, r.vNow.y, r.vNow.z, r.vYaw,
                r.at.x, r.at.y, r.at.z, r.root.x, r.root.y, r.root.z, r.rootYaw, r.hipsW.x, r.hipsW.y, r.hipsW.z, r.hipsMs.x, r.hipsMs.y,
                r.hipsMs.z, r.haveSlot, r.slot.x, r.slot.y, r.slot.z, r.weight);
    }
    fclose(f);
    Logf(log, "stage C2: follow trace: the last %zu frames written to %ls", g_traceCount, file.c_str());
}

void DumpRtti(const std::filesystem::path& file, IScriptable* player, LogFn log)
{
    FILE* f = nullptr;
    _wfopen_s(&f, file.c_str(), L"w");
    if (!f) { log("stage C0: cannot open the dump file"); return; }
    fprintf(f, "Deckrunner C0 RTTI dump (read-only), Cyberpunk 2077 2.5\n");
    for (const char* c : {"gameActivateTPPRepresentationEvent", "gameDeactivateTPPRepresentationEvent", "gamePrepareTPPRepresentationEvent",
                          "gameFinalizeActivationTPPRepresentationEvent", "gameFinalizeDeactivationTPPRepresentationEvent",
                          "gameAppearancesReadyTPPRepresentationEvent", "gameTPPRepresentationComponent", "gameAnimFeature_TPPRepresentation",
                          "TakeOverControlSystem", "entIPlacedComponent", "entBaseCameraComponent", "gameCameraComponent", "gameFPPCameraComponent",
                          "vehicleTPPCameraComponent", "entAnimatedComponent", "entAnimationControllerComponent", "StatusEffectHelper",
                          "gameStatusEffectSystem", "entEntity", "gameObject", "gamePuppet", "ScriptedPuppet", "PlayerPuppet",
                          "gameITeleportationFacility", "gameTransactionSystem", "gameItemID"})
        DumpClass(f, c);

    // Every scriptable class's functions whose names matter to Stage C.
    const char* patterns[] = {"TPP", "Perspective", "ThirdPerson", "Representation", "LocalPosition", "LocalOrientation", "LocalTransform",
                              "FPPCamera", "QueueEvent", "StatusEffect", "Teleport", "BoneTransform", "SetInput", "ApplyFeature",
                              "GameplayRestriction", "Restriction"};
    auto* r = CRTTISystem::Get();
    DynArray<CClass*> classes;
    r->GetClasses(Cls("IScriptable"), classes, nullptr, true);
    fprintf(f, "\n== functions matching Stage C names, across %u scriptable classes\n", classes.Size());
    uint32_t hits = 0;
    for (CClass* c : classes)
    {
        if (!c) continue;
        auto scan = [&](CBaseFunction* fn, const char* kind) {
            if (!fn) return;
            const std::string n = fn->shortName.ToString();
            for (const char* p : patterns)
                if (n.find(p) != std::string::npos)
                {
                    fprintf(f, "%s %s::%s\n", kind, c->GetName().ToString(), Signature(fn).c_str());
                    ++hits;
                    return;
                }
        };
        for (auto* fn : c->funcs) scan(fn, "func");
        for (auto* fn : c->staticFuncs) scan(fn, "static");
    }
    DynArray<CBaseFunction*> globals;
    r->GetGlobalFunctions(globals);
    fprintf(f, "\n== global functions matching (%u globals)\n", globals.Size());
    for (CBaseFunction* fn : globals)
    {
        if (!fn) continue;
        const std::string n = fn->shortName.ToString();
        for (const char* p : patterns)
            if (n.find(p) != std::string::npos) { fprintf(f, "global %s\n", Signature(fn).c_str()); ++hits; break; }
    }

    fprintf(f, "\n== TweakDB records\n");
    uint32_t present = 0, checked = 0;
    for (const char* id : {"GameplayRestriction.NoMovement", "GameplayRestriction.NoCameraControl", "GameplayRestriction.NoJump",
                           "GameplayRestriction.NoWeapons", "GameplayRestriction.NoWorldInteractions", "GameplayRestriction.NoRadialMenus",
                           "GameplayRestriction.NoPhone", "GameplayRestriction.NoCombat", "GameplayRestriction.NoDriving",
                           "GameplayRestriction.NoSprint", "GameplayRestriction.NoScanning", "GameplayRestriction.VehicleNoSummoning",
                           "Items.PlayerWaTppHead", "Items.PlayerMaTppHead", "AttachmentSlots.TppHead"})
    {
        std::string type;
        const bool ok = RecordExists(id, &type);
        ++checked, present += ok;
        fprintf(f, "%s: %s\n", id, ok ? type.c_str() : "missing");
    }

    fprintf(f, "\n== V\n");
    if (player)
    {
        fprintf(f, "class %s\n", player->GetType()->GetName().ToString());
        Handle<IScriptable> cam;
        if (Call(player, Func(player->GetType(), "GetFPPCameraComponent"), &cam, {}) && cam.instance)
        {
            Vector4 p{};
            Quaternion q{};
            const bool ok = Call(cam.instance, Func(cam.instance->GetType(), "GetLocalPosition"), &p, {}) &&
                            Call(cam.instance, Func(cam.instance->GetType(), "GetLocalOrientation"), &q, {});
            fprintf(f, "FPP camera %s: local position %s (%.4f, %.4f, %.4f, %.4f) orientation (%.4f, %.4f, %.4f, %.4f)\n",
                    cam.instance->GetType()->GetName().ToString(), ok ? "read" : "NOT read", p.X, p.Y, p.Z, p.W, q.i, q.j, q.k, q.r);
            Mat m{};
            if (Call(cam.instance, Func(cam.instance->GetType(), "GetLocalToWorld"), &m, {}))
                for (int i = 0; i < 4; ++i) fprintf(f, "  world matrix %c: %.4f %.4f %.4f %.4f\n", "XYZW"[i], m.r[i].X, m.r[i].Y, m.r[i].Z, m.r[i].W);
        }
        else fprintf(f, "FPP camera: GetFPPCameraComponent unavailable\n");
        CName gender;
        if (Call(player, Func(player->GetType(), "GetResolvedGenderName"), &gender, {})) fprintf(f, "gender %s\n", gender.ToString());
    }
    fclose(f);
    Logf(log, "stage C0: RTTI dump written (%u matching functions, %u of %u TweakDB records present): %ls", hits, present, checked,
         file.c_str());
}


void InstallCameraHook(const v1::Sdk* sdk, v1::PluginHandle handle, LogFn log)
{
    constexpr uint32_t kHash = 2531201123;  // FPPCameraComponent rotation update (Let There Be Flight's FPPCamera_Update hook)
    // RED4ext's own resolver returns 0 for an unknown hash; UniversalRelocBase::Resolve would end the process instead.
    using ResolveFn = uintptr_t (*)(uint32_t);
    HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
    const auto resolve = red4ext ? reinterpret_cast<ResolveFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress")) : nullptr;
    const uintptr_t addr = resolve ? resolve(kHash) : 0;
    MEMORY_BASIC_INFORMATION mbi{};
    const bool code = addr && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) &&
                      (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    if (!code) { Logf(log, "stage C1: FPP camera hook not attached (hash %u -> %p is not code); the camera is placed after the frame", kHash, (void*)addr); return; }
    const bool ok = sdk->hooking->Attach(handle, reinterpret_cast<void*>(addr), reinterpret_cast<void*>(&RotateDetour), reinterpret_cast<void**>(&g_rotate));
    Logf(log, "stage C1: FPP camera hook (hash %u at %p): %s", kHash, (void*)addr, ok ? "attached" : "FAILED");
}

// Also true while restrictions we applied are still being confirmed off V after an exit.
bool Active() { return g.mode != Mode::Off || !g_ours.ids.empty() || g_head.pending || AmmPending(); }


// #3: the on-foot restrictions are in place (the weapon wheel section at the end of this file).
bool g_wheelOnFoot = false;

// ----- C5: lifecycle (0.15.0) -----
bool EnterSkating(const Handle<IScriptable>& player, LogFn log)
{
    if (!player.instance) { log("stage C5: V not available"); return false; }
    if (g.mode == Mode::Restricted) return true;
    if (g.mode == Mode::Off) EnterThird(player, log);
    if (g.mode == Mode::Off) return false;
    if (reinterpret_cast<uintptr_t>(player.instance) != g.player) { ExitAll(player, log, "player replaced"); return false; }
    ApplyRestrictions(player, log);
    g.mode = Mode::Restricted;
    g_headOff = false;  // 0.26.0: every ride starts with V's head on
    g_wheelOnFoot = false;  // #3: all of the restrictions are on until the skater is on foot
    log("stage C5: skating ON (third person and the skating restrictions in one step)");
    return true;
}

void ExitSkating(const Handle<IScriptable>& player, LogFn log, const char* why)
{
    if (g.mode != Mode::Off) ExitAll(player, log, why);
    g_follow = FollowState{};
    g_wheelOnFoot = false;  // #3: the next ride starts with every restriction on
}

const char* ExitReason(const Handle<IScriptable>& player)
{
    static uint32_t n = 0;
    if (g.mode != Mode::Restricted || !player.instance || (++n % 15) != 0) return nullptr;  // about 8 times a second
    IScriptable* v = player.instance;
    bool dead = false;
    if (Call(v, Func(v->GetType(), "IsDead"), &dead, {}) && dead) return "V died";
    Handle<IScriptable> vehicle;
    if (Call(v, Func(v->GetType(), "GetMountedVehicle"), &vehicle, {}) && vehicle.instance) return "V is in a vehicle";
    static CClass* wc = Cls("workWorkspotGameSystem") ? Cls("workWorkspotGameSystem") : Cls("WorkspotGameSystem");
    static CBaseFunction* inWorkspot = wc ? Func(wc, "IsActorInWorkspot") : nullptr;
    IScriptable* ws = nullptr;
    bool busy = false;
    if (inWorkspot && SelfFor(wc, inWorkspot, ws) && Call(ws, inWorkspot, &busy, {const_cast<Handle<IScriptable>*>(&player)}) && busy)
        return "V is in a scene or workspot";
    if (g_follow.farSince > 0 && NowMs() - g_follow.farSince > kFarMs) return "the game moved V (fast travel, a scene or a quest)";
    return nullptr;
}

std::string LifecycleSnapshot(const Handle<IScriptable>& player)
{
    if (!player.instance) return "V unavailable";
    std::string s = "restrictions on V:";
    bool any = false;
    for (const char* id : kRestrictions)
        if (HasEffect(player, id)) s += std::string(" ") + (std::strchr(id, '.') ? std::strchr(id, '.') + 1 : id), any = true;
    if (!any) s += " none";
    IScriptable* v = player.instance;
    Handle<IScriptable> cam;
    Vector4 p{};
    Quaternion q{};
    if (Call(v, Func(v->GetType(), "GetFPPCameraComponent"), &cam, {}) && cam.instance &&
        Call(cam.instance, Func(cam.instance->GetType(), "GetLocalPosition"), &p, {}) &&
        Call(cam.instance, Func(cam.instance->GetType(), "GetLocalOrientation"), &q, {}))
    {
        char b[160];
        snprintf(b, sizeof b, "; FPP camera local (%.2f, %.2f, %.2f | %.2f, %.2f, %.2f, %.2f)", p.X, p.Y, p.Z, q.i, q.j, q.k, q.r);
        s += b;
    }
    else s += "; FPP camera unreadable";
    s += std::string("; head slot ") + HeadSlotItem(player);
    return s;
}

bool Settled() { return g.mode == Mode::Off && g_ours.ids.empty() && !g_head.pending && g_hx.step == 0; }

// ----- Skate audio (0.16.0): the listener -----
bool Listener(float pos[3], float fwd[3], float right[3])
{
    if (g.mode == Mode::Off || !g.camera.instance) return false;
    Pose c;
    if (!CameraWorld(c)) return false;
    const V3 f = Rot(c.q, {0, 1, 0}), r = Rot(c.q, {1, 0, 0});
    pos[0] = c.p.x, pos[1] = c.p.y, pos[2] = c.p.z;
    fwd[0] = f.x, fwd[1] = f.y, fwd[2] = f.z;
    right[0] = r.x, right[1] = r.y, right[2] = r.z;
    return true;
}

bool PlaceV(const Handle<IScriptable>& player, const float pos[3], float yawDeg)
{
    return player.instance && TeleportV(player, V3{pos[0], pos[1], pos[2]}, yawDeg);
}

void Cycle(const Handle<IScriptable>& player, LogFn log)
{
    if (!player.instance) { log("stage C1: V not available"); return; }
    switch (g.mode)
    {
    case Mode::Off: EnterThird(player, log); break;
    case Mode::FirstBody:
        if (reinterpret_cast<uintptr_t>(player.instance) != g.player) { ExitAll(player, log, "player replaced"); break; }
        // 0.9.5: straight to the restrictions. The game's third-person body (ThirdBody, the TPP representation) is left
        // out: it puts V in the device-control crouch (C1 runs 1-4), and AMM's third person does without it.
        Snapshot(player, log, "third person", true);
        ApplyRestrictions(player, log);
        g.mode = Mode::Restricted;
        log("stage C1: skating restrictions ON (V should not move, jump, draw weapons, interact or use the phone). Ctrl+Shift+F7 again exits.");
        break;
    case Mode::ThirdBody:
        if (reinterpret_cast<uintptr_t>(player.instance) != g.player) { ExitAll(player, log, "player replaced"); break; }
        Snapshot(player, log, "step 2 end", true);
        ApplyRestrictions(player, log);
        g.mode = Mode::Restricted;
        log("stage C1: step 3 of 3: skating restrictions ON (V should not move, jump, draw weapons, interact or use the phone). Ctrl+Shift+F7 again exits.");
        break;
    case Mode::Restricted: ExitAll(player, log, "hotkey"); break;
    }
}

// Undoes everything, and clears all five restrictions from V whoever applied them: in C1 run 2 they outlived the
// exit and this key, which then only acted while third person was on, could not help.
void HeadExperiment(const Handle<IScriptable>& player, LogFn log)
{
    if (g.mode == Mode::Off || !player.instance || reinterpret_cast<uintptr_t>(player.instance) != g.player)
    {
        log("stage C1: head experiment: turn third person on first (Ctrl+Shift+F7)");
        return;
    }
    // 0.9.7: AMM's head swap (Items.CharacterCustomization{Wa,Ma}Head), on this key only, with one add per attempt.
    // 0.9.6's appearance-only change did nothing visible.
    HeadBegin(player, !g_head.on, log);
}

void EmergencyExit(const Handle<IScriptable>& player, LogFn log)
{
    if (g.mode != Mode::Off) ExitAll(player, log, "emergency exit");
    else PoseOff(player, log), board::Remove(log, "emergency exit"), board::CamRemove(log, "emergency exit");
    if (!player.instance) { log("stage C1: emergency exit: V not available"); return; }
    if (g_head.on && !g_head.pending) HeadBegin(player, false, log);
    if (g_look.changed) HeadLook(player, false, log);
    g_ours.player = reinterpret_cast<uintptr_t>(player.instance);
    uint32_t found = 0;
    for (const char* id : kRestrictions)
        if (HasEffect(player, id))
        {
            ++found;
            if (std::find(g_ours.ids.begin(), g_ours.ids.end(), id) == g_ours.ids.end()) g_ours.ids.push_back(id);
        }
    Logf(log, "stage C1: emergency exit: %u of the %zu skating restrictions on V; removing them", found, kRestrictionCount);
    if (!g_ours.ids.empty()) BeginCleanup(player, log);
}


namespace
{
// P2-10 (0.25.3): the animation feature Immersive Third Person applies on entering third person:
// gameAnimFeature_TPPRepresentation (IsActive), input name TPPRepresentation, on V's AnimationControllerComponent.
bool ApplyTppFeature(const Handle<IScriptable>& player, bool active, LogFn log)
{
    CClass* fc = Cls("gameAnimFeature_TPPRepresentation");
    CClass* ac = Cls("entAnimationControllerComponent");
    CClass* ec = Cls("entEntity");
    IScriptable* anim = nullptr;
    if (player.instance && ec && ac && player.instance->GetType()->IsA(ec))
        for (auto& c : reinterpret_cast<ent::Entity*>(player.instance)->components)
            if (c.instance && c.instance->GetType()->IsA(ac)) { anim = c.instance; break; }
    CBaseFunction* apply = anim ? Func(anim->GetType(), "ApplyFeature") : nullptr;
    CProperty* p = fc ? Prop(fc, "IsActive") : nullptr;
    if (!apply || !p || !ParamIs(apply, 0, "CName") || std::string(TypeName(p->type)) != "Bool")
    {
        Logf(log, "stage C4: head experiment: the TPPRepresentation animation feature cannot be applied (%s)",
             !anim ? "V has no AnimationControllerComponent" : !apply ? "no ApplyFeature" : "unexpected types");
        return false;
    }
    Handle<IScriptable> feature(static_cast<IScriptable*>(fc->CreateInstance(true)));
    *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(feature.instance) + p->valueOffset) = active;
    CName input("TPPRepresentation");
    const bool ok = Call(anim, apply, nullptr, {&input, &feature});
    Logf(log, "stage C4: head experiment: animation feature TPPRepresentation (IsActive %s) applied to V: %s", active ? "true" : "false",
         ok ? "ok" : "FAILED");
    return ok;
}

// A head part by its component name (the game's character-customization prefixes), or any morph-target mesh.
bool HeadPart(IScriptable* c, const std::string& n)
{
    static CClass* const morph = Cls("entMorphTargetSkinnedMeshComponent");
    auto starts = [&](const char* pre) { return n.rfind(pre, 0) == 0; };
    return starts("h0_") || starts("he_") || starts("heb_") || starts("hel_") || starts("ht_") || starts("hx_") || starts("hh_") ||
           starts("hb_") || starts("i1_") || n.find("head") != std::string::npos || n.find("face") != std::string::npos ||
           (morph && c->GetType()->IsA(morph));
}

// V's scalp hair: hh_ parts that are not shadow meshes (hh_078_wa__evelyn_shadow and the like only cast shadows).
bool HairPart(const std::string& n) { return n.rfind("hh_", 0) == 0 && n.find("shadow") == std::string::npos; }

// P2-10 (0.25.3): head parts as they are now, on V's own entity (True First Person Camera: "the head stays on the puppet")
// and on the item in TppHead: name, enabled, mesh appearance, chunk mask when not all chunks.
void DumpParts(IScriptable* owner, const char* who, const char* when, LogFn log)
{
    CClass* ec = Cls("entEntity");
    if (!owner || !ec || !owner->GetType()->IsA(ec))
    {
        Logf(log, "stage C4: head parts on %s, %s: none (no entity)", who, when);
        return;
    }
    std::vector<std::string> parts;
    uint32_t found = 0, enabled = 0;
    for (auto& c : reinterpret_cast<ent::Entity*>(owner)->components)
    {
        if (!c.instance) continue;
        const std::string n = c->name.ToString();
        if (!HeadPart(c.instance, n)) continue;
        ++found;
        const bool on = reinterpret_cast<ent::IComponent*>(c.instance)->isEnabled;
        enabled += on;
        std::string piece = n + (on ? "" : " (off)");
        if (CProperty* p = Prop(c.instance->GetType(), "meshAppearance"); p && std::string(TypeName(p->type)) == "CName")
            piece += " [" + std::string(reinterpret_cast<CName*>(reinterpret_cast<uint8_t*>(c.instance) + p->valueOffset)->ToString()) + "]";
        // 0.25.4: the render scene layers (RenderSceneLayerMask, one byte), for every part.
        if (CProperty* p = Prop(c.instance->GetType(), "renderSceneLayerMask"); p && p->type->GetSize() == 1)
        {
            char b[16];
            snprintf(b, sizeof b, " L%X", *(reinterpret_cast<uint8_t*>(c.instance) + p->valueOffset));
            piece += b;
        }
        // 0.25.5: the engine's own visibility fields (Visibility.hpp): camera target, hide reasons, the second bit set.
        if (vis::Flags f; vis::Read(c.instance, f))
        {
            char b[48];
            snprintf(b, sizeof b, " cam%u hide%04X vis%02X", f.cam, f.hide, f.vis);
            piece += b;
        }
        if (CProperty* p = Prop(c.instance->GetType(), "chunkMask"); p && std::string(TypeName(p->type)) == "Uint64")
        {
            const uint64_t mask = *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(c.instance) + p->valueOffset);
            if (mask != ~0ull)
            {
                char b[32];
                snprintf(b, sizeof b, " mask %llX", static_cast<unsigned long long>(mask));
                piece += b;
            }
        }
        parts.push_back(piece);
    }
    std::string line;
    int chunk = 1;
    Logf(log, "stage C4: head parts on %s, %s: %u found, %u enabled", who, when, found, enabled);
    for (size_t i = 0; i < parts.size(); ++i)
    {
        line += parts[i] + (i + 1 < parts.size() ? "; " : "");
        if (line.size() > 900 || i + 1 == parts.size())
        {
            Logf(log, "stage C4:   %s (%d): %s", who, chunk++, line.c_str());
            line.clear();
        }
    }
}

void DumpHeadParts(const Handle<IScriptable>& player, const char* when, LogFn log)
{
    DumpParts(player.instance, "V", when, log);
    CClass* tc = Cls("gameTransactionSystem");
    CBaseFunction* inSlot = Func(tc, "GetItemInSlot");
    IScriptable* ts = nullptr;
    if (!SelfFor(tc, inSlot, ts)) return;
    TweakDBID slot(kHeadSlot);
    Handle<IScriptable> item;
    Call(ts, inSlot, &item, {const_cast<Handle<IScriptable>*>(&player), &slot});
    DumpParts(item.instance, "the TppHead item", when, log);
}

// ----- P2-10 (0.25.7): V's head shielded from the engine's hide (Visibility.hpp) -----
// 0.25.5: V's own face parts lose hide reason 8 when the third-person representation comes up; the parts of the item in
// TppHead (hair, earrings, makeup, a cyberware tattoo) keep it and gain +0x12C bit 0, and the engine set both again within
// 0.25 s each time we cleared them (38 times in 10 s). 0.25.6: taking TppHead out of the representation's attach and
// detach lists stopped the exchange that shows her face, and the hair was hidden all the same. TFPC 9 hooks the two game
// setters and, while its shield is up, ignores setting reason 8 or bit 0 on its head parts; this does the same. Every
// frame of step 2: V's head parts (by name, not shadow meshes, not body or nails) are handed to the shield, and any reason
// 8 or bit 0 already set on them is cleared through the game's setters. Off on undo; the engine then hides the first-person
// head again by itself (it re-sets these bits continuously, as the tests showed).

// ----- P2-10 (0.25.8): hats and glasses -----
// The twentieth test: head, hair and makeup show; sunglasses and a hat do not (the hat still hid the hair, as its
// hide_Hair tag should). The twenty-first showed why: the engine hides them as it hid the hair, with reason 8 and bit 0,
// and ShieldStep now shields any of V's parts it finds hidden that way, except first-person pieces and shadow meshes.
// (0.25.8 also switched Head and Eyes items from their "&FPP" to their "&TPP" appearance, as JB Third Person Mod does; it
// never changed anything and is gone in 0.26.0.)

// Every visual part of V's that is hidden by a reason bit right now, with its mesh appearance.
void DumpHiddenParts(IScriptable* v, const char* when, LogFn log)
{
    if (!v || !vis::Available()) return;
    std::string line;
    uint32_t n = 0;
    for (auto& c : reinterpret_cast<ent::Entity*>(v)->components)
    {
        vis::Flags f;
        if (!c.instance || !vis::Read(c.instance, f) || (!f.hide && !f.vis)) continue;
        ++n;
        char b[48];
        snprintf(b, sizeof b, " hide%04X vis%02X%s", f.hide, f.vis, c->isEnabled ? "" : " (off)");
        std::string piece = c->name.ToString();
        if (CProperty* p = Prop(c.instance->GetType(), "meshAppearance"); p && std::string(TypeName(p->type)) == "CName")
            piece += " [" + std::string(reinterpret_cast<CName*>(reinterpret_cast<uint8_t*>(c.instance) + p->valueOffset)->ToString()) + "]";
        line += (line.empty() ? "" : "; ") + piece + b;
        if (line.size() > 900) Logf(log, "stage C4:   hidden: %s", line.c_str()), line.clear();
    }
    Logf(log, "stage C4: V's parts hidden by a reason bit, %s: %u", when, n);
    if (!line.empty()) Logf(log, "stage C4:   hidden: %s", line.c_str());
}

bool ShieldPart(IScriptable* c, const std::string& n)
{
    return vis::IsVisual(c) && HeadPart(c, n) && n.find("shadow") == std::string::npos && n.rfind("t0_", 0) != 0 &&
           n.rfind("a0_", 0) != 0;
}

void ShieldStep(LogFn log)
{
    if (g_vx.step != 2) return;
    IScriptable* v = g.playerHandle.instance;
    if (!v || reinterpret_cast<uintptr_t>(v) != g.player) return;
    const double now = NowMs();
    if (!vis::ShieldInstalled())
    {
        if (!g_vx.shieldReported) log("stage C4: head shield: not installed (see the load line); V's head left to the engine");
        g_vx.shieldReported = true;
        return;
    }
    IScriptable* parts[64];
    uint32_t n = 0, hair = 0, cleared = 0, hidden = 0;
    uint16_t otherHide = 0;
    uint8_t otherVis = 0;
    for (auto& c : reinterpret_cast<ent::Entity*>(v)->components)
    {
        if (!c.instance || n == 64 || !vis::IsVisual(c.instance)) continue;
        const std::string name = c->name.ToString();
        const auto at = reinterpret_cast<uintptr_t>(c.instance);
        if (!ShieldPart(c.instance, name))
        {
            // 0.25.8: hidden the hair's way (reason 8 or bit 0), and not a first-person piece or a shadow mesh.
            const bool known = std::find(g_vx.extraParts.begin(), g_vx.extraParts.end(), at) != g_vx.extraParts.end();
            if (!known)
            {
                std::string lower = name;
                for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                vis::Flags f0;
                if (lower.find("fpp") != std::string::npos || lower.find("shadow") != std::string::npos || !vis::Read(c.instance, f0) ||
                    !((f0.hide & (1u << 8)) || (f0.vis & 1u)))
                    continue;
                g_vx.extraParts.push_back(at);
                Logf(log, "stage C4: head shield: also shielding %s (hide %04X, vis %02X)", name.c_str(), f0.hide, f0.vis);
            }
        }
        parts[n++] = c.instance;
        hair += HairPart(name);
        vis::Flags f;
        if (!vis::Read(c.instance, f)) continue;
        if (f.hide & (1u << 8)) vis::SetHide(c.instance, false, 8), ++cleared;
        if (f.vis & 1u) vis::SetVis(c.instance, false, 0), ++cleared;
        vis::Flags after;
        vis::Read(c.instance, after);
        if (after.hide || after.vis) ++hidden, otherHide |= after.hide, otherVis |= after.vis;
    }
    vis::SetShield(parts, n);
    g_vx.shieldCleared += cleared;

    if (!g_vx.shieldReported || n != g_vx.shieldParts)
        Logf(log, "stage C4: head shield on: %u head parts (%u hair); %u bits cleared now", n, hair, cleared);
    g_vx.shieldReported = true, g_vx.shieldParts = n;
    if (now >= g_vx.shieldLogAt)
    {
        g_vx.shieldLogAt = now + 10000;
        uint32_t bh = 0, bv = 0;
        vis::ShieldBlocked(bh, bv);
        Logf(log, "stage C4: head shield: %u parts; engine requests ignored so far: reason 8 %u, bit 0 %u; bits we had to clear "
                  "since step 2: %u; parts still hidden for another reason: %u (hide %04X, vis %02X)",
             n, bh, bv, g_vx.shieldCleared, hidden, otherHide, otherVis);
    }
}

// Undo: the shield down; the engine is free to hide V's first-person head again.
void ShieldEnd(LogFn log, const char* why)
{
    if (!vis::ShieldOn()) return;
    vis::ShieldOff();
    uint32_t bh = 0, bv = 0;
    vis::ShieldBlocked(bh, bv);
    Logf(log, "stage C4: head shield off (%s); engine requests it ignored in all: reason 8 %u, bit 0 %u", why, bh, bv);
}

} // namespace

// P2-10 (0.25.2): the camera the game renders from, by CameraSystem (GetActiveCameraFOV, GetActiveCameraWorldTransform).
bool ActiveCamera(float& fov, V3& pos)
{
    CClass* c = Cls("gameCameraSystem");
    CBaseFunction* getFov = Func(c, "GetActiveCameraFOV");
    CBaseFunction* getTr = Func(c, "GetActiveCameraWorldTransform");
    IScriptable* self = nullptr;
    if (!SelfFor(c, getFov, self) || !Call(self, getFov, &fov, {})) return false;
    Transform t{};
    bool ok = false;
    pos = {NAN, NAN, NAN};
    if (getTr && Call(self, getTr, &ok, {&t}) && ok) pos = {t.position.X, t.position.Y, t.position.Z};
    return std::isfinite(fov);
}

// A camera component placed in the world through its own parent binding (its world matrix and local transform give the
// parent), as the first-person camera is placed. For V's tppCamera.
bool PlaceCameraComponent(IScriptable* cam, const Pose& wanted)
{
    CClass* t = cam->GetType();
    Mat m{};
    Vector4 lp{};
    Quaternion lq{};
    if (!Call(cam, Func(t, "GetLocalToWorld"), &m, {}) || !Call(cam, Func(t, "GetLocalPosition"), &lp, {}) ||
        !Call(cam, Func(t, "GetLocalOrientation"), &lq, {}))
        return false;
    const Pose world = Decode(m, g.rows);
    Pose local;
    local.p = {lp.X, lp.Y, lp.Z}, local.q = {lq.i, lq.j, lq.k, lq.r};
    Pose parent;
    parent.q = Mul(world.q, Conj(local.q));
    parent.p = world.p - Rot(parent.q, local.p);
    Pose l;
    l.q = Mul(Conj(parent.q), wanted.q);
    l.p = Rot(Conj(parent.q), wanted.p - parent.p);
    if (!Finite(l.p) || Len(l.p) > 50.f) return false;
    Vector4 p{};
    p.X = l.p.x, p.Y = l.p.y, p.Z = l.p.z, p.W = 1.f;
    Quaternion q{};
    q.i = l.q.x, q.j = l.q.y, q.k = l.q.z, q.r = l.q.w;
    return Call(cam, Func(t, "SetLocalTransform"), nullptr, {&p, &q});
}

// Activates a camera component at high priority with the given field of view.
bool TakeView(IScriptable* cam, float fov)
{
    bool high = true;
    CBaseFunction* hp = Func(cam->GetType(), "SetIsHighPriority");
    if (hp && ParamIs(hp, 0, "Bool")) Call(cam, hp, nullptr, {&high});
    Call(cam, Func(cam->GetType(), "SetFOV"), nullptr, {&fov});
    return CamSwitch(cam, true);
}

// P2-10 (0.25.4): V's hair after step 2. The sixteenth test (0.25.3) showed her face with no hair, although the hair mesh
// was enabled with every chunk: the TPP representation hides it underneath (the ITP/TFPC Compatibility Fix calls it the
// engine's "internal representation-exclusion flag"). The ITP TFPC Bald V Fix (Nexus 37932) cures the same symptom by
// reloading the head once third person has settled: 0.35 s after the switch, the head item taken out of TppHead
// (destroy, keep transform, skip anim events), and the same item added back 0.1 s later once the slot is empty and not
// spawning; at most 2 s, once per switch. Here only our character-creator head is reloaded, once per step 2.
constexpr double kReloadAfterMs = 350, kReloadGapMs = 100, kReloadLimitMs = 2000, kReloadGiveUpMs = 5000;

void HeadReloadStep(LogFn log)
{
    using R = ViewExp::Reload;
    if (g_vx.step != 2 || g_vx.reload == R::Done || !g.activated) return;
    const Handle<IScriptable>& player = g.playerHandle;
    if (!player.instance || reinterpret_cast<uintptr_t>(player.instance) != g.player) return;
    const double now = NowMs();
    const Transactions t;
    CClass* tc = Cls("gameTransactionSystem");
    CBaseFunction* inSlot = Func(tc, "GetItemInSlot");
    CBaseFunction* spawning = Func(tc, "IsSlotSpawningAnyItem");
    if (!t.ok || !inSlot) return;
    auto* pl = const_cast<Handle<IScriptable>*>(&player);
    TweakDBID slot(kHeadSlot);
    Handle<IScriptable> item;
    Call(t.self, inSlot, &item, {pl, &slot});
    bool busy = false;
    if (spawning) Call(t.self, spawning, &busy, {pl, &slot});
    const uintptr_t at = reinterpret_cast<uintptr_t>(item.instance);

    if (g_vx.reload == R::Waiting)
    {
        if (now - g.tppAt > kReloadGiveUpMs)
        {
            Logf(log, "stage C4: head reload skipped: no settled character-creator head in TppHead %.0f s after step 2 (it holds %s)",
                 kReloadGiveUpMs / 1000, SlotReport(player, kHeadSlot).c_str());
            g_vx.reload = R::Done;
            return;
        }
        if (g_amm.dir != AmmHead::Dir::None || !g_amm.on || now - g.tppAt < kReloadAfterMs || !item.instance || busy) return;
        alignas(16) uint8_t id[16] = {};
        Call(item.instance, Func(item.instance->GetType(), "GetItemID"), id, {});
        TweakDBID rec;
        Call(nullptr, Func(Cls("gameItemID"), "GetTDBID"), &rec, {id});
        if (!g_amm.record || rec != TweakDBID(g_amm.record)) return;  // not ours (yet): wait, up to the give-up time
        DumpHeadParts(player, "before the reload", log);
        std::memcpy(g_vx.reloadId, id, 16);
        g_vx.removedItem = at;
        bool yes = true, r = false;
        Call(t.self, t.fromSlot, &r, {pl, &slot, &yes, &yes, &yes});
        g_vx.reload = R::Removed, g_vx.removedAt = now, g_vx.reloadExpire = now + kReloadLimitMs;
        Logf(log, "stage C4: head reload (the Bald V Fix's way), %.0f ms after step 2: %s removed from TppHead: %s", now - g.tppAt, g_amm.record,
             r ? "true" : "false");
        return;
    }
    // A new head in the slot (a different object; or, after our add, anything there 0.5 s on, in case the new object
    // reused the old address): done.
    if (item.instance && (at != g_vx.removedItem || (g_vx.reload == R::Added && now - g_vx.addedAt > 500)))
    {
        Logf(log, "stage C4: head reload done in %.0f ms: TppHead holds %s", now - g_vx.removedAt, SlotReport(player, kHeadSlot).c_str());
        g_vx.reload = R::Done;
        g_vx.dumpAt[0] = now + 1000;
        return;
    }
    if (now >= g_vx.reloadExpire)
    {
        Logf(log, "stage C4: head reload timed out after %.0f ms: TppHead holds %s", kReloadLimitMs, SlotReport(player, kHeadSlot).c_str());
        g_vx.reload = R::Done;
        g_vx.dumpAt[0] = now + 1000;
        return;
    }
    if (g_vx.reload == R::Removed && !item.instance && !busy && now - g_vx.removedAt >= kReloadGapMs)
    {
        bool has = false, ok = false;
        Call(t.self, t.has, &has, {pl, g_vx.reloadId});
        if (has) Call(t.self, t.add, &ok, {pl, &slot, g_vx.reloadId});
        g_vx.reload = R::Added, g_vx.addedAt = now;
        Logf(log, "stage C4: head reload: %.0f ms after the removal the slot was empty: AddItemToSlot(the same %s) %s", now - g_vx.removedAt,
             g_amm.record, !has ? "skipped (no longer in V's inventory)" : ok ? "true" : "false");
    }
}

// P2-10: our camera while the head experiment wants it: placed where V's camera was just placed, 1 degree wider, made
// the active camera, and checked against the camera the game renders from.
void HeadAuto(const Handle<IScriptable>& player, LogFn log);  // 0.26.0, below

void ViewFrame(LogFn log)
{
    HeadAuto(g.playerHandle, log);  // 0.26.0: step 1, then step 2, by themselves while skating
    const bool want = g_vx.step > 0 && g.mode != Mode::Off && g.camera.instance;
    const bool entityWanted = want && !g_vx.entityGaveUp;
    board::CamFrame(entityWanted, entityWanted ? &g.wanted.p.x : nullptr, entityWanted ? &g.wanted.q.x : nullptr, log);
    if (!want) return;
    const double now = NowMs();
    float fov = 0;
    Call(g.camera.instance, Func(g.camera.instance->GetType(), "GetFOV"), &fov, {});
    const float ours = fov + 1.f;
    float afov = 0;
    V3 apos{0, 0, 0};
    const bool haveActive = ActiveCamera(afov, apos);
    if (!g_vx.haveBaseline && haveActive && g_vx.which == ViewExp::Cam::None)
    {
        g_vx.baselineFov = afov, g_vx.haveBaseline = true;
        Logf(log, "stage C4: head experiment: before ours, the game renders at field of view %.2f (V's camera reports %.2f)", afov, fov);
    }
    IScriptable* ent = board::CamComponent();
    if (g_vx.which == ViewExp::Cam::None && ent && !g_vx.entityGaveUp && fov > 1.f)
    {
        const bool ok = TakeView(ent, ours);
        g_vx.which = ok ? ViewExp::Cam::Entity : ViewExp::Cam::None, g_vx.activatedAt = now, g_vx.notOursSince = 0;
        Logf(log, "stage C4: head experiment: our camera activated at field of view %.1f: %s", ours, ok ? "Activate called" : "FAILED");
        if (!ok) g_vx.entityGaveUp = true;
    }
    IScriptable* cam = g_vx.which == ViewExp::Cam::Tpp ? g_vx.tpp.instance : g_vx.which == ViewExp::Cam::Entity ? ent : nullptr;
    if (cam && fov > 1.f) Call(cam, Func(cam->GetType(), "SetFOV"), nullptr, {const_cast<float*>(&ours)});
    if (g_vx.which == ViewExp::Cam::Tpp && cam) PlaceCameraComponent(cam, g.wanted);
    // Which camera the game renders from: ours is the one 1 degree wider than V's was.
    const bool oursActive = haveActive && g_vx.haveBaseline && std::fabs(afov - g_vx.baselineFov) > 0.3f;
    g_vx.oursActive = oursActive && g_vx.which != ViewExp::Cam::None;
    const char* who = !haveActive ? "an unknown camera"
                      : oursActive ? (g_vx.which == ViewExp::Cam::Tpp ? "V's third-person camera (tppCamera)" : "our camera")
                                   : "V's first-person camera";
    if (std::strcmp(who, g_vx.lastWho) != 0)
    {
        Logf(log, "stage C4: head experiment: the game now renders from %s (active field of view %.2f, %.2f before ours; the active camera is "
                  "%.2f m from where the skating camera should be)",
             who, afov, g_vx.baselineFov, std::isfinite(apos.x) ? Len(apos - g.wanted.p) : -1.f);
        g_vx.lastWho = who;
    }
    g_vx.notOursSince = g_vx.which != ViewExp::Cam::None && !oursActive ? (g_vx.notOursSince > 0 ? g_vx.notOursSince : now) : 0;
    const bool stuck = g_vx.which != ViewExp::Cam::None && now - g_vx.activatedAt > 1000 && g_vx.notOursSince > 0 && now - g_vx.notOursSince > 500;
    if (stuck && g_vx.which == ViewExp::Cam::Entity)
    {
        CamSwitch(ent, false);
        g_vx.entityGaveUp = true, g_vx.which = ViewExp::Cam::None;
        Handle<IScriptable> comp;
        CName name("tppCamera");
        IScriptable* v = g.playerHandle.instance;
        CBaseFunction* find = v ? Func(v->GetType(), "FindComponentByName") : nullptr;
        if (find) Call(v, find, &comp, {&name});
        CClass* cc = Cls("gameCameraComponent");
        if (comp.instance && cc && comp.instance->GetType()->IsA(cc))
        {
            g_vx.tpp = comp;
            PlaceCameraComponent(comp.instance, g.wanted);
            const bool ok = TakeView(comp.instance, ours);
            g_vx.which = ok ? ViewExp::Cam::Tpp : ViewExp::Cam::None, g_vx.activatedAt = now, g_vx.notOursSince = 0;
            if (!ok) g_vx.tppGaveUp = true;
            Logf(log, "stage C4: head experiment: our camera did not take the view in 1 s; trying V's own third-person camera (%s \"tppCamera\"): "
                      "Activate %s", comp.instance->GetType()->GetName().ToString(), ok ? "called" : "FAILED");
        }
        else
        {
            g_vx.tppGaveUp = true;
            CamSwitch(g.camera.instance, true);
            log("stage C4: head experiment: our camera did not take the view in 1 s, and V has no tppCamera camera component; back to her own camera");
        }
    }
    else if (stuck && g_vx.which == ViewExp::Cam::Tpp)
    {
        CamSwitch(g_vx.tpp.instance, false);
        CamSwitch(g.camera.instance, true);
        g_vx.tppGaveUp = true, g_vx.which = ViewExp::Cam::None;
        log("stage C4: head experiment: V's tppCamera did not take the view either; back to her own camera");
    }
    HeadReloadStep(log);
    ShieldStep(log);
    for (int k = 0; k < 2; ++k)
        if (g_vx.dumpAt[k] > 0 && now >= g_vx.dumpAt[k])
        {
            g_vx.dumpAt[k] = 0;
            DumpHeadParts(g.playerHandle, "1 s after the reload", log);
            DumpHiddenParts(g.playerHandle.instance, "1 s after the reload", log);  // 0.25.8
        }
    if (now >= g_vx.nextDiag)
    {
        g_vx.nextDiag = now + 10000;
        Logf(log, "stage C4: head experiment step %d: rendering from %s (active field of view %.2f, ours %.1f, V's %.1f); TPP representation %s; "
                  "TppHead holds %s",
             g_vx.step, who, afov, ours, fov, g.activated ? "on" : g.prepared ? "requested" : "off", SlotReport(g.playerHandle, kHeadSlot).c_str());
    }
}

void PoseFrame(const Handle<IScriptable>& player, LogFn log)
{
    if (!player.instance || (g_pose.player && reinterpret_cast<uintptr_t>(player.instance) != g_pose.player))
    {
        g_pose = PoseState{};  // a new player object: the old component is not touched again
        if (!player.instance) return;
    }
    BoardFrame(log);  // C3b
    CamFrame(log);    // C4: placed here, right after V's teleport, while it has any weight
    if (g.chase && g.camera.instance && g_cam.weight > 0 && player.instance) UpdateChase(player.instance, log);
    ViewFrame(log);  // P2-10
    const double now = NowMs();
    const float dt = g_pose.lastAt > 0 ? std::clamp(float(now - g_pose.lastAt) / 1000.f, 0.f, 0.1f) : 0.f;
    g_pose.lastAt = now;
    float target = 0;
    const char* source = "none";
    stageb::DkrPose r;
    // 0.11.1 (C3 run 1): a test started while waiting for the skater ends once it rides. 0.11.0 held the reference
    // pose until the key was pressed again, which looked as if the skater's pose had not arrived.
    if (g_pose.test && g_pose.testUntilRide && now - g_pose.drivenAt < 100 && !g_pose.holding && g_pose.haveMs)
    {
        g_pose.test = false;
        log("stage C3: pose test ended: the skater is riding, so V now takes its pose");
    }
    if (g_pose.test)
    {
        if (g.mode != Mode::Off && stageb::ReferencePose(r))
        {
            for (int i = 0; i < 24; ++i) g_pose.ms[i] = QOf(r.rot[i]);
            g_pose.hipsMs = {r.hips[0], r.hips[1], r.hips[2]};
            g_pose.haveMs = true;
            target = 1, source = "reference pose (test)";
        }
        else
        {
            log(g.mode == Mode::Off ? "stage C3: pose test off: third person is off"
                                    : "stage C3: pose test: the rig's reference pose is not available (start Stage B with Ctrl+Shift+F10; the runtime log says whether the rig fitted)");
            g_pose.test = false;
        }
    }
    else if (g.mode == Mode::Restricted && now - g_pose.drivenAt < 100 && !g_pose.holding && g_pose.haveMs) target = 1, source = "skater";
    const float step = dt / kPoseFadeS;
    g_pose.weight = target > g_pose.weight ? std::min(target, g_pose.weight + step) : std::max(target, g_pose.weight - step);
    if (g_pose.weight <= 0 && g_pose.sentWeight <= 0) return;  // nothing to do: the graph is at weight 0
    if (!PoseBind(player, log)) return;
    PoseSend(g_pose.weight);
    if (now >= g_pose.nextDiag)
    {
        g_pose.nextDiag = now + 2000;
        const Q& hq = g_pose.ms[7];
        Logf(log, "stage C3: weight %.2f (%s); hips in V's model space (%.2f, %.2f, %.2f); head (%.2f, %.2f, %.2f, %.2f); %u sends, %u failed; "
                  "%u frames interpolated between ticks; unreachable limbs %x",
             g_pose.weight, source, g_pose.hipsMs.x, g_pose.hipsMs.y, g_pose.hipsMs.z, hq.x, hq.y, hq.z, hq.w, g_pose.sends, g_pose.failed,
             g_pose.interpolated, g_pose.cur.unreachable);
    }
}

void PoseTest(const Handle<IScriptable>& player, LogFn log)
{
    if (g.mode == Mode::Off || !player.instance || reinterpret_cast<uintptr_t>(player.instance) != g.player)
    {
        log("stage C3: pose test: turn third person on first (Ctrl+Shift+F7)");
        return;
    }
    const uintptr_t id = reinterpret_cast<uintptr_t>(player.instance);
    if (g_pose.player != id) g_pose = PoseState{}, g_pose.player = id;
    g_pose.test = !g_pose.test;
    g_pose.testUntilRide = g_pose.test && NowMs() - g_pose.drivenAt >= 100;  // not riding yet: the test ends when it does
    log(!g_pose.test ? "stage C3: pose test OFF"
        : g_pose.testUntilRide ? "stage C3: pose test ON until the skater rides: V should take her rig's reference pose (arms out, a T or A shape). "
                                 "Ctrl+Shift+F11 again ends it sooner."
                               : "stage C3: pose test ON: V should take her rig's reference pose (arms out, a T or A shape). Ctrl+Shift+F11 again: back to the skater's pose.");
}


// ----- P2-10 (0.26.0): V's head, part of skating -----
// Twenty-first and twenty-second tests (0.25.8): head, hair, makeup, a hat and sunglasses render; cycled off and on, with
// headwear put on or taken off in between, and across both kinds of session-marker return. So the two steps the
// experiment ran on Ctrl+Shift+F2 now run by themselves while skating: step 1 (our camera) at once, step 2 (the head)
// once our camera has the view and the skating camera has finished moving out from V's eyes, so no frame looks out
// from inside her head (the ITP/TFPC Compatibility Fix guards the same moment). The exit undoes both, as before.
// Ctrl+Shift+F2 turns the head off for the rest of the ride, and on again.
void HeadStep1(LogFn log)
{
    g_vx = ViewExp{};
    g_vx.step = 1, g_vx.step1At = NowMs();
    Logf(log, "stage C4: head: our camera on (the engine's scene camera where the skating camera is, 1 degree wider); V's head follows "
              "once it has the view. Hide functions %s; head shield hooks %s",
         vis::Available() ? "found" : "NOT found", vis::ShieldInstalled() ? "attached" : "NOT attached");
}

void HeadStep2(const Handle<IScriptable>& player, LogFn log)
{
    g_vx.step = 2;
    // 0.25.3: Immersive Third Person's order (its init.lua): the TPPRepresentation animation feature on V, the
    // representation event (Activate only), and at once the head re-equipped in TppHead. The head is the
    // character-creator one: PlayerWaTppHead with appearance character_customization, as the ITP/TFPC Compatibility
    // Fix's records are, which keeps the face parts that the game's resolved skin appearance lost (0.25.2). Then the
    // Bald V Fix's reload (HeadReloadStep) and the head shield (ShieldStep) follow by themselves.
    g_vx.featureOn = ApplyTppFeature(player, true, log);
    const bool ev = QueueEvent(player.instance, "gameActivateTPPRepresentationEvent", log);
    g.prepared = g.activated = ev, g.settled = false, g.tppAt = NowMs();
    AmmStart(player, true, log);
    Logf(log, "stage C4: head: on %.0f ms after our camera (animation feature %s, ActivateTPPRepresentationEvent %s, the character-creator "
              "head swapped into TppHead; then the reload and the head shield)",
         g.tppAt - g_vx.step1At, g_vx.featureOn ? "on" : "NOT applied", ev ? "queued" : "FAILED");
}

void HeadAuto(const Handle<IScriptable>& player, LogFn log)
{
    if (g.mode != Mode::Restricted || g_headOff || !g.camera.instance || !player.instance ||
        reinterpret_cast<uintptr_t>(player.instance) != g.player)
        return;
    if (g_vx.step == 0) { HeadStep1(log); return; }
    if (g_vx.step != 1) return;
    if (g_vx.which == ViewExp::Cam::None)
    {
        if (g_vx.entityGaveUp && g_vx.tppGaveUp && !g_vx.noCamReported)
            log("stage C4: head: no camera of ours took the view; V's head stays off this ride");
        g_vx.noCamReported = g_vx.entityGaveUp && g_vx.tppGaveUp;
        return;
    }
    if (!g_vx.oursActive || g_cam.weight < 0.999f || NowMs() - g_vx.step1At < 300) return;
    HeadStep2(player, log);
}

void HeadToggle(const Handle<IScriptable>& player, LogFn log)
{
    if (g.mode != Mode::Restricted || !player.instance || reinterpret_cast<uintptr_t>(player.instance) != g.player)
    {
        log("stage C4: head: start skating first (Ctrl+Shift+F10)");
        return;
    }
    g_headOff = !g_headOff;
    if (g_headOff)
    {
        ViewExpUndo(player, log, "Ctrl+Shift+F2");
        log("stage C4: head OFF for this ride: V's own camera and first-person body again. Ctrl+Shift+F2 turns it back on.");
    }
    else log("stage C4: head back ON (our camera first, then the head)");
}

bool g_gamePaused = false;  // GameClock's last reading
bool GamePaused() { return g_gamePaused; }

float GameClock(float wallDt, LogFn log)
{
    static CClass* tc = nullptr;
    static CBaseFunction *simTime = nullptr, *paused = nullptr;
    static IScriptable* sys = nullptr;
    static uint64_t last = 0;
    static bool reported = false, wasPaused = false;
    static double freq = 0;
    if (!tc)
    {
        tc = Cls("gameTimeSystem");
        simTime = Func(tc, "GetSimTime");
        paused = Func(tc, "IsPausedState");
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        freq = double(f.QuadPart);
    }
    // The system object can change with the world (a loaded save): look it up every frame, as SelfFor does.
    if (!SelfFor(tc, simTime, sys) || !sys)
    {
        if (!reported) reported = true, log("stage B: the game's time system is not readable; Skate steps on wall-clock time");
        return wallDt;
    }
    uint64_t ticks = 0;  // EngineTime: simulation time in performance-counter ticks
    if (!Call(sys, simTime, &ticks, {})) return wallDt;
    // Only called while a ride runs: a reading from the last ride would hand the first frame of the next one the whole
    // gap (capped at 0.25 s, so 15 Skate ticks in one frame at each restart).
    static double lastCall = 0;
    const double nowMs = NowMs();
    if (nowMs - lastCall > 500) last = 0;
    lastCall = nowMs;
    bool isPaused = false;
    if (paused) Call(sys, paused, &isPaused, {});
    if (!reported) reported = true, log("stage B: Skate steps on the game's simulation time (TimeSystem.GetSimTime), and stops while the game is paused");
    float dt = last && ticks >= last ? float(double(ticks - last) / freq) : wallDt;
    if (last && ticks < last) dt = 0;  // time went back (a loaded save): no step this frame
    // P1-4: IsPausedState never reported the pause menu in the first in-game ride; Skate held only because simulation
    // time stopped, and the Skate sound kept playing. So the pause is also simulation time standing still for 0.1 s of
    // frames. (Slow motion still advances it.)
    static double still = 0;
    still = last && ticks == last ? still + wallDt : 0;
    last = ticks;
    const bool stopped = still >= 0.1;
    if (isPaused) dt = 0;
    const bool nowPaused = isPaused || stopped;
    g_gamePaused = nowPaused;
    if (nowPaused != wasPaused)
        Logf(log, "stage B: game %s", nowPaused ? (isPaused ? "paused (IsPausedState): Skate holds" : "paused (simulation time stopped): Skate holds")
                                                : "unpaused: Skate resumes"),
            wasPaused = nowPaused;
    return std::min(dt, 0.25f);
}

std::string RigFor(const Handle<IScriptable>& player, LogFn log)
{
    CName gender;
    const bool known = player.instance && Call(player.instance, Func(player.instance->GetType(), "GetResolvedGenderName"), &gender, {});
    const bool female = !known || gender == CName("Female");
    const std::string path = data::Utf8(data::Rig(female));  // R1: from the data folder (Data.hpp)
    Logf(log, "stage C3: V's gender %s: rig %s", known ? gender.ToString() : "unknown", path.c_str());
    return path;
}

void Frame(const Handle<IScriptable>& player, LogFn log)
{
    g_mainThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (g.mode != Mode::Restricted) StepCleanup(player, log);
    HeadStep(player, log);
    AmmStep(player, log);  // P2-10: runs on after an exit until V's own head is back
    if (g.mode == Mode::Off) return;
    if (!player.instance || reinterpret_cast<uintptr_t>(player.instance) != g.player)
    {
        ExitAll(player, log, "V replaced or unavailable");
        return;
    }
    ++g.frames;
    const double now = NowMs();
    if (g_tagCheckAt > 0 && now >= g_tagCheckAt && g.mode == Mode::Restricted) g_tagCheckAt = 0, LogBlockedTags(player, log);
    if (g.prepared && !g.activated && now - g.tppAt > 500) g.activated = QueueEvent(player.instance, "gameActivateTPPRepresentationEvent", log);
    if (g.activated && !g.settled && now - g.tppAt > 1500)
    {
        g.settled = true;
        Settle(player, log);
    }
    if (g.chase && g.camera.instance && g_cam.weight <= 0) UpdateChase(player.instance, log);  // else PoseFrame places it (C4)
    if (g_hx.step > 0 && g_hx.checkAt > 0 && NowMs() >= g_hx.checkAt)
    {
        g_hx.checkAt = 0;
        Logf(log, "stage C4: head experiment step %d, a moment later: TppHead holds %s%s%s", g_hx.step, SlotReport(player, kHeadSlot).c_str(),
             g_hx.slot ? "; " : "", g_hx.slot ? (std::string(g_hx.slot) + " holds " + SlotReport(player, g_hx.slot)).c_str() : "");
    }
    if (g.frames % 120 == 0) Snapshot(player, log, g.mode == Mode::Restricted ? "restricted" : "third person", false);
}
// ----- #3: the skateboard in the weapon wheel (0.32.0) -----
// The item is Items.Deckrunner_Skateboard, a copy of the knuckles defined by r6\tweaks\deckrunner\skateboard.yaml
// (TweakXL; its name comes from ArchiveXL). Without them there is no item, and the key works as before. The game's rules,
// from its scripts (tools\redmod\scripts): the wheel opens unless V has the NoRadialMenus (or NoWeaponWheel) tag
// (QuickSlotsTransitions), and refuses a pick under NoCombat (RadialWheelController.DrawItem). So on foot only
// NoRadialMenus comes off, and wheel.reds takes the pick from the wheel's DrawItem before that refusal and hands it to
// the plugin (DeckrunnerHud.WheelPick). NoCombat stays on, which keeps V's hands empty (UpperBodyTransition's
// ForceEmptyHands) and a tap of Y silent: 0.32.0 lifted NoCombat too, and every tap then showed "Action Blocked"
// (NoWeapons). Riding, all of the skating restrictions are on, as before.
namespace
{
constexpr const char* kBoardItem = "Items.Deckrunner_Skateboard";
constexpr const char* kBoardTag = "Deckrunner";  // in the item's tags (skateboard.yaml)
constexpr const char* kOnFootOff = "GameplayRestriction.NoRadialMenus";
uintptr_t g_boardChecked = 0;  // the V object the skateboard was last given or found on

bool EnumValue(const char* enumName, const char* valueName, int64_t& out)
{
    auto* e = CRTTISystem::Get()->GetEnum(CName(enumName));
    if (!e) return false;
    const CName v(valueName);
    for (uint32_t i = 0; i < e->hashList.size() && i < e->valueList.size(); ++i)
        if (e->hashList[i] == v) { out = e->valueList[i]; return true; }
    return false;
}

Handle<IScriptable> EquipmentSys()
{
    Handle<IScriptable> sys;
    CClass* sc = Cls("gameScriptableSystemsContainer");
    CBaseFunction* get = Func(sc, "Get");
    IScriptable* container = nullptr;
    if (!SelfFor(sc, get, container) || !ParamIs(get, 0, "CName")) return sys;
    CName name("EquipmentSystem");
    Call(container, get, &sys, {&name});
    return sys;
}

bool ApplyEffect(const Handle<IScriptable>& player, const char* id)
{
    CClass* sc = Cls("gameStatusEffectSystem");
    CBaseFunction* apply = Func(sc, "ApplyStatusEffect");
    IScriptable* self = nullptr;
    alignas(16) uint8_t eid[16] = {};
    if (!SelfFor(sc, apply, self) || !ParamIs(apply, 0, "entEntityID") || !ParamIs(apply, 1, "TweakDBID") || !EntityIdOf(player.instance, eid))
        return false;
    TweakDBID tdb(id);
    uint32_t one = 1;
    bool savable = false, result = false;
    Call(self, apply, &result, {eid, &tdb, nullptr, nullptr, ParamIs(apply, 4, "Uint32") ? &one : nullptr, nullptr,
                                ParamIs(apply, 6, "Bool") ? &savable : nullptr});
    return result;
}

TweakDBID RecordOf(const uint8_t (&id)[16])
{
    TweakDBID rec;
    if (CBaseFunction* getTdb = Func(Cls("gameItemID"), "GetTDBID")) Call(nullptr, getTdb, &rec, {const_cast<uint8_t*>(id)});
    return rec;
}
} // namespace

bool BoardItemDefined() { return RecordExists(kBoardItem); }

void EnsureBoardItem(const Handle<IScriptable>& player, LogFn log)
{
    if (!player.instance || reinterpret_cast<uintptr_t>(player.instance) == g_boardChecked) return;
    g_boardChecked = reinterpret_cast<uintptr_t>(player.instance);
    if (!BoardItemDefined())
    {
        log("weapon wheel: Items.Deckrunner_Skateboard isn't defined (TweakXL and ArchiveXL aren't installed, or Deckrunner's "
            "r6\\tweaks file is missing), so there's no skateboard item; the skate key works as before");
        return;
    }
    CClass* tc = Cls("gameTransactionSystem");
    CBaseFunction *count = Func(tc, "GetItemQuantityByTag"), *give = Func(tc, "GiveItemByTDBID");
    IScriptable* ts = nullptr;
    if (!SelfFor(tc, give, ts) || !count) { log("weapon wheel: the transaction system is unavailable; no skateboard given"); return; }
    auto* pl = const_cast<Handle<IScriptable>*>(&player);
    CName tag(kBoardTag);
    int32_t have = 0;
    Call(ts, count, &have, {pl, &tag});
    if (have > 0) { Logf(log, "weapon wheel: V has the skateboard (%d)", have); return; }
    TweakDBID rec(kBoardItem);
    int32_t one = 1;
    bool given = false;
    Call(ts, give, &given, {pl, &rec, &one});
    Logf(log, "weapon wheel: skateboard %s; equip it in a weapon slot (Inventory, Weapons) to have it in the weapon wheel",
         given ? "given to V" : "could NOT be given");
}

Held InRightHand(const Handle<IScriptable>& player)
{
    CClass* tc = Cls("gameTransactionSystem");
    CBaseFunction* inSlot = Func(tc, "GetItemInSlot");
    IScriptable* ts = nullptr;
    if (!player.instance || !SelfFor(tc, inSlot, ts)) return Held::Nothing;
    TweakDBID slot("AttachmentSlots.WeaponRight");
    Handle<IScriptable> item;
    Call(ts, inSlot, &item, {const_cast<Handle<IScriptable>*>(&player), &slot});
    if (!item.instance) return Held::Nothing;
    alignas(16) uint8_t id[16] = {};
    if (!Call(item.instance, Func(item.instance->GetType(), "GetItemID"), id, {})) return Held::Nothing;
    return RecordOf(id) == TweakDBID(kBoardItem) ? Held::Board : Held::Other;
}

bool ActiveWeapon(const Handle<IScriptable>& player, ItemId& out)
{
    out = ItemId{};
    Handle<IScriptable> sys = EquipmentSys();
    int64_t weapon = 0;
    if (!player.instance || !sys.instance || !EnumValue("gamedataEquipmentArea", "Weapon", weapon)) return false;
    CBaseFunction* active = Func(sys.instance->GetType(), "GetActiveItem");
    if (!active || !ParamIs(active, 1, "gamedataEquipmentArea")) return false;
    alignas(16) uint8_t id[16] = {};
    bool left = false;
    if (!Call(sys.instance, active, id, {const_cast<Handle<IScriptable>*>(&player), &weapon, &left})) return false;
    std::memcpy(out.bytes, id, sizeof id);
    const TweakDBID rec = RecordOf(id);
    out.board = rec == TweakDBID(kBoardItem);
    out.valid = rec.value != 0;
    return true;
}

bool DrawWeapon(const Handle<IScriptable>& player, const ItemId& item, LogFn log)
{
    CClass* rc = Cls("DrawItemRequest");
    Handle<IScriptable> sys = EquipmentSys();
    CProperty* owner = rc ? Prop(rc, "owner") : nullptr;
    CProperty* itemId = rc ? Prop(rc, "itemID") : nullptr;
    CBaseFunction* queue = sys.instance ? Func(sys.instance->GetType(), "QueueRequest") : nullptr;
    if (!player.instance || !item.valid || !owner || !itemId || !queue)
    {
        log("weapon wheel: can't ask the game to draw the picked weapon (DrawItemRequest unavailable)");
        return false;
    }
    Handle<IScriptable> req(static_cast<IScriptable*>(rc->CreateInstance(true)));
    auto* base = reinterpret_cast<uint8_t*>(req.instance);
    const std::string type = TypeName(owner->type);
    if (type.rfind("whandle:", 0) == 0)
    {
        WeakHandle<IScriptable> w(player);
        owner->type->Assign(base + owner->valueOffset, &w);
    }
    else if (type.rfind("handle:", 0) == 0) owner->type->Assign(base + owner->valueOffset, &player);
    else return false;
    itemId->type->Assign(base + itemId->valueOffset, const_cast<uint8_t*>(item.bytes));
    const bool ok = Call(sys.instance, queue, nullptr, {&req});
    Logf(log, "weapon wheel: drawing the weapon picked in the wheel: %s", ok ? "requested" : "FAILED");
    return ok;
}

void SetOnFoot(const Handle<IScriptable>& player, bool onFoot, LogFn log)
{
    if (g.mode != Mode::Restricted || !player.instance || onFoot == g_wheelOnFoot) return;
    g_wheelOnFoot = onFoot;
    const bool ok = onFoot ? RemoveEffect(player, kOnFootOff) : ApplyEffect(player, kOnFootOff);
    Logf(log, onFoot ? "weapon wheel: on foot: a held Y opens the wheel (NoRadialMenus removed%s)"
                     : "weapon wheel: riding: the wheel is off again (NoRadialMenus applied%s)", ok ? "" : ", not accepted");
    g_ours.appliedAt = NowMs();  // RestrictionsLost waits for them to land, as after entry
}

ItemId ItemFromBytes(const uint8_t (&bytes)[16])
{
    ItemId out;
    std::memcpy(out.bytes, bytes, sizeof bytes);
    const TweakDBID rec = RecordOf(out.bytes);
    out.board = rec == TweakDBID(kBoardItem);
    out.valid = rec.value != 0;
    return out;
}

bool WheelScriptPresent()
{
    static bool present = false;  // kept once found (the scripts load once per game run), looked up again until then
    if (!present) present = Cls("DeckrunnerWheel") != nullptr;
    return present;
}

bool OnFootRestrictions() { return g.mode == Mode::Restricted && g_wheelOnFoot; }

int RadialWheelOpen()
{
    // RadialWheelController sets the quest fact radial_wheel_open to 1 when the wheel opens and to 0 when it shuts.
    static CClass* qc = Cls("QuestsSystem");
    static CBaseFunction* get = Func(qc, "GetFact");
    IScriptable* self = nullptr;
    if (!SelfFor(qc, get, self) || !ParamIs(get, 0, "CName")) return -1;
    CName fact("radial_wheel_open");
    int32_t v = 0;
    return Call(self, get, &v, {&fact}) ? v : -1;
}
} // namespace stagec
