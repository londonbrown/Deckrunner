// Offline check (not shipped): heightfield cells must face +y in PhysX heightfield space, for both diagonals, with
// holes, and under a mirrored scale. Probe 0.6.0 and earlier failed it: Night City terrain faced down, and the skater
// fell through it (in-game Stage B, Glen).
#include "../src/HeightFieldWinding.hpp"

#include <cstdio>

struct V3 { float x, y, z; };

static float NormalY(const V3& p0, const V3& p1, const V3& p2)
{
    const V3 e1{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z}, e2{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
    return e1.z * e2.x - e1.x * e2.z;  // y component of cross(e1, e2)
}

int main()
{
    int failures = 0, checked = 0;
    for (float rowScale : {0.5f, -0.5f})
        for (bool tess : {false, true})
            for (int holes = 0; holes < 4; ++holes)
            {
                // Corners a, b, c, d in shape space: rows along x (scaled), columns along z, uneven heights.
                const V3 v[4] = {{0, 5.70f, 0}, {rowScale, 5.71f, 0}, {0, 5.69f, 0.5f}, {rowScale, 5.73f, 0.5f}};
                std::vector<uint32_t> idx;
                probe::AppendHeightFieldCell(idx, 0, 1, 2, 3, tess, holes & 1, holes & 2, rowScale < 0);
                const size_t expected = 3 * (!(holes & 1) + !(holes & 2));
                if (idx.size() != expected) { printf("FAIL: %zu indices, expected %zu\n", idx.size(), expected); ++failures; }
                for (size_t t = 0; t + 2 < idx.size(); t += 3)
                {
                    // With a mirrored row scale the shape's +y side is still up, so the normal must stay +y.
                    const float ny = NormalY(v[idx[t]], v[idx[t + 1]], v[idx[t + 2]]);
                    ++checked;
                    if (!(ny > 0)) { printf("FAIL: rowScale %.1f tess %d holes %d triangle %zu normal y %.4f\n", rowScale, tess, holes, t / 3, ny); ++failures; }
                }
            }
    printf("%s: %d triangles checked, %d failures\n", failures ? "FAILED" : "OK", checked, failures);
    return failures ? 1 : 0;
}
