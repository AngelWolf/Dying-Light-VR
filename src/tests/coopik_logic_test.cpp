// Standalone unit test for src/src/ovr2xr/coopik_logic.h (co-op arm-pose channel: the remote body's pure solve) - NOT part of the DLL.
// Spec docs/superpowers/specs/2026-11-13-coop-arm-pose-channel-design.md section 8; trace docs/traces/20261113_2000_coopik_logic.md.
// Exit code = failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\coopik_logic_test.cpp
#include "coopik_logic.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dlvr::coopik;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))
#define RUN(fn)                                                                  \
    do {                                                                         \
        const int before_ = g_fail;                                              \
        fn();                                                                    \
        std::printf("%s %s\n", g_fail == before_ ? "PASS" : "FAIL", #fn);        \
    } while (0)

namespace {

float Dist(const float a[3], const float b[3])
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// Rotation about a unit axis, row-major.
void AxisAngle(const float ax[3], float ang, float m[9])
{
    const float c = std::cos(ang), s = std::sin(ang), t = 1.0f - c;
    const float x = ax[0], y = ax[1], z = ax[2];
    m[0] = t * x * x + c;     m[1] = t * x * y - s * z; m[2] = t * x * z + s * y;
    m[3] = t * x * y + s * z; m[4] = t * y * y + c;     m[5] = t * y * z - s * x;
    m[6] = t * x * z - s * y; m[7] = t * y * z + s * x; m[8] = t * z * z + c;
}

void Mul3(const float a[9], const float b[9], float o[9])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c]; }
    }
}

// Spec 8.2 truth table - and the misses on purpose: a held item, no data, a two-hand grip, a reload lock, an unknown kind.
void HandOwnership()
{
    CHECK(HandIsOurs(0x00, true));                     // left empty
    CHECK(HandIsOurs(0x00, false));                    // right empty
    CHECK(!HandIsOurs(0x01, false));                   // right holds melee
    CHECK(!HandIsOurs(0x02, false));                   // right holds a gun
    CHECK(!HandIsOurs(0x03, true));                    // kind not known
    CHECK(!HandIsOurs(kNoData, true));                 // no data
    CHECK(!HandIsOurs(kTwoHand, true));                // left gripping a two-handed gun
    CHECK(!HandIsOurs(kReloadLock, true));             // left in a reload step
    CHECK(HandIsOurs(kTwoHand | kReloadLock, false));  // those two bits are the LEFT's only: right empty stays ours
}

// T = rootPos + Ry(rootYaw) x (headRel + handOff) - Ry the solve's own (x' = c x + s z, z' = -s x + c z).
void TargetComposesRootYaw()
{
    const float root[3] = { 10.0f, 2.0f, -3.0f };
    const float head[3] = { 0.0f, 1.6f, 0.1f };
    const float hand[3] = { 0.2f, -0.4f, 0.3f };
    float t[3];
    TargetWorld(root, 0.0f, head, hand, t);
    NEAR(t[0], 10.2f, 1e-5f); NEAR(t[1], 3.2f, 1e-5f); NEAR(t[2], -2.6f, 1e-5f);
    TargetWorld(root, 1.57079633f, head, hand, t);   // 90 deg: (x, z) = (0.2, 0.4) -> (0.4, -0.2)
    NEAR(t[0], 10.4f, 1e-5f); NEAR(t[1], 3.2f, 1e-5f); NEAR(t[2], -3.2f, 1e-5f);
}

void IkReachesReachableTarget()
{
    const float S[3] = { 0, 0, 0 }, T[3] = { 0.3f, -0.2f, 0.25f }, pole[3] = { 0, -1, 0 };
    float E[3], H[3];
    CHECK(TwoBoneIk(S, 0.30f, 0.28f, T, pole, E, H));
    NEAR(Dist(H, T), 0.0f, 1e-4f);
    NEAR(Dist(S, E), 0.30f, 1e-4f);
    NEAR(Dist(E, H), 0.28f, 1e-4f);
}

