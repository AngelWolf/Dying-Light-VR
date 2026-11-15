// Standalone unit test for src/src/ovr2xr/reload_fx.h + the GENERATED reload_fx_table.inc - NOT part of the DLL build.
// Solid guns plan Task 3: per-gun racking + the game's own reload sounds per step (RESEARCH-SOLID-GUNS-2026-11-02 step 5).
// Exit code = number of failures.
// Compile: cl /nologo /EHsc /W4 /WX /std:c++17 /I src\src\ovr2xr src\src\tests\reload_fx_test.cpp
#include "reload_fx.h"

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

bool Is(const Snd* s, const char* name) { return s && s->name && std::strcmp(s->name, name) == 0; }

void EveryReloadGunHasRow()
{
    const char* const kReload[] = { "wn_submachine_a", "wn_submachine_rust", "wn_smg_harraneagle", "wn_submachine_b",
                                    "wn_submachine_d", "wn_pistol_a", "wn_pistol_b", "wn_pistol_d", "wn_pistol_scifi",
                                    "wn_pistol_harraneagle" };
    for (const char* m : kReload) {
        const FxRow* r = FindFxRow(m);
        if (!r) { std::printf("  no fx row: %s\n", m); }
        CHECK(r != nullptr);
    }
}

void PistolRowDoesNotRack()
{
    const FxRow* r = FindFxRow("wn_pistol_b_hq.msh");
    CHECK(r != nullptr);
    if (!r) { return; }
    CHECK(!r->racks);   // D-306 (Jay 2026-11-02: "Yes, no pistol rack")
}

void SmgARowRacks()
{
    const FxRow* r = FindFxRow("wn_submachine_a");
    CHECK(r != nullptr && r->racks);
    const FxRow* b = FindFxRow("wn_submachine_b");
    CHECK(b != nullptr && b->racks);
}

void SmgASounds()
{
    // RESEARCH step 5: SubMachineA_offload_01 vol 1 / SubMachineA_load_01 vol 0.5 / SubMachineA_reload_01 vol 0.5
    const FxRow* r = FindFxRow("wn_submachine_a");
    if (!r) { CHECK(false); return; }
    int n = 0;
    // Jay 2026-11-02 after RUN SG2 ("Yes." / "Do it"): the mag-out sound at the GRAB (the release), none at the pull-out; each sound
    // started at its measured offset (tools/data/reload_sound_offsets.json - 10 ms before the click's attack; Jay "That should be fine").
    const Snd* s = SoundsFor(r, Fx::MagGrab, &n);
    CHECK(n == 1 && Is(s, "SubMachineA_offload_01.wav") && std::fabs(s->vol - 1.0f) < 1e-6f);
    if (n == 1) { CHECK(std::fabs(s->startS - 0.2309f) < 1e-4f); }
    CHECK(SoundsFor(r, Fx::MagOut, &n) == nullptr && n == 0);
    s = SoundsFor(r, Fx::Seat, &n);
    CHECK(n == 1 && Is(s, "SubMachineA_load_01.wav") && std::fabs(s->vol - 0.5f) < 1e-6f);
    if (n == 1) { CHECK(std::fabs(s->startS - 0.0329f) < 1e-4f); }
    s = SoundsFor(r, Fx::RackDone, &n);
    CHECK(n == 1 && Is(s, "SubMachineA_reload_01.wav") && std::fabs(s->vol - 0.5f) < 1e-6f);
    if (n == 1) { CHECK(std::fabs(s->startS - 0.2590f) < 1e-4f); }
}

void SmgBSounds()
{
    const FxRow* r = FindFxRow("wn_submachine_d");
    if (!r) { CHECK(false); return; }
    int n = 0;
    CHECK(Is(SoundsFor(r, Fx::MagGrab, &n), "SubMachineB_reload_00.wav") && n == 1);
    CHECK(Is(SoundsFor(r, Fx::Seat, &n), "SubMachineB_reload_01.wav") && n == 1);
    CHECK(Is(SoundsFor(r, Fx::RackDone, &n), "SubMachineB_reload_02.wav") && n == 1);
}

