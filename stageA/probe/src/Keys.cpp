#include "Keys.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace keys
{
namespace
{
std::string Upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// The key part of a binding: its virtual-key code and canonical name, or 0.
std::pair<int, std::string> Key(const std::string& raw)
{
    const std::string k = Upper(raw);
    if (k.size() >= 2 && k[0] == 'F' && std::all_of(k.begin() + 1, k.end(), ::isdigit))
    {
        const int n = std::atoi(k.c_str() + 1);
        if (n >= 1 && n <= 24) return {VK_F1 + n - 1, "F" + std::to_string(n)};
        return {0, ""};
    }
    if (k.size() == 1 && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return {k[0], k};
    if (k.size() == 4 && k.rfind("NUM", 0) == 0 && k[3] >= '0' && k[3] <= '9') return {VK_NUMPAD0 + (k[3] - '0'), "Num" + k.substr(3)};
    static const std::pair<const char*, std::pair<int, const char*>> named[] = {
        {"INSERT", {VK_INSERT, "Insert"}}, {"INS", {VK_INSERT, "Insert"}}, {"DELETE", {VK_DELETE, "Delete"}},
        {"DEL", {VK_DELETE, "Delete"}}, {"HOME", {VK_HOME, "Home"}}, {"END", {VK_END, "End"}},
        {"PAGEUP", {VK_PRIOR, "PageUp"}}, {"PGUP", {VK_PRIOR, "PageUp"}}, {"PAGEDOWN", {VK_NEXT, "PageDown"}},
        {"PGDN", {VK_NEXT, "PageDown"}}, {"PAUSE", {VK_PAUSE, "Pause"}}, {"SCROLLLOCK", {VK_SCROLL, "ScrollLock"}},
        {"BACKSPACE", {VK_BACK, "Backspace"}}, {"TAB", {VK_TAB, "Tab"}}, {"ENTER", {VK_RETURN, "Enter"}},
        {"SPACE", {VK_SPACE, "Space"}}, {"UP", {VK_UP, "Up"}}, {"DOWN", {VK_DOWN, "Down"}}, {"LEFT", {VK_LEFT, "Left"}},
        {"RIGHT", {VK_RIGHT, "Right"}}};
    for (const auto& [name, v] : named)
        if (k == name) return {v.first, v.second};
    return {0, ""};
}
} // namespace

bool Parse(const std::string& spec, Chord& out, std::string& error)
{
    Chord c;
    std::string key;
    size_t start = 0;
    const std::string s = Trim(spec);
    if (s.empty()) { error = "empty"; return false; }
    while (start <= s.size())
    {
        const size_t plus = s.find('+', start);
        const std::string part = Trim(s.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        const std::string up = Upper(part);
        if (up == "CTRL" || up == "CONTROL") c.ctrl = true;
        else if (up == "SHIFT") c.shift = true;
        else if (up == "ALT") c.alt = true;
        else if (part.empty()) { error = "an empty part (two '+' in a row?)"; return false; }
        else if (!key.empty()) { error = "two keys (\"" + key + "\" and \"" + part + "\"); a binding has one key"; return false; }
        else key = part;
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    if (key.empty()) { error = "no key, only modifiers"; return false; }
    const auto [vk, name] = Key(key);
    if (!vk) { error = "unknown key \"" + key + "\""; return false; }
    if (vk == VK_F5 || vk == VK_F9) { error = name + " is the game's quick save / quick load, whatever the modifiers"; return false; }
    c.vk = vk;
    c.text = std::string(c.ctrl ? "Ctrl+" : "") + (c.shift ? "Shift+" : "") + (c.alt ? "Alt+" : "") + name;
    out = c;
    return true;
}

bool Held(const Chord& c)
{
    if (!c.vk) return false;
    const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    return down(c.vk) && down(VK_CONTROL) == c.ctrl && down(VK_SHIFT) == c.shift && down(VK_MENU) == c.alt;
}
} // namespace keys
