#pragma once
#include <cstddef>
#include <cstdint>

// PlayStation controllers (DualShock 4, DualSense, DualSense Edge) read directly over HID, USB or Bluetooth, for Skate's
// controls. Ported from SK8V (host/src/ps_pad.{h,cpp}), where it reads the user's DualSense with no Steam Input: the
// game drives these pads natively (haptics, adaptive triggers) and XInput does not see them. Input reports are only
// read, through a shared handle; nothing is sent to the controller, so the game keeps full use of it.
// The pad is presented in XInput's layout, which is Skate 3's own Xbox 360 layout: Cross A, Circle B, Square X,
// Triangle Y, L1/R1 the shoulders, L2/R2 the triggers, L3/R3 the stick clicks, Create (and a touchpad click) Back,
// Options Start.
namespace pspad
{
struct Pad
{
    bool connected = false;
    uint16_t buttons = 0;  // XInput bits
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;  // XInput convention: +Y is up
};

// Latest state of the first connected PlayStation pad; false when none. `name` (optional) gets e.g. "DualSense (USB)".
// Starts the background reader on first use; it rescans every 2 s while no pad is open.
bool Read(Pad& out, const char** name = nullptr);

// One input report (report ID first) -> pad. `usb`: the device's input report is 64 bytes (USB or the DS4 wireless
// adapter); Bluetooth otherwise. Pure function; tests/pad_parse.cpp checks it.
bool Parse(const uint8_t* report, size_t length, bool dualSense, bool usb, Pad& out);
} // namespace pspad
