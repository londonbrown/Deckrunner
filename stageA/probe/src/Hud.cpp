#include "Hud.hpp"

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/RTTITypes.hpp>
#include <RED4ext/Scripting/IScriptable.hpp>
#include <RED4ext/Scripting/Utils.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace hud
{
namespace
{
using namespace RED4ext;

std::mutex g_lock;  // the game may call the natives from a script thread; Set runs on the main thread
View g_view;
bool g_skating = false;
bool g_picked = false;
ItemID g_pick{};
std::atomic<uint32_t> g_reads{0};
LogFn g_log = nullptr;

// The game's objects here are never destroyed at DLL unload: by then RED4ext has unloaded the other plugins, and the
// HUD script's service is an instance of Codeware's ScriptableService. Dropping the last reference to it from a
// static's destructor called its destructor through Codeware's unloaded code and crashed the game on every exit
// (deckrunner.dll+0x57ac, a Handle's release, in the lab's dumps from 2026-11-19). So the service is held weakly (the
// game's service container owns it) and let go of as the game stops running (Release), and both live on the heap,
// never freed, so no destructor of theirs runs at unload.
// The HUD script's service (hud.reds), handed over by DeckrunnerHud.Attach.
WeakHandle<IScriptable>& g_service = *new WeakHandle<IScriptable>();

struct DeckrunnerHud : IScriptable
{
    CClass* GetNativeType();
};
TTypedClass<DeckrunnerHud>& g_class = *new TTypedClass<DeckrunnerHud>("DeckrunnerHud");
CClass* DeckrunnerHud::GetNativeType() { return &g_class; }

View Snapshot()
{
    std::lock_guard lock(g_lock);
    return g_view;
}

// One static native per value, read from the snapshot. The game passes the return slot; with no parameters the frame's
// code pointer only steps past ParamEnd.
template<class T, T (*Get)(const View&)>
void Native(IScriptable*, CStackFrame* frame, T* out, int64_t)
{
    ++frame->code;
    if (out) *out = Get(Snapshot());
}

template<CString (*Get)(const View&)>
void NativeText(IScriptable*, CStackFrame* frame, CString* out, int64_t)
{
    ++frame->code;
    if (out) *out = Get(Snapshot());
}

void Visible(IScriptable*, CStackFrame* frame, bool* out, int64_t)
{
    ++frame->code;
    ++g_reads;
    if (out) *out = Snapshot().visible;
}

void Skating(IScriptable*, CStackFrame* frame, bool* out, int64_t)
{
    ++frame->code;
    std::lock_guard lock(g_lock);
    if (out) *out = g_skating;
}

void WheelPick(IScriptable*, CStackFrame* frame, void*, int64_t)
{
    ItemID item{};
    GetParameter(frame, &item);
    ++frame->code;
    std::lock_guard lock(g_lock);
    g_pick = item, g_picked = true;
}

void Attach(IScriptable*, CStackFrame* frame, void*, int64_t)
{
    Handle<IScriptable> service;
    GetParameter(frame, &service);
    ++frame->code;
    std::lock_guard lock(g_lock);
    g_service = service;
}

void Log(IScriptable*, CStackFrame* frame, void*, int64_t)
{
    CString text;
    GetParameter(frame, &text);
    ++frame->code;
    if (g_log) g_log(text.c_str());
}

template<class F>
CBaseFunction* Add(const char* name, F* fn, const char* returns)
{
    auto* f = CClassStaticFunction::Create(&g_class, name, name, fn);
    f->flags.isNative = true;
    f->flags.isStatic = true;
    if (returns) f->SetReturnType(returns);
    g_class.RegisterFunction(f);
    return f;
}

void RegisterTypes()
{
    CNamePool::Add("DeckrunnerHud");
    g_class.flags = {.isNative = true};
    CRTTISystem::Get()->RegisterType(&g_class);
}

void PostRegisterTypes()
{
    g_class.parent = CRTTISystem::Get()->GetClass("IScriptable");
    Add("Visible", &Visible, "Bool");
    Add("Skating", &Skating, "Bool");
    Add("OnFoot", &Native<bool, [](const View& v) { return v.onFoot; }>, "Bool");
    Add("Switch", &Native<bool, [](const View& v) { return v.switchStance; }>, "Bool");
    Add("TrickScore", &Native<int32_t, [](const View& v) { return v.trickScore; }>, "Int32");
    Add("LineScore", &Native<int32_t, [](const View& v) { return v.lineScore; }>, "Int32");
    Add("Multiplier", &Native<float, [](const View& v) { return v.multiplier; }>, "Float");
    Add("LineFraction", &Native<float, [](const View& v) { return v.lineFraction; }>, "Float");
    Add("LineSeconds", &Native<float, [](const View& v) { return v.lineSeconds; }>, "Float");
    Add("Landing", &Native<int32_t, [](const View& v) { return v.landing; }>, "Int32");
    Add("NewTricks", &Native<int32_t, [](const View& v) { return v.newTricks; }>, "Int32");
    Add("UpgradedTricks", &Native<int32_t, [](const View& v) { return v.upgradedTricks; }>, "Int32");
    Add("ClosedLines", &Native<int32_t, [](const View& v) { return v.closedLines; }>, "Int32");
    Add("Trick", &NativeText<[](const View& v) { return CString(v.trick.c_str()); }>, "String");
    Add("LandingText", &NativeText<[](const View& v) { return CString(v.landingText.c_str()); }>, "String");
    Add("Stance", &NativeText<[](const View& v) { return CString(v.stance.c_str()); }>, "String");
    Add("LineLabel", &NativeText<[](const View& v) { return CString(v.lineLabel.c_str()); }>, "String");
    Add("TrickStance", &NativeText<[](const View& v) { return CString(v.trickStance.c_str()); }>, "String");
    Add("MarkerProgress", &Native<float, [](const View& v) { return v.markerProgress; }>, "Float");
    Add("MarkersPlaced", &Native<int32_t, [](const View& v) { return v.markersPlaced; }>, "Int32");
    Add("MarkerReturns", &Native<int32_t, [](const View& v) { return v.markerReturns; }>, "Int32");
    Add("MarkerShortHolds", &Native<int32_t, [](const View& v) { return v.markerShortHolds; }>, "Int32");
    Add("MarkerPlacedText", &NativeText<[](const View& v) { return CString(v.markerPlacedText.c_str()); }>, "String");
    Add("MarkerReturnText", &NativeText<[](const View& v) { return CString(v.markerReturnText.c_str()); }>, "String");
    Add("HoldText", &NativeText<[](const View& v) { return CString(v.holdText.c_str()); }>, "String");
    Add("WheelPick", &WheelPick, nullptr)->AddParam("gameItemID", "item");
    Add("Log", &Log, nullptr)->AddParam("String", "text");
    Add("Attach", &Attach, nullptr)->AddParam("handle:IScriptable", "service");
}
} // namespace

void Set(const View& v)
{
    std::lock_guard lock(g_lock);
    g_view = v;
}

void SetSkating(bool skating)
{
    std::lock_guard lock(g_lock);
    g_skating = skating;
    if (!skating) g_picked = false;
}

bool TakePick(uint8_t (&item)[16])
{
    std::lock_guard lock(g_lock);
    if (!g_picked) return false;
    static_assert(sizeof(ItemID) == sizeof item);
    std::memcpy(item, &g_pick, sizeof item);
    g_picked = false;
    return true;
}

uint32_t Reads() { return g_reads.load(); }

namespace
{
// What the HUD shows changed: a refresh is due. The line's timer drains every tick, so only its refill (a trick added to
// the line) and its end count; the HUD script animates the drain itself. The marker's hold fills its bar as it runs.
bool Changed(const View& a, const View& b)
{
    return a.visible != b.visible || a.onFoot != b.onFoot || a.switchStance != b.switchStance || a.trickScore != b.trickScore ||
           a.lineScore != b.lineScore || a.multiplier != b.multiplier || a.landing != b.landing || a.newTricks != b.newTricks ||
           a.upgradedTricks != b.upgradedTricks || a.closedLines != b.closedLines || a.trick != b.trick ||
           a.landingText != b.landingText || a.stance != b.stance || a.lineLabel != b.lineLabel || a.trickStance != b.trickStance ||
           a.lineFraction > b.lineFraction + 0.02f || (a.lineFraction <= 0 && b.lineFraction > 0) ||
           a.markersPlaced != b.markersPlaced || a.markerReturns != b.markerReturns || a.markerShortHolds != b.markerShortHolds ||
           std::fabs(a.markerProgress - b.markerProgress) > 0.01f || (a.markerProgress > 0) != (b.markerProgress > 0);
}
} // namespace

void NotifyScript(LogFn log)
{
    static CBaseFunction* refresh = nullptr;
    static CClass* refreshOn = nullptr;
    static bool told = false;
    static View sent;
    static bool sentOnce = false;
    Handle<IScriptable> service;
    {
        std::lock_guard lock(g_lock);
        service = g_service.Lock();
    }
    if (!service.instance) return;
    CClass* type = service.instance->GetType();
    if (type != refreshOn)
    {
        refreshOn = type, refresh = nullptr, sentOnce = false;
        const CName name("Refresh");
        for (CClass* c = type; c && !refresh; c = c->parent)
            for (auto* f : c->funcs)
                if (f && f->shortName == name) { refresh = f; break; }
        if (!refresh)
        {
            std::string names;
            for (auto* f : type->funcs) if (f) names += std::string(" ") + f->shortName.ToString();
            char b[512];
            snprintf(b, sizeof b, "scripts: the HUD script (%s) has no Refresh; its functions:%s", type->GetName().ToString(), names.c_str());
            if (log) log(b);
        }
    }
    if (!refresh) return;
    const View now = Snapshot();
    if (sentOnce && !Changed(now, sent)) return;
    const bool ok = ExecuteFunction(service.instance, refresh, nullptr);
    sent = now, sentOnce = true;
    if (!told || !ok)
    {
        told = true;
        char b[256];
        snprintf(b, sizeof b, "scripts: %s.Refresh %s (called when what the HUD shows changes)", type->GetName().ToString(),
                 ok ? "called" : "FAILED");
        if (log) log(b);
    }
}

void Release()
{
    std::lock_guard lock(g_lock);
    g_service.Reset();
}

void Register(LogFn log)
{
    g_log = log;
    auto* rtti = CRTTISystem::Get();
    rtti->AddRegisterCallback(RegisterTypes);
    rtti->AddPostRegisterCallback(PostRegisterTypes);
}
} // namespace hud
