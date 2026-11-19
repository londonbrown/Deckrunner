#include "Wheel.hpp"

namespace wheel
{
uint16_t YSplit::Filter(uint16_t buttons, double now, bool split, Event& ev)
{
    ev = Event::None;
    const bool down = (buttons & kY) != 0;
    const bool edge = down && !wasDown_;
    wasDown_ = down;
    uint16_t out = buttons;
    if (pressedAt_ >= 0)  // a press being held back
    {
        if (down)
        {
            out &= ~kY;
            if (!held_ && now - pressedAt_ >= kHoldS) held_ = true, ev = Event::HoldStart;
        }
        else
        {
            if (held_) ev = Event::HoldEnd;
            else ev = Event::Tap, injectUntil_ = now + kTapPressS;
            pressedAt_ = -1, held_ = false;
        }
    }
    else if (edge && split)
    {
        pressedAt_ = now;
        out &= ~kY;
    }
    // A press that began while riding (or before the wheel could open) goes to Skate as it is.
    if (now < injectUntil_) out |= kY;
    return out;
}
} // namespace wheel