void IkClampsUnreachableAlongLine()
{
    const float S[3] = { 0, 0, 0 }, T[3] = { 2.0f, 0.0f, 0.0f }, pole[3] = { 0, -1, 0 };
    float E[3], H[3];
    CHECK(TwoBoneIk(S, 0.30f, 0.28f, T, pole, E, H));
    NEAR(H[0], 0.58f * 0.999f, 1e-4f); NEAR(H[1], 0.0f, 1e-5f); NEAR(H[2], 0.0f, 1e-5f);
    NEAR(Dist(S, E), 0.30f, 1e-4f);
    NEAR(Dist(E, H), 0.28f, 1e-4f);
}

void IkElbowOnPoleSide()
{
    const float S[3] = { 0, 0, 0 }, T[3] = { 0.0f, 0.0f, 0.4f };
    const float down[3] = { 0, -1, 0 }, up[3] = { 0, 1, 0 };
    float E[3], H[3];
    CHECK(TwoBoneIk(S, 0.30f, 0.28f, T, down, E, H));
    CHECK(E[1] < -0.05f);
    CHECK(TwoBoneIk(S, 0.30f, 0.28f, T, up, E, H));
    CHECK(E[1] > 0.05f);
    // pole along the reach line: still a valid elbow, lengths kept
    const float along[3] = { 0, 0, 1 };
    CHECK(TwoBoneIk(S, 0.30f, 0.28f, T, along, E, H));
    NEAR(Dist(S, E), 0.30f, 1e-4f);
    // degenerate: zero length / target on the shoulder -> refused
    CHECK(!TwoBoneIk(S, 0.0f, 0.28f, T, down, E, H));
    CHECK(!TwoBoneIk(S, 0.30f, 0.28f, S, down, E, H));
}

void SwingToPointsAndKeepsRoll()
{
    const float ax[3] = { 0.267261f, 0.534522f, 0.801784f };
    float R[9];
    AxisAngle(ax, 0.7f, R);
    const float from[3] = { 1, 0, 0 }, to[3] = { 0, 0.6f, 0.8f };
    float O[9];
    CHECK(SwingTo(R, from, to, O));
    // the swing maps `from` onto `to`: (O x R^-1) from == to
    float Rt[9] = { R[0], R[3], R[6], R[1], R[4], R[7], R[2], R[5], R[8] }, D[9];
    Mul3(O, Rt, D);
    NEAR(D[0] * from[0] + D[1] * from[1] + D[2] * from[2], to[0], 1e-5f);
    NEAR(D[3] * from[0] + D[4] * from[1] + D[5] * from[2], to[1], 1e-5f);
    NEAR(D[6] * from[0] + D[7] * from[1] + D[8] * from[2], to[2], 1e-5f);
    // same direction -> R unchanged; opposite -> still a rotation (det 1) mapping from to -from
    CHECK(SwingTo(R, from, from, O));
    for (int i = 0; i < 9; ++i) { NEAR(O[i], R[i], 1e-6f); }
    const float back[3] = { -1, 0, 0 };
    CHECK(SwingTo(R, from, back, O));
    Mul3(O, Rt, D);
    NEAR(D[0], -1.0f, 1e-5f);
}

void LocalWorldRoundTrip()
{
    const float ax[3] = { 0.0f, 0.6f, 0.8f };
    float pr[9], wr[9];
    AxisAngle(ax, 1.1f, pr);
    AxisAngle(ax, -0.4f, wr);
    const float parent[12] = { pr[0], pr[1], pr[2], 1.0f, pr[3], pr[4], pr[5], -2.0f, pr[6], pr[7], pr[8], 0.5f };
    const float world[12] = { wr[0], wr[1], wr[2], 0.3f, wr[3], wr[4], wr[5], 0.7f, wr[6], wr[7], wr[8], -0.2f };
    float local[12], back[12];
    CHECK(LocalFromWorld(parent, world, local));
    WorldFromLocal(parent, local, back);
    for (int i = 0; i < 12; ++i) { NEAR(back[i], world[i], 1e-5f); }
    // a scaled parent (INV-11: flat reads ~0.4 scale) still round-trips
    float sp[12];
    for (int i = 0; i < 12; ++i) { sp[i] = parent[i]; }
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) { sp[r * 4 + c] *= 0.4f; } }
    CHECK(LocalFromWorld(sp, world, local));
    WorldFromLocal(sp, local, back);
    for (int i = 0; i < 12; ++i) { NEAR(back[i], world[i], 1e-4f); }
    const float zero[12] = {};
    CHECK(!LocalFromWorld(zero, world, local));
}