void BerettaSeatIsTwoSoundsApart()
{
    // part_01 at Loop frame 6, part_02 at frame 18, 30 fps: 0.40 s apart in the script. Started at their offsets (part_01 0.1492 s,
    // part_02 0.0965 s), the second is played 0.40 + 0.0965 - 0.1492 = 0.3473 s after the first so the two CLICKS keep the script's
    // spacing.
    const FxRow* r = FindFxRow("wn_pistol_b");
    if (!r) { CHECK(false); return; }
    int n = 0;
    const Snd* s = SoundsFor(r, Fx::Seat, &n);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(Is(&s[0], "beretta_reload_part_01.wav") && std::fabs(s[0].delayS) < 1e-6f && std::fabs(s[0].startS - 0.1492f) < 1e-4f);
        CHECK(Is(&s[1], "beretta_reload_part_02.wav") && std::fabs(s[1].delayS - 0.3473f) < 1e-4f &&
              std::fabs(s[1].startS - 0.0965f) < 1e-4f);
    }
    CHECK(Is(SoundsFor(r, Fx::MagGrab, &n), "beretta_reload_part_00.wav") && n == 1);
}

void EmptySoundNameIsSilent()
{
    // The Beretta script has no Finish sound and no rack: no sound for RackDone; the pull-out (the mag-out sound moved to the grab),
    // the pouch and the rack grab never have sounds.
    const FxRow* r = FindFxRow("wn_pistol_b");
    int n = -1;
    CHECK(SoundsFor(r, Fx::RackDone, &n) == nullptr && n == 0);
    CHECK(SoundsFor(r, Fx::MagOut, &n) == nullptr && n == 0);
    CHECK(SoundsFor(r, Fx::RackGrab, &n) == nullptr && n == 0);
    CHECK(SoundsFor(r, Fx::PouchTake, &n) == nullptr && n == 0);
    CHECK(SoundsFor(nullptr, Fx::Seat, &n) == nullptr && n == 0);
}

void UnknownMeshHasNoRow()
{
    CHECK(FindFxRow("wn_axe_a") == nullptr);
    CHECK(FindFxRow(nullptr) == nullptr);
}

// ---- Rifle manual reload Task 2 (plan 2026-11-03-manual-reload-rifle): the rifle + Railgun rows --------------------------------------

const char* const kM16Meshes[] = { "wn_rifle_a", "wn_rifle_b", "wn_rifle_b_reddot", "wn_rifle_c", "wn_rifle_comando",
                                   "wn_rifle_retrowave", "wn_rifle_rust" };

void M16RowSounds()
{
    // m_fpp_m16.scr: Begin f0 m16_magazine_draw.wav, Loop f0 m16_magazine_load.wav + f23 m16_rack.wav (the rack sound is the RACK
    // step's wherever the script puts it), all vol 1. Seat holds ONE sound.
    const FxRow* r = FindFxRow("wn_rifle_a");
    CHECK(r != nullptr);
    if (!r) { return; }
    CHECK(r->racks);
    int n = 0;
    const Snd* s = SoundsFor(r, Fx::MagGrab, &n);
    CHECK(n == 1 && Is(s, "m16_magazine_draw.wav") && std::fabs(s->vol - 1.0f) < 1e-6f);
    s = SoundsFor(r, Fx::Seat, &n);
    CHECK(n == 1 && Is(s, "m16_magazine_load.wav") && std::fabs(s->vol - 1.0f) < 1e-6f);
    s = SoundsFor(r, Fx::RackDone, &n);
    CHECK(n == 1 && Is(s, "m16_rack.wav") && std::fabs(s->vol - 1.0f) < 1e-6f);
    for (const char* m : kM16Meshes) {
        const FxRow* q = FindFxRow(m);
        if (!q) { std::printf("  no fx row: %s\n", m); }
        CHECK(q != nullptr && q->racks && std::strcmp(q->prefix, "m16") == 0);
    }
}

