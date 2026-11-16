// Standalone unit test for src/src/ovr2xr/reload_guns.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-11-01-manual-reload-smg.md Task 2. Exit code = number of failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\reload_guns_test.cpp
#include "reload_guns.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace dlvr::reload;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

namespace {

bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
bool Has(const char* s, const char* sub) { return s && std::strstr(s, sub) != nullptr; }

void SmgMeshesClassify()
{
    struct { const char* mesh; const char* prefix; float travel; } k[] = {
        { "wn_submachine_a_hq.msh", "submachinegun", 0.0826f },
        { "wn_submachine_rust_hq.msh", "submachinegun", 0.0826f },
        { "wn_smg_harraneagle_hq.msh", "submachinegun", 0.0826f },
        { "wn_submachine_b_hq.msh", "submachinegun_alt", 0.0461f },
        { "wn_submachine_d_hq.msh", "submachinegun_alt", 0.0461f },
    };
    for (const auto& g : k) {
        const Classified c = Classify(18, g.prefix, g.mesh, true, true);
        CHECK(c.family == Family::Smg);
        CHECK(c.row != nullptr);
        if (c.row) { CHECK(Near(c.row->boltTravelM, g.travel, 1e-6f)); }
    }
}

void PrefixAndMeshAnyCase()
{
    const Classified c = Classify(18, "SUBMACHINEGUN_ALT", "WN_SUBMACHINE_B_HQ", true, true);
    CHECK(c.family == Family::Smg);
    CHECK(c.row != nullptr);
}

void UnknownSmgMeshIsRefused()
{
    const Classified c = Classify(18, "submachinegun", "wn_submachine_c_hq.msh", true, true);
    CHECK(c.family == Family::None);
    CHECK(c.row == nullptr);
    CHECK(Has(c.why, "wn_submachine_c"));
}

void SmgMeshWithTheOtherPrefixIsRefused()
{
    const Classified c = Classify(18, "submachinegun", "wn_submachine_b_hq", true, true);
    CHECK(c.family == Family::None);
}

void MissingBoneIsRefused()
{
    const Classified c = Classify(18, "submachinegun", "wn_submachine_a_hq", false, true);
    CHECK(c.family == Family::None);
    CHECK(Has(c.why, "bone_magazine"));
    const Classified d = Classify(18, "beretta", nullptr, true, false);
    CHECK(d.family == Family::None);
    CHECK(Has(d.why, "bone_chamber"));
}

void PistolNeedsNoMeshAndHasZeroSpots()
{
    const Classified c = Classify(18, "beretta", nullptr, true, true);
    CHECK(c.family == Family::Pistol);
    CHECK(c.row == nullptr);
    const Classified u = Classify(18, "Beretta", "wn_pistol_a_hq", true, true);
    CHECK(u.family == Family::Pistol);
}

void WrongTypeIsRefused()
{
    CHECK(Classify(24, "submachinegun", "wn_submachine_a_hq", true, true).family == Family::None);
    // 2026-11-09: the Walker is built now (Family::Gate) - the unbuilt example is a prefix no family takes
    CHECK(Classify(18, "flaregun", "wn_flaregun_hq", false, true).family == Family::None);
}

// Jay after RUN HP4 ("It completes too early, needs the full slide travel" -> "Pistol only"): the pistol's rack completes at the
// game's own full slide travel, whatever the key says; the SMG keeps its share of its bolt travel (below).
void PistolRackPullIsTheFullSlideTravel()
{
    CHECK(RackPullFor(Family::Pistol, nullptr, 0.03f) == kPistolSlideTravelM);
    CHECK(RackPullFor(Family::Pistol, nullptr, 0.05f) == kPistolSlideTravelM);
    CHECK(Near(kPistolSlideTravelM, 0.0368f, 1e-6f));
}

// Jay after RUN HP4 ("the radius for grabbing it felt off" -> "Too small" -> "12 cm, pistol slide only"): the pistol's SLIDE grab is
// 0.12 m; the SMG's handle keeps the key (and every mag grab keeps it - not this function's).
void RackGrabRadius()
{
    CHECK(RackGrabFor(Family::Pistol, 0.08f) == kPistolRackGrabM);
    CHECK(Near(kPistolRackGrabM, 0.12f, 1e-6f));
    CHECK(Near(kXbowRackGrabM, 0.10f, 1e-6f));   // the crossbow rack (Jay 2026-11-04)
    CHECK(RackGrabFor(Family::Smg, 0.08f) == 0.08f);
    CHECK(RackGrabFor(Family::Smg, 0.05f) == 0.05f);
    CHECK(RackGrabFor(Family::None, 0.08f) == 0.08f);
    // trace 20261116_1126 (Jay: "Widen it"): the drawn palm now on the controller - +0.04 on the mag / rack / crossbow grabs
    CHECK(Near(kGrabWidenM, 0.04f, 1e-6f));
    CHECK(Near(kXbowRackGrabM + kGrabWidenM, 0.14f, 1e-6f));
    CHECK(Near(RackGrabFor(Family::Pistol, 0.08f) + kGrabWidenM, 0.16f, 1e-6f));
}

void SmgRackPullScalesWithTheKey()
{
    const GunRow* a = Classify(18, "submachinegun", "wn_submachine_a", true, true).row;
    const GunRow* b = Classify(18, "submachinegun_alt", "wn_submachine_b", true, true).row;
    CHECK(a && b);
    if (a && b) {
        CHECK(Near(RackPullFor(Family::Smg, a, 0.03f), 0.0673f, 1e-4f));
        CHECK(Near(RackPullFor(Family::Smg, b, 0.03f), 0.0376f, 1e-4f));
        CHECK(Near(RackPullFor(Family::Smg, a, 0.06f), 0.1347f, 1e-4f));
    }
}

void BoltNeverMovesForwardOfWhereItWasGrabbed()
{
    CHECK(Near(BoltLocalX(0.2025f, 0.1199f, 0.0826f, 0.0f), 0.2025f, 1e-6f));
}

void BoltFollowsTheHandBack()
{
    CHECK(Near(BoltLocalX(0.1199f, 0.1199f, 0.0826f, 0.03f), 0.1499f, 1e-6f));
}

void BoltStopsAtTheGamesTravel()
{
    CHECK(Near(BoltLocalX(0.1199f, 0.1199f, 0.0826f, 0.5f), 0.2025f, 1e-6f));
}

void BoltHandForwardIsIgnored()
{
    CHECK(Near(BoltLocalX(0.1199f, 0.1199f, 0.0826f, -0.05f), 0.1199f, 1e-6f));
    CHECK(Near(BoltLocalX(0.15f, 0.1199f, 0.0826f, -0.05f), 0.15f, 1e-6f));
}

// ---- rifle rows, Task 1 (plan 2026-11-03-manual-reload-rifle). ItemType_Rifle = 21 (Task 0, gamedll switch; control Pistol = 18).
constexpr unsigned kRifleType = 21;

void RifleARowClassifies()
{
    const Classified c = Classify(kRifleType, "M16", "wn_rifle_a_hq.msh", true, true);
    CHECK(c.family == Family::Rifle);
    CHECK(c.row != nullptr);
    if (c.row) {
        CHECK(std::strcmp(c.row->mesh, "wn_rifle_a") == 0);
        CHECK(std::strcmp(c.row->magBone, "bone_clip") == 0);
        CHECK(c.row->hasRack);
        CHECK(!c.row->hasRack2);
        CHECK(Near(c.row->boltTravelM, 0.0831f, 1e-6f));
    }
}

void AllEightRifleMeshesClassify()
{
    const char* m16[] = { "wn_rifle_a_hq", "wn_rifle_b_hq", "wn_rifle_b_reddot_hq", "wn_rifle_c_hq", "wn_rifle_comando",
                          "wn_rifle_retrowave_hq", "wn_rifle_rust_hq" };
    for (const char* m : m16) {
        const Classified c = Classify(kRifleType, "m16", m, true, true);
        CHECK(c.family == Family::Rifle);
        CHECK(c.row != nullptr);
        if (c.row) { CHECK(Near(c.row->boltTravelM, 0.0831f, 1e-6f)); CHECK(c.row->hasRack); }
    }
    const Classified r = Classify(kRifleType, "Railgun", "wn_rifle_d_hq", true, false);
    CHECK(r.family == Family::Rifle);
}

void RailgunClassifiesWithoutChamber()
{
    const Classified c = Classify(kRifleType, "railgun", "wn_rifle_d_hq", true, false);
    CHECK(c.family == Family::Rifle);
    CHECK(c.row != nullptr);
    if (c.row) {
        CHECK(!c.row->hasRack);
        CHECK(!c.row->hasRack2);
        CHECK(std::strcmp(c.row->magBone, "bone_clip") == 0);
    }
    // an M16 mesh still needs its bone_chamber (it has a rack handle) ...
    const Classified m = Classify(kRifleType, "m16", "wn_rifle_a_hq", true, false);
    CHECK(m.family == Family::None);
    CHECK(Has(m.why, "bone_chamber"));
    // ... and every rifle needs its bone_clip
    const Classified n = Classify(kRifleType, "railgun", "wn_rifle_d_hq", false, false);
    CHECK(n.family == Family::None);
    CHECK(Has(n.why, "bone_clip"));
}

void UnknownRifleMeshRefused()
{
    const Classified c = Classify(kRifleType, "m16", "wn_rifle_x", true, true);
    CHECK(c.family == Family::None);
    CHECK(c.row == nullptr);
    CHECK(Has(c.why, "wn_rifle_x"));
    // the Railgun's prefix on an M16 mesh (and the reverse) is refused naming the mesh
    const Classified p = Classify(kRifleType, "railgun", "wn_rifle_a_hq", true, true);
    CHECK(p.family == Family::None);
    CHECK(Has(p.why, "wn_rifle_a"));
    CHECK(Classify(kRifleType, "m16", nullptr, true, true).family == Family::None);
}

void RifleTypeAndPrefixAreChecked()
{
    CHECK(Classify(18, "m16", "wn_rifle_a_hq", true, true).family == Family::None);          // the pistol type with a rifle prefix
    CHECK(Classify(kRifleType, "beretta", "wn_pistol_a_hq", true, true).family == Family::None);
    CHECK(Classify(kRifleType, "submachinegun", "wn_submachine_a_hq", true, true).family == Family::None);
    CHECK(Classify(kRifleType, "shotgun", "wn_rifle_a_hq", true, true).family == Family::None);
    CHECK(Classify(kRifleType, nullptr, "wn_rifle_a_hq", true, true).family == Family::None);
}

void CommandoHasTwoRackSpots()
{
    const GunRow* r = Classify(kRifleType, "m16", "wn_rifle_comando", true, true).row;
    CHECK(r != nullptr);
    if (!r) { return; }
    CHECK(r->hasRack && r->hasRack2);
    // Jay's two dots differ only in mesh x (+0.0641 / -0.0597 = 0.1238 m), which is bone_chamber's local y (its basis is a
    // 90-degree turn): the same x / z, the y 12.38 cm apart. Left dot = rackSpot, right dot = rackSpot2 (ledger ruling).
    CHECK(Near(r->rackSpot.x, r->rackSpot2.x, 1e-4f));
    CHECK(Near(r->rackSpot.z, r->rackSpot2.z, 1e-4f));
    CHECK(Near(std::fabs(r->rackSpot.y - r->rackSpot2.y), 0.1238f, 2e-4f));
    CHECK(r->rackSpot2.y < r->rackSpot.y);   // chamber local y = -mesh x: the +x dot (right) has the smaller local y
}

void NearerRackSpotPicksRight()
{
    const GunRow* r = Classify(kRifleType, "m16", "wn_rifle_comando", true, true).row;
    CHECK(r != nullptr);
    if (!r) { return; }
    bool second = false;
    Vec3 h = r->rackSpot2;
    h.x += 0.01f;
    const Vec3& s = NearerRackSpot(*r, h, &second);
    CHECK(&s == &r->rackSpot2);
    CHECK(second);
    Vec3 l = r->rackSpot;
    l.x += 0.01f;
    second = true;
    const Vec3& t = NearerRackSpot(*r, l, &second);
    CHECK(&t == &r->rackSpot);
    CHECK(!second);
    // a single-handle gun always answers rackSpot, second = false
    const GunRow* a = Classify(kRifleType, "m16", "wn_rifle_a_hq", true, true).row;
    CHECK(a != nullptr);
    if (a) {
        second = true;
        CHECK(&NearerRackSpot(*a, r->rackSpot2, &second) == &a->rackSpot);
        CHECK(!second);
        CHECK(&NearerRackSpot(*a, r->rackSpot2, nullptr) == &a->rackSpot);   // null out-flag is allowed
    }
}

void SmgUnchanged()
{
    const Classified c = Classify(18, "submachinegun", "wn_submachine_a_hq.msh", true, true);
    CHECK(c.family == Family::Smg);
    CHECK(c.row != nullptr);
    if (c.row) {
        CHECK(std::strcmp(c.row->magBone, "bone_magazine") == 0);
        CHECK(c.row->hasRack);
        CHECK(!c.row->hasRack2);
        CHECK(Near(c.row->boltTravelM, 0.0826f, 1e-6f));
        CHECK(Near(c.row->magSpot.x, 0.047695f, 1e-6f) && Near(c.row->rackSpot.x, 0.207408f, 1e-6f));
    }
    // type 18 / the SMG prefix stays the only SMG path: a rifle type does not classify an SMG
    CHECK(Classify(18, "submachinegun", "wn_submachine_a_hq", false, true).family == Family::None);
    CHECK(IsTableGun(Family::Smg) && IsTableGun(Family::Rifle) && !IsTableGun(Family::Pistol));
}

// Rifle plan Task 5 fix round 1 (+ final fix wave: a full point): the rifle rows carry bone_chamber's REST local point from the mesh;
// SMG rows (0.1182, 0, 0) = kChamberClipRest; the Railgun zero (no bone_chamber).
void RifleRowsCarryChamberRest()
{
    const Classified a = Classify(kItemTypeRifle, "m16", "wn_rifle_a_hq.msh", true, true);
    CHECK(a.row != nullptr);
    if (a.row) {
        CHECK(Near(a.row->chamberRest.x, 0.1551f, 1e-4f));
        CHECK(Near(a.row->chamberRest.y, 0.0064f, 1e-4f) && Near(a.row->chamberRest.z, -0.0467f, 1e-4f));   // research §3
    }
    const Classified s = Classify(kItemTypePistol, "submachinegun", "wn_submachine_a_hq.msh", true, true);
    CHECK(s.row != nullptr);
    if (s.row) { CHECK(s.row->chamberRest.x == 0.1182f && s.row->chamberRest.y == 0.0f && s.row->chamberRest.z == 0.0f); }
    int m16 = 0;
    for (const GunRow& r : kGunRows) {
        if (r.family == Family::Smg) { CHECK(r.chamberRest.x == 0.1182f); }
        if (r.family == Family::Rifle && r.hasRack) { CHECK(Near(r.chamberRest.x, 0.1551f, 1e-4f)); ++m16; }
        if (r.family == Family::Rifle && !r.hasRack) { CHECK(r.chamberRest.x == 0.0f); }   // the Railgun: no bone_chamber
    }
    CHECK(m16 == 7);
}

// The rest an unlatched bolt settles at: latched > the row's mesh rest (x > 0) > the SMG clip rest - never the grab position.
void BoltRestPointOrder()
{
    const GunRow* rifle = Classify(kItemTypeRifle, "m16", "wn_rifle_a_hq", true, true).row;
    const GunRow* smg = Classify(kItemTypePistol, "submachinegun", "wn_submachine_a_hq", true, true).row;
    const GunRow* rail = Classify(kItemTypeRifle, "railgun", "wn_rifle_d_hq", true, false).row;
    CHECK(rifle != nullptr && smg != nullptr && rail != nullptr);
    const Vec3 latched{ 0.1600f, 0.01f, -0.05f };
    CHECK(BoltRestPoint(true, latched, rifle, 0.1182f).x == 0.1600f);                    // latched wins
    CHECK(Near(BoltRestPoint(false, Vec3{}, rifle, 0.1182f).x, 0.155061f, 1e-6f));      // unlatched rifle: the row's
    CHECK(BoltRestPoint(false, Vec3{}, smg, 0.1182f).x == 0.1182f);                      // unlatched SMG: unchanged
    CHECK(BoltRestPoint(false, Vec3{}, nullptr, 0.1182f).x == 0.1182f);                  // no row
    CHECK(BoltRestPoint(false, Vec3{}, rail, 0.1182f).x == 0.1182f);                     // zero row -> the constant (never racks)
}

// ---- final fix wave (C1 / C2): the axes are row data ----
// bone_root's WORLD rotation (rows; columns = its axes in the mesh frame), identical on all 8 rifle meshes:
// backups/20261102_rifle_reload_renders/extract/*.nodes.json (bone_root world [[0,-1,0],[0.9591,0,-0.2829],[0.2829,0,0.9591]]).
const float kRootR[9] = { 0.0f, -1.0f, 0.0f, 0.95913f, 0.0f, -0.28294f, 0.28294f, 0.0f, 0.95913f };
Vec3 ToMesh(const Vec3& v)
{
    return Vec3{ kRootR[0] * v.x + kRootR[1] * v.y + kRootR[2] * v.z, kRootR[3] * v.x + kRootR[4] * v.y + kRootR[5] * v.z,
                 kRootR[6] * v.x + kRootR[7] * v.y + kRootR[8] * v.z };
}
float AngleDeg(const Vec3& a, const Vec3& b)
{
    const float la = std::sqrt(Dot3(a, a)), lb = std::sqrt(Dot3(b, b));
    float c = Dot3(a, b) / (la * lb);
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    return std::acos(c) * 57.29578f;
}
const GunRow* RowOf(const char* mesh)
{
    for (const GunRow& r : kGunRows) {
        if (std::strcmp(r.mesh, mesh) == 0) { return &r; }
    }
    return nullptr;
}

void SmgAxesUnchanged()
{
    int n = 0;
    for (const GunRow& r : kGunRows) {
        if (r.family != Family::Smg) { continue; }
        ++n;
        CHECK(r.magOutAxis.x == 0.0f && r.magOutAxis.y == 0.0f && r.magOutAxis.z == 1.0f);   // the parent's +Z, as before
        CHECK(r.rackAxis.x == 1.0f && r.rackAxis.y == 0.0f && r.rackAxis.z == 0.0f);         // the parent's +X, as before
    }
    CHECK(n == 5);
    const GunRow* a = RowOf("wn_submachine_a");
    CHECK(a && a->magOutAxis.z == 1.0f && a->rackAxis.x == 1.0f);
    // the generalised bolt on the SMG axis is BoltLocalX bit for bit (x), the grab's y / z kept
    const Vec3 ax{ 1.0f, 0.0f, 0.0f };
    const float grabs[] = { 0.1182f, 0.1300f, 0.1500f, 0.2008f };
    const float hands[] = { -0.02f, 0.0f, 0.013f, 0.05f, 0.0826f, 0.2f };
    for (float gx : grabs) {
        for (float h : hands) {
            const Vec3 g{ gx, -0.0009f, 0.0011f };
            const Vec3 p = BoltLocalAlong(g, Vec3{ 0.1182f, 0.0f, 0.0f }, ax, 0.0826f, h);
            CHECK(p.x == BoltLocalX(gx, 0.1182f, 0.0826f, h));
            CHECK(p.y == g.y && p.z == g.z);
        }
    }
}

void M16OutAxisMatchesClip()
{
    const char* m16[] = { "wn_rifle_a", "wn_rifle_b", "wn_rifle_b_reddot", "wn_rifle_c", "wn_rifle_comando", "wn_rifle_retrowave",
                          "wn_rifle_rust" };
    for (const char* m : m16) {
        const GunRow* r = RowOf(m);
        CHECK(r != nullptr);
        if (!r) { continue; }
        CHECK(Near(Dot3(r->magOutAxis, r->magOutAxis), 1.0f, 1e-4f));
        // gun_shapes' M16 well axis (0, +0.9917, -0.1283) is INTO the well, mesh frame - out is its reverse
        const float a = AngleDeg(ToMesh(r->magOutAxis), Vec3{ 0.0f, -0.9917f, 0.1283f });
        std::printf("  %-20s out axis in the mesh frame %.2f deg from -(gun_shapes well axis)\n", m, a);
        CHECK(a <= 2.0f);
        // and the old runtime axis (the parent's +Z) was ~66 deg off it - the C1 finding, kept as a control
        CHECK(AngleDeg(ToMesh(Vec3{ 0.0f, 0.0f, 1.0f }), Vec3{ 0.0f, -0.9917f, 0.1283f }) > 60.0f);
    }
}

void M16RackAxisBackAlongBarrel()
{
    int n = 0;
    for (const GunRow& r : kGunRows) {
        if (r.family != Family::Rifle || !r.hasRack) { continue; }
        ++n;
        CHECK(Near(Dot3(r.rackAxis, r.rackAxis), 1.0f, 1e-4f));
        const float a = AngleDeg(ToMesh(r.rackAxis), Vec3{ 0.0f, 0.0f, -1.0f });   // straight back along the barrel (research §2)
        std::printf("  %-20s rack axis in the mesh frame %.2f deg from (0 0 -1)\n", r.mesh, a);
        CHECK(a <= 5.0f);
        CHECK(AngleDeg(ToMesh(Vec3{ 1.0f, 0.0f, 0.0f }), Vec3{ 0.0f, 0.0f, -1.0f }) > 90.0f);   // the old +X: the C2 finding (control)
    }
    CHECK(n == 7);
}

void RailgunOutAxisMatchesShape()
{
    const GunRow* r = RowOf("wn_rifle_d");
    CHECK(r != nullptr);
    if (!r) { return; }
    // gun_shapes' Railgun well axis (+0.0000 +0.9962 -0.0872) (the mag's LONGEST axis, Task 4 ruling), INTO, mesh frame
    const float a = AngleDeg(ToMesh(r->magOutAxis), Vec3{ 0.0f, -0.996195f, 0.087156f });
    std::printf("  wn_rifle_d           out axis in the mesh frame %.2f deg from -(gun_shapes well axis)\n", a);
    CHECK(a <= 0.5f);
    CHECK(r->rackAxis.x == 0.0f && r->rackAxis.y == 0.0f && r->rackAxis.z == 0.0f);   // no rack
}

void M16BoltReachesFullTravel()
{
    const GunRow* r = RowOf("wn_rifle_a");
    CHECK(r != nullptr);
    if (!r) { return; }
    const Vec3 rest = r->chamberRest;
    const float T = r->boltTravelM;
    CHECK(Near(T, 0.0831f, 1e-4f));
    // grabbed at rest, the hand 0.0831 m back along rackAxis -> the bolt is rest + 0.0831 x axis (the clip's own racked point)
    const Vec3 full = BoltLocalAlong(rest, rest, r->rackAxis, T, 0.0831f);
    CHECK(Near(full.x, rest.x + 0.0831f * r->rackAxis.x, 1e-5f) && Near(full.y, rest.y + 0.0831f * r->rackAxis.y, 1e-5f) &&
          Near(full.z, rest.z + 0.0831f * r->rackAxis.z, 1e-5f));
    // research §2: the clip's racked bone_chamber is (0.1315, -0.1264) in x / z
    CHECK(Near(full.x, 0.1315f, 5e-4f) && Near(full.z, -0.1264f, 5e-4f));
    // past the travel: stops there; half way: half; hand forward: stays at the grab (never forward of it)
    const Vec3 over = BoltLocalAlong(rest, rest, r->rackAxis, T, 0.2f);
    CHECK(Near(over.x, full.x, 1e-6f) && Near(over.z, full.z, 1e-6f));
    const Vec3 half = BoltLocalAlong(rest, rest, r->rackAxis, T, 0.04f);
    CHECK(Near(Dot3(Vec3{ half.x - rest.x, half.y - rest.y, half.z - rest.z }, r->rackAxis), 0.04f, 1e-5f));
    const Vec3 fwd = BoltLocalAlong(rest, rest, r->rackAxis, T, -0.03f);
    CHECK(Near(fwd.x, rest.x, 1e-6f) && Near(fwd.z, rest.z, 1e-6f));
    // grabbed already 0.05 back: a smaller hand travel does not move it forward
    const Vec3 g5 = BoltPointAt(rest, r->rackAxis, Dot3(rest, r->rackAxis) + 0.05f);
    const Vec3 k = BoltLocalAlong(g5, rest, r->rackAxis, T, 0.02f);
    CHECK(Near(k.x, g5.x, 1e-6f) && Near(k.z, g5.z, 1e-6f));
}

// ---- crossbow rows, Task 2 (plan 2026-11-03-manual-reload-crossbow). ItemType_Crossbow = 37 (spec §6a: gamedll switch +6F12E0 case
// 37; all 7 crossbow items AnimPrefix "crossbow"). Rows from tools/reload_guns.py --xbow-* (reload_xbow_table.inc).
const char* const kXbowMeshes[] = { "wn_crossbow_f_hq.msh", "dlc_wn_crossbow_a", "DLC_WN_CROSSBOW_A_UPGRADE_A.msh" };
const char* const kXbowNorm[] = { "wn_crossbow_f", "dlc_wn_crossbow_a", "dlc_wn_crossbow_a_upgrade_a" };

static_assert(kItemTypeCrossbow == 37u, "ItemType_Crossbow = 37 (spec 6a)");

void CrossbowClassifiesType37()
{
    for (int i = 0; i < 3; ++i) {
        const Classified c = Classify(kItemTypeCrossbow, "crossbow", kXbowMeshes[i], true, true);
        CHECK(c.family == Family::Crossbow);
        CHECK(c.row == nullptr);                       // not a table gun: no GunRow
        CHECK(c.xrow != nullptr);
        if (c.xrow) { CHECK(std::strcmp(c.xrow->mesh, kXbowNorm[i]) == 0 && std::strcmp(c.xrow->prefix, "crossbow") == 0); }
        const Classified d = ClassifyCrossbow("CrossBow", kXbowMeshes[i], true, true, true);   // any case
        CHECK(d.family == Family::Crossbow && d.xrow == c.xrow);
    }
    CHECK(!IsTableGun(Family::Crossbow));
    // the other types never hand out a crossbow row; the crossbow type never a gun row
    CHECK(Classify(18, "submachinegun", "wn_submachine_a_hq", true, true).xrow == nullptr);
    CHECK(Classify(kItemTypeRifle, "m16", "wn_rifle_a_hq", true, true).xrow == nullptr);
    CHECK(Classify(18, "crossbow", "wn_crossbow_f_hq", true, true).family == Family::None);
    CHECK(Classify(kItemTypeRifle, "crossbow", "wn_crossbow_f_hq", true, true).family == Family::None);
    CHECK(Classify(kItemTypeCrossbow, "m16", "wn_crossbow_f_hq", true, true).family == Family::None);
    CHECK(Classify(kItemTypeCrossbow, nullptr, "wn_crossbow_f_hq", true, true).family == Family::None);
}

void CrossbowNeedsItsBones()
{
    const Classified s = ClassifyCrossbow("crossbow", "wn_crossbow_f_hq", false, true, true);
    CHECK(s.family == Family::None && s.xrow == nullptr);
    CHECK(Has(s.why, "bone_string") && !Has(s.why, "bone_stringreload"));
    const Classified r = ClassifyCrossbow("crossbow", "wn_crossbow_f_hq", true, false, true);
    CHECK(r.family == Family::None && r.xrow == nullptr);
    CHECK(Has(r.why, "bone_stringreload"));
    const Classified b = ClassifyCrossbow("crossbow", "wn_crossbow_f_hq", true, true, false);
    CHECK(b.family == Family::None && b.xrow == nullptr);
    CHECK(Has(b.why, "bolt_pose"));
    CHECK(ClassifyCrossbow("crossbow", "wn_crossbow_f_hq", true, true, true).family == Family::Crossbow);
}

void UnknownCrossbowMeshRefused()
{
    const Classified c = ClassifyCrossbow("crossbow", "wn_crossbow_x_hq.msh", true, true, true);
    CHECK(c.family == Family::None);
    CHECK(c.xrow == nullptr && c.row == nullptr);
    CHECK(Has(c.why, "wn_crossbow_x"));
    CHECK(ClassifyCrossbow("crossbow", nullptr, true, true, true).family == Family::None);
    CHECK(ClassifyCrossbow("crossbow", "wn_rifle_a_hq", true, true, true).family == Family::None);   // a gun mesh is not a crossbow row
    const Classified p = ClassifyCrossbow("beretta", "wn_crossbow_f_hq", true, true, true);
    CHECK(p.family == Family::None && p.xrow == nullptr);
}

const XbowRow* XRowOf(const char* mesh)
{
    for (const XbowRow& r : kXbowRows) {
        if (std::strcmp(r.mesh, mesh) == 0) { return &r; }
    }
    return nullptr;
}

void CrossbowRowAxesAreUnit()
{
    int n = 0;
    for (const XbowRow& r : kXbowRows) {
        ++n;
        CHECK(Near(Dot3(r.rackAxis, r.rackAxis), 1.0f, 1e-4f));
        CHECK(Near(Dot3(r.stringAxis, r.stringAxis), 1.0f, 1e-4f));
        CHECK(Near(Dot3(r.grooveAxis, r.grooveAxis), 1.0f, 1e-4f));
        // the pulls point BACK, the groove FORWARD (all three in bone_root's rest local frame): near-opposite
        std::printf("  %-28s rack.groove %+.5f string.groove %+.5f\n", r.mesh, Dot3(r.rackAxis, r.grooveAxis),
                    Dot3(r.stringAxis, r.grooveAxis));
        CHECK(Dot3(r.rackAxis, r.grooveAxis) < -0.99f);
        CHECK(Dot3(r.stringAxis, r.grooveAxis) < -0.99f);
        // boltCarry: an orthonormal right-handed basis in the columns (3x4 row-major)
        const float* m = r.boltCarry;
        const Vec3 cx{ m[0], m[4], m[8] }, cy{ m[1], m[5], m[9] }, cz{ m[2], m[6], m[10] };
        CHECK(Near(Dot3(cx, cx), 1.0f, 1e-4f) && Near(Dot3(cy, cy), 1.0f, 1e-4f) && Near(Dot3(cz, cz), 1.0f, 1e-4f));
        CHECK(Near(Dot3(cx, cy), 0.0f, 1e-4f) && Near(Dot3(cy, cz), 0.0f, 1e-4f) && Near(Dot3(cz, cx), 0.0f, 1e-4f));
        const Vec3 xy{ cx.y * cy.z - cx.z * cy.y, cx.z * cy.x - cx.x * cy.z, cx.x * cy.y - cx.y * cy.x };
        CHECK(Dot3(xy, cz) > 0.999f);
        CHECK(std::fabs(m[3]) < 0.5f && std::fabs(m[7]) < 0.5f && std::fabs(m[11]) < 0.5f);   // the nock within 0.5 m of the hand
    }
    CHECK(n == 3);
    for (const char* m : kXbowNorm) { CHECK(XRowOf(m) != nullptr); }
}

void CrossbowTravels()
{
    for (const XbowRow& r : kXbowRows) {
        std::printf("  %-28s rack travel %.4f string travel %.4f\n", r.mesh, r.rackTravelM, r.stringTravelM);
        CHECK(Near(r.rackTravelM, 0.2175f, 1e-3f));     // INV-676 / spec §6a: bone_stringreload
        CHECK(Near(r.stringTravelM, 0.2719f, 1e-3f));   // INV-676: bone_string
    }
}

void CrossbowSeatIsCocked()
{
    // the seat = bolt_pose's rest local = the string's REST (cocked) local, bone_root's frame: nodes.json bone_string local t
    // (+0.19465, +0.00645, +0.06088) on all three meshes (backups/20261103_crossbow_reload_renders/extract/*.nodes.json)
    for (const XbowRow& r : kXbowRows) {
        CHECK(Near(r.seatPoint.x, 0.19465f, 1e-4f) && Near(r.seatPoint.y, 0.00645f, 1e-4f) && Near(r.seatPoint.z, 0.06088f, 1e-4f));
    }
}

// Task 5 fix round 1 (ledger ruling T5-a): the three rests are MESH DATA. wn_crossbow_f's pinned from its nodes.json
// (backups/20261103_crossbow_reload_renders/extract/wn_crossbow_f_hq.nodes.json, printed to 6 decimals); every row's bolt_pose rest t
// is its seat point and its string rest t is the nock spot (INV-676: the string rests at bolt_pose).
void CrossbowRowsCarryMeshRests()
{
    const XbowRow* r = XRowOf("wn_crossbow_f");
    CHECK(r != nullptr);
    if (!r) { return; }
    const float kString[12] = { -0.000000f, 0.282945f, -0.959136f, 0.194651f, -1.000000f, -0.000000f, -0.000000f, 0.006448f,
                                -0.000000f, 0.959136f, 0.282945f, 0.060883f };
    const float kRack[12] = { -0.966108f, 0.000000f, 0.258139f, 0.194953f, 0.000000f, -1.000000f, 0.000000f, -0.024415f,
                              0.258139f, 0.000000f, 0.966108f, 0.062820f };
    const float kBolt[12] = { 0.000415f, 0.959446f, 0.281891f, 0.194651f, -1.000000f, 0.000310f, 0.000415f, 0.006448f,
                              0.000310f, -0.281891f, 0.959446f, 0.060883f };
    for (int i = 0; i < 12; ++i) {
        CHECK(Near(r->stringRest[i], kString[i], 2e-6f));
        CHECK(Near(r->rackRest[i], kRack[i], 2e-6f));
        CHECK(Near(r->boltRest[i], kBolt[i], 2e-6f));
    }
    for (const XbowRow& x : kXbowRows) {
        CHECK(Near(x.boltRest[3], x.seatPoint.x, 1e-6f) && Near(x.boltRest[7], x.seatPoint.y, 1e-6f) &&
              Near(x.boltRest[11], x.seatPoint.z, 1e-6f));
        CHECK(Near(x.stringRest[3], x.seatPoint.x, 1e-5f) && Near(x.stringRest[7], x.seatPoint.y, 1e-5f) &&
              Near(x.stringRest[11], x.seatPoint.z, 1e-5f));
        // the groove axis is bolt_pose's rest +z column
        CHECK(Near(x.boltRest[2], x.grooveAxis.x, 1e-5f) && Near(x.boltRest[6], x.grooveAxis.y, 1e-5f) &&
              Near(x.boltRest[10], x.grooveAxis.z, 1e-5f));
    }
    // negative: the rack's rest is NOT the string's (the pin can fail)
    CHECK(!Near(r->rackRest[7], r->stringRest[7], 1e-3f));
}

// ---- Pump shotgun manual reload Task 2 (plan docs/superpowers/plans/2026-11-04-manual-reload-pump-shotgun.md; INV-677). The rows
// are GENERATED by tools/reload_guns.py --pump-out (reload_pump_table.inc): port = Jay's round 2 dots (mean of the views that know
// each axis), pumpRest = bone_reload_shotgun's rest local, pumpAxis / travelM = OUR m_fpp_shotgun_pa_shoot.
void PumpRowsBF()
{
    const PumpRow* b = PumpRowFor("wn_shotgun_b");
    const PumpRow* f = PumpRowFor("wn_shotgun_f");
    CHECK(b != nullptr);
    CHECK(f != nullptr);
    if (!b || !f) { return; }
    CHECK(std::strcmp(b->mesh, "wn_shotgun_b") == 0 && std::strcmp(f->mesh, "wn_shotgun_f") == 0);
    CHECK(Near(b->port.x, 0.0037f, 1e-4f) && Near(b->port.y, 0.0361f, 1e-4f) && Near(b->port.z, 0.2031f, 1e-4f));
    CHECK(Near(f->port.x, -0.0026f, 1e-4f) && Near(f->port.y, 0.0288f, 1e-4f) && Near(f->port.z, 0.1818f, 1e-4f));
    for (const PumpRow* r : { b, f }) {
        CHECK(Near(r->pumpRest.x, 0.0f, 1e-4f) && Near(r->pumpRest.y, 0.0448f, 1e-4f) && Near(r->pumpRest.z, 0.3374f, 1e-4f));
        CHECK(Near(r->pumpAxis.x, 0.0f, 0.01f) && Near(r->pumpAxis.y, 0.0f, 0.01f) && Near(r->pumpAxis.z, -1.0f, 0.01f));
        CHECK(Near(Dot3(r->pumpAxis, r->pumpAxis), 1.0f, 1e-5f));     // unit
        CHECK(Near(r->travelM, 0.0574f, 1e-4f));
    }
    // the ports are each gun's own (Jay's dots differ by ~2 cm in z): a copied row fails here
    CHECK(!Near(b->port.z, f->port.z, 0.01f));
    CHECK(PumpRowFor("WN_SHOTGUN_B") == b);   // any case
    // 2026-11-07: the pumps rack, hold 6 (INV-677 AmmoCount 6, read from OUR paks by the generator), prefix shotgun_pa
    for (const PumpRow* r : { b, f }) {
        CHECK(r->racks && r->capacity == 6 && std::strcmp(r->prefix, "shotgun_pa") == 0);
    }
}

// 2026-11-07 THE SEMI-AUTO wn_shotgun_c (Shotgun_SA, no bone_reload_shotgun; Jay "No rack"): its own row - prefix shotgun_sa, racks
// false, capacity 8 (OUR paks: 7 items, AmmoCount 8), Jay's 2026-11-07 port dots; the pump fields are zero (never read). The
// double-barrel still has no row.
void PumpRowC()
{
    const PumpRow* c = PumpRowFor("wn_shotgun_c");
    CHECK(c != nullptr);
    if (!c) { return; }
    CHECK(std::strcmp(c->mesh, "wn_shotgun_c") == 0 && std::strcmp(c->prefix, "shotgun_sa") == 0);
    CHECK(!c->racks && c->capacity == 8);
    CHECK(Near(c->port.x, -0.0028f, 1e-4f) && Near(c->port.y, 0.0332f, 1e-4f) && Near(c->port.z, 0.1947f, 1e-4f));
    CHECK(c->travelM == 0.0f && c->pumpAxis.x == 0.0f && c->pumpAxis.y == 0.0f && c->pumpAxis.z == 0.0f);
    // its own dots, not b's copied (b's port y 0.0361 / z 0.2031)
    const PumpRow* b = PumpRowFor("wn_shotgun_b");
    CHECK(b != nullptr && !(Near(c->port.y, b->port.y, 1e-3f) && Near(c->port.z, b->port.z, 1e-3f)));
    CHECK(PumpRowFor("wn_shotgun_a") == nullptr);
    CHECK(PumpRowFor(nullptr) == nullptr);
    CHECK(PumpRowFor("") == nullptr);
    CHECK(PumpRowFor("wn_shotgun_b_hq") == nullptr);   // the key is the NORMALISED mesh
}
static_assert(kPumpRowCount == 3, "reload_pump_table.inc: 3 rows (the pumps wn_shotgun_b / f + the semi-auto wn_shotgun_c, 2026-11-07)");
static_assert(kItemTypeShotgun == 24u, "ItemType_Shotgun = 24 (OUR gamedll +6F12E0 case 24, spec 6c Task 0 answers 2)");

// ---- Pump plan Task 4 Step 1: the classification. ItemType_Shotgun = 24 (spec 6c Task 0 answers 2); AnimPrefix Shotgun_PA; mesh b / f.
void ClassifyPumpBF()
{
    const char* meshes[] = { "wn_shotgun_b_hq.msh", "WN_SHOTGUN_F_HQ" };
    const char* norm[] = { "wn_shotgun_b", "wn_shotgun_f" };
    for (int i = 0; i < 2; ++i) {
        const Classified c = Classify(kItemTypeShotgun, "shotgun_pa", meshes[i], false, false);   // the gun bones are not needed
        CHECK(c.family == Family::Pump);
        CHECK(c.row == nullptr && c.xrow == nullptr);
        CHECK(c.prow != nullptr);
        if (c.prow) { CHECK(std::strcmp(c.prow->mesh, norm[i]) == 0); }
        CHECK(c.prow == PumpRowFor(norm[i]));
        const Classified d = ClassifyPump("Shotgun_PA", meshes[i], true, true);   // any case (the script spells Shotgun_PA)
        CHECK(d.family == Family::Pump && d.prow == c.prow);
    }
    CHECK(!IsTableGun(Family::Pump));
    // the driver's real test: both elements it writes must exist (each refusal names its element)
    const Classified np = ClassifyPump("shotgun_pa", "wn_shotgun_b_hq", false, true);
    CHECK(np.family == Family::None && np.prow == nullptr && Has(np.why, "bone_reload_shotgun"));
    const Classified na = ClassifyPump("shotgun_pa", "wn_shotgun_f_hq", true, false);
    CHECK(na.family == Family::None && na.prow == nullptr && Has(na.why, "ammo_bullet"));
    // the other types never hand out a pump row
    CHECK(Classify(18, "shotgun_pa", "wn_shotgun_b_hq", true, true).family == Family::None);
    CHECK(Classify(kItemTypeRifle, "shotgun_pa", "wn_shotgun_b_hq", true, true).prow == nullptr);
    CHECK(Classify(kItemTypeCrossbow, "shotgun_pa", "wn_shotgun_b_hq", true, true).prow == nullptr);
}

// 2026-11-07: wn_shotgun_c classifies as a tube-loaded shotgun with prefix shotgun_sa - WITHOUT the pump bone (it has none) and
// refused without ammo_bullet; with the pumps' prefix it is refused naming the mesh. The double-barrel wn_shotgun_a (AnimPrefix
// Shotgun, INV-654) keeps the game's own X reload: refused, the reason naming the mesh; a pump mesh with another prefix is refused
// naming the prefix.
void ClassifyPumpCAndRefusesDouble()
{
    const Classified c = Classify(kItemTypeShotgun, "shotgun_sa", "wn_shotgun_c_hq.msh", true, true);
    CHECK(c.family == Family::Pump && c.prow == PumpRowFor("wn_shotgun_c") && c.prow != nullptr);
    const Classified cr = ClassifyPump("Shotgun_SA", "wn_shotgun_c_hq.msh", false, true);   // no pump bone: still the semi-auto
    CHECK(cr.family == Family::Pump && cr.prow == c.prow);
    const Classified cn = ClassifyPump("shotgun_sa", "wn_shotgun_c_hq.msh", false, false);  // no shell element: refused, named
    CHECK(cn.family == Family::None && cn.prow == nullptr && Has(cn.why, "ammo_bullet"));
    const Classified c2 = Classify(kItemTypeShotgun, "shotgun_pa", "wn_shotgun_c_hq.msh", true, true);
    CHECK(c2.family == Family::None && c2.prow == nullptr && Has(c2.why, "wn_shotgun_c") && Has(c2.why, "shotgun_pa"));
    // 2026-11-07: the double-barrel wn_shotgun_a IS driven now (ClassifyDBarrel); d / e / g are not fitted yet and keep the game's reload
    const Classified a = Classify(kItemTypeShotgun, "shotgun", "wn_shotgun_a_hq.msh", true, true);
    CHECK(a.family == Family::DBarrel && a.prow == nullptr && Has(a.why, "ok"));
    CHECK(ClassifyShotgun("Shotgun", "wn_shotgun_a_hq.msh", true, true, true).family == Family::DBarrel);
    const Classified an = ClassifyShotgun("shotgun", "wn_shotgun_a_hq.msh", true, true, false);
    CHECK(an.family == Family::None && Has(an.why, "ammo_bullet_b"));
    const Classified ah = ClassifyShotgun("shotgun", "wn_shotgun_a_hq.msh", false, true, true);
    CHECK(ah.family == Family::None && Has(ah.why, "bone_reload_shotgun"));
    const Classified ap = ClassifyShotgun("shotgun_pa", "wn_shotgun_a_hq.msh", true, true, true);
    CHECK(ap.family == Family::None && Has(ap.why, "wn_shotgun_a") && Has(ap.why, "shotgun_pa"));
    // 2026-11-07 (Jay: d / e / g "Ship ON with a's"): the other three double-barrels classify too, refusals naming THEIR mesh
    for (const char* m : { "wn_shotgun_d_hq.msh", "WN_SHOTGUN_E_HQ", "wn_shotgun_g" }) {
        CHECK(Classify(kItemTypeShotgun, "shotgun", m, true, true).family == Family::DBarrel);
    }
    const Classified en = ClassifyShotgun("shotgun", "wn_shotgun_e_hq.msh", true, false, true);
    CHECK(en.family == Family::None && Has(en.why, "wn_shotgun_e") && Has(en.why, "ammo_bullet"));
    const Classified gp = ClassifyShotgun("shotgun_pa", "wn_shotgun_g_hq.msh", true, true, true);
    CHECK(gp.family == Family::None && Has(gp.why, "wn_shotgun_g") && Has(gp.why, "shotgun_pa"));
    // NEGATIVE: a mesh outside the family is not a double-barrel (the pump wn_shotgun_b keeps its own route)
    CHECK(!IsDBarrelMesh("wn_shotgun_b") && !IsDBarrelMesh("wn_shotgun_c") && !IsDBarrelMesh("wn_shotgun_h") && !IsDBarrelMesh(nullptr));
    CHECK(ClassifyDBarrel("shotgun", "wn_shotgun_b_hq", true, true, true).family == Family::None);
    const Classified p = Classify(kItemTypeShotgun, "shotgun_sa", "wn_shotgun_b_hq.msh", true, true);
    CHECK(p.family == Family::None && p.prow == nullptr && Has(p.why, "shotgun_sa") && Has(p.why, "wn_shotgun_b"));
    CHECK(Classify(kItemTypeShotgun, nullptr, "wn_shotgun_b_hq", true, true).family == Family::None);
    CHECK(Classify(kItemTypeShotgun, "shotgun_pa", nullptr, true, true).family == Family::None);
}

// The pistol / SMG classification is unchanged by the pump (type 18 never reaches ClassifyPump; the row pointers stay null).
void PistolUnchanged()
{
    const Classified p = Classify(18, "beretta", nullptr, true, true);
    CHECK(p.family == Family::Pistol && p.row == nullptr && p.xrow == nullptr && p.prow == nullptr && Has(p.why, "ok"));
    const Classified s = Classify(18, "submachinegun", "wn_submachine_a_hq", true, true);
    CHECK(s.family == Family::Smg && s.row != nullptr && s.prow == nullptr);
    CHECK(Classify(18, "beretta", nullptr, false, true).family == Family::None);
}

// Ruling R3: the rows carry the FULL rest locals the driver writes back (each mesh's nodes.json, generated): the pump's translation
// is pumpRest, the shell's is holder_bullet-local z +0.025361 (spec 6c Task 0 answers 3); both bases ~ the identity.
void PumpRowsCarryRests()
{
    for (const PumpRow& r : kPumpRows) {
        CHECK(Near(r.pumpLocal[3], r.pumpRest.x, 1e-6f) && Near(r.pumpLocal[7], r.pumpRest.y, 1e-6f) &&
              Near(r.pumpLocal[11], r.pumpRest.z, 1e-6f));
        CHECK(Near(r.ammoLocal[11], 0.025361f, 1e-5f) && Near(r.ammoLocal[3], 0.0f, 1e-4f) && Near(r.ammoLocal[7], 0.0f, 1e-4f));
        for (int k : { 0, 5, 10 }) { CHECK(Near(r.pumpLocal[k], 1.0f, 1e-4f) && Near(r.ammoLocal[k], 1.0f, 1e-4f)); }
        // a copied field fails: the shell's rest is not the pump's (the semi-auto's pump local is the zero-translation identity)
        CHECK(!Near(r.ammoLocal[11], r.pumpLocal[11], 0.02f));
        // a row that racks has a pump at its real rest (z 0.3374); one that does not has none
        CHECK(r.racks ? Near(r.pumpRest.z, 0.3374f, 1e-4f) : (r.pumpRest.z == 0.0f && r.pumpLocal[11] == 0.0f));
    }
}

// PS1 fixes (ruling R19 - the approved render's placement, 50 mm back per Jay): the port-lock rows - one per pump mesh, an orthonormal
// handR, the frame-15 shell placed on that mesh's render_gun.py S_START - 0.050 z (b (0, 0.0243, 0.165), f (0, 0.0187, 0.165)), the hand below /
// beside the gun (not a zero row), b and f placed on their OWN S_START (a copied row fails), and NOT on Jay's port dot (R18's rows fail).
void PumpLockRowsBF()
{
    static_assert(kPumpLockRowCount == 3, "reload_pump_lock.inc: 3 rows (wn_shotgun_b / f + the semi-auto c, 2026-11-07)");
    const PumpLockRow* b = PumpLockFor("wn_shotgun_b");
    const PumpLockRow* f = PumpLockFor("WN_SHOTGUN_F");
    const PumpLockRow* sc = PumpLockFor("wn_shotgun_c");   // 2026-11-07: Jay "Yes" to anim_c13_c_back50 (same construction)
    CHECK(b != nullptr && f != nullptr && sc != nullptr);
    CHECK(PumpLockFor("wn_shotgun_a") == nullptr && PumpLockFor(nullptr) == nullptr && PumpLockFor("wn_shotgun_b_hq") == nullptr);
    if (!b || !f || !sc) { return; }
    for (const PumpLockRow* r : { b, f, sc }) {
        const PumpRow* pr = PumpRowFor(r->mesh);
        CHECK(pr != nullptr);
        // render_gun.py S_START per mesh (the generator's L1a / ROW lines), z 0.215 - c's receiver underside reads y 0.0288 from its
        // own mesh (73 vertices), the same as b's, so c's y is b's 0.0243
        const float sy = r == f ? 0.0187f : 0.0243f;
        // moved 0.050 toward the trigger (Jay: "50 mm back, both") -> z 0.165
        CHECK(Near(r->shellAt[0], 0.0f, 1e-3f) && Near(r->shellAt[1], sy, 1e-3f) && Near(r->shellAt[2], 0.165f, 1e-3f));
        if (pr) { CHECK(!(Near(r->shellAt[1], pr->port.y, 1e-3f) && Near(r->shellAt[2], pr->port.z, 1e-3f))); }
        for (int a = 0; a < 3; ++a) {
            for (int c = 0; c < 3; ++c) {
                const float d = r->handR[a] * r->handR[c] + r->handR[3 + a] * r->handR[3 + c] + r->handR[6 + a] * r->handR[6 + c];
                CHECK(Near(d, a == c ? 1.0f : 0.0f, 1e-4f));
            }
        }
        const float len = std::sqrt(r->handT[0] * r->handT[0] + r->handT[1] * r->handT[1] + r->handT[2] * r->handT[2]);
        CHECK(len > 0.05f && len < 0.40f);   // the hand bone sits a hand's length from the gun's origin, not at it
    }
    CHECK(!Near(b->handT[1], f->handT[1], 0.004f));   // each mesh's own S_START (b y 0.0243, f y 0.0187 -> handT y -0.0966 / -0.1021)
}

// ---- The remaining guns (plan 2026-11-09-manual-reload-remaining-guns Task 1) ----
// The Desert Eagle: its slide node is bone_chamber_up (no bone_chamber) and it never racks (D-306) - its row says hasRack false, so the
// chamber is not needed; the mag is bone_magazine.
void ClassifyDeagleNoSlideNeeded()
{
    const Classified c = Classify(18, "Beretta", "wn_pistol_harraneagle.msh", true, false);
    CHECK(c.family == Family::Pistol);
    CHECK(c.row != nullptr);
    if (c.row) {
        CHECK(std::strcmp(c.row->magBone, "bone_magazine") == 0);
        CHECK(!c.row->hasRack);
    }
}

// The toy gun: no bone_magazine, no slide - its can (bone_tube_a) is the mag; it pulls DOWN off the nozzle.
void ClassifyToyGunCanIsMag()
{
    const Classified c = Classify(18, "beretta", "wn_shisha_gun_hq", true, false);
    CHECK(c.family == Family::Pistol);
    CHECK(c.row != nullptr);
    if (c.row) {
        CHECK(std::strcmp(c.row->magBone, "bone_tube_a") == 0);
        CHECK(!c.row->hasRack);
        const Vec3& a = c.row->magOutAxis;
        CHECK(Near(a.x * a.x + a.y * a.y + a.z * a.z, 1.0f, 1e-4f));
    }
    CHECK(Classify(18, "beretta", "wn_shisha_gun_hq", false, false).family == Family::None);   // the can must resolve
}

// The grenade launcher: type 24 (Shotgun) with AnimPrefix GrenadeLauncher - the rifle mag flow, the drum (bone_clip) out to the LEFT.
void ClassifyGrenadeLauncher()
{
    const Classified c = Classify(24, "GrenadeLauncher", "wn_grenadelauncher_a_hq.msh", true, false);
    CHECK(c.family == Family::Rifle);
    CHECK(c.row != nullptr);
    if (c.row) {
        CHECK(std::strcmp(c.row->magBone, "bone_clip") == 0);
        CHECK(!c.row->hasRack);
    }
    CHECK(Classify(24, "grenadelauncher", "wn_grenadelauncher_a_hq", false, false).family == Family::None);   // the drum must resolve
}

// The single-shot (Task 8a; Jay: "Break the barrel by hand, like the OX shotgun"): type 18 + prefix Singleshot + wn_pistol_e = the
// break-open family (DBarrel - the Ox's machine with its own set); its hinge and its carried round must resolve.
void ClassifySingleShotIsBreakOpen()
{
    CHECK(Classify(18, "Singleshot", "wn_pistol_e_hq.msh", true, false).family == Family::DBarrel);
    CHECK(ClassifySingleShot("singleshot", "wn_pistol_e_hq", true, true).family == Family::DBarrel);
    const Classified a = ClassifySingleShot("singleshot", "wn_pistol_e_hq", false, true);
    CHECK(a.family == Family::None && Has(a.why, "bone_reload_schofield"));
    const Classified b = ClassifySingleShot("singleshot", "wn_pistol_e_hq", true, false);
    CHECK(b.family == Family::None && Has(b.why, "bullet_pistol_vancrane"));
    CHECK(ClassifySingleShot("beretta", "wn_pistol_e_hq", true, true).family == Family::None);
    CHECK(ClassifySingleShot("singleshot", "wn_pistol_a_hq", true, true).family == Family::None);
}

// Task 8b-8e: the Schofield (a six-chamber break-top), the Sixshooter / Walker (the loading gate), the chainsaw (pour + cord)
void ClassifyRevolversAndChainsaw()
{
    CHECK(Classify(18, "Schofield", "wn_revolver_schofield_hq.msh", true, false).family == Family::Break6);
    CHECK(Classify(18, "Sixshooter", "wn_revolver_sixshooter_hq.msh", true, false).family == Family::Gate);
    CHECK(Classify(18, "Walker", "wn_revolver_walker_hq.msh", true, false).family == Family::Gate);
    CHECK(Classify(18, "walker", "wn_revolver_walker.msh", true, false).family == Family::Gate);
    CHECK(Classify(21, "Chainsaw", "wn_chainsaw_a_hq.msh", true, false).family == Family::Fuel);
    CHECK(ClassifySchofield("schofield", "wn_revolver_schofield_hq", false, true).family == Family::None);
    CHECK(Has(ClassifySchofield("schofield", "wn_revolver_schofield_hq", true, false).why, "bone_cylinder"));
    CHECK(Has(ClassifyGateRevolver("sixshooter", "wn_revolver_sixshooter_hq", true, false).why, "bone_reload_hammer"));
    CHECK(ClassifyGateRevolver("walker", "wn_revolver_sixshooter_hq", true, true).family == Family::None);   // prefix and mesh must agree
    CHECK(Has(ClassifyChainsaw("chainsaw", "wn_chainsaw_a_hq", false, true).why, "bone_clip"));
    CHECK(Classify(21, "m16", "wn_rifle_a_hq", true, true).family == Family::Rifle);   // the rifles unchanged
    CHECK(std::strcmp(FamilyLabel(Family::Break6), "Break6") == 0 && std::strcmp(FamilyLabel(Family::Fuel), "Fuel") == 0);
}

// Nothing else moved: a row-less pistol still needs both bones; the pumps still classify as before.
void OtherPistolsStillNeedTheirBones()
{
    CHECK(Classify(18, "beretta", "wn_pistol_a_hq", true, false).family == Family::None);
    CHECK(Classify(18, "beretta", "wn_pistol_a_hq", false, true).family == Family::None);
    CHECK(Classify(18, "beretta", "wn_pistol_a_hq", true, true).row == nullptr);
    CHECK(Classify(24, "shotgun_pa", "wn_shotgun_b_hq", true, true).family == Family::Pump);
}

}  // namespace

