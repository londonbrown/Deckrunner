#include "Transform.hpp"

#include <RED4ext/Scripting/Natives/Generated/WorldTransform.hpp>
#include <RED4ext/Scripting/Natives/entEntity.hpp>
#include <RED4ext/Scripting/Natives/entIPlacedComponent.hpp>

#include <Windows.h>

namespace xform
{
namespace
{
// IPlacedComponent::SetTransform(component, const WorldTransform&): the address hash Codeware (MIT) uses for it, present
// in the game's cyberpunk2077_addresses.json for 2.5.
constexpr uint32_t kSetTransformHash = 1828854026;
using SetTransformFn = void (*)(RED4ext::ent::IPlacedComponent*, const RED4ext::WorldTransform&);

SetTransformFn Fn()
{
    // RED4ext's exported resolver returns 0 for an unknown hash (the SDK's own wrapper would end the game instead).
    static const SetTransformFn f = []() -> SetTransformFn {
        const HMODULE m = GetModuleHandleW(L"RED4ext.dll");
        using ResolveFn = uintptr_t (*)(uint32_t);
        const auto resolve = m ? reinterpret_cast<ResolveFn>(GetProcAddress(m, "RED4ext_ResolveAddress")) : nullptr;
        return resolve ? reinterpret_cast<SetTransformFn>(resolve(kSetTransformHash)) : nullptr;
    }();
    return f;
}
} // namespace

bool Available() { return Fn() != nullptr; }

namespace
{
bool Set(RED4ext::ent::IPlacedComponent* c, rigid::V3 p, rigid::Q q)
{
    const SetTransformFn f = Fn();
    if (!f || !c) return false;
    RED4ext::WorldTransform t{};
    t.Position = RED4ext::WorldPosition(RED4ext::Vector4{p.x, p.y, p.z, 1.f});
    t.Orientation.i = q.x, t.Orientation.j = q.y, t.Orientation.k = q.z, t.Orientation.r = q.w;
    f(c, t);
    return true;
}
} // namespace

bool Place(RED4ext::IScriptable* entity, rigid::V3 p, rigid::Q q)
{
    if (!Fn() || !entity) return false;
    static RED4ext::CClass* const entityCls = RED4ext::CRTTISystem::Get()->GetClass(RED4ext::CName("entEntity"));
    if (!entityCls || !entity->GetType()->IsA(entityCls)) return false;
    return Set(reinterpret_cast<RED4ext::ent::Entity*>(entity)->transformComponent, p, q);
}

bool PlaceComponent(RED4ext::IScriptable* component, rigid::V3 p, rigid::Q q)
{
    if (!Fn() || !component) return false;
    static RED4ext::CClass* const placedCls = RED4ext::CRTTISystem::Get()->GetClass(RED4ext::CName("entIPlacedComponent"));
    if (!placedCls || !component->GetType()->IsA(placedCls)) return false;
    return Set(reinterpret_cast<RED4ext::ent::IPlacedComponent*>(component), p, q);
}
} // namespace xform
