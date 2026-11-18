// Offline check of deckrunner.ini [keys] parsing (not shipped).
#include "../src/Keys.hpp"

#include <Windows.h>

#include <cstdio>

int main()
{
    int failed = 0;
    const auto ok = [&](const char* spec, const char* text, int vk, bool ctrl, bool shift, bool alt) {
        keys::Chord c;
        std::string err;
        const bool good = keys::Parse(spec, c, err) && c.text == text && c.vk == vk && c.ctrl == ctrl && c.shift == shift && c.alt == alt;
        if (!good) ++failed, std::printf("FAIL %s -> %s (%s)\n", spec, c.text.c_str(), err.c_str());
    };
    const auto bad = [&](const char* spec) {
        keys::Chord c;
        std::string err;
        if (keys::Parse(spec, c, err)) ++failed, std::printf("FAIL %s parsed as %s\n", spec, c.text.c_str());
        else std::printf("refused %-22s %s\n", spec, err.c_str());
    };
    ok("Ctrl+Shift+F10", "Ctrl+Shift+F10", VK_F10, true, true, false);
    ok(" shift + ctrl + f12 ", "Ctrl+Shift+F12", VK_F12, true, true, false);
    ok("Alt+K", "Alt+K", 'K', false, false, true);
    ok("F24", "F24", VK_F24, false, false, false);
    ok("Control+num7", "Ctrl+Num7", VK_NUMPAD7, true, false, false);
    ok("ctrl+pgdn", "Ctrl+PageDown", VK_NEXT, true, false, false);
    ok("Ctrl+Shift+Alt+Insert", "Ctrl+Shift+Alt+Insert", VK_INSERT, true, true, true);
    bad("");
    bad("Ctrl+Shift");
    bad("Ctrl++F10");
    bad("Ctrl+F10+F11");
    bad("Ctrl+Shift+F5");
    bad("Ctrl+F9");
    bad("F25");
    bad("Ctrl+Banana");
    std::printf(failed ? "%d FAILED\n" : "all passed\n", failed);
    return failed;
}
