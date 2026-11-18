// Read-only PhysX inspection of Cyberpunk 2077 2.5's physics scenes. See PhysxProbe.hpp for the safety model.
// Compiled against third_party/physx-3.4.2-cdpr-include: public PhysX 3.4 headers with the one ABI difference
// found in CDPR's build (PxShape::getReferenceCount after acquireReference).
#include "PhysxProbe.hpp"
#include "HeightFieldWinding.hpp"

#include <windows.h>

#include <PxPhysics.h>
#include <PxRigidActor.h>
#include <PxRigidDynamic.h>
#include <PxScene.h>
#include <PxShape.h>
#include <geometry/PxBoxGeometry.h>
#include <geometry/PxCapsuleGeometry.h>
#include <geometry/PxConvexMesh.h>
#include <geometry/PxConvexMeshGeometry.h>
#include <geometry/PxHeightField.h>
#include <geometry/PxHeightFieldGeometry.h>
#include <geometry/PxHeightFieldSample.h>
#include <geometry/PxSphereGeometry.h>
#include <geometry/PxTriangleMesh.h>
#include <geometry/PxTriangleMeshGeometry.h>

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace physx;

namespace probe
{
namespace
{
// Verified offline (investigation/probes/physx_vtables.py) for these exact builds.
constexpr DWORD kPhysXStamp = 0x64c978b4;        // PhysX3_x64.dll
constexpr DWORD kCommonStamp = 0x64c978a8;       // PhysX3Common_x64.dll
constexpr uintptr_t kNpShape = 0x20e9a8;         // PhysX3_x64.dll vtable RVAs
constexpr uintptr_t kNpRigidStatic = 0x20b980;
constexpr uintptr_t kNpRigidDynamic = 0x20b2e8;
constexpr uintptr_t kNpScene = 0x20caf0;         // verified: getFlags 0x18, getNbActors 0x90, getActors 0x98, getGravity 0x218
constexpr uintptr_t kBVH33Mesh = 0x1b6978;       // PhysX3Common_x64.dll vtable RVAs
constexpr uintptr_t kBVH34Mesh = 0x1b6868;
constexpr uintptr_t kConvex = 0x1b5200;
constexpr uintptr_t kHeightField = 0x1b6b18;

uintptr_t g_px = 0, g_common = 0;

struct CapsuleSample
{
    uint64_t actor;
    PxVec3 pos;
    float radius, halfHeight;
};
std::vector<CapsuleSample> g_capsules, g_prevCapsules;  // this probe's / previous probe's dynamic capsules
PxVec3 g_prevV(0, 0, 0);
bool g_havePrev = false;
bool g_checked = false, g_ok = false;
const char* g_why = "not checked";

bool ReadMem(const void* p, void* out, size_t n)
{
    SIZE_T got = 0;
    return p && ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}

DWORD Stamp(HMODULE m)
{
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(m) + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}

enum class Kind { Unknown, Shape, RigidStatic, RigidDynamic, TriMesh, Convex, HeightField };

Kind KindOf(const void* obj)
{
    uintptr_t vt = 0;
    if (!ReadMem(obj, &vt, sizeof vt)) return Kind::Unknown;
    if (vt == g_px + kNpShape) return Kind::Shape;
    if (vt == g_px + kNpRigidStatic) return Kind::RigidStatic;
    if (vt == g_px + kNpRigidDynamic) return Kind::RigidDynamic;
    if (vt == g_common + kBVH33Mesh || vt == g_common + kBVH34Mesh) return Kind::TriMesh;
    if (vt == g_common + kConvex) return Kind::Convex;
    if (vt == g_common + kHeightField) return Kind::HeightField;
    return Kind::Unknown;
}

bool SceneOk(const PxScene* scene)
{
    uintptr_t vt = 0;
    return ReadMem(scene, &vt, sizeof vt) && vt == g_px + kNpScene;
}

PxPhysics* Physics()
{
    using GetPhysicsFn = PxPhysics&(__cdecl*)();
    auto get = reinterpret_cast<GetPhysicsFn>(GetProcAddress(reinterpret_cast<HMODULE>(g_px), "PxGetPhysics"));
    return get ? &get() : nullptr;
}

struct Obj
{
    FILE* f;
    uint64_t base = 1, triangles = 0, max;
    PxVec3 shift{0, 0, 0};  // OBJ is written in game world coordinates
    bool full() const { return triangles >= max; }
    void v(const PxVec3& p) { fprintf(f, "v %.4f %.4f %.4f\n", p.x + shift.x, p.y + shift.y, p.z + shift.z); }
    void tri(uint64_t a, uint64_t b, uint64_t c) { fprintf(f, "f %llu %llu %llu\n", base + a, base + b, base + c); ++triangles; }
};

// Ground under `at` from collected world-space triangles: highest surface crossing the vertical line below at+0.5.
struct Ground
{
    PxVec3 at;
    bool found = false;
    float z = -1e30f;
    void tri(const PxVec3& a, const PxVec3& b, const PxVec3& c)
    {
        const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::fabs(d) < 1e-9f) return;
        const float l1 = ((b.y - c.y) * (at.x - c.x) + (c.x - b.x) * (at.y - c.y)) / d;
        const float l2 = ((c.y - a.y) * (at.x - c.x) + (a.x - c.x) * (at.y - c.y)) / d;
        const float l3 = 1 - l1 - l2;
        if (l1 < 0 || l2 < 0 || l3 < 0) return;
        const float hz = l1 * a.z + l2 * b.z + l3 * c.z;
        if (hz <= at.z + 0.5f && hz > z) { z = hz; found = true; }
    }
};

float DistanceToBounds(const PxBounds3& b, const PxVec3& p)
{
    const PxVec3 c(PxClamp(p.x, b.minimum.x, b.maximum.x), PxClamp(p.y, b.minimum.y, b.maximum.y), PxClamp(p.z, b.minimum.z, b.maximum.z));
    return (c - p).magnitude();
}

void CountFilter(Census& c, uint32_t w0)
{
    for (uint32_t i = 0; i < c.filterDistinct; ++i)
        if (c.filterWord0[i] == w0) { ++c.filterWord0Count[i]; return; }
    if (c.filterDistinct < 16) { c.filterWord0[c.filterDistinct] = w0; c.filterWord0Count[c.filterDistinct++] = 1; }
}

// Stage B: when set, extracted triangles are also appended here in game world coordinates (physx + shift).
struct TriSink
{
    std::vector<float>* tris;
    std::vector<uint8_t>* flags;
    uint8_t flag;  // 1 = physical layer (simulation filter word0 != 0), 0 = detail
    PxVec3 shift;
    size_t maxTriangles;
};
TriSink* g_sink = nullptr;

void Emit(Obj* o, Ground& g, const std::vector<PxVec3>& wv, const std::vector<uint32_t>& idx, Census& c)
{
    for (size_t t = 0; t + 2 < idx.size(); t += 3)
        if (idx[t] < wv.size() && idx[t + 1] < wv.size() && idx[t + 2] < wv.size())
        {
            g.tri(wv[idx[t]], wv[idx[t + 1]], wv[idx[t + 2]]);
            if (g_sink && g_sink->flags->size() < g_sink->maxTriangles)
            {
                for (int k = 0; k < 3; ++k)
                {
                    const PxVec3 p = wv[idx[t + k]] + g_sink->shift;
                    g_sink->tris->insert(g_sink->tris->end(), {p.x, p.y, p.z});
                }
                g_sink->flags->push_back(g_sink->flag);
            }
        }
    if (!o) return;
    for (auto& p : wv) o->v(p);
    for (size_t t = 0; t + 2 < idx.size() && !o->full(); t += 3) { o->tri(idx[t], idx[t + 1], idx[t + 2]); ++c.trianglesWritten; }
    o->base += wv.size();
}

void TriangleMesh(Obj* o, Ground& g, const PxShape& sh, const PxTransform& pose, Census& c)
{
    PxTriangleMeshGeometry geom;
    if (!sh.getTriangleMeshGeometry(geom) || KindOf(geom.triangleMesh) != Kind::TriMesh) { ++c.skippedMeshes; return; }
    const PxTriangleMesh& m = *geom.triangleMesh;
    const PxU32 nv = m.getNbVertices(), nt = m.getNbTriangles();
    if (nv == 0 || nv > 4'000'000 || nt == 0 || nt > 8'000'000) { ++c.skippedMeshes; return; }
    std::vector<PxVec3> v(nv);
    if (!ReadMem(m.getVertices(), v.data(), sizeof(PxVec3) * nv)) { ++c.skippedMeshes; return; }
    const bool i16 = m.getTriangleMeshFlags() & PxTriangleMeshFlag::e16_BIT_INDICES;
    std::vector<uint32_t> idx(size_t(nt) * 3);
    if (i16)
    {
        std::vector<uint16_t> s(idx.size());
        if (!ReadMem(m.getTriangles(), s.data(), s.size() * 2)) { ++c.skippedMeshes; return; }
        for (size_t i = 0; i < s.size(); ++i) idx[i] = s[i];
    }
    else if (!ReadMem(m.getTriangles(), idx.data(), idx.size() * 4)) { ++c.skippedMeshes; return; }
    if (geom.scale.hasNegativeDeterminant())
        for (size_t t = 0; t < idx.size(); t += 3) std::swap(idx[t + 1], idx[t + 2]);
    const PxMat33 s = geom.scale.toMat33();
    for (auto& p : v) p = pose.transform(s * p);
    Emit(o, g, v, idx, c);
}

void ConvexMesh(Obj* o, Ground& g, const PxShape& sh, const PxTransform& pose, Census& c)
{
    PxConvexMeshGeometry geom;
    if (!sh.getConvexMeshGeometry(geom) || KindOf(geom.convexMesh) != Kind::Convex) { ++c.skippedMeshes; return; }
    const PxConvexMesh& m = *geom.convexMesh;
    const PxU32 nv = m.getNbVertices(), np = m.getNbPolygons();
    if (nv == 0 || nv > 256 || np == 0 || np > 256) { ++c.skippedMeshes; return; }
    std::vector<PxVec3> v(nv);
    if (!ReadMem(m.getVertices(), v.data(), sizeof(PxVec3) * nv)) { ++c.skippedMeshes; return; }
    std::vector<PxHullPolygon> polys(np);
    uint32_t maxIndex = 0;
    for (PxU32 i = 0; i < np; ++i)
    {
        if (!m.getPolygonData(i, polys[i])) { ++c.skippedMeshes; return; }
        maxIndex = std::max<uint32_t>(maxIndex, polys[i].mIndexBase + polys[i].mNbVerts);
    }
    std::vector<uint8_t> ib(maxIndex);
    if (maxIndex && !ReadMem(m.getIndexBuffer(), ib.data(), maxIndex)) { ++c.skippedMeshes; return; }
    std::vector<uint32_t> idx;
    for (auto& p : polys)
        for (PxU32 k = 1; k + 1 < p.mNbVerts; ++k)
            idx.insert(idx.end(), {ib[p.mIndexBase], ib[p.mIndexBase + k], ib[p.mIndexBase + k + 1]});
    const PxMat33 s = geom.scale.toMat33();
    for (auto& p : v) p = pose.transform(s * p);
    Emit(o, g, v, idx, c);
}

void Box(Obj* o, Ground& g, const PxShape& sh, const PxTransform& pose, Census& c)
{
    PxBoxGeometry geom;
    if (!sh.getBoxGeometry(geom)) return;
    const PxVec3 h = geom.halfExtents;
    std::vector<PxVec3> v;
    for (int i = 0; i < 8; ++i) v.push_back(pose.transform(PxVec3(i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z)));
    const std::vector<uint32_t> idx = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    Emit(o, g, v, idx, c);
}

// P1-12 (0.21.0): round shapes as triangles. A ring list along the local x axis (x, radius), each ring `kRound`
// points around; a ring of radius 0 is a pole. Outward winding, so a tube's top faces point up for SK8V's rail finder.
constexpr int kRound = 8;

void Rings(Obj* o, Ground& g, const PxTransform& pose, const std::vector<std::pair<float, float>>& rings, Census& c)
{
    std::vector<PxVec3> v;
    std::vector<uint32_t> idx;
    std::vector<uint32_t> first;
    for (const auto& [x, r] : rings)
    {
        first.push_back((uint32_t)v.size());
        const int n = r > 0.f ? kRound : 1;
        for (int k = 0; k < n; ++k)
        {
            const float a = 6.2831853f * k / kRound;
            v.push_back(pose.transform(PxVec3(x, r * std::cos(a), r * std::sin(a))));
        }
    }
    for (size_t i = 0; i + 1 < rings.size(); ++i)
    {
        const bool poleA = rings[i].second <= 0.f, poleB = rings[i + 1].second <= 0.f;
        for (uint32_t k = 0; k < (uint32_t)kRound; ++k)
        {
            const uint32_t k1 = (k + 1) % kRound;
            const uint32_t a0 = first[i] + (poleA ? 0 : k), a1 = first[i] + (poleA ? 0 : k1);
            const uint32_t b0 = first[i + 1] + (poleB ? 0 : k), b1 = first[i + 1] + (poleB ? 0 : k1);
            if (!poleA) idx.insert(idx.end(), {a0, a1, b1});
            if (!poleB) idx.insert(idx.end(), {a0, b1, b0});
        }
    }
    Emit(o, g, v, idx, c);
}

void Capsule(Obj* o, Ground& g, const PxShape& sh, const PxTransform& pose, Census& c)
{
    PxCapsuleGeometry geom;
    if (!sh.getCapsuleGeometry(geom) || !(geom.radius > 0.005f) || geom.radius > 5.f || !(geom.halfHeight >= 0.f) || geom.halfHeight > 100.f) return;
    const float r = geom.radius, h = geom.halfHeight, d = r * 0.70710678f;
    Rings(o, g, pose, {{-h - r, 0.f}, {-h - d, d}, {-h, r}, {h, r}, {h + d, d}, {h + r, 0.f}}, c);
}

void Sphere(Obj* o, Ground& g, const PxShape& sh, const PxTransform& pose, Census& c)
{
    PxSphereGeometry geom;
    if (!sh.getSphereGeometry(geom) || !(geom.radius > 0.005f) || geom.radius > 10.f) return;
    const float r = geom.radius, d = r * 0.70710678f;
    Rings(o, g, pose, {{-r, 0.f}, {-d, d}, {0.f, r}, {d, d}, {r, 0.f}}, c);
}

void HeightField(Obj* o, Ground& g, const PxShape& sh, const PxTransform& pose, const PxVec3& at, float radius, Census& c)
{
    PxHeightFieldGeometry geom;
    if (!sh.getHeightFieldGeometry(geom) || KindOf(geom.heightField) != Kind::HeightField) { ++c.skippedMeshes; return; }
    const PxHeightField& hf = *geom.heightField;
    const PxU32 rows = hf.getNbRows(), cols = hf.getNbColumns();
    if (rows < 2 || cols < 2 || rows > 8192 || cols > 8192 || hf.getSampleStride() != sizeof(PxHeightFieldSample)) { ++c.skippedMeshes; return; }
    const PxVec3 local = pose.transformInv(at);
    auto clampi = [](float x, PxU32 hi) { return (PxU32)PxClamp(x, 0.f, float(hi)); };
    const PxU32 r0 = clampi(std::floor((local.x - radius) / geom.rowScale), rows - 1), r1 = clampi(std::ceil((local.x + radius) / geom.rowScale), rows - 1);
    const PxU32 c0 = clampi(std::floor((local.z - radius) / geom.columnScale), cols - 1), c1 = clampi(std::ceil((local.z + radius) / geom.columnScale), cols - 1);
    if (r1 <= r0 || c1 <= c0) return;
    const PxHeightFieldSample* base = &hf.getSample(0, 0);  // returns a pointer into the sample array (verified)
    const PxU32 w = c1 - c0 + 1;
    std::vector<PxHeightFieldSample> row(w);
    std::vector<PxHeightFieldSample> cells;
    std::vector<PxVec3> v;
    for (PxU32 r = r0; r <= r1; ++r)
    {
        if (!ReadMem(base + size_t(r) * cols + c0, row.data(), sizeof(PxHeightFieldSample) * w)) { ++c.skippedMeshes; return; }
        cells.insert(cells.end(), row.begin(), row.end());
        for (PxU32 k = 0; k < w; ++k)
            v.push_back(pose.transform(PxVec3(r * geom.rowScale, row[k].height * geom.heightScale, (c0 + k) * geom.columnScale)));
    }
    std::vector<uint32_t> idx;
    const bool mirrored = geom.rowScale * geom.heightScale * geom.columnScale < 0.f;
    for (PxU32 r = 0; r + 1 < r1 - r0 + 1; ++r)
        for (PxU32 k = 0; k + 1 < w; ++k)
        {
            const PxHeightFieldSample& s = cells[size_t(r) * w + k];
            const uint32_t a = r * w + k, b = a + w, cc = a + 1, d = b + 1;
            const bool hole0 = (s.materialIndex0 & 0x7f) == 127, hole1 = (s.materialIndex1 & 0x7f) == 127;
            AppendHeightFieldCell(idx, a, b, cc, d, s.tessFlag() != 0, hole0, hole1, mirrored);  // faces +y (HeightFieldWinding.hpp)
        }
    Emit(o, g, v, idx, c);
}

std::vector<PxActor*> Actors(PxScene& scene)
{
    const PxActorTypeFlags types = PxActorTypeFlag::eRIGID_STATIC | PxActorTypeFlag::eRIGID_DYNAMIC;
    const PxU32 total = scene.getNbActors(types);
    if (total > 2'000'000) return {};
    std::vector<PxActor*> actors(total);
    const PxU32 n = total ? scene.getActors(types, actors.data(), total, 0) : 0;
    actors.resize(std::min(n, total));
    return actors;
}

uint8_t FirstShape(PxRigidActor* ra, float capsule[2])
{
    PxShape* sh[1] = {};
    if (ra->getNbShapes() == 0 || ra->getShapes(sh, 1, 0) != 1 || KindOf(sh[0]) != Kind::Shape) return 255;
    const PxGeometryType::Enum t = sh[0]->getGeometryType();
    PxCapsuleGeometry cg;
    if (t == PxGeometryType::eCAPSULE && sh[0]->getCapsuleGeometry(cg)) capsule[0] = cg.radius, capsule[1] = cg.halfHeight;
    return (uint8_t)t;
}

void Insert(NearActor (&list)[5], const NearActor& a)
{
    for (int i = 0; i < 5; ++i)
    {
        if (!list[i].used) { list[i] = a; list[i].used = true; return; }
        if (a.dist < list[i].dist)
        {
            for (int j = 4; j > i; --j) list[j] = list[j - 1];
            list[i] = a;
            list[i].used = true;
            return;
        }
    }
}

// Pass 1: where things are. Bounds statistics, nearest actors to V (unshifted), and V's own actors via userData.
void Survey(PxScene& scene, uint32_t sceneIndex, const PxVec3& at, const uint64_t* owners, uint32_t nOwners, SceneInfo& info,
            Result& out)
{
    uintptr_t vt = 0;
    ReadMem(&scene, &vt, sizeof vt);
    info.vtableRva = vt >= g_px ? vt - g_px : 0;
    info.flags = (uint32_t)scene.getFlags();
    const PxVec3 grav = scene.getGravity();
    info.gravity[0] = grav.x, info.gravity[1] = grav.y, info.gravity[2] = grav.z;
    info.nbStatic = scene.getNbActors(PxActorTypeFlag::eRIGID_STATIC);
    info.nbDynamic = scene.getNbActors(PxActorTypeFlag::eRIGID_DYNAMIC);
    double sum[3] = {};
    for (int k = 0; k < 3; ++k) info.centerMin[k] = 1e30f, info.centerMax[k] = -1e30f;
    for (PxActor* a : Actors(scene))
    {
        const Kind k = KindOf(a);
        if (k != Kind::RigidStatic && k != Kind::RigidDynamic) { ++info.census.skippedActors; continue; }
        const PxBounds3 b = a->getWorldBounds(1.0f);
        const PxVec3 c = b.getCenter(), e = b.getExtents();
        NearActor na;
        na.dist = (c - at).magnitude();
        na.center[0] = c.x, na.center[1] = c.y, na.center[2] = c.z;
        na.extent[0] = e.x, na.extent[1] = e.y, na.extent[2] = e.z;
        na.isStatic = k == Kind::RigidStatic;
        auto* ra = static_cast<PxRigidActor*>(a);
        na.shapes = (uint16_t)std::min<PxU32>(ra->getNbShapes(), 65535);
        ReadMem(reinterpret_cast<const uint8_t*>(a) + 0x10, &na.userData, sizeof na.userData);  // PxActor::userData
        const bool sane = std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && e.maxElement() < 1e4f;
        if (na.isStatic && sane)
        {
            ++info.staticSeen;
            const float cc[3] = {c.x, c.y, c.z};
            for (int j = 0; j < 3; ++j)
                info.centerMin[j] = std::min(info.centerMin[j], cc[j]), info.centerMax[j] = std::max(info.centerMax[j], cc[j]), sum[j] += cc[j];
        }
        uint32_t depth = 99;
        uint64_t matched = 0;
        for (uint32_t i = 0; i < nOwners; ++i)
            if (na.userData == owners[i]) { depth = 0, matched = owners[i]; break; }
        if (depth == 99 && na.userData > 0x10000 && na.userData < 0x7FFFFFFFFFFFull)
        {
            uint64_t block[32] = {};
            if (ReadMem(reinterpret_cast<const void*>(na.userData), block, sizeof block))
                for (int w = 0; w < 32 && depth == 99; ++w)
                    for (uint32_t i = 0; i < nOwners; ++i)
                        if (block[w] && block[w] == owners[i]) { depth = 1, matched = owners[i]; break; }
        }
        float cap[2] = {};
        na.geom = FirstShape(ra, cap);
        if (depth != 99 && out.nOwners < 8)
        {
            OwnerHit& h = out.owners[out.nOwners++];
            h.scene = sceneIndex, h.depth = depth, h.userData = na.userData, h.matched = matched, h.isStatic = na.isStatic;
            const PxTransform pose = ra->getGlobalPose();
            h.pos[0] = pose.p.x, h.pos[1] = pose.p.y, h.pos[2] = pose.p.z;
            h.geom = na.geom, h.capsule[0] = cap[0], h.capsule[1] = cap[1];
        }
        if (!na.isStatic && na.geom == PxGeometryType::eCAPSULE && sane && g_capsules.size() < 8192)
        {
            const PxTransform pose = ra->getGlobalPose();
            g_capsules.push_back({reinterpret_cast<uint64_t>(a), pose.p, cap[0], cap[1]});
        }
        Insert(na.isStatic ? info.nearestStatic : info.nearestDynamic, na);
    }
    if (info.staticSeen)
        for (int j = 0; j < 3; ++j) info.centerMean[j] = float(sum[j] / info.staticSeen);
}

// P1-12 (0.21.0): what Stage B's gather leaves out near V, for choosing which dynamic actors it should take.
constexpr float kSkippedRadius = 3.f;

// P1-12 (0.21.1): props the game can knock over (bollards, railings, barriers) are kinematic rigid dynamics, held still
// until something hits them. The 0.21.0 probe listing (2026-11-17) showed their simulated shapes with type bit 0x8 in
// the simulation filter's word0 (0x8 for a railing, 0x1c008 for a bollard), where the static world uses 0x4, 0x40 or
// 0x80, V's character capsule 0x1, and query-only shapes 0. Such an actor is taken while it is kinematic and still;
// only its shapes of that type are used. A prop knocked over later stays where it stood until the next collision build.
// 0.21.2 (eighth test): a parking barrier's arm is kinematic with type 0x40 (word0 0x4040, the static world's type
// too), so 0x40 is taken as well; and the 1,000 kg limit, meant to keep cars out, went: it left out the barrier's 4.9 t
// post, and cars carry their own types (0x10 body, 0x4000 kinematic copy, 0x18000 query-only parts; P1-14). A gate or
// door of type 0x40 that moves stays where it was gathered until the next build.
constexpr uint32_t kPropTypes = 0x8 | 0x40, kCharacterType = 0x1;
constexpr float kPropMaxSpeed = 0.05f;

bool PropActor(PxActor* a)
{
    if (KindOf(a) != Kind::RigidDynamic) return false;
    auto* rd = static_cast<PxRigidDynamic*>(a);
    if (!(rd->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC)) return false;
    return rd->getLinearVelocity().magnitude() <= kPropMaxSpeed;
}

bool PropShape(const PxShape& sh)
{
    if (!(sh.getFlags() & PxShapeFlag::eSIMULATION_SHAPE) || (sh.getFlags() & PxShapeFlag::eTRIGGER_SHAPE)) return false;
    const uint32_t w0 = sh.getSimulationFilterData().word0;
    return (w0 & kPropTypes) && !(w0 & kCharacterType);
}

const char* GeomName(PxGeometryType::Enum t)
{
    switch (t)
    {
    case PxGeometryType::eSPHERE: return "sphere";
    case PxGeometryType::ePLANE: return "plane";
    case PxGeometryType::eCAPSULE: return "capsule";
    case PxGeometryType::eBOX: return "box";
    case PxGeometryType::eCONVEXMESH: return "convex";
    case PxGeometryType::eTRIANGLEMESH: return "trimesh";
    case PxGeometryType::eHEIGHTFIELD: return "heightfield";
    default: return "other";
    }
}

void ListSkipped(PxActor* a, PxRigidActor* ra, bool isStatic, const PxTransform& actorPose, PxShape* const* shapes, PxU32 ns,
                 const PxVec3& at, float dist, std::vector<std::string>& out)
{
    // A static actor is listed only for a shape the gather has no triangles for (planes since 0.21.0).
    bool gap = !isStatic;
    for (PxU32 s = 0; s < ns && !gap; ++s)
        if (KindOf(shapes[s]) == Kind::Shape)
        {
            const PxGeometryType::Enum t = shapes[s]->getGeometryType();
            gap = t == PxGeometryType::ePLANE || (unsigned)t > (unsigned)PxGeometryType::eHEIGHTFIELD;
        }
    if (!gap) return;
    char line[512];
    int n = 0;
    const PxBounds3 b = a->getWorldBounds(1.0f);
    const PxVec3 c = b.getCenter() - at, e = b.getExtents();
    if (isStatic) n += snprintf(line + n, sizeof line - n, "static");
    else
    {
        uint32_t taken = 0;
        if (PropActor(a))
            for (PxU32 s = 0; s < ns; ++s) taken += KindOf(shapes[s]) == Kind::Shape && PropShape(*shapes[s]);
        n += snprintf(line + n, sizeof line - n, taken ? "[TAKEN as a prop: %u shape%s] " : "[left out] ", taken, taken == 1 ? "" : "s");
        auto* rd = static_cast<PxRigidDynamic*>(a);
        const bool kinematic = rd->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC;
        const bool asleep = !kinematic && rd->isSleeping();
        n += snprintf(line + n, sizeof line - n, "dynamic%s%s mass %.1f kg, %.2f m/s", kinematic ? " kinematic" : "",
                      asleep ? " asleep" : "", rd->getMass(), rd->getLinearVelocity().magnitude());
    }
    char name[48] = {};
    const char* np = a->getName();
    if (np && ReadMem(np, name, sizeof name - 1)) name[sizeof name - 1] = 0; else name[0] = 0;
    uint64_t userData = 0;
    ReadMem(reinterpret_cast<const uint8_t*>(a) + 0x10, &userData, sizeof userData);  // PxActor::userData
    n += snprintf(line + n, sizeof line - n, "; %.1f m from V, bounds centre %+.2f %+.2f %+.2f from V, half size %.2f %.2f %.2f; name \"%s\"; userData 0x%llx; %u shape%s:",
                  dist, c.x, c.y, c.z, e.x, e.y, e.z, name, (unsigned long long)userData, (unsigned)ns, ns == 1 ? "" : "s");
    for (PxU32 s = 0; s < ns && s < 6 && n < (int)sizeof line - 80; ++s)
    {
        if (KindOf(shapes[s]) != Kind::Shape) { n += snprintf(line + n, sizeof line - n, " ?"); continue; }
        const PxShape& sh = *shapes[s];
        const PxGeometryType::Enum t = sh.getGeometryType();
        const PxShapeFlags f = sh.getFlags();
        const PxFilterData sim = sh.getSimulationFilterData(), q = sh.getQueryFilterData();
        n += snprintf(line + n, sizeof line - n, " [%s", GeomName(t));
        PxBoxGeometry bg; PxCapsuleGeometry cg; PxSphereGeometry sg;
        if (t == PxGeometryType::eBOX && sh.getBoxGeometry(bg)) n += snprintf(line + n, sizeof line - n, " %.2fx%.2fx%.2f", bg.halfExtents.x * 2, bg.halfExtents.y * 2, bg.halfExtents.z * 2);
        if (t == PxGeometryType::eCAPSULE && sh.getCapsuleGeometry(cg)) n += snprintf(line + n, sizeof line - n, " r %.3f len %.2f", cg.radius, cg.halfHeight * 2);
        if (t == PxGeometryType::eSPHERE && sh.getSphereGeometry(sg)) n += snprintf(line + n, sizeof line - n, " r %.3f", sg.radius);
        const PxVec3 lp = (actorPose * sh.getLocalPose()).p - at;
        n += snprintf(line + n, sizeof line - n, " at %+.2f %+.2f %+.2f; sim %x/%x/%x/%x query %x%s%s%s]", lp.x, lp.y, lp.z, sim.word0, sim.word1, sim.word2, sim.word3, q.word0,
                      (f & PxShapeFlag::eSIMULATION_SHAPE) ? " simulated" : "", (f & PxShapeFlag::eSCENE_QUERY_SHAPE) ? " queried" : "",
                      (f & PxShapeFlag::eTRIGGER_SHAPE) ? " trigger" : "");
    }
    out.emplace_back(line);
    (void)ra;
}

// Pass 2: census and OBJ around `at` (PhysX space).
void ScanScene(PxScene& scene, const PxVec3& at, float censusRadius, float dumpRadius, Obj* obj, SceneInfo& info,
               std::vector<std::string>* skipped)
{
    const std::vector<PxActor*> actors = Actors(scene);
    const PxU32 n = (PxU32)actors.size(), total = n;
    Ground ground{at};
    PxShape* shapes[64];
    for (PxU32 i = 0; i < n && i < total; ++i)
    {
        PxActor* a = actors[i];
        const Kind k = KindOf(a);
        if (k != Kind::RigidStatic && k != Kind::RigidDynamic) { ++info.census.skippedActors; continue; }
        const float dist = DistanceToBounds(a->getWorldBounds(1.0f), at);
        if (dist > censusRadius) continue;
        auto* ra = static_cast<PxRigidActor*>(a);
        const bool isStatic = k == Kind::RigidStatic;
        (isStatic ? info.census.staticActors : info.census.dynamicActors)++;
        const PxTransform actorPose = ra->getGlobalPose();
        const PxU32 nbShapes = ra->getNbShapes();
        const PxU32 ns = ra->getShapes(shapes, nbShapes < 64 ? nbShapes : 64, 0);
        if (dist <= kSkippedRadius && skipped && skipped->size() < 60) ListSkipped(a, ra, isStatic, actorPose, shapes, ns, at, dist, *skipped);
        for (PxU32 s = 0; s < ns; ++s)
        {
            if (KindOf(shapes[s]) != Kind::Shape) { ++info.census.skippedShapes; continue; }
            const PxShape& sh = *shapes[s];
            const PxGeometryType::Enum t = sh.getGeometryType();
            if ((unsigned)t < 8) info.census.byType[t]++;
            info.census.shapes++;
            const PxShapeFlags f = sh.getFlags();
            if (f & PxShapeFlag::eSIMULATION_SHAPE) info.census.simulationShapes++;
            if (f & PxShapeFlag::eSCENE_QUERY_SHAPE) info.census.queryShapes++;
            if (f & PxShapeFlag::eTRIGGER_SHAPE) info.census.triggerShapes++;
            const PxFilterData q = sh.getQueryFilterData();
            CountFilter(info.census, q.word0);
            if (!isStatic || dist > dumpRadius || (f & PxShapeFlag::eTRIGGER_SHAPE)) continue;  // static world only
            const PxTransform pose = actorPose * sh.getLocalPose();
            const PxFilterData sim = sh.getSimulationFilterData();
            Obj* o = obj && !obj->full() ? obj : nullptr;
            if (o)
                fprintf(o->f, "o a%u_s%u_%s\n# shapeFlags=0x%x query=%08x,%08x,%08x,%08x sim=%08x,%08x,%08x,%08x\n", i, s,
                        t == PxGeometryType::eTRIANGLEMESH ? "trimesh" : t == PxGeometryType::eHEIGHTFIELD ? "heightfield"
                        : t == PxGeometryType::eCONVEXMESH ? "convex" : t == PxGeometryType::eBOX ? "box"
                        : t == PxGeometryType::eCAPSULE ? "capsule" : t == PxGeometryType::eSPHERE ? "sphere" : "other",
                        (unsigned)(PxU8)f, q.word0, q.word1, q.word2, q.word3, sim.word0, sim.word1, sim.word2, sim.word3);
            switch (t)
            {
            case PxGeometryType::eTRIANGLEMESH: TriangleMesh(o, ground, sh, pose, info.census); break;
            case PxGeometryType::eCONVEXMESH: ConvexMesh(o, ground, sh, pose, info.census); break;
            case PxGeometryType::eBOX: Box(o, ground, sh, pose, info.census); break;
            case PxGeometryType::eHEIGHTFIELD: HeightField(o, ground, sh, pose, at, dumpRadius, info.census); break;
            case PxGeometryType::eCAPSULE: Capsule(o, ground, sh, pose, info.census); break;
            case PxGeometryType::eSPHERE: Sphere(o, ground, sh, pose, info.census); break;
            default: break;
            }
        }
    }
    info.groundFound = ground.found;
    info.groundZ = ground.z;
    info.groundDelta = ground.found ? at.z - ground.z : 0;
}
} // namespace