void BlendEndsAreExact()
{
    const float ax[3] = { 1, 0, 0 };
    float a[9], b[9];
    AxisAngle(ax, 0.2f, a);
    AxisAngle(ax, 1.0f, b);
    const float game[12] = { a[0], a[1], a[2], 0.1f, a[3], a[4], a[5], 0.2f, a[6], a[7], a[8], 0.3f };
    const float ours[12] = { b[0], b[1], b[2], 0.5f, b[3], b[4], b[5], 0.6f, b[6], b[7], b[8], 0.7f };
    float o[12];
    BlendLocal(game, ours, 0.0f, o);
    CHECK(std::memcmp(o, game, sizeof(o)) == 0);      // weight 0 = the game's locals byte for byte
    BlendLocal(game, ours, 1.0f, o);
    CHECK(std::memcmp(o, ours, sizeof(o)) == 0);
    BlendLocal(game, ours, 0.5f, o);                   // half way: 0.6 rad about X, translation midpoint (3x4: row 1 = o[4..7])
    NEAR(o[5], std::cos(0.6f), 1e-5f);
    NEAR(o[6], -std::sin(0.6f), 1e-5f);
    NEAR(o[3], 0.3f, 1e-6f);
    CHECK(std::fabs(o[5] - std::cos(0.2f)) > 0.05f);   // not an end - the midpoint test can miss
    CHECK(std::fabs(o[5] - std::cos(1.0f)) > 0.05f);
}

void HeadSplitSumsAndClamps()
{
    // 30 deg about Y, xyzw
    const float h = 0.5f * 0.5235988f;
    const float q[4] = { 0.0f, std::sin(h), 0.0f, std::cos(h) };
    float p[4];
    HeadSplit(q, 3, 1.0f, p);
    NEAR(2.0f * std::acos(p[3]), 0.5235988f / 3.0f, 1e-5f);   // each part 10 deg
    NEAR(p[1], std::sin(0.5235988f / 6.0f), 1e-5f);            // same axis
    // clamp: 90 deg asked, 60 deg allowed -> 20 deg each
    const float h9 = 0.5f * 1.5707963f;
    const float q9[4] = { std::sin(h9), 0.0f, 0.0f, std::cos(h9) };
    HeadSplit(q9, 3, 1.0471976f, p);
    NEAR(2.0f * std::acos(p[3]), 1.0471976f / 3.0f, 1e-5f);
    NEAR(p[1], 0.0f, 1e-6f);
    // identity -> identity; the double cover (w < 0) is the same turn
    const float id[4] = { 0, 0, 0, 1 };
    HeadSplit(id, 3, 1.0f, p);
    NEAR(p[3], 1.0f, 1e-6f);
    const float neg[4] = { -q[0], -q[1], -q[2], -q[3] };
    HeadSplit(neg, 3, 1.0f, p);
    NEAR(2.0f * std::acos(p[3]), 0.5235988f / 3.0f, 1e-5f);
    NEAR(p[1], std::sin(0.5235988f / 6.0f), 1e-5f);
}

// ---- step 3 (trace 20261113_2045): the head beyond the game's look ----
bool SameRot(const float a[4], const float b[4], float tol)
{
    const float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);   // |dot| = 1 for the same rotation
    return d >= 1.0f - tol;
}

void LookPartKeepsYawPitchDropsRoll()
{
    float yq[4], xq[4], zq[4], yx[4], yxz[4], L[4];
    QYaw(0.7f, yq);
    QAxis(0, 0.3f, xq);
    QAxis(2, 0.4f, zq);
    QMul(yq, xq, yx);
    QMul(yx, zq, yxz);
    LookPart(yx, L);
    CHECK(SameRot(L, yx, 1e-6f));            // no roll: the look IS the head
    LookPart(yxz, L);
    CHECK(SameRot(L, yx, 1e-6f));            // roll dropped, yaw + pitch kept
    CHECK(!SameRot(L, yxz, 1e-3f));          // and it is NOT the rolled head - the test can miss
}

