#pragma once
#include <cmath>

// Small rigid-transform maths for Stage C (header-only so tests/rigid_math.cpp can check it offline).
namespace rigid
{
// Z up, Y forward; quaternions (x, y, z, w) = RED's Quaternion (i, j, k, r).
struct V3
{
    float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Norm(V3 a) { const float l = Len(a); return l > 1e-6f ? a * (1.f / l) : V3{0, 0, 0}; }
struct Q
{
    float x, y, z, w;
};
inline Q Mul(Q a, Q b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Q Conj(Q q) { return {-q.x, -q.y, -q.z, q.w}; }
inline V3 Rot(Q q, V3 v)
{
    const V3 u{q.x, q.y, q.z};
    return u * (2.f * Dot(u, v)) + v * (q.w * q.w - Dot(u, u)) + Cross(u, v) * (2.f * q.w);
}
// Rotation whose columns are the images of the local X, Y, Z axes.
inline Q FromBasis(V3 X, V3 Y, V3 Z)
{
    X = Norm(X), Y = Norm(Y), Z = Norm(Z);
    const float m00 = X.x, m01 = Y.x, m02 = Z.x, m10 = X.y, m11 = Y.y, m12 = Z.y, m20 = X.z, m21 = Y.z, m22 = Z.z;
    const float tr = m00 + m11 + m22;
    Q q;
    if (tr > 0) { const float s = std::sqrt(tr + 1.f) * 2.f; q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s}; }
    else if (m00 > m11 && m00 > m22) { const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f; q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s}; }
    else if (m11 > m22) { const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f; q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s}; }
    else { const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f; q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s}; }
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}
// Heading in degrees, the game's convention: 0 faces +Y, counter-clockwise about +Z.
inline Q YawQ(float deg)
{
    const float h = deg * 3.14159265f / 360.f;
    return {0, 0, std::sin(h), std::cos(h)};
}
struct Pose
{
    V3 p{0, 0, 0};
    Q q{0, 0, 0, 1};
};
inline bool Finite(V3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
} // namespace rigid
