// Offline check of pspad::Parse on synthetic reports in each layout the PlayStation pads send.
#include "../src/Pad.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

// Writes the controls at `b` (left stick X first) in the DualSense full layout or the DS4/simple layout.
static void Put(uint8_t* b, bool ds5, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, uint8_t l2, uint8_t r2, uint8_t b0, uint8_t b1, uint8_t b2)
{
    b[0] = lx, b[1] = ly, b[2] = rx, b[3] = ry;
    if (ds5) b[4] = l2, b[5] = r2, b[6] = 0x5A /* counter */, b[7] = b0, b[8] = b1, b[9] = b2;
    else b[4] = b0, b[5] = b1, b[6] = b2, b[7] = l2, b[8] = r2;
}

static void Case(const char* name, uint8_t id, size_t length, size_t base, bool ds5Layout, bool dualSense, bool usb)
{
    std::vector<uint8_t> r(length, 0);
    r[0] = id;
    // Left stick fully right and up, right stick centred, L2 200, R2 0, D-pad up-right (hat 1) with Cross and Square,
    // L1, Options, R3, touchpad click.
    Put(r.data() + base, ds5Layout, 255, 0, 128, 128, 200, 0, 0x01 | 0x20 | 0x10, 0x01 | 0x20 | 0x80, 0x02);
    pspad::Pad p;
    const bool ok = pspad::Parse(r.data(), r.size(), dualSense, usb, p);
    std::printf("%-28s parsed %d: lx %6d ly %6d rx %6d ry %6d lt %3u rt %3u buttons %#06x\n", name, ok, p.lx, p.ly, p.rx, p.ry, p.leftTrigger,
                p.rightTrigger, p.buttons);
    CHECK(ok);
    CHECK(p.connected);
    CHECK(p.lx > 32000);              // right
    CHECK(p.ly > 32000);              // up is +Y, as XInput
    CHECK(p.rx > -300 && p.rx < 300); // centred
    CHECK(p.ry > -300 && p.ry < 300);
    CHECK(p.leftTrigger == 200 && p.rightTrigger == 0);
    CHECK(p.buttons == (0x0001 | 0x0008 | 0x1000 | 0x4000 | 0x0100 | 0x0010 | 0x0080 | 0x0020));  // up, right, A, X, LB, Start, R3, Back
}

int main()
{
    Case("DualSense USB (0x01, 64)", 0x01, 64, 1, true, true, true);
    Case("DualSense Bluetooth (0x31)", 0x31, 78, 2, true, true, false);
    Case("DualSense BT simple (0x01)", 0x01, 10, 1, false, true, false);
    Case("DualShock 4 USB (0x01, 64)", 0x01, 64, 1, false, false, true);
    Case("DualShock 4 Bluetooth (0x11)", 0x11, 78, 3, false, false, false);
    // Each other face button alone, DualSense USB.
    const struct { uint8_t b0, b1; uint16_t want; } single[] = {
        {0x08 | 0x40, 0, 0x2000}, {0x08 | 0x80, 0, 0x8000}, {0x08, 0x02, 0x0200}, {0x08, 0x10, 0x0020}, {0x08, 0x40, 0x0040},
        {0x04, 0, 0x0002}, {0x06, 0, 0x0004}, {0x07, 0, 0x0001 | 0x0004}, {0x03, 0, 0x0002 | 0x0008}};  // Circle, Triangle, R1, Create, L3, D-pad down, left, up-left, down-right
    for (const auto& s : single)
    {
        uint8_t r[64] = {0x01};
        Put(r + 1, true, 128, 128, 128, 128, 0, 0, s.b0, s.b1, 0);
        pspad::Pad p;
        CHECK(pspad::Parse(r, sizeof r, true, true, p));
        CHECK(p.buttons == s.want);
    }
    // Wrong or short reports are refused.
    uint8_t bad[64] = {0x11};
    pspad::Pad p;
    CHECK(!pspad::Parse(bad, sizeof bad, true, false, p));   // DS4 report on a DualSense
    bad[0] = 0x31;
    CHECK(!pspad::Parse(bad, sizeof bad, false, false, p));  // DualSense report on a DS4
    bad[0] = 0x05;
    CHECK(!pspad::Parse(bad, sizeof bad, true, true, p));    // unknown report
    bad[0] = 0x01;
    CHECK(!pspad::Parse(bad, 6, true, true, p));             // too short
    std::printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "OK", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