int main()
{
    ClassifyDeagleNoSlideNeeded();
    ClassifyToyGunCanIsMag();
    ClassifyGrenadeLauncher();
    OtherPistolsStillNeedTheirBones();
    ClassifySingleShotIsBreakOpen();
    ClassifyRevolversAndChainsaw();
    PumpLockRowsBF();
    ClassifyPumpBF();
    ClassifyPumpCAndRefusesDouble();
    PistolUnchanged();
    PumpRowsCarryRests();
    PumpRowsBF();
    PumpRowC();
    CrossbowRowsCarryMeshRests();
    CrossbowClassifiesType37();
    CrossbowNeedsItsBones();
    UnknownCrossbowMeshRefused();
    CrossbowRowAxesAreUnit();
    CrossbowTravels();
    CrossbowSeatIsCocked();
    RifleARowClassifies();
    AllEightRifleMeshesClassify();
    RailgunClassifiesWithoutChamber();
    UnknownRifleMeshRefused();
    RifleTypeAndPrefixAreChecked();
    CommandoHasTwoRackSpots();
    NearerRackSpotPicksRight();
    SmgUnchanged();
    SmgMeshesClassify();
    PrefixAndMeshAnyCase();
    UnknownSmgMeshIsRefused();
    SmgMeshWithTheOtherPrefixIsRefused();
    MissingBoneIsRefused();
    PistolNeedsNoMeshAndHasZeroSpots();
    WrongTypeIsRefused();
    PistolRackPullIsTheFullSlideTravel();
    RackGrabRadius();
    SmgRackPullScalesWithTheKey();
    BoltNeverMovesForwardOfWhereItWasGrabbed();
    BoltFollowsTheHandBack();
    BoltStopsAtTheGamesTravel();
    BoltHandForwardIsIgnored();
    RifleRowsCarryChamberRest();
    BoltRestPointOrder();
    SmgAxesUnchanged();
    M16OutAxisMatchesClip();
    M16RackAxisBackAlongBarrel();
    RailgunOutAxisMatchesShape();
    M16BoltReachesFullTravel();
    std::printf("reload_guns_test: %d failure(s)\n", g_fail);
    return g_fail;
}