void RackSoundNotInSeat()
{
    // No rifle row's magOut / seat holds a "_rack" sound - it belongs to the rack step only.
    const char* const all[] = { "wn_rifle_a", "wn_rifle_b", "wn_rifle_b_reddot", "wn_rifle_c", "wn_rifle_comando",
                                "wn_rifle_retrowave", "wn_rifle_rust", "wn_rifle_d" };
    for (const char* m : all) {
        const FxRow* r = FindFxRow(m);
        CHECK(r != nullptr);
        if (!r) { continue; }
        for (int i = 0; i < r->nSeat; ++i) { CHECK(std::strstr(r->seat[i].name, "_rack") == nullptr); }
        for (int i = 0; i < r->nMagOut; ++i) { CHECK(std::strstr(r->magOut[i].name, "_rack") == nullptr); }
    }
}

void RailgunHasNoRackSound()
{
    // m_fpp_railgun.scr puts m16_rack.wav in the Loop; the Railgun does not rack (Jay: "I can't see a rack pull on it") so it is dropped.
    const FxRow* r = FindFxRow("wn_rifle_d_hq.msh");
    CHECK(r != nullptr);
    if (!r) { return; }
    CHECK(!r->racks);
    CHECK(r->nRack == 0);
    int n = -1;
    CHECK(SoundsFor(r, Fx::RackDone, &n) == nullptr && n == 0);
    CHECK(SoundsFor(r, Fx::MagGrab, &n) != nullptr && n == 1 && Is(&r->magOut[0], "astro_rifle_magazine_draw.wav"));
    CHECK(SoundsFor(r, Fx::Seat, &n) != nullptr && n == 1 && Is(&r->seat[0], "astro_rifle_magazine_load.wav"));
}

void CommandoRowExists()
{
    // wn_rifle_comando's mesh is in common_meshes_PC.rpack, but the row comes from its inventory Item block.
    const FxRow* r = FindFxRow("wn_rifle_comando");
    CHECK(r != nullptr && r->racks);
}

void SmgARowUnchanged()
{
    // Full-row snapshot of the pre-rifle table (values in reload_fx_table.inc before Task 2).
    const FxRow* r = FindFxRow("wn_submachine_a");
    CHECK(r != nullptr);
    if (!r) { return; }
    CHECK(std::strcmp(r->prefix, "submachinegun") == 0 && r->racks);
    CHECK(r->nMagOut == 1 && Is(&r->magOut[0], "SubMachineA_offload_01.wav") && r->magOut[0].vol == 1.0f &&
          r->magOut[0].delayS == 0.0f && std::fabs(r->magOut[0].startS - 0.2309f) < 1e-6f);
    CHECK(r->nSeat == 1 && Is(&r->seat[0], "SubMachineA_load_01.wav") && r->seat[0].vol == 0.5f && r->seat[0].delayS == 0.0f &&
          std::fabs(r->seat[0].startS - 0.0329f) < 1e-6f);
    CHECK(r->nRack == 1 && Is(&r->rack[0], "SubMachineA_reload_01.wav") && r->rack[0].vol == 0.5f && r->rack[0].delayS == 0.0f &&
          std::fabs(r->rack[0].startS - 0.2590f) < 1e-6f);
}

void PistolBRowUnchanged()
{
    const FxRow* r = FindFxRow("wn_pistol_b");
    CHECK(r != nullptr);
    if (!r) { return; }
    CHECK(std::strcmp(r->prefix, "beretta") == 0 && !r->racks);
    CHECK(r->nMagOut == 1 && Is(&r->magOut[0], "beretta_reload_part_00.wav") && r->magOut[0].vol == 1.0f &&
          std::fabs(r->magOut[0].startS - 0.0949f) < 1e-6f);
    CHECK(r->nSeat == 2 && Is(&r->seat[0], "beretta_reload_part_01.wav") && std::fabs(r->seat[0].startS - 0.1492f) < 1e-6f &&
          Is(&r->seat[1], "beretta_reload_part_02.wav") && std::fabs(r->seat[1].delayS - 0.3473f) < 1e-6f &&
          std::fabs(r->seat[1].startS - 0.0965f) < 1e-6f);
    CHECK(r->nRack == 0);
}

