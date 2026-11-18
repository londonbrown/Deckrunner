#pragma once
#include <cstdint>

#include "StageB.hpp"

// Skate audio (0.16.0): SK8V's audio engine runs Skate 3's own sound scripts inside the runtime (ABI 7) and renders
// three mono emitters at 48 kHz: the deck (wheels, pops, landings, grinds), the body (foot plants, bails) and the
// "sense of speed" (wind; heard at the listener). This module plays them through XAudio2 on its own thread, beside the
// game's audio: each emitter panned and attenuated from the game camera, mixed to stereo, through a limiter.
namespace skateaudio
{
using LogFn = void (*)(const char*);

struct Fns
{
    uint32_t (*configure)(void* rt, const char* cacheDir, float masterGain) = nullptr;
    uint32_t (*pull)(const void* rt, uint32_t id, float* out, uint32_t frames, stageb::DkrAudioEmitter* info) = nullptr;
    uint32_t (*setPaused)(const void* rt, uint32_t paused) = nullptr;
    uint32_t (*status)(const void* rt, stageb::DkrAudioStatus* out) = nullptr;
};

// Once, right after the runtime is created: reads deckrunner.ini [audio] (enabled, cache, volume), starts the
// runtime's audio engine on the prepared cache (it loads on its own thread, ~6 s) and opens the output.
void Begin(void* rt, const Fns& fns, LogFn log);

// Main thread, every frame: the listener (the game camera, game world; pos == nullptr when unknown), whether the game
// is paused, and the log. With no frame for 250 ms (a loading screen, a hitch) the output pauses by itself.
void Frame(const float* pos, const float* fwd, const float* right, bool paused, LogFn log);
} // namespace skateaudio
