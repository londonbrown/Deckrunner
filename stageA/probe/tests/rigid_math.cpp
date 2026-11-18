// Offline check of Stage C1's chase-camera maths (src/Rigid.hpp): the game's yaw convention, quaternions from axes,
// and the parent recovery + world-space placement done every frame, including a parent rolled upside down.
#include "../src/Rigid.hpp"

#include <cstdio>
#include <random>

using namespace rigid;

static int g_fail = 0, g_checks = 0;
static void Near(const char* what, V3 a, V3 b, float tol = 1e-4f)
{
    ++g_checks;
    if (Len(a - b) > tol) { ++g_fail; printf("FAIL %s: (%.5f, %.5f, %.5f) vs (%.5f, %.5f, %.5f)\n", what, a.x, a.y, a.z, b.x, b.y, b.z); }
}
static void SameRot(const char* what, Q a, Q b)
{
    Near(what, Rot(a, {1, 0, 0}), Rot(b, {1, 0, 0}));
    Near(what, Rot(a, {0, 1, 0}), Rot(b, {0, 1, 0}));
}
static Q AxisAngle(V3 axis, float deg)
{
    axis = Norm(axis);
    const float h = deg * 3.14159265f / 360.f, s = std::sin(h);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(h)};
}

int main()
{
    // The game's heading: 0 faces +Y, counter-clockwise; Stage B's autopilot used dir = (-sin h, cos h).
    Near("yaw 0", Rot(YawQ(0), {0, 1, 0}), {0, 1, 0});
    Near("yaw 90", Rot(YawQ(90), {0, 1, 0}), {-1, 0, 0});
    Near("yaw 180", Rot(YawQ(180), {0, 1, 0}), {0, -1, 0});
    Near("yaw -45", Rot(YawQ(-45), {0, 1, 0}), {0.70710678f, 0.70710678f, 0});

    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1, 1), ang(-180, 180), pos(-50, 50);
    auto rq = [&] { return AxisAngle({u(rng), u(rng), u(rng) + 1e-3f}, ang(rng)); };
    auto rv = [&] { return V3{pos(rng), pos(rng), pos(rng)}; };
    for (int i = 0; i < 2000; ++i)
    {
        const Q q = rq();
        SameRot("axes -> quaternion", FromBasis(Rot(q, {1, 0, 0}), Rot(q, {0, 1, 0}), Rot(q, {0, 0, 1})), q);

        // One frame of UpdateChase: world = parent * local; recover the parent; place the camera at `want`.
        Pose parent{rv(), i % 4 == 0 ? Mul(YawQ(ang(rng)), AxisAngle({0, 1, 0}, 180)) : rq()};  // every 4th rolled upside down
        Pose local{rv() * 0.05f, rq()};
        const Pose world{parent.p + Rot(parent.q, local.p), Mul(parent.q, local.q)};
        Pose rec;
        rec.q = Mul(world.q, Conj(local.q));
        rec.p = world.p - Rot(rec.q, local.p);
        SameRot("parent rotation recovered", rec.q, parent.q);
        Near("parent position recovered", rec.p, parent.p, 1e-3f);
        const float yaw = ang(rng);
        const V3 v = rv();
        Pose want{v + Rot(YawQ(yaw), {0, -2.8f, 1.75f}), YawQ(yaw)};
        Pose next{Rot(Conj(rec.q), want.p - rec.p), Mul(Conj(rec.q), want.q)};
        const Pose placed{parent.p + Rot(parent.q, next.p), Mul(parent.q, next.q)};
        Near("camera lands where wanted", placed.p, want.p, 1e-3f);
        SameRot("camera faces V's heading", placed.q, want.q);
        Near("camera is upright", Rot(placed.q, {0, 0, 1}), {0, 0, 1});
        Near("camera is behind V at 1.75 m", placed.p - v, {2.8f * std::sin(yaw * 3.14159265f / 180.f), -2.8f * std::cos(yaw * 3.14159265f / 180.f), 1.75f}, 1e-3f);
    }
    printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "OK", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
