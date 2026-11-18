#pragma once
// Triangle order for one PhysX heightfield cell, pulled out so tests/hf_winding.cpp can check it.
//
// PhysX heightfield space: rows run along +x, columns along +z, heights along +y, and the surface faces +y (the
// solid side is below). Corners: a = (row, col), b = (row + 1, col), c = (row, col + 1), d = (row + 1, col + 1).
// Every triangle is wound so that cross(v1 - v0, v2 - v0) points along +y.
//
// Probe 0.6.0 and earlier wound these cells the other way. After the shape pose, every Night City terrain triangle
// faced down, and Skate's one-sided collision let the skater fall through wherever terrain was the only floor
// (in-game Stage B, Glen, 2026-11-14).
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace probe
{
inline void AppendHeightFieldCell(std::vector<uint32_t>& idx, uint32_t a, uint32_t b, uint32_t c, uint32_t d, bool tessFlag,
                                  bool hole0, bool hole1, bool mirrored)
{
    // tessFlag set: the diagonal runs a-d; clear: it runs b-c (PxHeightFieldSample::tessFlag).
    uint32_t t[6];
    if (tessFlag) { const uint32_t s[6] = {a, d, b, a, c, d}; std::copy(s, s + 6, t); }
    else { const uint32_t s[6] = {a, c, b, c, d, b}; std::copy(s, s + 6, t); }
    if (mirrored) std::swap(t[1], t[2]), std::swap(t[4], t[5]);  // an odd number of negative scales flips the winding
    if (!hole0) idx.insert(idx.end(), t, t + 3);
    if (!hole1) idx.insert(idx.end(), t + 3, t + 6);
}
} // namespace probe
