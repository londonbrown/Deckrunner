#pragma once
#include <RED4ext/RED4ext.hpp>

#include <filesystem>
#include <string>

// Stage C (investigation §7.2o), lab only.
//  C0: read-only dump of the game's RTTI (classes, properties, functions) and TweakDB records that Stage C needs.
//  C1: third person on foot with the game's own TPP representation, a simple chase camera, and the game's own
//      gameplay restrictions; no Skate involved. Everything it changes is undone on exit and checked afterwards.
namespace stagec
{
using LogFn = void (*)(const char*);

// C1, at plugin load: hooks the FPP camera's per-frame rotation (address hash 2531201123, the function Let There Be
// Flight hooks) so the chase camera is placed inside the frame, after animation. From the Running-state update it
// landed a frame late, after the bone it hangs off had moved again, and shook (C1 runs 2-3).
void InstallCameraHook(const RED4ext::v1::Sdk* sdk, RED4ext::v1::PluginHandle handle, LogFn log);

// C0. Writes `file` and logs a summary. Calls only getters.
void DumpRtti(const std::filesystem::path& file, RED4ext::IScriptable* player, LogFn log);

// C1, Ctrl+Shift+F7: off -> third person (V walks normally) -> third person with skating restrictions -> off.
void Cycle(const RED4ext::Handle<RED4ext::IScriptable>& player, LogFn log);
// C1, Ctrl+Shift+F11 while third person is on: toggles the head experiment (the head slot item's appearance).
void HeadExperiment(const RED4ext::Handle<RED4ext::IScriptable>& player, LogFn log);
// P2-10 (0.26.0), Ctrl+Shift+F2 while skating: V's head off for the rest of the ride, or back on. It is on by default:
// our camera, then the third-person representation with her character-creator head, shielded from the engine's hide.
void HeadToggle(const RED4ext::Handle<RED4ext::IScriptable>& player, LogFn log);
// C5: straight into skating from normal play (third person and the restrictions together). True when skating is on.
bool EnterSkating(const RED4ext::Handle<RED4ext::IScriptable>& player, LogFn log);
// C5: leave skating: everything undone (camera, restrictions, pose, board, head experiment). No-op when off.
void ExitSkating(const RED4ext::Handle<RED4ext::IScriptable>& player, LogFn log, const char* why);
// C5, per frame while skating: a reason the game needs V back (she died, is in a vehicle, a scene or workspot, or the game
// moved her), or nullptr.
const char* ExitReason(const RED4ext::Handle<RED4ext::IScriptable>& player);
// C5: what the cycle test compares before and after: skating restrictions on V, the FPP camera's local transform, the
// head slot's item.
std::string LifecycleSnapshot(const RED4ext::Handle<RED4ext::IScriptable>& player);
// C5: skating is off and everything it applied is confirmed gone.
bool Settled();
// C5: teleports V (the cycle test puts her back at its start between cycles).
bool PlaceV(const RED4ext::Handle<RED4ext::IScriptable>& player, const float pos[3], float yawDeg);
// C1, Ctrl+Shift+F6: undo everything now, whatever the state.
void EmergencyExit(const RED4ext::Handle<RED4ext::IScriptable>& player, LogFn log);
// Per frame from the main thread while active: holds the chase camera and notices a replaced player (save load).
void Frame(const RED4ext::Handle<RED4ext::IScriptable>& player, LogFn log);
bool Active();
// C2: true while third person and the skating restrictions are on.
bool Restricted();
// 0.24.0: skating is on, its restrictions were applied over 2 s ago, and none of them is on V any more. They are never
// saved, so a loaded save always comes back without them.
bool RestrictionsLost(const RED4ext::Handle<RED4ext::IScriptable>& player);
// P1-15 (0.24.0): C2 is moving V with a riding skater.
bool Following();
// 0.26.1: the last 15 s of V's follow, frame by frame, to a CSV in `dir` (Ctrl+Shift+F12, with Skate's capture).
void DumpFollowTrace(const std::filesystem::path& dir, LogFn log);
// P1-15 (0.24.1), Ctrl+Shift+F11 while riding: V moved by sparse teleports (the default) or by a teleport every frame (as
// up to 0.23.1, which stops the world streaming in), to compare.
void CycleMove(LogFn log);
// C2, per frame while Restricted() and a Stage B session rides: teleports V to the skater (interpolated between the
// skater's 60 Hz ticks), facing its direction of travel.
void Follow(const RED4ext::Handle<RED4ext::IScriptable>& player, const float skaterPos[3], const float skaterVel[3], float heading,
            uint64_t tick, const char* skaterState, LogFn log);
// C3, per frame after Follow while Active(): sets V's graph variables (lab archive deckrunner_c3.archive) to the skater's
// pose, or to the rig's reference pose while the test is on; fades the weight in and out.
void PoseFrame(const RED4ext::Handle<RED4ext::IScriptable>& player, LogFn log);
// C3, Ctrl+Shift+F11 while third person is on: toggles the reference-pose test (checks the graph's conventions).
void PoseTest(const RED4ext::Handle<RED4ext::IScriptable>& player, LogFn log);
// The game's own clock for this frame: seconds of simulation time since the last call, 0 while the game is paused
// (menus, pause screen). Skate steps on this, so it stops when the game does (C3b run 4: the skater rode on through a
// pause). Returns the wall-clock `wallDt` if the game's time system cannot be read.
float GameClock(float wallDt, LogFn log);
// Skate audio: GameClock's last pause reading (it reads the game's time system while the skater runs).
bool GamePaused();
// Skate audio: the listener, the game camera's world position and its forward and right axes; false while Stage C is
// off (the camera is the game's own then) or the camera cannot be read.
bool Listener(float pos[3], float fwd[3], float right[3]);
// C3: the rig JSON for V's body (female or male player skeleton; deckrunner.ini [stagec] rig_female / rig_male).
std::string RigFor(const RED4ext::Handle<RED4ext::IScriptable>& player, LogFn log);
} // namespace stagec
