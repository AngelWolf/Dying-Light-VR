// Standalone unit test for src/src/ovr2xr/gun_shapes.h + the GENERATED gun_shapes_table.inc - NOT part of the DLL build.
// Solid guns plan Task 2 (docs/superpowers/plans/2026-11-02-solid-guns-reload-sound-haptics.md). Exit code = number of failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\gun_shapes_test.cpp
#include "gun_shapes.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dlvr::gunshape;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

namespace {

void PistolBHasRowAndWell()
{
    const GunShapeRow* r = FindGunShapeRow("wn_pistol_b");
    CHECK(r != nullptr);
    if (!r) { return; }
    CHECK(r->shape.nBoxes > 0 && r->shape.boxes != nullptr);
    CHECK(r->shape.hasWell);
    CHECK(r->hasMag);
    CHECK(std::strcmp(r->frameNode, "holder_0") == 0);   // research step 3: the pistol's root is holder_0
    // Task 2 ruling: the channel follows the mag's own axis - mostly model +Y (into the gun), ~12 deg tilted along the grip
    CHECK(r->shape.well.axis[1] > 0.95f);
    CHECK(FindGunShape("wn_pistol_b") == &r->shape);
}

void SmgAHasRowAndWell()
{
    const GunShapeRow* r = FindGunShapeRow("wn_submachine_a");
    CHECK(r != nullptr);
    if (!r) { return; }
    CHECK(r->shape.hasWell && r->hasMag);
    CHECK(std::strcmp(r->frameNode, "holder") == 0);      // the SMG's root is holder (differs from the pistol)
    CHECK(r->shape.well.axis[1] > 0.95f);
}

void EveryReloadGunHasShape()
{
    // Every mesh the manual reload covers today (reload_guns_table.inc + the pistols the hands table names).
    const char* const kReload[] = { "wn_submachine_a", "wn_submachine_rust", "wn_smg_harraneagle", "wn_submachine_b",
                                    "wn_submachine_d", "wn_pistol_a", "wn_pistol_b", "wn_pistol_d", "wn_pistol_scifi" };
    for (const char* m : kReload) {
        const GunShapeRow* r = FindGunShapeRow(m);
        if (!r || !r->shape.hasWell || !r->hasMag) { std::printf("  missing / no well: %s\n", m); }
        CHECK(r != nullptr && r->shape.hasWell && r->hasMag);
    }
}

void UnknownMeshHasNoRow()
{
    CHECK(FindGunShapeRow("wn_axe_a") == nullptr);
    CHECK(FindGunShape("wn_axe_a") == nullptr);
    CHECK(FindGunShapeRow(nullptr) == nullptr);
    CHECK(FindGunShapeRow("") == nullptr);
}

void HqSuffixNormalised()
{
    CHECK(FindGunShapeRow("wn_pistol_b_hq.msh") == FindGunShapeRow("wn_pistol_b"));
    CHECK(FindGunShapeRow("WN_PISTOL_B_HQ") == FindGunShapeRow("wn_pistol_b"));
}

void PistolBSilencerIsItsOwnPart()
{
    // The game hides element 'silencer' on pistol b (STATE: 'silencer' (hidden 1)); its boxes must be their own part so the runtime
    // can skip them, and the body must not contain them.
    const GunShapeRow* r = FindGunShapeRow("wn_pistol_b");
    CHECK(r != nullptr);
    if (!r) { return; }
    int sil = -1;
    for (int i = 0; i < r->nParts; ++i) { if (std::strcmp(r->parts[i], "silencer") == 0) { sil = i; } }
    CHECK(sil > 0);
    int nSil = 0, nOther = 0;
    for (int i = 0; i < r->shape.nBoxes; ++i) { (r->shape.boxes[i].part == sil ? nSil : nOther) += 1; }
    CHECK(nSil > 0 && nOther > 0);
    CHECK(r->shape.hiddenParts == 0u);   // the table never hides; the runtime decides per update
}

void MagRestPositionInModelFrame()
{
    // The runtime's frame self-check compares bone_magazine's live position in the frame node's frame with this. Research step 4:
    // bone_magazine's rest world t = (-0.0000, -0.0735, -0.0059) on both wn_pistol_b and wn_submachine_a.
    const char* const kMeshes[] = { "wn_pistol_b", "wn_submachine_a" };
    for (const char* m : kMeshes) {
        const GunShapeRow* r = FindGunShapeRow(m);
        if (!r) { CHECK(false); continue; }
        CHECK(std::fabs(r->magRest34[3] - 0.0f) < 1e-3f);
        CHECK(std::fabs(r->magRest34[7] + 0.0735f) < 1e-3f);
        CHECK(std::fabs(r->magRest34[11] + 0.0059f) < 1e-3f);
    }
}

void SeatedMagFitsItsWell()
{
    // Review finding 2: every row's mag, posed at bone_magazine's REST in the model frame, must not touch its own gun's boxes (a seated
    // mag that already overlaps the walls would buzz and fight at the start of every pull).
    const float kIdent[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
    for (int i = 0; i < kGunShapeRowCount; ++i) {
        const GunShapeRow& r = kGunShapeRows[i];
        if (!r.hasMag) { continue; }
        float shift[3];
        const bool hit = StopBoxOnHull(r.shape, kIdent, r.mag, r.magRest34, 45.0f, shift);
        if (hit) { std::printf("  seated mag touches its gun: %s shift (%+.4f %+.4f %+.4f)\n", r.mesh, shift[0], shift[1], shift[2]); }
        CHECK(!hit);
    }
}

void MagCannotPassTheCeiling()
{
    // Found while diagnosing review finding 2 (seatdiag): boxes ABOVE the mag's top were flagged walls, and an admitted mag's walls
    // push only across - so an aligned mag could slide up through the top of the well. Every row's mag pushed 1 cm PAST its rest along
    // its insertion axis must be pushed back (an axial component against the push).
    const float kIdent[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
    for (int i = 0; i < kGunShapeRowCount; ++i) {
        const GunShapeRow& r = kGunShapeRows[i];
        if (!r.hasMag) { continue; }
        // 2026-11-09 (RG3, the well-only launcher row): the mesh has NO box past the drum's top (0 ceiling boxes of 666 - its cradle is
        // open on the far side), so nothing in the mesh stops a drum pushed past its seat; the reload's well rail seats it at the clear
        // point instead. Exempt BY NAME, and only while the row is walls-only (any non-wall box - a ceiling or a solid body - makes
        // this check run for it again).
        if (std::strcmp(r.mesh, "wn_grenadelauncher_a") == 0) {
            int ceil = 0;
            for (int b = 0; b < r.shape.nBoxes; ++b) { ceil += r.shape.boxes[b].wall ? 0 : 1; }
            if (ceil == 0) { continue; }
        }
        float pose[12];
        for (int k = 0; k < 12; ++k) { pose[k] = r.magRest34[k]; }
        const float* ax = r.shape.well.axis;
        pose[3] += 0.01f * ax[0]; pose[7] += 0.01f * ax[1]; pose[11] += 0.01f * ax[2];
        float shift[3];
        const bool hit = StopBoxOnHull(r.shape, kIdent, r.mag, pose, 45.0f, shift);
        const float back = shift[0] * ax[0] + shift[1] * ax[1] + shift[2] * ax[2];
        if (!hit || back > -0.005f) { std::printf("  mag passes the ceiling: %s hit %d axial %+.4f\n", r.mesh, hit ? 1 : 0, back); }
        CHECK(hit && back <= -0.005f);
    }
}

void MagInsertionAxisIsUnit()
{
    for (int i = 0; i < kGunShapeRowCount; ++i) {
        const GunShapeRow& r = kGunShapeRows[i];
        if (!r.hasMag) { continue; }
        for (int a = 0; a < 3; ++a) {
            const float* v = r.mag.axis[a];
            const float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            CHECK(l > 0.999f && l < 1.001f);
            CHECK(r.mag.half[a] > 0.0f);
        }
    }
}

void SeatedMagClearIsWhereTheTopLeavesTheWell()
{
    // The held-mag rail (magrail_logic.h): the clear distance per gun. Independent check: the GENERATOR's own well depth (tools/
    // gun_shapes.py:410, depth = the mag's top - the lowest wall beside it, computed in Python from the mesh) is the same quantity, so
    // the C++ transform of the seated mag must agree with it within 1 mm on every row. (A box-shift check was tried first and dropped:
    // it tests the wall boxes' coverage of the grip - 4 pistols have none 2 cm to the side half way down - not this distance.)
    int n = 0;
    for (int i = 0; i < kGunShapeRowCount; ++i) {
        const GunShapeRow& r = kGunShapeRows[i];
        float c = 0.0f;
        const bool ok = SeatedMagClearM(r, &c);
        if (!r.hasMag || !r.shape.hasWell) { CHECK(!ok); continue; }
        ++n;
        std::printf("  clear %-26s %.4f m (well depth %.4f)\n", r.mesh, ok ? c : -1.0f, r.shape.well.depth);
        CHECK(ok && c > 0.02f && c < 0.25f);
        CHECK(ok && std::fabs(c - r.shape.well.depth) <= 0.001f);
    }
    CHECK(n > 0);
    // control: no mag in the table -> no clear distance. (Rifle plan Task 4: wn_rifle_a has a mag now; wn_shotgun_a has neither
    // bone_magazine nor bone_clip.)
    const GunShapeRow* noMag = FindGunShapeRow("wn_shotgun_a");
    float c = 0.0f;
    CHECK(noMag != nullptr && !noMag->hasMag && !SeatedMagClearM(*noMag, &c));
}

// ---- Rifle manual-reload plan Task 4 (docs/superpowers/plans/2026-11-03-manual-reload-rifle.md): the rifle magwells ----

const char* const kRifles[] = { "wn_rifle_a", "wn_rifle_b", "wn_rifle_b_reddot", "wn_rifle_c", "wn_rifle_comando",
                                "wn_rifle_retrowave", "wn_rifle_rust", "wn_rifle_d" };

// FNV-1a over a row's boxes (centre, axes, halves, wall, part) - pins a row's hull box for box.
unsigned long long BoxHash(const GunShapeRow& r)
{
    unsigned long long h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    for (int i = 0; i < r.shape.nBoxes; ++i) {
        const Obb& o = r.shape.boxes[i];
        mix(o.c, sizeof(o.c)); mix(o.axis, sizeof(o.axis)); mix(o.half, sizeof(o.half));
        const unsigned char w = o.wall ? 1 : 0;
        mix(&w, 1); mix(&o.part, 1);
    }
    return h;
}

bool Same(const float* a, const float* b, int n)
{
    return std::memcmp(a, b, sizeof(float) * static_cast<size_t>(n)) == 0;
}

void RifleAHasWell()
{
    const GunShapeRow* r = FindGunShapeRow("wn_rifle_a");
    CHECK(r != nullptr);
    if (!r) { return; }
    CHECK(r->shape.hasWell);
    CHECK(r->hasMag);
    CHECK(r->magBone != nullptr && std::strcmp(r->magBone, "bone_clip") == 0);
}

void EveryRifleHasRowAndWell()
{
    for (const char* m : kRifles) {
        const GunShapeRow* r = FindGunShapeRow(m);
        const bool ok = r && r->shape.hasWell && r->hasMag && r->magBone && std::strcmp(r->magBone, "bone_clip") == 0;
        if (!r) { std::printf("  rifle %-20s NO ROW\n", m); }
        else {
            std::printf("  rifle %-20s well %s mag %s magBone %s axis (%+.4f %+.4f %+.4f) depth %.4f\n", m, r->shape.hasWell ? "yes" : "NO",
                        r->hasMag ? "yes" : "NO", r->magBone ? r->magBone : "(null)", r->shape.well.axis[0], r->shape.well.axis[1],
                        r->shape.well.axis[2], r->shape.well.depth);
        }
        CHECK(ok);
    }
}

void RifleWellAxisFollowsClip()
{
    // Research section 2: the M16 clip's first move of bone_clip in the mesh frame is (0, -0.0795, +0.0101) per 4 frames = straight down,
    // ~7 deg forward. The channel axis must lie within 5 deg of that line (either sense: the channel axis points INTO the gun).
    const GunShapeRow* r = FindGunShapeRow("wn_rifle_a");
    CHECK(r != nullptr && r->shape.hasWell);
    if (!r || !r->shape.hasWell) { return; }
    const float L = std::sqrt(0.0795f * 0.0795f + 0.0101f * 0.0101f);
    const float out[3] = { 0.0f, -0.0795f / L, 0.0101f / L };
    const float* a = r->shape.well.axis;
    const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const float d = std::fabs(a[0] * out[0] + a[1] * out[1] + a[2] * out[2]) / la;
    const float deg = std::acos(d > 1.0f ? 1.0f : d) * 57.29578f;
    std::printf("  wn_rifle_a well axis (%+.4f %+.4f %+.4f) vs the clip's out line: %.2f deg\n", a[0], a[1], a[2], deg);
    CHECK(deg <= 5.0f);
}

float AxisDegToLine(const float* a, const float* line)
{
    const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const float ll = std::sqrt(line[0] * line[0] + line[1] * line[1] + line[2] * line[2]);
    const float d = std::fabs(a[0] * line[0] + a[1] * line[1] + a[2] * line[2]) / (la * ll);
    return std::acos(d > 1.0f ? 1.0f : d) * 57.29578f;
}

void M16RustAxisFollowsClip()
{
    // Fix round 2: wn_rifle_rust runs the same M16 clip as wn_rifle_a (research section 2) - its well follows the same line.
    const GunShapeRow* r = FindGunShapeRow("wn_rifle_rust");
    CHECK(r != nullptr && r->shape.hasWell);
    if (!r || !r->shape.hasWell) { return; }
    const float out[3] = { 0.0f, -0.0795f, 0.0101f };
    const float deg = AxisDegToLine(r->shape.well.axis, out);
    std::printf("  wn_rifle_rust well axis vs the clip's out line: %.2f deg\n", deg);
    CHECK(deg <= 5.0f);
}

void RailgunAxisIsMagLongest()
{
    // Fix round 1 ruling: the Railgun (clip rotates) takes the mag's LONGEST axis - generator printed (+0.0000 +0.9962 -0.0872).
    const GunShapeRow* r = FindGunShapeRow("wn_rifle_d");
    CHECK(r != nullptr && r->shape.hasWell);
    if (!r || !r->shape.hasWell) { return; }
    const float want[3] = { 0.0f, 0.9962f, -0.0872f };
    const float deg = AxisDegToLine(r->shape.well.axis, want);
    std::printf("  wn_rifle_d well axis vs the mag's longest axis: %.2f deg\n", deg);
    CHECK(deg <= 2.0f);
}

void RustChannelFitsWell()
{
    // Fix round 2 (review finding 2): the curved mag's channel is sized from the mag's IN-WELL section, not the whole mag.
    // Assertion: halfD <= 0.05 m, and the channel's front-most point (max model z, the muzzle side, over its 8 corners from the open
    // end to depth) lies no more than 5 mm forward of the in-well section's front edge, model z +0.1907 - printed by tools/
    // gun_shapes.py (wn_rifle_rust well pass 3, "section front (max model z)"). Before the fix halfD was 0.0741.
    const GunShapeRow* r = FindGunShapeRow("wn_rifle_rust");
    CHECK(r != nullptr && r->shape.hasWell);
    if (!r || !r->shape.hasWell) { return; }
    const Channel& ch = r->shape.well;
    const float dx[3] = { ch.axis[1] * ch.wAxis[2] - ch.axis[2] * ch.wAxis[1], ch.axis[2] * ch.wAxis[0] - ch.axis[0] * ch.wAxis[2],
                          ch.axis[0] * ch.wAxis[1] - ch.axis[1] * ch.wAxis[0] };
    float front = -1e9f;
    for (int t = 0; t < 2; ++t) {
        for (int sw = -1; sw <= 1; sw += 2) {
            for (int sd = -1; sd <= 1; sd += 2) {
                const float z = ch.origin[2] + (t ? ch.depth : 0.0f) * ch.axis[2] + sw * ch.halfW * ch.wAxis[2] + sd * ch.halfD * dx[2];
                front = z > front ? z : front;
            }
        }
    }
    std::printf("  wn_rifle_rust channel halfD %.4f, front z %+.4f (in-well section front +0.1907)\n", ch.halfD, front);
    CHECK(ch.halfD <= 0.05f);
    CHECK(front <= 0.1907f + 0.005f);
}

bool InHull(const GunShapeRow& r, const float p[3])
{
    for (int i = 0; i < r.shape.nBoxes; ++i) {   // the table's boxes are axis-aligned (axis = identity rows)
        const Obb& o = r.shape.boxes[i];
        if (std::fabs(p[0] - o.c[0]) <= o.half[0] && std::fabs(p[1] - o.c[1]) <= o.half[1] && std::fabs(p[2] - o.c[2]) <= o.half[2]) {
            return true;
        }
    }
    return false;
}

void RustHullNotCarvedForwardOfWell()
{
    // Fix round 3 (re-review): the carve must be the IN-WELL section's alone. Probe: model (0, +0.050, +0.2207) - 3 cm forward of
    // the in-well section's front edge (z +0.1907, generator well pass 3), on the receiver above the mag top's level, OUTSIDE the
    // in-well channel (its front-most point is z +0.1942). It must lie inside a hull box. On the round-2 table it did not: pass 1's
    // whole-mag prism (halfD 0.0741) was carved into the base copy and every later pass inherited it.
    const GunShapeRow* r = FindGunShapeRow("wn_rifle_rust");
    CHECK(r != nullptr);
    if (!r) { return; }
    const float p[3] = { 0.0f, 0.050f, 0.2207f };
    const bool in = InHull(*r, p);
    std::printf("  wn_rifle_rust probe (0 +0.050 +0.2207) in the hull: %s (%d boxes)\n", in ? "yes" : "NO", r->shape.nBoxes);
    CHECK(in);
}

void RifleHeldMagBoxIsTopSection()
{
    // Fix round 3 ruling: on a bone_clip gun the held-mag box is the mag's TOP SECTION - the in-well section the channel is sized
    // from. Assertion: its extent along the insertion axis (2 x half[0]) <= the channel depth + 1 mm, and its cross-section halves
    // + CLEAR (0.002 m, tools/gun_shapes.py: the channel = the section + CLEAR per side) equal the channel's halfW / halfD +- 1 mm.
    // The whole curved mag (round 2) was 0.218 x 0.045 x 0.144 against a 0.0343 m well.
    const GunShapeRow* r = FindGunShapeRow("wn_rifle_rust");
    CHECK(r != nullptr && r->hasMag && r->shape.hasWell);
    if (!r || !r->hasMag || !r->shape.hasWell) { return; }
    const float kClear = 0.002f;
    const Channel& ch = r->shape.well;
    std::printf("  wn_rifle_rust held-mag box %.4f x %.4f x %.4f, well depth %.4f halfW %.4f halfD %.4f\n", 2 * r->mag.half[0],
                2 * r->mag.half[1], 2 * r->mag.half[2], ch.depth, ch.halfW, ch.halfD);
    CHECK(2.0f * r->mag.half[0] <= ch.depth + 0.001f);
    CHECK(std::fabs(r->mag.half[1] + kClear - ch.halfW) <= 0.001f);
    CHECK(std::fabs(r->mag.half[2] + kClear - ch.halfD) <= 0.001f);
}

void PistolBWellUnchanged()
{
    // Today's row (gun_shapes_table.inc before Task 4), number for number: the generator change must not move a pistol.
    const GunShapeRow* r = FindGunShapeRow("wn_pistol_b");
    CHECK(r != nullptr);
    if (!r) { return; }
    const float o[3] = { -0.000009f, -0.057233f, -0.005676f }, ax[3] = { -0.000000f, 0.990268f, 0.139173f }, w[3] = { 1.000000f, 0.000000f, -0.000000f };
    const float hw[3] = { 0.015310f, 0.032401f, 0.132277f };
    CHECK(Same(r->shape.well.origin, o, 3) && Same(r->shape.well.axis, ax, 3) && Same(r->shape.well.wAxis, w, 3));
    CHECK(r->shape.well.halfW == hw[0] && r->shape.well.halfD == hw[1] && r->shape.well.depth == hw[2]);
    CHECK(r->shape.hasWell && r->hasMag && r->shape.nBoxes == 258 && r->nParts == 2);
    const float mc[3] = { 0.075006f, -0.000009f, 0.007328f };
    const float ma[9] = { 0.997564f, -0.000000f, 0.069758f, 0.000000f, 1.000000f, -0.000001f, -0.069758f, 0.000001f, 0.997564f };
    const float mh[3] = { 0.073043f, 0.013310f, 0.030401f };
    CHECK(Same(r->mag.c, mc, 3) && Same(&r->mag.axis[0][0], ma, 9) && Same(r->mag.half, mh, 3));
    const float rest[12] = { 0.000000f, 1.000000f, -0.000001f, -0.000000f, 0.978147f, 0.000000f, 0.207913f, -0.073466f, 0.207913f,
                             -0.000001f, -0.978147f, -0.005859f };
    CHECK(Same(r->magRest34, rest, 12));
    CHECK(BoxHash(*r) == 0x1a56192307e47667ull);
    CHECK(r->magBone != nullptr && std::strcmp(r->magBone, "bone_magazine") == 0);
}

void SmgAWellUnchanged()
{
    const GunShapeRow* r = FindGunShapeRow("wn_submachine_a");
    CHECK(r != nullptr);
    if (!r) { return; }
    const float o[3] = { 0.000000f, -0.018090f, 0.145002f }, ax[3] = { 0.000000f, 0.996195f, -0.087156f }, w[3] = { 1.000000f, 0.000000f, 0.000000f };
    const float hw[3] = { 0.012200f, 0.031325f, 0.087619f };
    CHECK(Same(r->shape.well.origin, o, 3) && Same(r->shape.well.axis, ax, 3) && Same(r->shape.well.wAxis, w, 3));
    CHECK(r->shape.well.halfW == hw[0] && r->shape.well.halfD == hw[1] && r->shape.well.depth == hw[2]);
    CHECK(r->shape.hasWell && r->hasMag && r->shape.nBoxes == 251 && r->nParts == 1);
    const float mc[3] = { 0.064365f, -0.000000f, -0.142522f };
    const float ma[9] = { 0.956304f, 0.000000f, 0.292374f, -0.000000f, 1.000000f, -0.000001f, -0.292374f, 0.000001f, 0.956304f };
    const float mh[3] = { 0.109754f, 0.010200f, 0.029325f };
    CHECK(Same(r->mag.c, mc, 3) && Same(&r->mag.axis[0][0], ma, 9) && Same(r->mag.half, mh, 3));
    const float rest[12] = { -0.000000f, 1.000000f, -0.000001f, -0.000000f, 0.978147f, 0.000000f, 0.207914f, -0.073467f, 0.207914f,
                             -0.000001f, -0.978147f, -0.005859f };
    CHECK(Same(r->magRest34, rest, 12));
    CHECK(BoxHash(*r) == 0x6036dc97bac0471eull);
    CHECK(r->magBone != nullptr && std::strcmp(r->magBone, "bone_magazine") == 0);
}

// ---- Crossbow manual-reload plan Task 2 (spec 4.10, U-x6): ItemType_Crossbow is no longer excluded - the three crossbow meshes have
// a solid shape; no magazine bone, so no well and no mag box.
void CrossbowHasShapeRows()
{
    const char* const kXbows[] = { "wn_crossbow_f", "dlc_wn_crossbow_a", "dlc_wn_crossbow_a_upgrade_a" };
    for (const char* m : kXbows) {
        const GunShapeRow* r = FindGunShapeRow(m);
        if (!r) { std::printf("  crossbow %-28s NO ROW\n", m); CHECK(false); continue; }
        std::printf("  crossbow %-28s %d box(es), frame %s, parts %d, well %s, mag %s\n", m, r->shape.nBoxes, r->frameNode, r->nParts,
                    r->shape.hasWell ? "yes" : "no", r->hasMag ? "yes" : "no");
        CHECK(r->shape.nBoxes > 0);
        CHECK(!r->shape.hasWell && !r->hasMag);
        CHECK(r->magBone != nullptr && r->magBone[0] == 0);
    }
    CHECK(FindGunShapeRow("WN_CROSSBOW_F_HQ.msh") != nullptr);   // the game's spelling
}

}  // namespace

int main()
{
    CrossbowHasShapeRows();
    PistolBHasRowAndWell();
    SmgAHasRowAndWell();
    EveryReloadGunHasShape();
    UnknownMeshHasNoRow();
    HqSuffixNormalised();
    PistolBSilencerIsItsOwnPart();
    MagRestPositionInModelFrame();
    SeatedMagFitsItsWell();
    MagCannotPassTheCeiling();
    MagInsertionAxisIsUnit();
    SeatedMagClearIsWhereTheTopLeavesTheWell();
    RifleAHasWell();
    EveryRifleHasRowAndWell();
    RifleWellAxisFollowsClip();
    M16RustAxisFollowsClip();
    RailgunAxisIsMagLongest();
    RustChannelFitsWell();
    RustHullNotCarvedForwardOfWell();
    RifleHeldMagBoxIsTopSection();
    PistolBWellUnchanged();
    SmgAWellUnchanged();
    std::printf("gun_shapes_test: %d row(s), %d failure(s)\n", kGunShapeRowCount, g_fail);
    return g_fail;
}
