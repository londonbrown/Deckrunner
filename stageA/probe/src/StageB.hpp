#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Stage B: SK8V's Skate session (deckrunner_runtime.dll) running inside the game on live collision. No presentation.
namespace stageb
{
// Mirrors stageB/deckrunner-runtime/src/lib.rs (layout asserted on both sides).
struct DkrPad
{
    uint32_t connected;
    uint16_t buttons;
    uint8_t leftTrigger, rightTrigger;
    int16_t lx, ly, rx, ry;
};
static_assert(sizeof(DkrPad) == 16);

struct DkrState
{
    uint32_t size, status;
    uint64_t tick;
    float pos[3], velocity[3], speed;
    char state[48], trick[48];
    float stepMsAvg, stepMsMax, ticksPerS;
    uint32_t triangles, rails, collisionBuilds;
    float collisionCenter[3], heading;
    uint32_t grindTicks, airTicks;
    char error[160];
    // ABI 8 (P1-6): the worker's clock. tick + subTick is the simulation time, in ticks, through `steps` (every dkr_step
    // the worker has taken off its queue, used or skipped).
    float subTick;  // time left in the accumulator, in ticks (usually 0..1)
    float period;   // Skate's tick period, seconds
    uint32_t pad_;
    uint64_t steps;
};
static_assert(sizeof(DkrState) == 368);
static_assert(offsetof(DkrState, state) == 44);
static_assert(offsetof(DkrState, stepMsAvg) == 140);
static_assert(offsetof(DkrState, collisionCenter) == 164);
static_assert(offsetof(DkrState, error) == 188);
static_assert(offsetof(DkrState, subTick) == 348 && offsetof(DkrState, steps) == 360);

// Stage C3 (ABI 3, the board from ABI 4): Skate's pose retargeted onto V's rig, and the board. Bone order: stagec's kPoseBones (the graph patch's order).
struct DkrPose
{
    uint32_t size, valid;
    uint64_t tick;         // the skater tick it belongs to
    float hips[3];         // Hips position: game world, or model space for the reference pose
    uint32_t unreachable;  // bit per limb (left leg, right leg, left arm, right arm): out of V's reach
    float rot[24][4];      // orientation (i, j, k, r): game-world axes, or model space for the reference pose
    float root[3];         // skater root, game world (DkrState.pos of the same tick)
    uint32_t bones;        // bones in V's rig
    float boardPos[3];     // C3b (ABI 4): the board joint, game world
    uint32_t boardValid;   // 1 when boardPos/boardRot hold this tick's board
    float boardRot[4];     // (i, j, k, r), game-world axes
    float camPos[3];       // C4 (ABI 5): Skate's camera, game world
    uint32_t camValid;     // 1 when camPos/camFwd/camFov hold this tick's camera
    float camFwd[3];       // its look direction (unit; no roll)
    float camFov;          // Skate's field of view, degrees
};
static_assert(sizeof(DkrPose) == 496);
static_assert(offsetof(DkrPose, camPos) == 464 && offsetof(DkrPose, camFwd) == 480);
static_assert(offsetof(DkrPose, rot) == 32);
static_assert(offsetof(DkrPose, root) == 416);
static_assert(offsetof(DkrPose, boardPos) == 432 && offsetof(DkrPose, boardRot) == 448);

// Skate audio (ABI 7): SK8V's audio engine, mirrored from skatev-runtime (SvAudioEmitter, SvAudioStatus).
struct DkrAudioEmitter
{
    uint32_t size, id, flags, channels, sampleRate, queuedFrames;
    float position[3];  // game world
    float velocity[3];
    float gain;
    uint32_t voices;
    uint64_t frame;
};
static_assert(sizeof(DkrAudioEmitter) == 64 && offsetof(DkrAudioEmitter, position) == 24 && offsetof(DkrAudioEmitter, frame) == 56);
struct DkrAudioStatus
{
    uint32_t size, state, voices, underruns;  // state: 0 off, 1 loading, 2 ready, 3 error
    uint64_t frames;
    char message[96];
};
static_assert(sizeof(DkrAudioStatus) == 120);
constexpr uint32_t kAudioEmitterSounding = 1u << 1;

// P1-14 (ABI 10): one moving host body for Skate (deckrunner-runtime's DkrBody), game world.
struct DkrBody
{
    uint32_t size, id, kind, firstTri, triCount;
    float com[3];
    float comRot[4];  // (i, j, k, r)
    float linVel[3], angVel[3];
    float invMass;
    float invInertia[3];
};
static_assert(sizeof(DkrBody) == 88 && offsetof(DkrBody, com) == 20 && offsetof(DkrBody, invMass) == 72);

using LogFn = void (*)(const char*);
// Starts a session at V (game world position, yaw in degrees). Returns false and logs why if it cannot.
bool Start(const float vPos[3], float vYaw, LogFn log);
// #2: loads Skate in the background at V and keeps it parked, so the first Start resumes at once. Once per game; false
// (logged) if the runtime already exists or can't be made (the first Start then loads as before).
bool Preload(const float vPos[3], float vYaw, LogFn log);
// Reads V's current game world position; false if V is unavailable.
using PlayerPosFn = bool (*)(float out[3]);
// Per frame from the main thread: forwards the frame time, feeds collision requests, logs every 2 s. On each
// re-centre it re-checks the PhysX origin shift against V's capsule (`playerPos`).
void Frame(float dt, const DkrPad& pad, PlayerPosFn playerPos, LogFn log);
void Stop(LogFn log);
bool Active();
// P1-11 (ABI 9): asks the runtime for a capture of the skater's last 5 s and the collision around them (lab data).
bool Capture(LogFn log);
// P0-2: while riding, the engine's error message if Skate's engine stopped (status 3), else nullptr. The next Start
// then reloads Skate.
const char* Failed();
// The skater's state (game world) for Stage C2, as of this frame's snapshot; false unless a session is riding
// (status 2). P1-6 (0.18.0): Frame() takes one snapshot of the runtime (state and pose, one tick) before it posts the
// frame's step; Skater() and Pose() return it, so everything drawn this frame comes from the same tick.
bool Skater(float pos[3], float vel[3], float& heading, uint64_t& tick, char state[48]);
// P1-6: the tick to show this frame, fractional: one tick behind the runtime's own clock (tick + subTick, plus any
// step still queued), from the snapshot. NaN until a session rides.
double ShowTick();
// P1-15 (0.24.0): how far V's PhysX character capsule sits from where it belongs for V's feet at `v`, in metres; false
// when no capsule is known. Near 0 while the game carries her capsule with her.
bool CapsuleOffset(const float v[3], float& metres);
// P1-15 (0.24.0): static and dynamic actors in the game's physics scenes now: how much of the world has streamed in.
void ActorCounts(uint32_t& statics, uint32_t& dynamics);
// P1-6: the weight of tick `cur` over tick `prev` at ShowTick(), 0..1; 1 when they are the same tick or there is no
// clock. Every interpolation between two skater ticks (V, her pose, the board, Skate's camera) uses this, not the wall
// time at which the plugin noticed a tick, which stood V still for a frame and then moved her two ticks (0.17.0).
float Blend(uint64_t prev, uint64_t cur);
// C3: the rig (WolvenKit JSON of V's player .rig) the next Start fits the skater's skeleton to.
void SetRig(const std::string& path);
// C3: the retargeted pose (game world) from this frame's snapshot, false unless it holds the skater, the board or the
// camera; and V's rig in its reference pose (model space), read directly, false unless valid.
bool Pose(DkrPose& out);
bool ReferencePose(DkrPose& out);
} // namespace stageb