void HeadBeyondLookIsWhatTheLookCannotShow()
{
    float yq[4], xq[4], zq[4], t[4], real[4], rep[4], L[4], D[4], back[4];
    QYaw(0.9f, yq);
    QAxis(0, 0.3f, xq);
    QAxis(2, 0.2f, zq);
    QMul(xq, zq, rep);          // what the decouple reports: the head without its yaw
    QMul(yq, rep, real);        // the real head = stripped yaw x reported
    HeadBeyondLook(real, rep, 0.0f, D);
    LookPart(rep, L);
    QMul(D, L, back);
    CHECK(SameRot(back, real, 1e-6f));       // D x look = the real head: D is exactly what the look leaves out
    // nothing beyond the look: real == reported, no roll -> identity
    float yx[4], I[4];
    QMul(yq, xq, yx);
    HeadBeyondLook(yx, yx, 0.0f, I);
    NEAR(std::fabs(I[3]), 1.0f, 1e-6f);
    // roll only (decouple off: reported == real): D is a turn about the look's forward axis by the roll
    float yxz[4];
    QMul(yx, zq, yxz);
    HeadBeyondLook(yxz, yxz, 0.0f, D);
    NEAR(2.0f * std::acos(std::fabs(D[3])), 0.2f, 1e-5f);
    // the frame chain: the receiver's qY(r) Dbody qY(-r), Dbody sent with d = b - r, equals qY(b) D qY(-b)
    const float b = 1.3f, r = -0.4f;
    float Db[4], w1[4], w2[4], qr[4], qri[4], qb[4], qbi[4], tmp[4];
    HeadBeyondLook(real, rep, b - r, Db);
    HeadBeyondLook(real, rep, 0.0f, D);
    QYaw(r, qr); QYaw(-r, qri); QYaw(b, qb); QYaw(-b, qbi);
    QMul(qr, Db, tmp); QMul(tmp, qri, w1);
    QMul(qb, D, tmp);  QMul(tmp, qbi, w2);
    CHECK(SameRot(w1, w2, 1e-6f));
    (void)t;
}

// INV-736 build: both hands follow whenever the sender has data; kNoRot does not gate the arm.
void BothHandsFollow()
{
    CHECK(HandHasData(0x00));
    CHECK(HandHasData(0x02));                       // a gun in the hand - still follows (Jay "Yes, it should")
    CHECK(HandHasData(kTwoHand | kReloadLock));
    CHECK(HandHasData(kNoRot));
    CHECK(!HandHasData(kNoData));
    CHECK(!HandHasData(kNoData | 0x01));
}

// QFromMat is MatFromQ's inverse (x y z w), including a turn past 90 deg about a tilted axis.
void QFromMatRoundTrip()
{
    const float ax[3] = { 0.48f, -0.6f, 0.64f };
    float m[9], q[4], back[9];
    AxisAngle(ax, 2.4f, m);
    QFromMat(m, q);
    NEAR(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0f, 1e-5f);
    MatFromQ(q, back);
    for (int i = 0; i < 9; ++i) { NEAR(back[i], m[i], 1e-5f); }
}

// The sender's body-frame bone rotation b = Ry(-rootYawS) W, rebuilt by the receiver with ITS root yaw: equal to W when the two root yaws
// agree, and turned by exactly their difference about +Y when they do not (what the CHECK line's root-yaw comparison measures).
void HandRotationSenderToReceiver()
{
    const float ax[3] = { 0.0f, 0.6f, 0.8f };
    float W[9], qy[4], my[9], b[9], q[4], R[9];
    AxisAngle(ax, 1.1f, W);
    const float rootS = 0.7f;
    QYaw(-rootS, qy);
    MatFromQ(qy, my);
    Mul3(my, W, b);
    QFromMat(b, q);
    HandWorldRot(rootS, q, R);
    for (int i = 0; i < 9; ++i) { NEAR(R[i], W[i], 1e-5f); }
    float R2[9], qd[4], md[9], W2[9];
    HandWorldRot(rootS + 0.3f, q, R2);
    QYaw(0.3f, qd);
    MatFromQ(qd, md);
    Mul3(md, W, W2);
    for (int i = 0; i < 9; ++i) { NEAR(R2[i], W2[i], 1e-5f); }
}