// ---- Crossbow manual reload Task 2 (plan 2026-11-03-manual-reload-crossbow; spec 4.9): m_fpp_crossbow.scr ReloadBegin plays
// crossbow_load_begin_00.wav -> OUR catch (RackDone), ReloadLoop crossbow_load_bolt_00.wav -> the seat; no mag-out sound.
void CrossbowRacksAndSounds()
{
    const char* const kXbows[] = { "wn_crossbow_f_hq.msh", "dlc_wn_crossbow_a", "dlc_wn_crossbow_a_upgrade_a" };
    for (const char* m : kXbows) {
        const FxRow* r = FindFxRow(m);
        if (!r) { std::printf("  no fx row: %s\n", m); }
        CHECK(r != nullptr);
        if (!r) { continue; }
        CHECK(std::strcmp(r->prefix, "crossbow") == 0 && r->racks);
        int n = -1;
        const Snd* s = SoundsFor(r, Fx::RackDone, &n);
        CHECK(n == 1 && Is(s, "crossbow_load_begin_00.wav"));
        if (n == 1) { CHECK(s->delayS == 0.0f && s->startS >= 0.0f && s->startS < 1.0f && s->vol > 0.0f); }
        s = SoundsFor(r, Fx::Seat, &n);
        CHECK(n == 1 && Is(s, "crossbow_load_bolt_00.wav"));
        if (n == 1) { CHECK(s->delayS == 0.0f && s->startS >= 0.0f && s->startS < 1.0f && s->vol > 0.0f); }
        CHECK(SoundsFor(r, Fx::MagGrab, &n) == nullptr && n == 0);
        CHECK(r->nMagOut == 0);
        CHECK(SoundsFor(r, Fx::RackGrab, &n) == nullptr && n == 0);
    }
}

// ---- Pump shotgun manual reload Task 2 (plan 2026-11-04-manual-reload-pump-shotgun; INV-677): m_fpp_spas.scr's Shotgun_PA rows.
// Fix round 1 (ledger rulings R10 / R12): CONCRETE bank names, each with its OWN start offset (tools/data/reload_sound_offsets.json):
// seat = ShotgunB_bullet_load_00.wav (0.1296), rack = ShotgunB_reload_00.wav (0.0927, script vol 0.7), and the magOut slot carries the
// stroke's BACK-end sound ShotgunB_move_00.wav (0.0614, script vol 0.7) - Task 4 plays it on RackBack.
void ShotgunPaRows()
{
    const char* const kPumps[] = { "wn_shotgun_b_hq.msh", "wn_shotgun_f" };
    for (const char* m : kPumps) {
        const FxRow* r = FindFxRow(m);
        if (!r) { std::printf("  no fx row: %s\n", m); }
        CHECK(r != nullptr);
        if (!r) { continue; }
        CHECK(std::strcmp(r->prefix, "shotgun_pa") == 0 && r->racks);
        int n = -1;
        const Snd* s = SoundsFor(r, Fx::Seat, &n);
        CHECK(n == 1 && Is(s, "ShotgunB_bullet_load_00.wav"));
        if (n == 1) { CHECK(s->delayS == 0.0f && std::fabs(s->startS - 0.1296f) < 1e-6f && std::fabs(s->vol - 1.0f) < 1e-6f); }
        s = SoundsFor(r, Fx::RackDone, &n);
        CHECK(n == 1 && Is(s, "ShotgunB_reload_00.wav"));
        if (n == 1) { CHECK(s->delayS == 0.0f && std::fabs(s->startS - 0.0927f) < 1e-6f && std::fabs(s->vol - 0.7f) < 1e-6f); }
        CHECK(r->nMagOut == 1 && Is(&r->magOut[0], "ShotgunB_move_00.wav"));
        if (r->nMagOut == 1) {
            CHECK(r->magOut[0].delayS == 0.0f && std::fabs(r->magOut[0].startS - 0.0614f) < 1e-6f && std::fabs(r->magOut[0].vol - 0.7f) < 1e-6f);
        }
        // no script "_XX" name survives in the row (R10)
        for (const Snd* q : { &r->magOut[0], &r->seat[0], &r->rack[0] }) { CHECK(q->name && !std::strstr(q->name, "_XX")); }
    }
    // 2026-11-07 the semi-auto (Shotgun_SA, Jay "No rack"): racks false, ONE sound - each shell's load (the same ShotgunB_bullet_load_00
    // as the pumps, its own start offset 0.1296); no mag-out, no rack sound
    const FxRow* c = FindFxRow("wn_shotgun_c_hq.msh");
    CHECK(c != nullptr);
    if (c) {
        CHECK(std::strcmp(c->prefix, "shotgun_sa") == 0 && !c->racks);
        int n = -1;
        const Snd* s = SoundsFor(c, Fx::Seat, &n);
        CHECK(n == 1 && Is(s, "ShotgunB_bullet_load_00.wav"));
        if (n == 1) { CHECK(s->delayS == 0.0f && std::fabs(s->startS - 0.1296f) < 1e-6f && std::fabs(s->vol - 1.0f) < 1e-6f); }
        CHECK(c->nMagOut == 0 && c->nRack == 0);
        SoundsFor(c, Fx::RackDone, &n);
        CHECK(n == 0);
    }
    // 2026-11-07 the double-barrel (prefix Shotgun): magOut = the break open, seat = each shell, rack = the close (m_fpp_spas.scr)
    const FxRow* a = FindFxRow("wn_shotgun_a_hq.msh");
    CHECK(a != nullptr);
    if (a) {
        CHECK(std::strcmp(a->prefix, "shotgun") == 0 && a->racks && a->nMagOut == 1 && a->nSeat == 1 && a->nRack == 1);
        int n = -1;
        const Snd* s = SoundsFor(a, Fx::MagGrab, &n);
        CHECK(n == 1 && Is(s, "rmngtn_open.wav") && std::fabs(s->startS - 0.0733f) < 1e-6f);
        s = SoundsFor(a, Fx::Seat, &n);
        CHECK(n == 1 && Is(s, "rmngtn_bullet_load.wav") && std::fabs(s->startS - 0.1567f) < 1e-6f);
        s = SoundsFor(a, Fx::RackDone, &n);
        CHECK(n == 1 && Is(s, "rmngtn_close.wav") && std::fabs(s->startS - 0.2127f) < 1e-6f);
    }
}

