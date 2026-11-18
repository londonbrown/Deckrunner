#pragma once
#include <RED4ext/RED4ext.hpp>

#include <cstdint>

// P2-10 (0.25.5): why a visual component is not drawn although it is enabled.
// The sixteenth and seventeenth tests (0.25.3, 0.25.4) showed V's face in third person with no hair, though her hair
// mesh was enabled with every chunk, on the same render scene layers as the face. The engine keeps, on every visual
// component (entIVisualComponent, game 2.5), three internal fields that its enabled flag does not show:
//  - +0x124, one byte: which cameras draw it (True First Person Camera 9's "camera target": 0 every camera, 1 offscreen
//    only, 2 none);
//  - +0x12A, 16 bits of hide reasons: any bit set hides the component;
//  - +0x12C, 8 more bits of the same kind.
// Two game functions set or clear one bit each (found by reading TFPC 9.1.1's MIT-licensed plugin, which unhides head
// parts this way while the third-person body is up, and confirmed in the 2.5 executable; both are in RED4ext's
// cyberpunk2077_addresses.json, without names):
//  - hash 3734051506 (RVA 0x402318): (component, bool on, uint8 reason) on the +0x12A word; when the word turns zero or
//    non-zero it tells the component's renderer;
//  - hash 3960219843 (RVA 0x3D1CD8): (component, bool on, uint8 bit, bool force) on the +0x12C byte.
// Each is used only after its first bytes are checked against the 2.5 code, so a game update that moves these fields
// turns this off instead of writing to the wrong place.
namespace vis
{
// Both functions found and their code as expected (checked once, on first use).
bool Available();
// The component is an entIVisualComponent (and so has the fields).
bool IsVisual(RED4ext::IScriptable* c);
struct Flags
{
    uint8_t cam = 0;
    uint16_t hide = 0;
    uint8_t vis = 0;
};
// The three fields, or false when unavailable or not a visual component.
bool Read(RED4ext::IScriptable* c, Flags& out);
// One hide-reason bit (0-15) on or off, through the game's own function.
bool SetHide(RED4ext::IScriptable* c, bool on, uint8_t reason);
// One bit (0-7) of the +0x12C byte on or off, through the game's own function.
bool SetVis(RED4ext::IScriptable* c, bool on, uint8_t bit);
// The camera-target byte written directly, as TFPC 9 writes it (no game function sets it); the caller then toggles the
// component off and on so its render proxy takes the change.
bool SetCam(RED4ext::IScriptable* c, uint8_t cam);

// ----- 0.25.7: the head shield -----
// 0.25.5 and 0.25.6 cleared hide reason 8 and +0x12C bit 0 on V's hair, and the engine set both again within 0.25 s every
// time. TFPC 9's "head shield" (SetHeadShield: "head parts shielded from the engine's hide") hooks both game functions
// through RED4ext's hooking API, and while it is up ignores exactly two requests for its head parts: setting reason 8, and
// setting bit 0. Everything else, and every other component, goes to the game as before. Ours does the same, for the parts
// set by SetShield. The hooks are attached once at load (pass-through until a shield is set); a failed check of either
// function's code attaches nothing.
bool InstallShield(const RED4ext::v1::Sdk* sdk, RED4ext::v1::PluginHandle handle);
bool ShieldInstalled();
// The shielded components (replacing the last set) and the shield on; at most 64.
void SetShield(RED4ext::IScriptable* const* parts, uint32_t count);
void ShieldOff();
bool ShieldOn();
// Requests ignored so far: reason 8 sets, bit 0 sets.
void ShieldBlocked(uint32_t& hide, uint32_t& vis);
} // namespace vis