// INV-736 (c): with body-follows-head the look already holds the folded yaw; adding it back to the reported head leaves no yaw "beyond".
void FoldedLookLeavesNoYaw()
{
    float yq[4], xq[4], rep[4], real[4], look[4], D[4];
    const float h = 0.6f;                           // the stripped OpenXR yaw, rad
    QYaw(h, yq);
    QAxis(0, 0.25f, xq);
    rep[0] = xq[0]; rep[1] = xq[1]; rep[2] = xq[2]; rep[3] = xq[3];   // reported = pitch only (yaw stripped)
    QMul(yq, rep, real);
    // fold fully on: folded (deg, flip applied) = h in the game's sign; flip latched -> folded = -h deg
    const float foldedDeg = -h * 57.2957795f;
    LookWithFold(rep, FoldedLookYawRad(foldedDeg, true), look);
    HeadBeyondLook(real, look, 0.0f, D);
    NEAR(std::fabs(D[3]), 1.0f, 1e-5f);             // nothing beyond the look
    // and the old formula (folded ignored) WOULD turn it by h - the double count this fixes; the test can miss
    HeadBeyondLook(real, rep, 0.0f, D);
    NEAR(2.0f * std::acos(std::fabs(D[3])), h, 1e-4f);
    // folded 0 (fold off / decouple off) = today's formula, bit for bit
    float look0[4], D0[4], Dold[4];
    LookWithFold(rep, FoldedLookYawRad(0.0f, true), look0);
    HeadBeyondLook(real, look0, 0.3f, D0);
    HeadBeyondLook(real, rep, 0.3f, Dold);
    for (int i = 0; i < 4; ++i) { NEAR(D0[i], Dold[i], 1e-6f); }
}

// ---- trace 20261115_1735_coop_arms_world ----

// Item 1: built the way the sender builds it (cooppose.cpp CoopPosePublishSide: hand = Ry(bodyYaw - rootYaw)(ctl - headRep) -
// Ry(-rootYaw) wristBack, head = Ry(-rootYaw)(anchor - rootPos)), the receiver's target with the SENT yaw is the sender's own world
// wrist T = anchor + Ry(bodyYaw)(ctl - headRep) - wristBack, for several sent yaws - and with a drawn yaw 20 deg off it is NOT (the miss).
void SentYawRebuildsSenderWrist()
{
    const float rootYaws[4] = { 0.0f, 1.2f, -2.9f, 3.1f };
    const float bodyYaw = 0.7f;
    const float rootPos[3] = { 105.0f, 20.0f, -310.0f };
    const float anchor[3] = { 105.1f, 21.6f, -309.8f };
    const float off[3] = { 0.25f, -0.45f, -0.35f };          // ctl - headRep, tracking
    const float wristBack[3] = { 0.03f, -0.01f, 0.08f };     // world
    for (float ry : rootYaws) {
        float hand[3], wb[3], head[3], rel[3];
        dlvr::coop::RotYVec(bodyYaw - ry, off, hand);
        dlvr::coop::RotYVec(-ry, wristBack, wb);
        for (int i = 0; i < 3; ++i) { hand[i] -= wb[i]; rel[i] = anchor[i] - rootPos[i]; }
        dlvr::coop::RotYVec(-ry, rel, head);
        float want[3], ro[3];
        dlvr::coop::RotYVec(bodyYaw, off, ro);
        for (int i = 0; i < 3; ++i) { want[i] = anchor[i] + ro[i] - wristBack[i]; }
        float got[3];
        TargetWorld(rootPos, ry, head, hand, got);
        CHECK(Dist(got, want) < 1e-4f);
        float drawn[3];
        TargetWorld(rootPos, ry + 0.349f, head, hand, drawn);   // the drawn body 20 deg off (CO3: 15-25 one-sided)
        CHECK(Dist(drawn, want) > 0.1f);
    }
}