bool CheckAbi(const char*& why)
{
    if (g_checked) { why = g_why; return g_ok; }
    g_checked = true;
    HMODULE px = GetModuleHandleW(L"PhysX3_x64.dll"), common = GetModuleHandleW(L"PhysX3Common_x64.dll");
    if (!px || !common) g_why = "PhysX modules not loaded";
    else if (Stamp(px) != kPhysXStamp) g_why = "PhysX3_x64.dll is not the verified build (timestamp differs); PhysX probing disabled";
    else if (Stamp(common) != kCommonStamp) g_why = "PhysX3Common_x64.dll is not the verified build; PhysX probing disabled";
    else
    {
        g_px = reinterpret_cast<uintptr_t>(px), g_common = reinterpret_cast<uintptr_t>(common);
        g_ok = Physics() != nullptr;
        g_why = g_ok ? "verified PhysX 3.4.2 (CDPR) build" : "PxGetPhysics unavailable";
    }
    why = g_why;
    return g_ok;
}

bool FindShift(const Vec3& v3, float shift[3], float& error, uint32_t& capsules, uint64_t* capsuleActor)
{
    const char* why;
    error = 1e30f;
    capsules = 0;
    if (!CheckAbi(why)) return false;
    PxPhysics* physics = Physics();
    PxScene* scenes[16] = {};
    const PxU32 n = physics->getScenes(scenes, 16, 0);
    struct Cap { PxVec3 c; float dims; uint64_t actor; };
    std::vector<Cap> caps;  // centre, radius-dims mismatch, actor
    for (PxU32 i = 0; i < n; ++i)
    {
        if (!SceneOk(scenes[i])) continue;
        for (PxActor* a : Actors(*scenes[i]))
        {
            if (KindOf(a) != Kind::RigidDynamic) continue;
            float cap[2] = {};
            auto* ra = static_cast<PxRigidActor*>(a);
            if (FirstShape(ra, cap) != PxGeometryType::eCAPSULE) continue;
            caps.push_back({ra->getGlobalPose().p, std::fabs(cap[0] - 0.32f) + std::fabs(cap[1] - 0.40f), reinterpret_cast<uint64_t>(a)});
        }
    }
    capsules = (uint32_t)caps.size();
    // V's character controller centre sits 1.0 m above V's feet (run 4: capsule r 0.32, hh 0.40, rest offset 0.28).
    // The PhysX origin is on a 1,024 m grid: try each nearby grid shift and keep the best capsule match.
    const PxVec3 v(v3.x, v3.y, v3.z);
    for (int kx = -4; kx <= 4; ++kx)
        for (int ky = -4; ky <= 4; ++ky)
        {
            const PxVec3 s(1024.f * kx, 1024.f * ky, 0.f);
            const PxVec3 predicted = v - s + PxVec3(0, 0, 1.0f);
            for (const auto& c : caps)
            {
                const float e = (c.c - predicted).magnitude() + c.dims;
                if (e < error)
                {
                    error = e;
                    shift[0] = s.x, shift[1] = s.y, shift[2] = s.z;
                    if (capsuleActor) *capsuleActor = c.actor;
                }
            }
        }
    return error < 0.3f;
}

