#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Read-only PhysX inspection around a point (Cyberpunk 2077 2.5, CDPR's PhysX 3.4.2 build).
// Safety model (a fault is fatal in this game even when caught):
//  - PhysX3_x64.dll / PhysX3Common_x64.dll must be the exact builds whose vtables were verified offline;
//  - a virtual is only ever called on an object whose vtable pointer equals one of those verified vtables;
//  - bulk data (vertices, indices, height samples) is copied with ReadProcessMemory, never dereferenced.
namespace probe
{
struct Vec3
{
    float x, y, z;
};

struct Census
{
    uint32_t byType[8] = {};  // PxGeometryType: sphere, plane, capsule, box, convex, trianglemesh, heightfield
    uint32_t staticActors = 0, dynamicActors = 0, skippedActors = 0, skippedShapes = 0, skippedMeshes = 0;
    uint32_t shapes = 0, simulationShapes = 0, queryShapes = 0, triggerShapes = 0;
    uint64_t trianglesWritten = 0;
    uint32_t filterWord0[16] = {}, filterWord0Count[16] = {}, filterDistinct = 0;
};

struct NearActor
{
    float dist = 0, center[3] = {}, extent[3] = {};
    uint8_t isStatic = 0, geom = 255;
    uint16_t shapes = 0;
    uint64_t userData = 0;
    bool used = false;
};

// A PhysX actor whose userData (directly, or one pointer away) refers to V's entity or one of its components.
struct OwnerHit
{
    uint32_t scene = 0, depth = 0;  // depth 0: userData is the address; 1: userData points at a block holding it
    float pos[3] = {};
    uint8_t isStatic = 0, geom = 255;
    float capsule[2] = {};  // radius, halfHeight when the first shape is a capsule
    uint64_t userData = 0, matched = 0;
};

struct SceneInfo
{
    uint32_t flags = 0;
    float gravity[3] = {};
    uint32_t nbStatic = 0, nbDynamic = 0;
    uint64_t vtableRva = 0;  // NpScene vtable (for offline confirmation)
    Census census;
    uint32_t staticSeen = 0;
    float centerMin[3] = {}, centerMax[3] = {}, centerMean[3] = {};  // static actor bounds centres
    NearActor nearestStatic[5], nearestDynamic[5];
    // ground under the probe point, from the collected triangles (no PhysX query used)
    bool groundFound = false;
    float groundZ = 0, groundDelta = 0;
};

struct Result
{
    bool abiOk = false;
    const char* abiWhy = "";
    uint32_t nbScenes = 0;
    SceneInfo scenes[16];
    OwnerHit owners[8];
    uint32_t nOwners = 0;
    // Capsules near the predicted point, and the one whose displacement since the last probe matches V's.
    struct Capsule
    {
        uint64_t actor = 0;
        float pos[3] = {}, radius = 0, halfHeight = 0, distToPredicted = 0, motionError = -1;
    } capsules[8];
    uint32_t nCapsules = 0;
    float vDisplacement = 0;    // V's world displacement since the previous probe (m)
    int matchedCapsule = -1;    // index into capsules when the motion match succeeded (error < 0.5 m)
    // world = physx + shift. From V's matched capsule when found; otherwise a rough estimate (streaming centroid).
    bool shiftFromOwner = false;
    float shift[3] = {};
    float probeAt[3] = {};  // PhysX-space point the census/dump used
    // P1-12 (0.21.0): every actor within 3 m of V that Stage B's gather leaves out (dynamic actors, and shapes it has
    // no triangles for), one line each: kind, kinematic, asleep, mass, speed, where, name, and its shapes.
    std::vector<std::string> skippedNear;
};

// Verifies the PhysX build once; must be true before Run is used.
bool CheckAbi(const char*& why);
// Total static actors across scenes (cheap; for "world is loaded" gating). Returns 0 if unavailable.
uint32_t StaticActorCount();
// P1-15 (0.24.0): static and dynamic actors across the scenes now (cheap): how much of the world has streamed in.
void ActorCounts(uint32_t& statics, uint32_t& dynamics);
// Stage B: the PhysX origin shift (world = physx + shift), found by locating V's character-controller capsule at
// each 1,024 m grid candidate. True when a capsule matched within 0.3 m; `capsuleActor` receives that capsule's actor.
// Walks every actor of every scene (3-4 ms on the main thread in Glen).
bool FindShift(const Vec3& vWorld, float shift[3], float& error, uint32_t& capsules, uint64_t* capsuleActor = nullptr);
// Stage B: re-checks a known shift against the capsule FindShift returned, without walking the scenes (microseconds).
// False when that actor is gone or no longer sits where V's capsule should be; then call FindShift.
bool ConfirmShift(const Vec3& vWorld, const float shift[3], uint64_t capsuleActor, float& error);

// P1-14 (0.22.0): a moving body near the skater (a car), for Skate's moving geometry. Game world; its triangles are
// `triCount` of 9 floats from `firstTri` in the buffer filled beside it. The dynamics are PhysX's: centre of mass and the
// orientation of the mass frame, velocities, inverse mass and the mass-frame inverse inertia.
struct MovingBody
{
    uint32_t id = 0, kind = 0;  // kind 0: vehicle
    uint32_t firstTri = 0, triCount = 0;
    float com[3] = {}, comRot[4] = {0, 0, 0, 1}, linVel[3] = {}, angVel[3] = {};
    float invMass = 0, invInertia[3] = {};
    float dist = 0;
};

// P1-14: the cars near a point, tracked across frames, main thread only. Each frame it reads the scenes' fresh dynamic
// actor lists (a tracked actor no longer in them is forgotten before anything is called on it), looks at a slice of them
// for new cars (non-kinematic rigid dynamics with simulated shapes of type 0x10, the game's vehicle bodies), reads each
// car's shapes once into its own frame, and every frame its pose and dynamics.
class Vehicles
{
public:
    struct Stats
    {
        uint32_t tracked = 0, found = 0, dropped = 0;
        double ms = 0;
    };
    Vehicles();
    ~Vehicles();
    Vehicles(const Vehicles&) = delete;
    Vehicles& operator=(const Vehicles&) = delete;
    // Bodies within `radius` of `centerWorld`, at most `maxBodies`, nearest first.
    void Update(const Vec3& centerWorld, const float shift[3], float radius, uint32_t maxBodies, std::vector<MovingBody>& bodies,
                std::vector<float>& tris, Stats& st);
    void Clear();

private:
    struct Impl;
    Impl* impl;
};

// Stage B: static collision within `radius` of a game-world point as game-world triangles (9 floats each), with a
// layer flag per triangle (1 physical, 0 detail).
//  - Begin and Scan run on the game's main thread: they walk the scenes and call PhysX virtuals. Scan stops after
//    `budgetMs` and resumes next frame from a freshly read actor list, skipping actors it has already visited; once
//    every list is walked it makes one more pass for actors that moved in a list between frames.
//  - Extract only copies mesh and height-field data with ReadProcessMemory and checks each object's vtable again
//    afterwards (a mesh freed meanwhile is skipped), so it may run on another thread once Scan has returned true.
class Collector
{
public:
    struct Stats
    {
        uint32_t frames = 0, actorsVisited = 0, actorsNear = 0, shapes = 0, meshJobs = 0, stale = 0;
        uint32_t props = 0, propShapes = 0;  // P1-12 (0.21.1): kinematic props taken in, and their shapes
        double scanMs = 0, scanMaxMs = 0, extractMs = 0;
    } stats;
    Collector();
    ~Collector();
    Collector(Collector&&) noexcept;
    Collector& operator=(Collector&&) noexcept;
    void Begin(const Vec3& centerWorld, const float shift[3], float radius, size_t maxTriangles);
    bool Scan(double budgetMs);
    void Extract(std::vector<float>& tris, std::vector<uint8_t>& flags);

private:
    struct Impl;
    Impl* impl = nullptr;
};

// Begin + Scan + Extract in one go on the calling (main) thread. Returns the triangle count.
uint32_t CollectTriangles(const Vec3& centerWorld, const float shift[3], float radius, size_t maxTriangles,
                          std::vector<float>& tris, std::vector<uint8_t>& flags, Collector::Stats* stats = nullptr);
// owners: V's entity and component addresses, used to recognise V's own physics actors through their userData.
void Run(const Vec3& at, float censusRadius, float dumpRadius, uint64_t maxTriangles, FILE* obj, const uint64_t* owners,
         uint32_t nOwners, Result& out);
} // namespace probe
