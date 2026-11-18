// Offline ABI proof: the vtable offsets this compiler uses for every PhysX method the probe calls must equal the
// offsets read from the game's own PhysX3_x64.dll / PhysX3Common_x64.dll (investigation/probes/physx_vtables.py).
// MSVC x64 pointers-to-virtual-member point at a vcall thunk: `mov rax,[rcx]; jmp qword ptr [rax+disp]`.
#include <PxPhysics.h>
#include <PxRigidActor.h>
#include <PxScene.h>
#include <PxShape.h>
#include <geometry/PxConvexMesh.h>
#include <geometry/PxHeightField.h>
#include <geometry/PxTriangleMesh.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace physx;

template<class M> int Offset(M m)
{
    void* p;
    std::memcpy(&p, &m, sizeof p);
    auto b = static_cast<const uint8_t*>(p);
    if (b[0] == 0xE9) b = b + 5 + *reinterpret_cast<const int32_t*>(b + 1);  // incremental-link jump
    if (b[0] != 0x48 || b[1] != 0x8B || b[2] != 0x01 || b[3] != 0xFF) return -1;  // mov rax,[rcx]; jmp ...
    if (b[4] == 0x20) return 0;
    if (b[4] == 0x60) return b[5];
    if (b[4] == 0xA0) return *reinterpret_cast<const int32_t*>(b + 5);
    return -2;
}

int failures = 0;
template<class M> void Expect(const char* name, M m, int want)
{
    const int got = Offset(m);
    const bool ok = want < 0 || got == want;
    if (!ok) ++failures;
    printf("%-4s %-44s offset 0x%03x%s\n", ok ? "ok" : "FAIL", name, got, want < 0 ? "  (not verified in binary; logged only)" : "");
    if (!ok) printf("      expected 0x%03x from the game binary\n", want);
}

int main()
{
    // PxShape: CDPR build has getReferenceCount at 0x30, so everything from getGeometryType is one slot later.
    Expect("PxShape::getGeometryType", &PxShape::getGeometryType, 0x38);
    Expect("PxShape::getBoxGeometry", &PxShape::getBoxGeometry, 0x50);
    Expect("PxShape::getCapsuleGeometry", &PxShape::getCapsuleGeometry, 0x60);
    Expect("PxShape::getConvexMeshGeometry", &PxShape::getConvexMeshGeometry, 0x70);
    Expect("PxShape::getTriangleMeshGeometry", &PxShape::getTriangleMeshGeometry, 0x78);
    Expect("PxShape::getHeightFieldGeometry", &PxShape::getHeightFieldGeometry, 0x80);
    Expect("PxShape::getLocalPose", &PxShape::getLocalPose, 0x98);
    Expect("PxShape::getSimulationFilterData", &PxShape::getSimulationFilterData, 0xA8);
    Expect("PxShape::getQueryFilterData", &PxShape::getQueryFilterData, 0xB8);
    Expect("PxShape::getFlags", &PxShape::getFlags, 0x110);
    // PxActor / PxRigidActor (NpRigidStatic 0x20b980, NpRigidDynamic 0x20b2e8)
    Expect("PxActor::getType", &PxActor::getType, 0x28);
    Expect("PxActor::getWorldBounds", &PxActor::getWorldBounds, 0x48);
    Expect("PxRigidActor::getGlobalPose", &PxRigidActor::getGlobalPose, 0xA0);
    Expect("PxRigidActor::getNbShapes", &PxRigidActor::getNbShapes, 0xC8);
    Expect("PxRigidActor::getShapes", &PxRigidActor::getShapes, 0xD0);
    // Meshes (PhysX3Common_x64.dll)
    Expect("PxTriangleMesh::getNbVertices", &PxTriangleMesh::getNbVertices, 0x28);
    Expect("PxTriangleMesh::getVertices", &PxTriangleMesh::getVertices, 0x30);
    Expect("PxTriangleMesh::getNbTriangles", &PxTriangleMesh::getNbTriangles, 0x48);
    Expect("PxTriangleMesh::getTriangles", &PxTriangleMesh::getTriangles, 0x50);
    Expect("PxTriangleMesh::getTriangleMeshFlags", &PxTriangleMesh::getTriangleMeshFlags, 0x58);
    Expect("PxConvexMesh::getNbVertices", &PxConvexMesh::getNbVertices, 0x28);
    Expect("PxConvexMesh::getVertices", &PxConvexMesh::getVertices, 0x30);
    Expect("PxConvexMesh::getIndexBuffer", &PxConvexMesh::getIndexBuffer, 0x38);
    Expect("PxConvexMesh::getNbPolygons", &PxConvexMesh::getNbPolygons, 0x40);
    Expect("PxConvexMesh::getPolygonData", &PxConvexMesh::getPolygonData, 0x48);
    Expect("PxHeightField::getNbRows", &PxHeightField::getNbRows, 0x38);
    Expect("PxHeightField::getNbColumns", &PxHeightField::getNbColumns, 0x40);
    Expect("PxHeightField::getSampleStride", &PxHeightField::getSampleStride, 0x50);
    Expect("PxHeightField::getSample", &PxHeightField::getSample, 0x98);
    // Not anchored in the binary (no type-name string): consistent results in run 1; logged for the record.
    Expect("PxPhysics::getNbScenes", &PxPhysics::getNbScenes, -1);
    Expect("PxPhysics::getScenes", &PxPhysics::getScenes, -1);
    Expect("PxScene::getFlags", &PxScene::getFlags, -1);
    Expect("PxScene::getGravity", &PxScene::getGravity, -1);
    Expect("PxScene::getNbActors", &PxScene::getNbActors, -1);
    Expect("PxScene::getActors", &PxScene::getActors, -1);
    printf("%s (%d failures)\n", failures ? "ABI MISMATCH" : "all verified offsets match the game binary", failures);
    return failures ? 1 : 0;
}
