#pragma once
#include <cstdint>

#include <RED4ext/RED4ext.hpp>

// Stage C3b (lab only): Skate's skateboard as a game entity. The lab archive deckrunner_board.archive holds
// deckrunner\board\deckrunner_board.ent, a gameObject with one mesh component built from the user's own Skate 3 data
// (tools/board). The entity is spawned with the engine's own entity spawner, the way Codeware's StaticEntitySystem
// does it (address hashes from the game's cyberpunk2077_addresses.json), and placed every frame.
namespace board
{
using LogFn = void (*)(const char*);

// Per frame from the main thread. `want`: the board should exist (third person with restrictions, a session riding).
// `pos`/`rot`: the board joint's world transform this frame (game axes; rot = i, j, k, r), or nullptr when unknown.
void Frame(bool want, const float pos[3], const float rot[4], LogFn log);
// Removes the board now (exit, emergency exit, a new player object).
void Remove(LogFn log, const char* why);
// P1-15 (0.24.0): place the board by writing its root transform (true, the default) or by a teleport every frame (as up
// to 0.23.1).
void SetByTransform(bool on);

// P2-10 (0.25.0): a camera of our own to view V through, the engine's scene camera (engine\scenesystem\camera.ent), spawned
// with the same spawner. Per frame: `want` keeps it (spawning it first), placed at `pos`/`rot` (world; i, j, k, r).
void CamFrame(bool want, const float pos[3], const float rot[4], LogFn log);
// Its gameCameraComponent once spawned, or nullptr.
RED4ext::IScriptable* CamComponent();
// Removes it now (exit, emergency exit, a new player object).
void CamRemove(LogFn log, const char* why);
} // namespace board