bool ConfirmShift(const Vec3& v3, const float shift[3], uint64_t capsuleActor, float& error)
{
    const char* why;
    error = 1e30f;
    if (!capsuleActor || !CheckAbi(why)) return false;
    auto* a = reinterpret_cast<PxActor*>(capsuleActor);
    if (KindOf(a) != Kind::RigidDynamic) return false;  // freed or reused: the vtable no longer matches
    float cap[2] = {};
    auto* ra = static_cast<PxRigidActor*>(a);
    if (FirstShape(ra, cap) != PxGeometryType::eCAPSULE) return false;
    const PxVec3 predicted = PxVec3(v3.x, v3.y, v3.z) - PxVec3(shift[0], shift[1], shift[2]) + PxVec3(0, 0, 1.0f);
    error = (ra->getGlobalPose().p - predicted).magnitude() + std::fabs(cap[0] - 0.32f) + std::fabs(cap[1] - 0.40f);
    return error < 0.3f;
}

namespace
{
double NowMs()
{
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

// A triangle mesh or height field found by Scan, copied later by Extract. Everything here was read through verified
// virtuals on the main thread; Extract touches the pointed-to memory only through ReadProcessMemory.
struct ShapeJob
{
    bool field = false;
    uint8_t physical = 1;
    bool i16 = false, flip = false, mirrored = false;
    const void* object = nullptr;
    uintptr_t vtable = 0;
    const void* vertices = nullptr;  // mesh: PxVec3[nv]; field: first PxHeightFieldSample
    const void* indices = nullptr;
    uint32_t nv = 0, nt = 0;
    uint32_t cols = 0, r0 = 0, r1 = 0, c0 = 0, c1 = 0;
    float rowScale = 0, heightScale = 0, columnScale = 0;
    PxTransform pose;
    PxMat33 scale;
};
} // namespace

struct Collector::Impl
{
    PxVec3 at{0, 0, 0}, shift{0, 0, 0};
    float radius = 0;
    size_t maxTriangles = 0;
    bool done = false;
    std::unordered_set<const void*> visited;
    std::vector<std::pair<PxScene*, size_t>> cursors;  // scene, next index into its current actor list
    std::vector<ShapeJob> jobs;
    std::vector<float> ready;  // boxes and convexes, already in game world coordinates
    std::vector<uint8_t> readyFlags;

