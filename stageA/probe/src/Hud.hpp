// The scripts' side of the plugin (r6\scripts\deckrunner): a native class, DeckrunnerHud, registered with the game's
// RTTI. hud.reds (with Codeware) reads Skate's scorer from it for the HUD in Cyberpunk's own style; wheel.reds hands
// over the weapon wheel's pick while skating. Without the scripts nothing calls it, and nothing else changes.
#pragma once

#include <cstdint>
#include <string>

namespace hud
{
// What the HUD script shows, set once per frame from the main thread (Main.cpp).
struct View
{
    bool visible = false;   // skating, a ride's values are in, and deckrunner.ini [hud] enabled isn't 0
    bool onFoot = false;    // the skater is off the board
    bool switchStance = false;
    int32_t trickScore = 0, lineScore = 0;
    float multiplier = 0, lineFraction = 0, lineSeconds = 0;
    int32_t landing = 0;    // 0 none yet, 1 clean, 2 sketchy
    int32_t newTricks = 0, upgradedTricks = 0, closedLines = 0;
    std::string trick, landingText, stance, lineLabel, trickStance;
    // Skate's session marker: LB + d-pad up's hold toward the return (0..1), and counts since the ride started.
    float markerProgress = 0;
    int32_t markersPlaced = 0, markerReturns = 0, markerShortHolds = 0;
    std::string markerPlacedText, markerReturnText, holdText;  // Skate 3's words
};

using LogFn = void (*)(const char*);

void Set(const View& v);
// Skating (the restrictions are on): the weapon wheel's picks go to the plugin (wheel.reds).
void SetSkating(bool skating);
// The weapon wheel's last pick while skating (a gameItemID), once.
bool TakePick(uint8_t (&item)[16]);
// How many times a script has read Visible: the HUD script runs while this grows.
uint32_t Reads();
// The HUD script's refresh: Refresh() on the service hud.reds handed over (DeckrunnerHud.Attach), when what the HUD shows
// has changed (call once per frame). Logs the first call, or why it can't be made.
void NotifyScript(LogFn log);
// Hooks DeckrunnerHud into the RTTI system's registration (call at plugin load); `log` takes the scripts' Log lines.
void Register(LogFn log);
// Lets go of the HUD script's service: call as the game leaves its running state, while every plugin is still loaded.
void Release();
} // namespace hud