// Item 5: a bent arm comes out bent the same way on a receiver with another arm length and another yaw.
void BentArmBendsTheSameWay()
{
    // sender: shoulder, elbow, wrist (FPP lengths ~0.29 / 0.27), world; its root yaw 0.8
    const float S[3] = { 0.0f, 1.4f, 0.0f }, E[3] = { 0.12f, 1.18f, -0.18f }, W[3] = { 0.05f, 1.25f, -0.52f };
    const float sYaw = 0.8f;
    float bw[3], body[3];
    CHECK(BendDir(S, E, W, 0.01f, bw));
    dlvr::coop::RotYVec(-sYaw, bw, body);              // what goes on the wire
    // receiver: the same world target relative to its shoulder, TPP arm 10 % longer; the pole from the sent direction
    const float Sr[3] = { 50.0f, 3.0f, 7.0f };
    const float T[3] = { Sr[0] + (W[0] - S[0]), Sr[1] + (W[1] - S[1]), Sr[2] + (W[2] - S[2]) };
    float pole[3] = { 0.0f, -1.0f, 0.0f }, held[3] = {};
    bool heldOk = false;
    CHECK(ChooseElbowPole(true, body, sYaw, &heldOk, held, pole) == 0);
    CHECK(heldOk);
    float Er[3], Hr[3];
    CHECK(TwoBoneIk(Sr, 0.319f, 0.297f, T, pole, Er, Hr));
    float got[3];
    CHECK(BendDir(Sr, Er, Hr, 0.001f, got));
    CHECK(Dot3(got, bw) > 0.9999f);                    // the same bend direction in the world
    NEAR(BendAngleDeg(Sr, Er, Hr, pole), 0.0f, 0.05f); // the receiver's own check reads ~0
    // a receiver that ignored the sent yaw (used 0) would bend another way: the test can miss
    float wrong[3] = { 0.0f, -1.0f, 0.0f }, h2[3] = {};
    bool h2ok = false;
    ChooseElbowPole(true, body, 0.0f, &h2ok, h2, wrong);
    float Ew[3], Hw[3], gw[3];
    CHECK(TwoBoneIk(Sr, 0.319f, 0.297f, T, wrong, Ew, Hw));
    CHECK(BendDir(Sr, Ew, Hw, 0.001f, gw));
    CHECK(Dot3(gw, bw) < 0.99f);
}

// Item 5: a straight arm has no bend side; the receiver holds the last good direction; never had one = the rule's pole, untouched.
void StraightArmHoldsLastDirection()
{
    const float S[3] = { 0.0f, 0.0f, 0.0f }, W[3] = { 0.0f, 0.0f, -0.55f };
    const float Estraight[3] = { 0.004f, 0.0f, -0.29f };   // 4 mm off the line < 1 cm
    float d[3] = { 9.0f, 9.0f, 9.0f };
    CHECK(!BendDir(S, Estraight, W, 0.01f, d));
    NEAR(d[0], 9.0f, 0.0f);                                  // out untouched on refusal
    const float Ebent[3] = { 0.02f, 0.0f, -0.29f };          // 2 cm off: has a side
    CHECK(BendDir(S, Ebent, W, 0.01f, d));
    NEAR(d[0], 1.0f, 1e-5f);
    // the receiver: never had one -> rule (pole untouched)
    float pole[3] = { 0.1f, -1.0f, 0.2f }, held[3] = {};
    bool heldOk = false;
    const float none[3] = { 0.0f, 0.0f, 0.0f };
    CHECK(ChooseElbowPole(false, none, 0.5f, &heldOk, held, pole) == 2);
    NEAR(pole[0], 0.1f, 0.0f); NEAR(pole[1], -1.0f, 0.0f); NEAR(pole[2], 0.2f, 0.0f);
    // a good one arrives, then the arm straightens: the held one, turned by the CURRENT sent yaw
    const float sent[3] = { 1.0f, 0.0f, 0.0f };
    CHECK(ChooseElbowPole(true, sent, 0.0f, &heldOk, held, pole) == 0);
    CHECK(ChooseElbowPole(false, none, 1.57079633f, &heldOk, held, pole) == 1);
    NEAR(pole[0], 0.0f, 1e-5f); NEAR(pole[2], -1.0f, 1e-5f);   // +X turned 90 deg by the solve's Ry = -Z
}

// ===== trace 20261117_1905_coop_full_ik =====
void Compose34(const float R[9], float x, float y, float z, float m[12])
{
    m[0] = R[0]; m[1] = R[1]; m[2] = R[2];  m[3] = x;
    m[4] = R[3]; m[5] = R[4]; m[6] = R[5];  m[7] = y;
    m[8] = R[6]; m[9] = R[7]; m[10] = R[8]; m[11] = z;
}
float PosDist(const float a[12], const float b[12])
{
    const float p[3] = { a[3], a[7], a[11] }, q[3] = { b[3], b[7], b[11] };
    return Dist(p, q);
}

