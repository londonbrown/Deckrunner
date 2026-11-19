// #3: the weapon wheel while skating on foot. Cyberpunk opens its weapon wheel on a 0.3 s hold of Y / Triangle
// (r6\config\inputContexts.xml: <hold action="WeaponWheel" timeout="0.3" />), and Skate uses the same button to get on
// and off the board. While the skater is on foot, Y is kept from Skate until it's known which one the player meant:
// released within kHoldS it was a tap, and Skate gets a short press then (to get on the board); held longer, it's the
// game's weapon wheel, and Skate never sees it. While riding, Y goes to Skate untouched (getting off stays instant).
// Pure logic, so tests/wheel_split.cpp runs it without the game.
#pragma once

#include <cstdint>

namespace wheel
{
constexpr uint16_t kY = 0x8000;      // XInput's Y; the DualSense path maps Triangle to it (pspad)
constexpr double kHoldS = 0.25;      // under the game's 0.3 s, so a press is never both a tap for Skate and a wheel
constexpr double kTapPressS = 0.10;  // how long Skate sees the tap's press

enum class Event { None, Tap, HoldStart, HoldEnd };

class YSplit
{
public:
    // The buttons Skate gets this frame (`buttons` is the pad as read), and what happened. `split` is whether the wheel
    // can open now (the skater is on foot and the radial-menu restriction is off).
    uint16_t Filter(uint16_t buttons, double now, bool split, Event& ev);
    bool Holding() const { return pressedAt_ >= 0 && held_; }

private:
    double pressedAt_ = -1;    // when a Y press began while split (-1: none being held back)
    bool held_ = false;        // that press passed kHoldS
    double injectUntil_ = -1;  // a tap's press for Skate lasts until then
    bool wasDown_ = false;
};
} // namespace wheel