// 2026-11-09 (plan 2026-11-09-manual-reload-remaining-guns Task 2): the grenade launcher (prefix GrenadeLauncher, the drum = a mag, no
// rack - Jay: "pull the drum out by hand like a mag, put a new one in") has a row: its own reload sounds, racks false.
void GrenadeLauncherRow()
{
    const FxRow* g = FindFxRow("wn_grenadelauncher_a_hq.msh");
    CHECK(g != nullptr);
    if (g) {
        CHECK(std::strcmp(g->prefix, "grenadelauncher") == 0);
        CHECK(!g->racks);
        CHECK(g->nMagOut + g->nSeat >= 1);   // at least one of the drum's own sounds
    }
}

// 2026-11-10 (Jay: "Sort out the sounds for the revolver & chainsaw"): the revolvers' open / each round / close from their FPP reload
// sequences + the fan-cock = the Shot sequence's hammer_pull (m_fpp_walker / sixshooter / schofield.scr, OUR Data0.pak); no rack.
void RevolverRowsSounds()
{
    struct Want { const char* mesh; const char* prefix; const char* open; const char* seat; int nSeat; const char* close; int nClose;
                  const char* cock; };
    const Want w[] = {
        { "wn_revolver_walker_hq.msh", "walker", "RevolverRanger_reload_open.wav", "RevolverRanger_reload_bullet_insert.wav", 2,
          "RevolverRanger_reload_close.wav", 2, "RevolverRanger_hammer_pull.wav" },
        { "wn_revolver_sixshooter", "sixshooter", "RevolverSixshooter_reload_open.wav", "RevolverSixshooter_reload_bullet_insert.wav", 2,
          "RevolverSixshooter_reload_close.wav", 2, "RevolverSixshooter_hammer_pull_00.wav" },
        { "wn_revolver_schofield", "schofield", "RevolverQuickshooter_reload_open.wav", "RevolverQuickshooter_reload_bullet_insert.wav", 1,
          "RevolverQuickshooter_reload_close.wav", 1, "RevolverQuickshooter_hammer_pull.wav" },
    };
    for (const Want& x : w) {
        const FxRow* r = FindFxRow(x.mesh);
        if (!r) { std::printf("  no fx row: %s\n", x.mesh); }
        CHECK(r != nullptr);
        if (!r) { continue; }
        CHECK(std::strcmp(r->prefix, x.prefix) == 0 && !r->racks);
        int n = -1;
        CHECK(Is(SoundsFor(r, Fx::MagGrab, &n), x.open) && n == 1);
        CHECK(Is(SoundsFor(r, Fx::Seat, &n), x.seat) && n == x.nSeat);
        CHECK(Is(SoundsFor(r, Fx::RackDone, &n), x.close) && n == x.nClose);
        const Snd* c = SoundsFor(r, Fx::Cock, &n);
        CHECK(Is(c, x.cock) && n == 1);
        if (n == 1) { CHECK(c->delayS == 0.0f && c->startS >= 0.0f && c->startS < 0.3f && std::fabs(c->vol - 0.5f) < 1e-6f); }
    }
}