// The cap and the wrap: a gap past +-180 is taken the short way, then clamped; the miss on purpose - a gap under the cap is untouched.
void TorsoYawWrapsAndClamps()
{
    const float d = 0.0174532925f;
    bool c = true;
    NEAR(TorsoYaw(20.0f * d, 60.0f * d, &c), 20.0f * d, 1e-6f);
    CHECK(!c);
    NEAR(TorsoYaw(90.0f * d, 60.0f * d, &c), 60.0f * d, 1e-6f);
    CHECK(c);
    NEAR(TorsoYaw(-170.0f * d, 60.0f * d, &c), -60.0f * d, 1e-6f);
    CHECK(c);
    NEAR(TorsoYaw(350.0f * d, 60.0f * d, &c), -10.0f * d, 1e-5f);   // 350 = -10 the short way
    CHECK(!c);
}

// TurnWorldY raises the receiver's yaw formula by exactly the turn, keeps the position and the rotation orthonormal.
void TurnWorldYRaisesYaw()
{
    const float ax[3] = { 0.2672612f, 0.5345225f, 0.8017837f };
    float R[9], W[12], o[12];
    AxisAngle(ax, 0.7f, R);
    Compose34(R, 1.5f, 2.5f, -3.5f, W);
    for (float g : { 0.3f, -1.1f, 2.0f }) {
        TurnWorldY(g, W, o);
        NEAR(WrapPi(YawOf(o) - YawOf(W)), WrapPi(g), 1e-5f);
        NEAR(o[3], 1.5f, 0.0f); NEAR(o[7], 2.5f, 0.0f); NEAR(o[11], -3.5f, 0.0f);
        float r9[9];
        Rot34(o, r9);
        for (int r = 0; r < 3; ++r) { NEAR(r9[r * 3] * r9[r * 3] + r9[r * 3 + 1] * r9[r * 3 + 1] + r9[r * 3 + 2] * r9[r * 3 + 2], 1.0f, 1e-5f); }
        NEAR(o[4], W[4], 1e-6f); NEAR(o[5], W[5], 1e-6f); NEAR(o[6], W[6], 1e-6f);   // the Y row is untouched by a turn about +Y
    }
}

// The torso turn: the neck keeps its world (head stays the game's); hspine1's yaw rises by the turn; spine3 / hspine1 locals are not
// written (carried). The miss on purpose: without the neck restore the neck WOULD move.
void TorsoTurnKeepsNeckTurnsChest()
{
    const float ax1[3] = { 0.0f, 0.0f, 1.0f }, ax2[3] = { 1.0f, 0.0f, 0.0f }, ax3[3] = { 0.5773503f, 0.5773503f, 0.5773503f };
    float R0[9], Whs[12];
    AxisAngle(ax3, 0.4f, R0);
    Compose34(R0, 10.0f, 100.0f, -20.0f, Whs);
    float L[5][12], Ln[12];
    for (int i = 0; i < 5; ++i) {
        float Ri[9];
        AxisAngle(i % 2 ? ax1 : ax2, 0.1f * static_cast<float>(i + 1), Ri);
        Compose34(Ri, 0.12f, 0.02f * static_cast<float>(i), 0.0f, L[i]);
    }
    float Rn[9];
    AxisAngle(ax1, -0.2f, Rn);
    Compose34(Rn, 0.15f, 0.0f, 0.01f, Ln);
    // the game's chain
    float hs1Old[12], neckOld[12];
    std::memcpy(hs1Old, Whs, sizeof(hs1Old));
    for (int i = 0; i < 5; ++i) { WorldFromLocal(hs1Old, L[i], hs1Old); }
    WorldFromLocal(hs1Old, Ln, neckOld);
    const float g = 0.6f;
    float nL[3][12], nNeck[12], hs1New[12];
    CHECK(TorsoTurn(Whs, L, Ln, g, nL, nNeck, hs1New));
    // rebuild the chain from the written locals, exactly as the engine would
    float P[12];
    std::memcpy(P, Whs, sizeof(P));
    for (int i = 0; i < 3; ++i) { WorldFromLocal(P, nL[i], P); }
    WorldFromLocal(P, L[3], P);
    WorldFromLocal(P, L[4], P);
    NEAR(PosDist(P, hs1New), 0.0f, 1e-4f);
    NEAR(RotAngleDeg(P, hs1New), 0.0f, 0.05f);
    NEAR(WrapPi(YawOf(hs1New) - YawOf(hs1Old)), g, 1e-4f);   // the chest turned by the whole turn
    float neckNew[12];
    WorldFromLocal(P, nNeck, neckNew);
    NEAR(PosDist(neckNew, neckOld), 0.0f, 1e-4f);
    NEAR(RotAngleDeg(neckNew, neckOld), 0.0f, 0.05f);
    float neckIfNotRestored[12];
    WorldFromLocal(P, Ln, neckIfNotRestored);
    CHECK(RotAngleDeg(neckIfNotRestored, neckOld) > 10.0f);   // the miss: the old neck local would have turned the head ~34 deg
    // no turn: the locals come back as the game's
    CHECK(TorsoTurn(Whs, L, Ln, 0.0f, nL, nNeck, hs1New));
    NEAR(RotAngleDeg(nL[1], L[1]), 0.0f, 0.05f);
    NEAR(PosDist(hs1New, hs1Old), 0.0f, 1e-4f);
}

