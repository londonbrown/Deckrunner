#pragma once
#include <string>

// Players' key bindings, from deckrunner.ini [keys] (0.28.0):
//   [keys]
//   skate=Ctrl+Shift+F10     start / stop skating
//   capture=Ctrl+Shift+F12   save the last few seconds of a ride for a bug report
// A binding is modifiers (Ctrl, Shift, Alt) and one key joined with '+', in any case: F1-F24, A-Z, 0-9, Num0-Num9,
// Insert, Delete, Home, End, PageUp, PageDown, Pause, ScrollLock, Backspace, Tab, Enter, Space, Up, Down, Left, Right.
// The modifiers must match exactly (Ctrl+Shift+F10 does not fire on Ctrl+Shift+Alt+F10). F5 and F9 are refused: the
// game quick-saves and quick-loads on them whatever the modifiers.
namespace keys
{
struct Chord
{
    bool ctrl = false, shift = false, alt = false;
    int vk = 0;        // Windows virtual-key code; 0 = unbound
    std::string text;  // canonical spelling, e.g. "Ctrl+Shift+F10"
};

// Parses a binding; on failure returns false and says why in `error`.
bool Parse(const std::string& spec, Chord& out, std::string& error);

// Held right now (modifiers matching exactly). The caller checks that the game window has focus.
bool Held(const Chord& c);
} // namespace keys
