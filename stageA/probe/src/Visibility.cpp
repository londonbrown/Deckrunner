#include "Visibility.hpp"

#include <atomic>
#include <cstring>

#include <Windows.h>

namespace vis
{
namespace
{
constexpr uint32_t kHideHash = 3734051506, kVisHash = 3960219843;
constexpr size_t kCamAt = 0x124, kHideAt = 0x12A, kVisAt = 0x12C;
using HideFn = void (*)(void*, bool, uint8_t);
using VisFn = void (*)(void*, bool, uint8_t, bool);

// The 2.5 code at each function's start, up to and including its read of the field it changes:
//  hide: mov [rsp+8],rbx; mov [rsp+10h],rsi; push rdi; sub rsp,20h; mov dil,dl; mov sil,r8b; movzx edx,word ptr [rcx+12Ah]
//  vis:  push rbx; sub rsp,20h; mov r11b,byte ptr [rcx+12Ch]; mov r10,rcx
constexpr uint8_t kHideCode[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec,
                                 0x20, 0x40, 0x8a, 0xfa, 0x41, 0x8a, 0xf0, 0x0f, 0xb7, 0x91, 0x2a, 0x01, 0x00, 0x00};
constexpr uint8_t kVisCode[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x44, 0x8a, 0x99, 0x2c, 0x01, 0x00, 0x00, 0x4c, 0x8b, 0xd1};

struct Fns
{
    HideFn hide = nullptr;
    VisFn vis = nullptr;
};

const Fns& Get()
{
    static const Fns f = []() {
        Fns r;
        const HMODULE m = GetModuleHandleW(L"RED4ext.dll");
        using ResolveFn = uintptr_t (*)(uint32_t);
        const auto resolve = m ? reinterpret_cast<ResolveFn>(GetProcAddress(m, "RED4ext_ResolveAddress")) : nullptr;
        if (!resolve) return r;
        const uintptr_t h = resolve(kHideHash), v = resolve(kVisHash);
        if (h && std::memcmp(reinterpret_cast<const void*>(h), kHideCode, sizeof kHideCode) == 0) r.hide = reinterpret_cast<HideFn>(h);
        if (v && std::memcmp(reinterpret_cast<const void*>(v), kVisCode, sizeof kVisCode) == 0) r.vis = reinterpret_cast<VisFn>(v);
        return r;
    }();
    return f;
}
} // namespace

bool Available() { return Get().hide && Get().vis; }

bool IsVisual(RED4ext::IScriptable* c)
{
    static RED4ext::CClass* const cls = RED4ext::CRTTISystem::Get()->GetClass(RED4ext::CName("entIVisualComponent"));
    return c && cls && c->GetType()->IsA(cls);
}

bool Read(RED4ext::IScriptable* c, Flags& out)
{
    if (!Available() || !IsVisual(c)) return false;
    const auto* b = reinterpret_cast<const uint8_t*>(c);
    out.cam = b[kCamAt];
    std::memcpy(&out.hide, b + kHideAt, sizeof out.hide);
    out.vis = b[kVisAt];
    return true;
}

bool SetHide(RED4ext::IScriptable* c, bool on, uint8_t reason)
{
    if (!Available() || !IsVisual(c) || reason > 15) return false;
    Get().hide(c, on, reason);
    return true;
}

bool SetVis(RED4ext::IScriptable* c, bool on, uint8_t bit)
{
    if (!Available() || !IsVisual(c) || bit > 7) return false;
    Get().vis(c, on, bit, false);
    return true;
}

bool SetCam(RED4ext::IScriptable* c, uint8_t cam)
{
    if (!Available() || !IsVisual(c) || cam > 2) return false;
    reinterpret_cast<uint8_t*>(c)[kCamAt] = cam;
    return true;
}

namespace
{
constexpr uint8_t kShieldReason = 8, kShieldVisBit = 0;
constexpr uint32_t kMaxParts = 64;
HideFn g_hideOrig = nullptr;
VisFn g_visOrig = nullptr;
bool g_installed = false;
std::atomic<bool> g_on{false};
std::atomic<uint32_t> g_count{0};
std::atomic<uintptr_t> g_parts[kMaxParts];
std::atomic<uint32_t> g_blockedHide{0}, g_blockedVis{0};

// Called from whatever thread the engine sets visibility on: atomics only, no allocation, no locks.
bool Shielded(const void* c)
{
    if (!g_on.load(std::memory_order_acquire)) return false;
    const uint32_t n = g_count.load(std::memory_order_acquire);
    const auto p = reinterpret_cast<uintptr_t>(c);
    for (uint32_t i = 0; i < n && i < kMaxParts; ++i)
        if (g_parts[i].load(std::memory_order_relaxed) == p) return true;
    return false;
}

void HideDetour(void* c, bool on, uint8_t reason)
{
    if (on && reason == kShieldReason && Shielded(c)) { g_blockedHide.fetch_add(1, std::memory_order_relaxed); return; }
    g_hideOrig(c, on, reason);
}

void VisDetour(void* c, bool on, uint8_t bit, bool force)
{
    if (on && bit == kShieldVisBit && Shielded(c)) { g_blockedVis.fetch_add(1, std::memory_order_relaxed); return; }
    g_visOrig(c, on, bit, force);
}
} // namespace

bool InstallShield(const RED4ext::v1::Sdk* sdk, RED4ext::v1::PluginHandle handle)
{
    if (g_installed) return true;
    if (!sdk || !sdk->hooking || !Available()) return false;
    void* hide = reinterpret_cast<void*>(Get().hide);
    void* visf = reinterpret_cast<void*>(Get().vis);
    if (!sdk->hooking->Attach(handle, hide, reinterpret_cast<void*>(&HideDetour), reinterpret_cast<void**>(&g_hideOrig)))
        return false;
    if (!sdk->hooking->Attach(handle, visf, reinterpret_cast<void*>(&VisDetour), reinterpret_cast<void**>(&g_visOrig)))
    {
        sdk->hooking->Detach(handle, hide);
        return false;
    }
    g_installed = g_hideOrig && g_visOrig;
    return g_installed;
}

bool ShieldInstalled() { return g_installed; }

void SetShield(RED4ext::IScriptable* const* parts, uint32_t count)
{
    if (!g_installed) return;
    count = count > kMaxParts ? kMaxParts : count;
    // Shrink first so a reader never pairs the new count with entries not yet written.
    if (count < g_count.load(std::memory_order_relaxed)) g_count.store(count, std::memory_order_release);
    for (uint32_t i = 0; i < count; ++i) g_parts[i].store(reinterpret_cast<uintptr_t>(parts[i]), std::memory_order_relaxed);
    g_count.store(count, std::memory_order_release);
    g_on.store(true, std::memory_order_release);
}

void ShieldOff()
{
    g_on.store(false, std::memory_order_release);
    g_count.store(0, std::memory_order_release);
}

bool ShieldOn() { return g_on.load(std::memory_order_acquire); }

void ShieldBlocked(uint32_t& hide, uint32_t& vis)
{
    hide = g_blockedHide.load(std::memory_order_relaxed);
    vis = g_blockedVis.load(std::memory_order_relaxed);
}
} // namespace vis
