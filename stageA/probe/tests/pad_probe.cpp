// Read-only check of the PlayStation pad reader on this machine: finds the pad and prints its state for a few seconds.
// It only reads input reports (shared handle); nothing is sent to the controller.
// Usage: pad_probe [seconds]
#include "../src/Pad.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
    pspad::Pad last{};
    const char* name = nullptr;
    int reads = 0, changes = 0;
    for (int i = 0; i < seconds * 20; ++i)
    {
        pspad::Pad p;
        if (pspad::Read(p, &name))
        {
            ++reads;
            if (p.buttons != last.buttons || std::abs(p.lx - last.lx) > 2000 || std::abs(p.ly - last.ly) > 2000 || std::abs(p.rx - last.rx) > 2000 ||
                std::abs(p.ry - last.ry) > 2000 || p.leftTrigger != last.leftTrigger || p.rightTrigger != last.rightTrigger || reads == 1)
            {
                ++changes;
                std::printf("%5.2fs %s: left (%6d, %6d) right (%6d, %6d) L2 %3u R2 %3u buttons %#06x\n", i / 20.0, name, p.lx, p.ly, p.rx, p.ry,
                            p.leftTrigger, p.rightTrigger, p.buttons);
                last = p;
            }
        }
        Sleep(50);
    }
    std::printf("%s; %d reads with a pad, %d changes printed\n", name ? name : "no PlayStation pad found", reads, changes);
    return 0;
}
