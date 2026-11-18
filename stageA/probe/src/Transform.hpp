#pragma once
#include <RED4ext/RED4ext.hpp>

#include "Rigid.hpp"

// P1-15 (0.24.0): an entity moved by writing its root transform, without TeleportationFacility.Teleport.
// Tenth play test: with V (and the board) teleported every frame, the world stopped streaming in: no traffic, no
// enemies, props without collision, until the game was paused. The plugin log shows it: 35 s of riding in one block
// with no car added to the physics scene, then 14 within half a second of the pause.
//
// The write goes through the game's own IPlacedComponent::SetTransform (the function Codeware's
// Entity.SetWorldTransform calls), resolved by hash through RED4ext's address resolver. The script-side
// IPlacedComponent.SetLocalTransform is not used: on the board's mesh component it changed nothing on screen (C3b run 3).
namespace xform
{
// The game function was found (resolved once, on first use).
bool Available();
// Sets the entity's root transform (its transform component) to `p`, `q` in the world. False when unavailable or when
// `entity` is not an entity.
bool Place(RED4ext::IScriptable* entity, rigid::V3 p, rigid::Q q);
// P2-10 (0.25.0): sets one placed component's own transform (relative to its parent binding; in the world when it has
// none). False when unavailable or when `component` is not a placed component.
bool PlaceComponent(RED4ext::IScriptable* component, rigid::V3 p, rigid::Q q);
} // namespace xform
