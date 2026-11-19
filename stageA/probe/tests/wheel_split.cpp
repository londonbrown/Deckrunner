// Offline test of the Y split (Wheel.hpp): a tap on foot reaches Skate late and short, a hold never does, and riding is
// untouched. Exit code 0 when every case passes.
#include "../src/Wheel.hpp"

#include <cstdio>

namespace
{
int g_failed = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

// Runs a press of `holdS` seconds at 60 Hz from t = 1 s; counts the frames Skate saw Y and the events.
struct Run
{
    int skateFrames = 0, taps = 0, holdStarts = 0, holdEnds = 0;
    double firstSkateY = -1;
};

Run Press(double holdS, bool split, bool splitAfterPress = true)
{
    wheel::YSplit s;
    Run r;
    for (int f = 0; f < 180; ++f)
    {
        const double t = f / 60.0;
        const bool down = t >= 1.0 && t < 1.0 + holdS;
        wheel::Event ev;
        const bool sp = t < 1.0 + 1e-9 ? split : splitAfterPress;
        const uint16_t out = s.Filter(down ? wheel::kY : 0, t, sp, ev);
        if (out & wheel::kY) { ++r.skateFrames; if (r.firstSkateY < 0) r.firstSkateY = t; }
        r.taps += ev == wheel::Event::Tap, r.holdStarts += ev == wheel::Event::HoldStart, r.holdEnds += ev == wheel::Event::HoldEnd;
    }
    return r;
}
} // namespace

int main()
{
    const Run tap = Press(0.1, true);
    Check(tap.taps == 1 && tap.holdStarts == 0 && tap.holdEnds == 0, "on foot, a 0.1 s tap is a tap");
    Check(tap.firstSkateY >= 1.09 && tap.firstSkateY < 1.12, "the tap reaches Skate when Y is let go");
    Check(tap.skateFrames >= 5 && tap.skateFrames <= 7, "Skate sees the tap for about 0.1 s");

    const Run hold = Press(0.8, true);
    Check(hold.holdStarts == 1 && hold.holdEnds == 1 && hold.taps == 0, "on foot, a 0.8 s hold is the wheel");
    Check(hold.skateFrames == 0, "Skate never sees a hold");

    const Run edge = Press(0.24, true);
    Check(edge.taps == 1 && edge.holdStarts == 0, "0.24 s is still a tap (under kHoldS)");

    const Run riding = Press(0.8, false, false);
    Check(riding.skateFrames >= 47 && riding.skateFrames <= 49 && riding.taps == 0 && riding.holdStarts == 0,
          "riding, Y goes to Skate untouched for as long as it's held");

    const Run dismount = Press(0.8, false, true);
    Check(dismount.skateFrames >= 47 && dismount.taps == 0 && dismount.holdStarts == 0,
          "a press that began riding (a dismount) stays Skate's after the skater is on foot");

    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