    void Visit(PxActor* a, Stats& st)
    {
        const Kind kind = KindOf(a);
        if (kind != Kind::RigidStatic && kind != Kind::RigidDynamic) return;
        if (DistanceToBounds(a->getWorldBounds(1.0f), at) > radius) return;
        // P1-12 (0.21.1): of the dynamic actors, kinematic props only (`PropActor`), and only their prop shapes.
        const bool prop = kind == Kind::RigidDynamic;
        if (prop && !PropActor(a)) return;
        ++st.actorsNear;
        if (prop) ++st.props;
        auto* ra = static_cast<PxRigidActor*>(a);
        const PxTransform actorPose = ra->getGlobalPose();
        PxShape* shapes[64];
        const PxU32 nb = ra->getNbShapes();
        const PxU32 ns = ra->getShapes(shapes, nb < 64 ? nb : 64, 0);
        TriSink sink{&ready, &readyFlags, 1, shift, maxTriangles};
        Census census;
        Ground ground{at};
        for (PxU32 s = 0; s < ns; ++s)
        {
            if (KindOf(shapes[s]) != Kind::Shape) continue;
            const PxShape& sh = *shapes[s];
            if (sh.getFlags() & PxShapeFlag::eTRIGGER_SHAPE) continue;
            if (prop && !PropShape(sh)) continue;
            if (prop) ++st.propShapes;
            ++st.shapes;
            const uint8_t physical = sh.getSimulationFilterData().word0 != 0 ? 1 : 0;
            const PxTransform pose = actorPose * sh.getLocalPose();
            switch (sh.getGeometryType())
            {
            case PxGeometryType::eTRIANGLEMESH:
            {
                PxTriangleMeshGeometry geom;
                if (!sh.getTriangleMeshGeometry(geom) || KindOf(geom.triangleMesh) != Kind::TriMesh) break;
                const PxTriangleMesh& m = *geom.triangleMesh;
                ShapeJob j;
                j.physical = physical;
                j.nv = m.getNbVertices(), j.nt = m.getNbTriangles();
                if (j.nv == 0 || j.nv > 4'000'000 || j.nt == 0 || j.nt > 8'000'000) break;
                j.object = &m;
                ReadMem(&m, &j.vtable, sizeof j.vtable);
                j.vertices = m.getVertices(), j.indices = m.getTriangles();
                j.i16 = m.getTriangleMeshFlags() & PxTriangleMeshFlag::e16_BIT_INDICES;
                j.flip = geom.scale.hasNegativeDeterminant();
                j.pose = pose, j.scale = geom.scale.toMat33();
                jobs.push_back(j);
                break;
            }
            case PxGeometryType::eHEIGHTFIELD:
            {
                PxHeightFieldGeometry geom;
                if (!sh.getHeightFieldGeometry(geom) || KindOf(geom.heightField) != Kind::HeightField) break;
                const PxHeightField& hf = *geom.heightField;
                const PxU32 rows = hf.getNbRows(), cols = hf.getNbColumns();
                if (rows < 2 || cols < 2 || rows > 8192 || cols > 8192 || hf.getSampleStride() != sizeof(PxHeightFieldSample)) break;
                const PxVec3 local = pose.transformInv(at);
                auto clampi = [](float x, PxU32 hi) { return (PxU32)PxClamp(x, 0.f, float(hi)); };
                ShapeJob j;
                j.field = true, j.physical = physical;
                j.r0 = clampi(std::floor((local.x - radius) / geom.rowScale), rows - 1), j.r1 = clampi(std::ceil((local.x + radius) / geom.rowScale), rows - 1);
                j.c0 = clampi(std::floor((local.z - radius) / geom.columnScale), cols - 1), j.c1 = clampi(std::ceil((local.z + radius) / geom.columnScale), cols - 1);
                if (j.r1 <= j.r0 || j.c1 <= j.c0) break;
                j.object = &hf;
                ReadMem(&hf, &j.vtable, sizeof j.vtable);
                j.vertices = &hf.getSample(0, 0);  // a pointer into the sample array (verified)
                j.cols = cols;
                j.rowScale = geom.rowScale, j.heightScale = geom.heightScale, j.columnScale = geom.columnScale;
                j.mirrored = geom.rowScale * geom.heightScale * geom.columnScale < 0.f;
                j.pose = pose;
                jobs.push_back(j);
                break;
            }
            case PxGeometryType::eCONVEXMESH:  // small (at most 256 polygons); needs getPolygonData, so done here
                sink.flag = physical;
                g_sink = &sink;
                ConvexMesh(nullptr, ground, sh, pose, census);
                g_sink = nullptr;
                break;
            case PxGeometryType::eBOX:
                sink.flag = physical;
                g_sink = &sink;
                Box(nullptr, ground, sh, pose, census);
                g_sink = nullptr;
                break;
            // P1-12 (0.21.0): thin round rails and poles are capsules; static ones are world geometry, never characters.
            case PxGeometryType::eCAPSULE:
            case PxGeometryType::eSPHERE:
                sink.flag = physical;
                g_sink = &sink;
                if (sh.getGeometryType() == PxGeometryType::eCAPSULE) Capsule(nullptr, ground, sh, pose, census);
                else Sphere(nullptr, ground, sh, pose, census);
                g_sink = nullptr;
                break;
            default: break;
            }
        }
    }
};

Collector::Collector() : impl(new Impl) {}
Collector::~Collector() { delete impl; }
Collector::Collector(Collector&& o) noexcept : stats(o.stats), impl(o.impl) { o.impl = nullptr; }
Collector& Collector::operator=(Collector&& o) noexcept
{
    if (this != &o) { delete impl; impl = o.impl; stats = o.stats; o.impl = nullptr; }
    return *this;
}

void Collector::Begin(const Vec3& centerWorld, const float shift3[3], float radius, size_t maxTriangles)
{
    *impl = Impl{};
    stats = Stats{};
    impl->shift = PxVec3(shift3[0], shift3[1], shift3[2]);
    impl->at = PxVec3(centerWorld.x, centerWorld.y, centerWorld.z) - impl->shift;
    impl->radius = radius;
    impl->maxTriangles = maxTriangles;
    impl->visited.reserve(1 << 15);  // ~19k actors in a loaded district (Glen, H10)
}

bool Collector::Scan(double budgetMs)
{
    if (impl->done) return true;
    const char* why;
    if (!CheckAbi(why)) { impl->done = true; return true; }
    const double t0 = NowMs();
    ++stats.frames;
    PxScene* scenes[16] = {};
    const PxU32 n = Physics()->getScenes(scenes, 16, 0);
    bool outOfTime = false;
    for (PxU32 i = 0; i < n && !outOfTime; ++i)
    {
        if (!SceneOk(scenes[i])) continue;
        auto it = std::find_if(impl->cursors.begin(), impl->cursors.end(), [&](const auto& c) { return c.first == scenes[i]; });
        if (it == impl->cursors.end()) it = impl->cursors.insert(impl->cursors.end(), {scenes[i], 0});
        const std::vector<PxActor*> actors = Actors(*scenes[i]);  // fresh each frame: earlier pointers may be stale
        size_t& c = it->second;
        while (c < actors.size())
        {
            PxActor* a = actors[c++];
            if (impl->visited.insert(a).second) { ++stats.actorsVisited; impl->Visit(a, stats); }
            if ((c & 31) == 0 && NowMs() - t0 > budgetMs) { outOfTime = true; break; }
        }
    }
    if (!outOfTime)
    {
        // Every list walked once. Final pass, in this same frame: actors that moved into an already-walked index
        // between frames (PhysX removes an actor by swapping the last one into its slot) and actors added meanwhile.
        // Only those are new to `visited`, so this costs a set lookup per actor.
        for (PxU32 i = 0; i < n; ++i)
            if (SceneOk(scenes[i]))
                for (PxActor* a : Actors(*scenes[i]))
                    if (impl->visited.insert(a).second) { ++stats.actorsVisited; impl->Visit(a, stats); }
        impl->done = true;
    }
    const double ms = NowMs() - t0;
    stats.scanMs += ms;
    stats.scanMaxMs = std::max(stats.scanMaxMs, ms);
    return impl->done;
}

// ----- P1-14 (0.22.0): cars -----
namespace
{
constexpr uint32_t kVehicleType = 0x10;          // simulation filter word0 of a car's body and wheels (probe 3, 2026-11-17)
constexpr size_t kDiscoverPerFrame = 256;        // dynamic actors looked at per frame for new cars
constexpr float kForgetBeyond = 10.f;            // a tracked car further than radius + this is forgotten
constexpr size_t kMaxBodyTriangles = 1024;       // per car

bool VehicleShape(const PxShape& sh)
{
    const PxShapeFlags f = sh.getFlags();
    if (!(f & PxShapeFlag::eSIMULATION_SHAPE) || (f & PxShapeFlag::eTRIGGER_SHAPE)) return false;
    return (sh.getSimulationFilterData().word0 & kVehicleType) != 0;
}
} // namespace

struct Vehicles::Impl
{
    struct Tracked
    {
        uint32_t id = 0;
        std::vector<float> local;  // triangles in the actor's frame
    };
    std::unordered_map<PxActor*, Tracked> tracked;
    size_t cursor = 0;
    uint32_t nextId = 1;
};

Vehicles::Vehicles() : impl(new Impl) {}
Vehicles::~Vehicles() { delete impl; }
void Vehicles::Clear() { *impl = Impl{}; }

void Vehicles::Update(const Vec3& centerWorld, const float shift3[3], float radius, uint32_t maxBodies, std::vector<MovingBody>& bodies,
                      std::vector<float>& tris, Stats& st)
{
    const double t0 = NowMs();
    bodies.clear();
    tris.clear();
    st = Stats{};
    const char* why;
    if (!CheckAbi(why)) return;
    Impl& m = *impl;
    const PxVec3 shift(shift3[0], shift3[1], shift3[2]);
    const PxVec3 at = PxVec3(centerWorld.x, centerWorld.y, centerWorld.z) - shift;
    // This frame's dynamic actors in every scene: only these may be called.
    std::vector<PxActor*> all;
    PxScene* scenes[16] = {};
    const PxU32 n = Physics()->getScenes(scenes, 16, 0);
    for (PxU32 i = 0; i < n && i < 16; ++i)
    {
        if (!SceneOk(scenes[i])) continue;
        const PxU32 total = scenes[i]->getNbActors(PxActorTypeFlag::eRIGID_DYNAMIC);
        if (total == 0 || total > 200'000) continue;
        const size_t base = all.size();
        all.resize(base + total);
        const PxU32 got = scenes[i]->getActors(PxActorTypeFlag::eRIGID_DYNAMIC, all.data() + base, total, 0);
        all.resize(base + std::min(got, total));
    }
    const std::unordered_set<PxActor*> present(all.begin(), all.end());
    for (auto it = m.tracked.begin(); it != m.tracked.end();)
        if (!present.count(it->first) || KindOf(it->first) != Kind::RigidDynamic) { it = m.tracked.erase(it); ++st.dropped; }
        else ++it;
    // New cars: a slice of the list per frame.
    for (size_t k = 0; k < kDiscoverPerFrame && !all.empty(); ++k)
    {
        m.cursor = (m.cursor + 1) % all.size();
        PxActor* a = all[m.cursor];
        if (m.tracked.count(a) || KindOf(a) != Kind::RigidDynamic) continue;
        auto* rd = static_cast<PxRigidDynamic*>(a);
        if (rd->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) continue;
        if (DistanceToBounds(a->getWorldBounds(1.0f), at) > radius) continue;
        PxShape* shapes[64];
        const PxU32 nb = rd->getNbShapes();
        const PxU32 ns = rd->getShapes(shapes, nb < 64 ? nb : 64, 0);
        std::vector<float> local;
        std::vector<uint8_t> flags;
        TriSink sink{&local, &flags, 1, PxVec3(0, 0, 0), kMaxBodyTriangles};
        Ground ground{PxVec3(0, 0, -1e9f)};
        Census census;
        for (PxU32 s = 0; s < ns; ++s)
        {
            if (KindOf(shapes[s]) != Kind::Shape || !VehicleShape(*shapes[s])) continue;
            const PxShape& sh = *shapes[s];
            const PxTransform pose = sh.getLocalPose();  // the actor's frame
            g_sink = &sink;
            switch (sh.getGeometryType())
            {
            case PxGeometryType::eBOX: Box(nullptr, ground, sh, pose, census); break;
            case PxGeometryType::eCONVEXMESH: ConvexMesh(nullptr, ground, sh, pose, census); break;
            case PxGeometryType::eCAPSULE: Capsule(nullptr, ground, sh, pose, census); break;
            case PxGeometryType::eSPHERE: Sphere(nullptr, ground, sh, pose, census); break;
            default: break;
            }
            g_sink = nullptr;
        }
        if (local.empty()) continue;
        m.tracked[a] = Impl::Tracked{m.nextId++ & 0x0fffffffu, std::move(local)};
        ++st.found;
    }
    // Every tracked car near enough: pose and dynamics.
    std::vector<std::pair<float, PxActor*>> order;
    for (auto it = m.tracked.begin(); it != m.tracked.end();)
    {
        const float d = DistanceToBounds(it->first->getWorldBounds(1.0f), at);
        if (!(d <= radius + kForgetBeyond)) { it = m.tracked.erase(it); ++st.dropped; continue; }
        if (d <= radius) order.push_back({d, it->first});
        ++it;
    }
    std::sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    if (order.size() > maxBodies) order.resize(maxBodies);
    for (const auto& [d, a] : order)
    {
        auto* rd = static_cast<PxRigidDynamic*>(a);
        const Impl::Tracked& t = m.tracked[a];
        const PxTransform pose = rd->getGlobalPose();
        const PxTransform cm = pose * rd->getCMassLocalPose();
        const PxVec3 lin = rd->getLinearVelocity(), ang = rd->getAngularVelocity(), inv = rd->getMassSpaceInvInertiaTensor();
        MovingBody b;
        b.id = t.id, b.kind = 0, b.dist = d;
        b.com[0] = cm.p.x + shift.x, b.com[1] = cm.p.y + shift.y, b.com[2] = cm.p.z + shift.z;
        b.comRot[0] = cm.q.x, b.comRot[1] = cm.q.y, b.comRot[2] = cm.q.z, b.comRot[3] = cm.q.w;
        b.linVel[0] = lin.x, b.linVel[1] = lin.y, b.linVel[2] = lin.z;
        b.angVel[0] = ang.x, b.angVel[1] = ang.y, b.angVel[2] = ang.z;
        b.invMass = rd->getInvMass();
        b.invInertia[0] = inv.x, b.invInertia[1] = inv.y, b.invInertia[2] = inv.z;
        b.firstTri = uint32_t(tris.size() / 9);
        for (size_t i = 0; i + 2 < t.local.size(); i += 3)
        {
            const PxVec3 w = pose.transform(PxVec3(t.local[i], t.local[i + 1], t.local[i + 2])) + shift;
            tris.insert(tris.end(), {w.x, w.y, w.z});
        }
        b.triCount = uint32_t(tris.size() / 9) - b.firstTri;
        bodies.push_back(b);
    }
    st.tracked = uint32_t(m.tracked.size());
    st.ms = NowMs() - t0;
}

void Collector::Extract(std::vector<float>& tris, std::vector<uint8_t>& flags)
{
    const double t0 = NowMs();
    Impl& m = *impl;
    tris = std::move(m.ready);
    flags = std::move(m.readyFlags);
    stats.meshJobs = (uint32_t)m.jobs.size();
    std::vector<PxVec3> v;
    std::vector<uint32_t> idx;
    std::vector<uint16_t> idx16;
    std::vector<PxHeightFieldSample> cells;
    auto emit = [&](uint8_t physical) {
        for (size_t t = 0; t + 2 < idx.size() && flags.size() < m.maxTriangles; t += 3)
        {
            if (idx[t] >= v.size() || idx[t + 1] >= v.size() || idx[t + 2] >= v.size()) continue;
            for (int k = 0; k < 3; ++k)
            {
                const PxVec3 p = v[idx[t + k]] + m.shift;
                tris.insert(tris.end(), {p.x, p.y, p.z});
            }
            flags.push_back(physical);
        }
    };
    auto alive = [](const ShapeJob& j) { uintptr_t vt = 0; return ReadMem(j.object, &vt, sizeof vt) && vt == j.vtable; };
    for (const ShapeJob& j : m.jobs)
    {
        if (flags.size() >= m.maxTriangles) break;
        if (!j.field)
        {
            v.resize(j.nv);
            idx.resize(size_t(j.nt) * 3);
            bool ok = ReadMem(j.vertices, v.data(), sizeof(PxVec3) * j.nv);
            if (ok && j.i16)
            {
                idx16.resize(idx.size());
                ok = ReadMem(j.indices, idx16.data(), idx16.size() * 2);
                for (size_t i = 0; ok && i < idx16.size(); ++i) idx[i] = idx16[i];
            }
            else if (ok) ok = ReadMem(j.indices, idx.data(), idx.size() * 4);
            if (!ok || !alive(j)) { ++stats.stale; continue; }  // freed while we copied: skip it
            if (j.flip)
                for (size_t t = 0; t < idx.size(); t += 3) std::swap(idx[t + 1], idx[t + 2]);
            for (auto& p : v) p = j.pose.transform(j.scale * p);
            emit(j.physical);
        }
        else
        {
            const auto* base = static_cast<const PxHeightFieldSample*>(j.vertices);
            const uint32_t w = j.c1 - j.c0 + 1;
            std::vector<PxHeightFieldSample> row(w);
            cells.clear();
            v.clear();
            bool ok = true;
            for (uint32_t r = j.r0; r <= j.r1 && ok; ++r)
            {
                ok = ReadMem(base + size_t(r) * j.cols + j.c0, row.data(), sizeof(PxHeightFieldSample) * w);
                cells.insert(cells.end(), row.begin(), row.end());
                for (uint32_t k = 0; ok && k < w; ++k)
                    v.push_back(j.pose.transform(PxVec3(r * j.rowScale, row[k].height * j.heightScale, (j.c0 + k) * j.columnScale)));
            }
            if (!ok || !alive(j)) { ++stats.stale; continue; }
            idx.clear();
            for (uint32_t r = 0; r + 1 < j.r1 - j.r0 + 1; ++r)
                for (uint32_t k = 0; k + 1 < w; ++k)
                {
                    const PxHeightFieldSample& s = cells[size_t(r) * w + k];
                    const uint32_t a = r * w + k, b = a + w, cc = a + 1, d = b + 1;
                    const bool hole0 = (s.materialIndex0 & 0x7f) == 127, hole1 = (s.materialIndex1 & 0x7f) == 127;
                    AppendHeightFieldCell(idx, a, b, cc, d, s.tessFlag() != 0, hole0, hole1, j.mirrored);  // faces +y
                }
            emit(j.physical);
        }
    }
    m.jobs.clear();
    stats.extractMs = NowMs() - t0;
}

uint32_t CollectTriangles(const Vec3& centerWorld, const float shift3[3], float radius, size_t maxTriangles,
                          std::vector<float>& tris, std::vector<uint8_t>& flags, Collector::Stats* stats)
{
    Collector c;
    c.Begin(centerWorld, shift3, radius, maxTriangles);
    c.Scan(1e9);
    c.Extract(tris, flags);
    if (stats) *stats = c.stats;
    return (uint32_t)flags.size();
}

uint32_t StaticActorCount()
{
    const char* why;
    if (!CheckAbi(why)) return 0;
    PxPhysics* physics = Physics();
    PxScene* scenes[16] = {};
    const PxU32 n = physics->getScenes(scenes, 16, 0);
    uint32_t total = 0;
    for (PxU32 i = 0; i < n; ++i)
        if (SceneOk(scenes[i])) total += scenes[i]->getNbActors(PxActorTypeFlag::eRIGID_STATIC);
    return total;
}

void ActorCounts(uint32_t& statics, uint32_t& dynamics)
{
    statics = dynamics = 0;
    const char* why;
    if (!CheckAbi(why)) return;
    PxScene* scenes[16] = {};
    const PxU32 n = Physics()->getScenes(scenes, 16, 0);
    for (PxU32 i = 0; i < n && i < 16; ++i)
        if (SceneOk(scenes[i]))
            statics += scenes[i]->getNbActors(PxActorTypeFlag::eRIGID_STATIC), dynamics += scenes[i]->getNbActors(PxActorTypeFlag::eRIGID_DYNAMIC);
}

void Run(const Vec3& at3, float censusRadius, float dumpRadius, uint64_t maxTriangles, FILE* objFile, const uint64_t* owners,
         uint32_t nOwners, Result& out)
{
    out = Result{};
    out.abiOk = CheckAbi(out.abiWhy);
    if (!out.abiOk) return;
    PxPhysics* physics = Physics();
    out.nbScenes = physics->getNbScenes();
    PxScene* scenes[16] = {};
    const PxU32 n = physics->getScenes(scenes, 16, 0);
    const PxVec3 v(at3.x, at3.y, at3.z);
    g_capsules.clear();
    for (PxU32 i = 0; i < n && i < 16; ++i)
        if (SceneOk(scenes[i])) Survey(*scenes[i], i, v, owners, nOwners, out.scenes[i], out);
    // Rough shift from the busiest scene's streaming centroid (x, y only; z assumed unshifted).
    uint32_t busiest = 0;
    for (PxU32 i = 1; i < n && i < 16; ++i)
        if (out.scenes[i].staticSeen > out.scenes[busiest].staticSeen) busiest = i;
    PxVec3 shift(0, 0, 0);
    if (out.scenes[busiest].staticSeen)
        shift = PxVec3(v.x - out.scenes[busiest].centerMean[0], v.y - out.scenes[busiest].centerMean[1], 0);
    const PxVec3 predicted = v - shift;
    // Exact shift: the capsule whose displacement since the previous probe equals V's (V must have walked > 2 m).
    const PxVec3 dV = v - g_prevV;
    out.vDisplacement = g_havePrev ? dV.magnitude() : 0;
    std::vector<std::pair<float, size_t>> order;
    for (size_t i = 0; i < g_capsules.size(); ++i) order.push_back({(g_capsules[i].pos - predicted).magnitude(), i});
    std::sort(order.begin(), order.end());
    float bestError = 1e30f;
    size_t best = SIZE_MAX;
    if (g_havePrev && out.vDisplacement > 2.f)
        for (size_t i = 0; i < g_capsules.size(); ++i)
            for (const auto& p : g_prevCapsules)
                if (p.actor == g_capsules[i].actor)
                {
                    const float err = ((g_capsules[i].pos - p.pos) - dV).magnitude();
                    if (err < bestError) bestError = err, best = i;
                    break;
                }
    auto addCapsule = [&](size_t i, float dist) {
        if (out.nCapsules >= 8) return -1;
        auto& c = out.capsules[out.nCapsules];
        c.actor = g_capsules[i].actor;
        c.pos[0] = g_capsules[i].pos.x, c.pos[1] = g_capsules[i].pos.y, c.pos[2] = g_capsules[i].pos.z;
        c.radius = g_capsules[i].radius, c.halfHeight = g_capsules[i].halfHeight, c.distToPredicted = dist;
        for (const auto& p : g_prevCapsules)
            if (p.actor == c.actor) { c.motionError = ((g_capsules[i].pos - p.pos) - dV).magnitude(); break; }
        return (int)out.nCapsules++;
    };
    if (best != SIZE_MAX && bestError < 0.5f)
    {
        out.matchedCapsule = addCapsule(best, (g_capsules[best].pos - predicted).magnitude());
        const auto& c = g_capsules[best];
        // capsule axis is vertical for a character controller: feet = centre - (halfHeight + radius) in z
        shift = v - PxVec3(c.pos.x, c.pos.y, c.pos.z - c.halfHeight - c.radius);
        out.shiftFromOwner = true;
    }
    for (size_t k = 0; k < order.size() && out.nCapsules < 8; ++k)
        if (!(out.shiftFromOwner && order[k].second == best)) addCapsule(order[k].second, order[k].first);
    g_prevCapsules = g_capsules;
    g_prevV = v;
    g_havePrev = true;
    out.shift[0] = shift.x, out.shift[1] = shift.y, out.shift[2] = shift.z;
    const PxVec3 q = v - shift;
    out.probeAt[0] = q.x, out.probeAt[1] = q.y, out.probeAt[2] = q.z;
    Obj obj{objFile, 1, 0, maxTriangles, shift};
    for (PxU32 i = 0; i < n && i < 16; ++i)
        if (SceneOk(scenes[i])) ScanScene(*scenes[i], q, censusRadius, dumpRadius, objFile ? &obj : nullptr, out.scenes[i], &out.skippedNear);
}
} // namespace probe