// 2026-11-10: the chainsaw's one reload sound (a 5.37 s recording of the whole reload) at the CORD PULL only, from 2.70 s (Jay listened
// to the cuts: "Part 3 (2.7 s onward)"; "Silent pour, cord sound only"). Volume: first written 1.0; RUN 11-10 n3n4 (5 plays, Jay: "no pull
// sound on the cord") -> "Use the game's 20" = the script's own volume (m_fpp_chainsaw.scr PlaySound2DExternal(..., 20, 0, 1)).
void ChainsawRowCordOnly()
{
    const FxRow* r = FindFxRow("wn_chainsaw_a");
    CHECK(r != nullptr);
    if (!r) { return; }
    CHECK(std::strcmp(r->prefix, "chainsaw") == 0 && !r->racks);
    CHECK(r->nMagOut == 0 && r->nSeat == 0 && r->nCock == 0);
    int n = -1;
    const Snd* s = SoundsFor(r, Fx::RackDone, &n);
    CHECK(n == 1 && Is(s, "Chainsaw_reload.wav"));
    // RUN CF1 (from 0 s: heard) -> INV-714 + Jay: "Fix for all guns" (seek via slot 28) -> Jay's 2.70 pick restored
    if (n == 1) { CHECK(std::fabs(s->startS - 2.70f) < 1e-6f && std::fabs(s->vol - 20.0f) < 1e-6f && s->delayS == 0.0f); }
    CHECK(SoundsFor(r, Fx::PouchTake, &n) == nullptr && n == 0);
    CHECK(SoundsFor(r, Fx::Seat, &n) == nullptr && n == 0);
}

// The cock slot is the revolvers' only: every other row has none (no gun plays a hammer pull it never had).
void CockSlotOnlyOnRevolvers()
{
    int with = 0;
    for (const FxRow& r : kFxRows) {
        const bool rv = std::strcmp(r.prefix, "walker") == 0 || std::strcmp(r.prefix, "sixshooter") == 0 ||
                        std::strcmp(r.prefix, "schofield") == 0;
        if (!rv && r.nCock != 0) { std::printf("  cock sound on a non-revolver: %s\n", r.mesh); }
        CHECK(rv ? r.nCock == 1 : r.nCock == 0);
        with += r.nCock ? 1 : 0;
    }
    CHECK(with == 3);
    int n = -1;
    CHECK(SoundsFor(FindFxRow("wn_submachine_a"), Fx::Cock, &n) == nullptr && n == 0);
}

}  // namespace

int main()
{
    RevolverRowsSounds();
    ChainsawRowCordOnly();
    CockSlotOnlyOnRevolvers();
    GrenadeLauncherRow();
    ShotgunPaRows();
    CrossbowRacksAndSounds();
    M16RowSounds();
    RackSoundNotInSeat();
    RailgunHasNoRackSound();
    CommandoRowExists();
    SmgARowUnchanged();
    PistolBRowUnchanged();
    EveryReloadGunHasRow();
    PistolRowDoesNotRack();
    SmgARowRacks();
    SmgASounds();
    SmgBSounds();
    BerettaSeatIsTwoSoundsApart();
    EmptySoundNameIsSilent();
    UnknownMeshHasNoRow();
    std::printf("reload_fx_test: %d row(s), %d failure(s)\n", kFxRowCount, g_fail);
    return g_fail;
}