// A sent rotation + the game's translation; RotSane passes a rotation and refuses a stale / scaled / NaN matrix.
void LocalFromQAndRotSane()
{
    const float q[4] = { 0.0f, std::sin(0.25f), 0.0f, std::cos(0.25f) };   // 0.5 rad about +Y
    float keep[12] = { 1, 0, 0, 0.1718f, 0, 1, 0, -0.002f, 0, 0, 1, 0.003f };
    float o[12];
    LocalFromQ(q, keep, o);
    NEAR(o[3], 0.1718f, 0.0f); NEAR(o[7], -0.002f, 0.0f); NEAR(o[11], 0.003f, 0.0f);
    NEAR(o[0], std::cos(0.5f), 1e-6f); NEAR(o[2], std::sin(0.5f), 1e-6f);   // Ry: [c 0 s; 0 1 0; -s 0 c]
    CHECK(RotSane(o));
    float bad[12];
    std::memcpy(bad, o, sizeof(bad));
    bad[0] *= 2.0f; bad[1] *= 2.0f; bad[2] *= 2.0f;   // a scaled row
    CHECK(!RotSane(bad));
    std::memcpy(bad, o, sizeof(bad));
    bad[5] = std::nanf("");
    CHECK(!RotSane(bad));
    float zero[12] = {};
    CHECK(!RotSane(zero));                            // a freed / zeroed model
    const float a3[3] = { 1, 0, 0 }, b3[3] = { 0, 1, 0 }, z3[3] = { 0, 0, 0 };
    NEAR(VecAngleDeg(a3, b3), 90.0f, 1e-3f);
    NEAR(VecAngleDeg(a3, z3), 0.0f, 0.0f);
}

}  // namespace

int main()
{
    RUN(HandOwnership);
    RUN(TargetComposesRootYaw);
    RUN(IkReachesReachableTarget);
    RUN(IkClampsUnreachableAlongLine);
    RUN(IkElbowOnPoleSide);
    RUN(SwingToPointsAndKeepsRoll);
    RUN(LocalWorldRoundTrip);
    RUN(BlendEndsAreExact);
    RUN(HeadSplitSumsAndClamps);
    RUN(LookPartKeepsYawPitchDropsRoll);
    RUN(HeadBeyondLookIsWhatTheLookCannotShow);
    RUN(BothHandsFollow);
    RUN(QFromMatRoundTrip);
    RUN(HandRotationSenderToReceiver);
    RUN(FoldedLookLeavesNoYaw);
    RUN(SentYawRebuildsSenderWrist);
    RUN(BentArmBendsTheSameWay);
    RUN(StraightArmHoldsLastDirection);
    RUN(TorsoYawWrapsAndClamps);
    RUN(TurnWorldYRaisesYaw);
    RUN(TorsoTurnKeepsNeckTurnsChest);
    RUN(LocalFromQAndRotSane);
    std::printf("%d failure(s)\n", g_fail);
    return g_fail;
}
