// Solid guns - plan docs/superpowers/plans/2026-11-02-solid-guns-reload-sound-haptics.md Task 4; research
// docs/RESEARCH-SOLID-GUNS-2026-11-02.md; rulings .superpowers/sdd/2026-11-02-solid-guns/progress.md.
//
// HOW:
//   - The held gun = the right hand's model, *(BoneProbeCapturedEntity() + 0x1020) (hand 0 = right; reload.cpp kHeldItemOff, INV-277).
//     Its mesh name = ItemMeshName (boneblend.cpp, the guarded read [TWOHAND] / [RELOAD] use), ONCE per model change; the row =
//     gun_shapes.h FindGunShapeRow (29 firearms; none = this gun is not solid, said once by name).
//   - The boxes are in the gun's model frame = its skeleton root (row->frameNode, "holder" / "holder_0"), posed by that element's
//     GetElementWorldMatrix every update. FRAMECHECK (once per draw, first update with no reload step live): bone_magazine's live
//     position in that frame vs the table's magRestT - the run's proof that the frame is the one the boxes were built in.
//   - Parts (a suppressor, shells, an add-on): each part's element id by name once per draw; IsElementHidden per update - a hidden
//     part's boxes are skipped (pistol b's 'silencer' is hidden - STATE).
//   - The seated magazine is part of the gun: while no reload step is live (ReloadWithholdsR2() false) its box (row->mag, in
//     bone_magazine's frame) is posed by bone_magazine's world and stops the hand too. During a step the hand holds the mag.
//   - The stop: THE GHOST HAND (2026-11-02, D-309's replacement) - gunshape_logic.h GhostStep on the hull and the seated mag: one
//     wrist kept in the gun's frame, advanced by one owner thread, sliding on contact; the other solve reads it. One PulseController
//     per touch (handcoll::ContactEdge), hand_collision_buzz_amp / _ms.
// THREADS: the Tick on the game thread (HookTail: the tail solve, then ItemCtlPlace / ReloadTick, then the Tick - its pose is used by
// the NEXT solves, RUN SG2); the Stops on WHICHEVER thread solves the left arm (the tail solve AND the pad-poll solve - RUN SG3: the
// pad-poll solve is ~95 % of them), all under one exclusive SRW lock (g_lock); the pad-poll ones are counted in the TOUCH lines.
#include "gunshape.h"

#include "boneblend.h"      // ItemMeshName
#include "camhook.h"        // BoneProbeCapturedEntity
#include "controllers.h"    // PulseController
#include "gun_shapes.h"     // the table + FindGunShapeRow (this TU's copy - rows never leave this file)
#include "gunshape_logic.h"
#include "hand_shape.h"     // the LEFT hand's spheres (tools/hand_shape.py, fitted to the game's hand mesh)
#include "handcoll.h"       // HandCollision / HandCollisionBuzzAmp / HandCollisionBuzzMs
#include "reload.h"         // ReloadWithholdsR2 - a reload step is live

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {
namespace {

namespace gs = ::dlvr::gunshape;

const char* const kExpElemId = "?GetElementID@IModelObject@@QEBAHPEBD@Z";
const char* const kExpWorld = "?GetElementWorldMatrix@IModelObject@@QEBAAEBVmtx34@@H@Z";
const char* const kExpHidden = "?IsElementHidden@IModelObject@@QEBA_NH@Z";
using ElemIdFn = int(__fastcall*)(const void* model, const char* name);
using ElemMtxFn = const float*(__fastcall*)(const void* model, int i);
using ElemBoolFn = bool(__fastcall*)(const void* model, int i);

constexpr size_t kHeldItemOff = 0x1020;   // captured entity + 0x1020 + hand*8 -> the held model (reload.cpp, INV-277)
constexpr int    kMaxParts = 32;
constexpr long   kMaxEventLines = 200;     // DRAW / FRAMECHECK / TOUCH / refusals (INV-9: a cap per kind is below)

ElemIdFn   g_elemId = nullptr;
ElemMtxFn  g_world = nullptr;
ElemBoolFn g_hidden = nullptr;
int        g_resolved = 0;                 // 0 = not tried, 1 = ok, -1 = refused
DWORD      g_tickThread = 0;

// The held gun, per model change.
uintptr_t                g_model = 0;
uintptr_t                g_modelInv = 0;   // g_model's InventoryItem at OnDraw (HeldItemInventoryItem) - a reused address is another item
const gs::GunShapeRow*   g_row = nullptr;  // this TU's table copy
// Each row's box grid (gunshape_logic.h BuildBoxGrid - the solid hand's cost fix), built the first time the row is drawn and then
// NEVER changed or freed: a stop reads it outside the lock. g_rowGrid = the current row's (under the lock).
std::unique_ptr<gs::BoxGrid> g_grids[gs::kGunShapeRowCount];
const gs::BoxGrid*           g_rowGrid = nullptr;
int                      g_frameId = -1, g_magId = -1;
int                      g_partId[kMaxParts] = {};
bool                     g_frameChecked = false;
// Posed this update.
bool     g_posed = false;
float    g_gun34[12] = {};
float    g_mag34[12] = {};
bool     g_magSolid = false;
unsigned g_hiddenParts = 0;
// Contact.
handcoll::ContactEdge g_edge;
handcoll::ContactEdge g_magEdge;   // the held mag against the gun (Task 5)
long g_nMagTouch = 0;
LONG64 g_magSolves = 0, g_magStops = 0;
bool g_saidFirstMag = false;
// Review finding 3: the previous held-mag solve's COMMANDED handholder pose (after its own shift) - paired with the reload's drawn mag
// of that update, it gives the mag in the commanded frame (RelFromLast). Reset when no mag is in the hand.
float g_lastH[12] = {};
bool  g_haveLastH = false;
bool  g_escaping = false;   // RUN SG2's ESCAPE probe: the previous mag solve saw the mag leaving the well sideways
long  g_nEscape = 0;
long g_nDraw = 0, g_nTouch = 0, g_nCheck = 0;
LONG64 g_offThread = 0, g_solves = 0, g_stops = 0;
bool g_saidFirstStop = false;
// RUN SG3 (Jay: "Hands are still phasing through the gun"; the log: solves 914, OFF-THREAD 16458): ~95 % of the left solves are the
// pad-poll solve on ANOTHER thread, which the stops used to refuse. Now every stop acts on whichever thread solves, and ONE exclusive
// lock guards everything above (the posed gun the tick writes, the edges / counters / held-mag state the stops write). COST: one
// uncontended SRW acquire per tick and per left solve; the tick holds it for its few guarded reads, a stop for its own maths.
// THE HAND'S SHAPE (Jay 2026-11-02: "consider the players hand & fingers"): the left hand's joints in l_hand's frame (the write
// pass), the drawn-vs-commanded hand rotation (the left solve), and the last solve's wrist (the sweep's start). Under g_lock.
float  g_jm[gs::kHandJoints][12] = {};
bool   g_haveJoints = false;
double g_jointsAt = 0.0;
bool   g_saidJoints = false;
float  g_q[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };   // drawn = commanded x g_q (both with unit columns) - ONE shared value (D-310)
bool   g_haveQ = false;
const char* const kShapeUnsaid = "(not said yet)";
const char* g_saidShapeWhy = kShapeUnsaid;   // the last HAND SHAPE state said (nullptr = in use; else the reason) - once per change
LONG64 g_handSolves = 0;
constexpr double kJointsFreshS = 0.5;    // joints older than this: the hand's shape is not used (one fingertip sphere is)
constexpr float  kFallbackR = 0.005f;    // that fingertip sphere's radius (the old single point, as a 5 mm sphere)
// THE GHOST HAND (D-309's replacement, Jay 2026-11-02 "I want this fixed from the ground up" -> "Build it."): ONE wrist, kept in the
// GUN's model frame (g_gun34's frame - a moving gun carries it), advanced ONE STEP AT A TIME (gunshape_logic.h GhostStep).
// D-310 (RUN SG6): NO THREAD IDENTITY. The left solve rotates over the engine's job threads (INV-282) - SG6 picked an "owner thread"
// and it changed 8321 times in 9716 steps. Now: whichever solve takes g_stepLock (TryAcquire - never waits) steps the ghost; a solve
// that finds a step in progress READS the last published result instead. Under g_lock (state) / g_stepLock (one step at a time).
gs::GhostState g_ghostL;                 // .w = the ghost wrist, .nrm = the last contact normal, .to = the last target - GUN's frame
bool   g_ghostTouch = false;             // the last step held the hand back: a reading solve puts T at the ghost
LONG64 g_steps = 0, g_readerApplied = 0, g_busySkips = 0, g_nDepen = 0, g_nDropped = 0, g_nReleased = 0, g_nWaiting = 0;
long   g_nDropLine = 0;
unsigned g_drawSeq = 0;                  // bumped by every OnDraw / Forget - a step computed across a change is not written back
SRWLOCK g_stepLock = SRWLOCK_INIT;       // held for a whole ghost step; ORDER: g_stepLock may be held while taking g_lock, never the reverse
SRWLOCK g_lock = SRWLOCK_INIT;
struct Excl {
    Excl() { AcquireSRWLockExclusive(&g_lock); }
    ~Excl() { ReleaseSRWLockExclusive(&g_lock); }
    Excl(const Excl&) = delete;
    Excl& operator=(const Excl&) = delete;
};

double NowS()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

bool ReadPtr(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0; return false; }
}
bool ElemId(uintptr_t model, const char* name, int* out)
{
    __try { *out = g_elemId(reinterpret_cast<const void*>(model), name); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = -1; return false; }
}
bool ElemWorld(uintptr_t model, int i, float out12[12])
{
    __try {
        const float* m = g_world(reinterpret_cast<const void*>(model), i);
        if (!m) { return false; }
        for (int k = 0; k < 12; ++k) { out12[k] = m[k]; }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ElemHidden(uintptr_t model, int i, bool* out)
{
    __try { *out = g_hidden(reinterpret_cast<const void*>(model), i); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = false; return false; }
}

bool Resolve()
{
    if (g_resolved != 0) { return g_resolved > 0; }
    HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!en) {
        g_resolved = -1;
        // [PROBE: GUNSHAPE] gate=hand_collision  site=FLAT  log-only=yes
        LOG("gunshape: [GUNSHAPE] REFUSED: engine_x64_rwdi.dll is not loaded - guns are NOT solid this session.");
        return false;
    }
    g_elemId = reinterpret_cast<ElemIdFn>(GetProcAddress(en, kExpElemId));
    g_world = reinterpret_cast<ElemMtxFn>(GetProcAddress(en, kExpWorld));
    g_hidden = reinterpret_cast<ElemBoolFn>(GetProcAddress(en, kExpHidden));
    const char* missing = !g_elemId ? kExpElemId : !g_world ? kExpWorld : !g_hidden ? kExpHidden : nullptr;
    if (missing) {
        g_resolved = -1;
        // [PROBE: GUNSHAPE] gate=hand_collision  site=FLAT  log-only=yes
        LOG("gunshape: [GUNSHAPE] REFUSED: the engine does not export %s - guns are NOT solid this session.", missing);
        return false;
    }
    g_resolved = 1;
    int nBoxes = 0, nWells = 0;
    for (int i = 0; i < gs::kGunShapeRowCount; ++i) {
        nBoxes += gs::kGunShapeRows[i].shape.nBoxes;
        nWells += gs::kGunShapeRows[i].shape.hasWell ? 1 : 0;
    }
    // [PROBE: GUNSHAPE] gate=hand_collision  site=FLAT  log-only=yes
    LOG("gunshape: [GUNSHAPE] READY: %d gun row(s), %d box(es), %d with a magwell; exports GetElementID / GetElementWorldMatrix / "
        "IsElementHidden by name; the LEFT hand stops on the held gun (and its seated mag), buzz %.2f / %.0f ms; thread %lu.",
        gs::kGunShapeRowCount, nBoxes, nWells, HandCollisionBuzzAmp, HandCollisionBuzzMs, GetCurrentThreadId());
    return true;
}

void Forget()
{
    g_model = 0;
    g_modelInv = 0;
    g_row = nullptr;
    g_rowGrid = nullptr;
    g_posed = false;
    g_magSolid = false;
    g_edge.Reset();
    g_magEdge.Reset();
    g_haveLastH = false;
    g_ghostL = gs::GhostState{};
    g_ghostTouch = false;
    ++g_drawSeq;
}

// Unit columns of a row-major 3x3 (axes as columns). False = degenerate / non-finite.
bool UnitCols(const float in9[9], float out9[9])
{
    for (int c = 0; c < 3; ++c) {
        const float l = std::sqrt(in9[c] * in9[c] + in9[3 + c] * in9[3 + c] + in9[6 + c] * in9[6 + c]);
        if (!(l > 1e-6f) || !std::isfinite(l)) { return false; }
        for (int r = 0; r < 3; ++r) { out9[3 * r + c] = in9[3 * r + c] / l; }
    }
    return true;
}

// A new held model: find its row and its elements, once.
void OnDraw(uintptr_t model)
{
    Forget();
    g_model = model;
    g_frameChecked = false;
    char mesh[96] = {};
    const bool haveMesh = ItemMeshName(model, mesh, sizeof(mesh));
    g_row = haveMesh ? gs::FindGunShapeRow(mesh) : nullptr;
    if (!g_row) {
        if (++g_nDraw <= kMaxEventLines) {
            // [PROBE: GUNSHAPE] gate=hand_collision  site=FLAT  log-only=yes
            LOG("gunshape: [GUNSHAPE] DRAW model %p mesh '%s': NO SHAPE ROW - this item is not solid (not one of the %d firearm "
                "rows, or the mesh name was unreadable).", reinterpret_cast<void*>(model), haveMesh ? mesh : "(unreadable)",
                gs::kGunShapeRowCount);
        }
        return;
    }
    ElemId(model, g_row->frameNode, &g_frameId);
    g_magId = -1;
    // Rifle plan Task 5: the ROW's mag bone (bone_magazine on pistols / SMGs, bone_clip on the rifles; "" = neither) - once per draw.
    const char* magBone = (g_row->magBone && g_row->magBone[0]) ? g_row->magBone : nullptr;
    if (g_row->hasMag && magBone) { ElemId(model, magBone, &g_magId); }
    char parts[256] = {};
    size_t at = 0;
    for (int p = 0; p < g_row->nParts && p < kMaxParts; ++p) {
        g_partId[p] = -1;
        ElemId(model, g_row->parts[p], &g_partId[p]);
        bool hid = false;
        const bool readHid = g_partId[p] >= 0 && ElemHidden(model, g_partId[p], &hid);
        // Review finding 5: bounded by the bytes REMAINING (truncation returns -1 and ends the list - never a write past the buffer).
        const int w = _snprintf_s(parts + at, sizeof(parts) - at, _TRUNCATE, "%s%s=%d%s", p ? ", " : "", g_row->parts[p], g_partId[p],
                                  g_partId[p] < 0 ? " (NOT FOUND - solid)" : !readHid ? " (hidden UNREADABLE - solid)" : hid ? " hidden" : "");
        if (w > 0) { at += static_cast<size_t>(w); } else { break; }
    }
    if (g_frameId < 0) {
        if (++g_nDraw <= kMaxEventLines) {
            // [PROBE: GUNSHAPE] gate=hand_collision  site=FLAT  log-only=yes
            LOG("gunshape: [GUNSHAPE] DRAW model %p mesh '%s' row %s: REFUSED - frame element '%s' not found on the model; this gun is "
                "NOT solid.", reinterpret_cast<void*>(model), mesh, g_row->mesh, g_row->frameNode);
        }
        g_row = nullptr;
        return;
    }
    // The row's box grid, once (never changed after - stops read it outside the lock).
    const ptrdiff_t ri = g_row - gs::kGunShapeRows;
    if (ri >= 0 && ri < gs::kGunShapeRowCount) {
        if (!g_grids[ri]) {
            std::unique_ptr<gs::BoxGrid> gb(new gs::BoxGrid());
            gs::BuildBoxGrid(g_row->shape, gb.get());
            g_grids[ri] = std::move(gb);
        }
        g_rowGrid = g_grids[ri].get();
    }
    if (++g_nDraw <= kMaxEventLines) {
        // [PROBE: GUNSHAPE] gate=hand_collision  site=FLAT  log-only=yes
        LOG("gunshape: [GUNSHAPE] DRAW model %p mesh '%s' row %s: %d box(es), well %s, frame '%s' = element %d, mag bone %s %d, "
            "parts [%s]; box grid %s (%d x %d x %d cells, %zu entries).", reinterpret_cast<void*>(model), mesh, g_row->mesh,
            g_row->shape.nBoxes, g_row->shape.hasWell ? "yes" : "no", g_row->frameNode, g_frameId, magBone ? magBone : "(none)",
            g_magId, parts,
            g_rowGrid && g_rowGrid->ok ? "built" : "*** NOT BUILT - every box is tested ***", g_rowGrid ? g_rowGrid->n[0] : 0,
            g_rowGrid ? g_rowGrid->n[1] : 0, g_rowGrid ? g_rowGrid->n[2] : 0, g_rowGrid ? g_rowGrid->idx.size() : size_t(0));
    }
}

// FRAMECHECK: bone_magazine's live position in the frame element's frame vs the table's rest (mm). Once per draw.
void FrameCheck()
{
    if (g_frameChecked || !g_row || !g_row->hasMag || g_magId < 0 || !g_magSolid) { return; }
    g_frameChecked = true;
    float n[9];
    gs::detail::UnitRot(g_gun34, n);
    const float p[3] = { g_mag34[3], g_mag34[7], g_mag34[11] };
    float loc[3];
    gs::detail::ToLocalPt(n, g_gun34, p, loc);
    const float d[3] = { loc[0] - g_row->magRest34[3], loc[1] - g_row->magRest34[7], loc[2] - g_row->magRest34[11] };
    const float mm = 1000.0f * std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (++g_nCheck <= kMaxEventLines) {
        // [PROBE: GUNSHAPE] gate=hand_collision  site=FLAT  log-only=yes
        LOG("gunshape: [GUNSHAPE] FRAMECHECK row %s: %s in '%s' frame (%+.4f %+.4f %+.4f), table rest (%+.4f %+.4f %+.4f) "
            "-> %.1f mm off%s.", g_row->mesh, g_row->magBone, g_row->frameNode, loc[0], loc[1], loc[2], g_row->magRest34[3], g_row->magRest34[7],
            g_row->magRest34[11], mm, mm <= 5.0f ? " (the boxes' frame holds)" : " - LARGER than 5 mm: the frame or the seated pose differs");
    }
}

}  // namespace

void GunShapeTick(void* ent)
{
    (void)ent;
    Excl ex;   // the stops on the pad thread read what this writes (RUN SG3)
    if (!HandCollision) {
        if (g_model) { Forget(); }
        return;
    }
    if (!Resolve()) { return; }
    g_tickThread = GetCurrentThreadId();
    g_posed = false;
    const uintptr_t cap = BoneProbeCapturedEntity();
    uintptr_t model = 0;
    if (!cap || !ReadPtr(cap + kHeldItemOff, &model) || !model) {
        if (g_model) { Forget(); }
        return;
    }
    // [LOADBEARING: hand_collision] once per (pointer, InventoryItem): the game reuses the model address on a swap (a tester, 2026-11-08),
    // and a gun back at a reused address kept the previous item's row. COST: two guarded reads per tick.
    const uintptr_t inv = HeldItemInventoryItem(model);
    if (model != g_model || inv != g_modelInv) {
        OnDraw(model);
        g_modelInv = inv;
    }
    if (!g_row) { return; }
    if (!ElemWorld(model, g_frameId, g_gun34)) { return; }
    unsigned hidden = 0;
    for (int p = 0; p < g_row->nParts && p < kMaxParts; ++p) {
        bool h = false;
        if (g_partId[p] >= 0 && ElemHidden(model, g_partId[p], &h) && h) { hidden |= 1u << p; }
    }
    g_hiddenParts = hidden;
    g_magSolid = g_row->hasMag && g_magId >= 0 && !ReloadWithholdsR2() && ElemWorld(model, g_magId, g_mag34);
    g_posed = true;
    FrameCheck();
}

void GunShapeNoteHandJoints(const float jm[][12], int n)
{
    if (!HandCollision || n != gs::kHandJoints) { return; }
    Excl ex;
    for (int j = 0; j < gs::kHandJoints; ++j) { for (int k = 0; k < 12; ++k) { g_jm[j][k] = jm[j][k]; } }
    g_haveJoints = true;
    g_jointsAt = NowS();
    if (!g_saidJoints) {
        g_saidJoints = true;   // first sample unconditional
        // THE FK CHECK: each joint's bone length from the live locals vs the fitted skeleton (tools/hand_shape.py) - a different hand,
        // frame or unit shows here before anything relies on it.
        float worst = 0.0f;
        int worstJ = -1;
        for (int j = 0; j < gs::kHandJoints; ++j) {
            const int p = gs::kHandParent[j];
            if (p < 0) { continue; }
            const float d[3] = { jm[j][3] - jm[p][3], jm[j][7] - jm[p][7], jm[j][11] - jm[p][11] };
            const float diff = std::fabs(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - gs::kHandBoneLen[j]);
            if (diff > worst) { worst = diff; worstJ = j; }
        }
        // [PROBE: GUNSHAPE] gate=hand_collision  site=FLAT  log-only=yes
        LOG("gunshape: [GUNSHAPE] HAND JOINTS first publish: %d joints from the game's left finger locals; largest bone-length difference "
            "vs the fitted hand %.2f mm (%s)%s; %d spheres. Index tip joint at (%+.4f %+.4f %+.4f) in l_hand's frame.", n, 1000.0f * worst,
            worstJ >= 0 ? gs::kHandJointNames[worstJ] : "none",
            worst <= 0.002f ? " - the live hand IS the fitted hand" : " - *** LARGER than 2 mm: the spheres may not sit on this hand ***",
            gs::kHandSpheres, jm[7][3], jm[7][7], jm[7][11]);
    }
}

void GunShapeNoteDrawnHand(const float cmd9[9], const float drawn9[9])
{
    if (!HandCollision) { return; }
    float c[9], d[9];
    if (!UnitCols(cmd9, c) || !UnitCols(drawn9, d)) { return; }
    Excl ex;
    // ONE shared q, whichever job thread solved (D-310: per-thread q reset on every thread change in SG6). Drawn vs commanded is
    // ~77 deg (RUN SG2) - the spheres need it.
    // drawn = cmd x q  ->  q = cmd^T x drawn
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) { g_q[3 * r + k] = c[r] * d[k] + c[3 + r] * d[3 + k] + c[6 + r] * d[6 + k]; }
    }
    g_haveQ = true;
}

// GUN COLLISION OFF (Jay 2026-11-02, after RUN SG7: "We need to do something else. Disable the gun collision for now"). The hand
// stop on the held gun and the held-mag stop both return at once; hand_collision stays on for the live-enemy stop (handcoll.cpp),
// and GunShapeTick still poses the gun (cheap). Said once. To restore: kGunCollisionOn = true (the ghost code below is intact).
constexpr bool kGunCollisionOn = false;
volatile LONG g_saidGunOff = 0;
bool GunCollisionOff()
{
    if constexpr (kGunCollisionOn) { return false; }
    if (InterlockedExchange(&g_saidGunOff, 1) == 0) {
        // [PROBE: GUNSHAPE] gate=hand_collision  site=FLAT  log-only=yes
        LOG("gunshape: [GUNSHAPE] GUN COLLISION OFF in this build - the left hand and a held mag pass through the held gun; the "
            "enemy hand stop (hand_collision) is unaffected.");
    }
    return true;
}

bool GunShapeStop(int armIdx, bool inGrip, const float S[3], const float tipOff[3], const float* hand9, float T[3])
{
    (void)S;   // the ghost sweeps from its own last wrist, never from the shoulder
    if (!HandCollision || g_resolved != 1 || armIdx != 1) { return false; }   // the LEFT hand only (gs::ShouldStop's rule)
    if (GunCollisionOff()) { return false; }
    const double now = NowS();
    // ---- 0. ONE STEP AT A TIME, whatever thread (D-310): the solve that gets g_stepLock steps the ghost; a solve that finds a step
    //         in progress reads the last result. TryAcquire never waits. ----
    const bool stepper = TryAcquireSRWLockExclusive(&g_stepLock) != 0;
    struct StepRelease {
        bool on;
        ~StepRelease() { if (on) { ReleaseSRWLockExclusive(&g_stepLock); } }
    } stepRelease{ stepper };
    // ---- 1. UNDER THE LOCK: decide, and (the stepper) copy what the step needs. ----
    const gs::GunShapeRow* row = nullptr;
    const gs::BoxGrid* grid = nullptr;
    float gun34[12], mag34[12], jm[gs::kHandJoints][12], q[9];
    bool magSolid = false, haveQ = false;
    unsigned hidden = 0, seq = 0;
    const char* why = nullptr;
    gs::GhostState ghostL;
    {
        Excl ex;
        const bool want = gs::ShouldStop(armIdx, inGrip, g_posed && g_row != nullptr);
        if (!g_saidFirstStop) {
            g_saidFirstStop = true;   // first sample unconditional
            // [PROBE: GUNSHAPE] gate=hand_collision  site=FLAT  log-only=yes
            LOG("gunshape: [GUNSHAPE] left first stop call: %s (row %s, posed %d, in grip %d, hand shape %s, thread %lu).",
                want ? "STOPPING on the gun" : "not stopping", g_row ? g_row->mesh : "none", g_posed ? 1 : 0, inGrip ? 1 : 0,
                g_haveJoints ? "published" : "NOT YET published", GetCurrentThreadId());
        }
        if (!want) {
            g_edge.Update(false, now);
            g_ghostL = gs::GhostState{};
            g_ghostTouch = false;
            ++g_drawSeq;   // a step running now must not write its ghost back over this reset
            return false;
        }
        ++g_solves;
        if (!stepper) {
            // A step is in progress on another thread: this solve READS the last result. Touching -> T at the ghost (the gun's
            // pose now; the ghost lives in its frame).
            ++g_busySkips;
            if (!g_ghostTouch || !g_ghostL.have) { return false; }
            float n[9], w[3];
            gs::detail::UnitRot(g_gun34, n);
            gs::detail::ToWorldPt(n, g_gun34, g_ghostL.w, w);
            T[0] = w[0]; T[1] = w[1]; T[2] = w[2];
            ++g_readerApplied;
            return true;
        }
        row = g_row;
        grid = g_rowGrid;
        seq = g_drawSeq;
        for (int k = 0; k < 12; ++k) { gun34[k] = g_gun34[k]; mag34[k] = g_mag34[k]; }
        magSolid = g_magSolid;
        hidden = g_hiddenParts;
        ghostL = g_ghostL;
        why = !hand9 ? "the solve has no hand basis yet" : !g_haveJoints ? "the hand's joints were never published"
            : (now - g_jointsAt > kJointsFreshS) ? "the hand's joints are older than 0.5 s" : nullptr;
        if (!why) {
            for (int j = 0; j < gs::kHandJoints; ++j) { for (int k = 0; k < 12; ++k) { jm[j][k] = g_jm[j][k]; } }
            for (int k = 0; k < 9; ++k) { q[k] = g_q[k]; }
            haveQ = g_haveQ;
        }
    }
    // ---- 2. OUTSIDE g_lock, inside g_stepLock (the one writer of the ghost): the step. ----
    gs::GunShape g = row->shape;
    g.hiddenParts = hidden;
    float R[9];
    if (!why && !UnitCols(hand9, R)) { why = "the hand basis is degenerate"; }
    if (!why && !haveQ) { why = "the drawn-vs-commanded hand turn is not known yet"; }   // never place the spheres 77 deg off (D-310)
    float off[gs::kHandSpheres][3], rad[gs::kHandSpheres];
    int nSph = gs::kHandSpheres;
    if (!why) {
        float Rq[9];
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) { Rq[3 * r + k] = R[3 * r] * q[k] + R[3 * r + 1] * q[3 + k] + R[3 * r + 2] * q[6 + k]; }
        }
        if (!gs::HandSpheresFromJoints(Rq, jm, off, rad)) { why = "the hand's joints are not finite"; }
    }
    if (why) {
        // One 5 mm sphere at the fingertip point (the old single point) - in the same ghost.
        off[0][0] = tipOff[0]; off[0][1] = tipOff[1]; off[0][2] = tipOff[2];
        rad[0] = kFallbackR;
        nSph = 1;
    }
    gs::HandBody bodies[2] = { { &g, gun34, grid }, { nullptr, nullptr, nullptr } };
    const gs::GunShape mg{ &row->mag, 1, gs::Channel{}, false, 0u };
    int nb = 1;
    if (magSolid) { bodies[nb++] = gs::HandBody{ &mg, mag34, nullptr }; }
    float rel[12], drawn[12];
    const bool mouth = row->hasMag && ReloadMagInHand(rel, drawn);   // a reload's mag step: the hand is free at the well's mouth
    // The ghost: gun frame -> world (this step's gun pose), the step, world -> gun frame.
    float ng[9];
    gs::detail::UnitRot(gun34, ng);
    gs::GhostState gw = ghostL;
    if (ghostL.have) {
        gs::detail::ToWorldPt(ng, gun34, ghostL.w, gw.w);
        gs::detail::ToWorldDir(ng, ghostL.nrm, gw.nrm);
        gs::detail::ToWorldPt(ng, gun34, ghostL.to, gw.to);   // the slide budget's controller move, in the gun's frame
    }
    float out[3];
    gs::GhostResult res{};
    const bool touching = gs::GhostStep(bodies, nb, off, rad, nSph, T, mouth, &gw, out, &res);
    gs::GhostState nl = gw;
    if (gw.have) {
        gs::detail::ToLocalPt(ng, gun34, gw.w, nl.w);
        gs::detail::ToLocalDir(ng, gw.nrm, nl.nrm);
        gs::detail::ToLocalPt(ng, gun34, gw.to, nl.to);
    }
    // ---- 3. UNDER g_lock again: publish the ghost, the edge, the counters, the lines. ----
    Excl ex;
    if (why != g_saidShapeWhy) {
        g_saidShapeWhy = why;
        // [PROBE: GUNSHAPE] gate=hand_collision  site=FLAT  log-only=yes
        LOG("gunshape: [GUNSHAPE] HAND SHAPE %s%s (grid %s).", why ? "NOT USED - one 5 mm sphere at the fingertip point instead: " : "IN USE - ",
            why ? why : "the left hand's game-mesh spheres on its live finger joints, in the ghost",
            grid && grid->ok ? "built" : "NONE - every box is tested");
    }
    if (seq != g_drawSeq) { return false; }   // the gun changed, or the ghost was reset, while this step ran: drop it
    if (!why) { ++g_handSolves; }
    ++g_steps;
    if (res.depen) { ++g_nDepen; }
    if (res.released) { ++g_nReleased; }
    if (res.waiting) { ++g_nWaiting; }
    if (res.dropped) {
        ++g_nDropped;
        if (++g_nDropLine <= 50) {
            // [PROBE: GUNSHAPE] gate=hand_collision  site=HEADSET  log-only=yes
            LOG("gunshape: [GUNSHAPE] GHOST DROPPED (row %s): the hand's own turn / curl put %s more than %.0f mm into %s - the hand "
                "passes free until it is clear (no shove); dropped %lld of %lld steps.", row->mesh,
                res.sphere >= 0 && !why ? gs::kHandSphereDefs[res.sphere].part : "the hand", 1000.0f * gs::kGhostDepenMaxM,
                res.body == 1 ? "the seated mag" : "the gun", g_nDropped, g_steps);
        }
    }
    g_ghostL = nl;
    g_ghostTouch = touching;
    const bool rise = g_edge.Update(touching, now);
    if (!touching) { return false; }
    T[0] = out[0]; T[1] = out[1]; T[2] = out[2];
    ++g_stops;
    if (rise) {
        const bool pulsed = HandCollisionBuzzAmp > 0.0f && PulseController(kLeftHand, HandCollisionBuzzAmp, static_cast<int>(HandCollisionBuzzMs));
        if (++g_nTouch <= kMaxEventLines) {
            // [PROBE: GUNSHAPE] gate=hand_collision  site=HEADSET  log-only=no
            LOG("gunshape: [GUNSHAPE] TOUCH left on %s (row %s): %s first, the hand held back %.1f mm%s, pulse %s; ghost steps %lld (with "
                "the hand's shape %lld), touching %lld, walked back %lld, dropped %lld, released %lld, waited inside %lld; left solves "
                "%lld, of them a step was already running %lld (put at the ghost %lld).",
                res.body == 1 ? "the seated mag" : "the gun", row->mesh,
                res.sphere >= 0 && !why ? gs::kHandSphereDefs[res.sphere].part : "the fingertip point", 1000.0f * res.heldBackM,
                mouth ? " [mag step: free at the well's mouth]" : "", pulsed ? "yes" : "NO", g_steps, g_handSolves, g_stops, g_nDepen,
                g_nDropped, g_nReleased, g_nWaiting, g_solves, g_busySkips, g_readerApplied);
        } else if (g_nTouch == kMaxEventLines + 1) {
            // [PROBE: GUNSHAPE] gate=hand_collision  site=HEADSET  log-only=yes
            LOG("gunshape: [GUNSHAPE] TOUCH lines capped at %ld (INV-9).", kMaxEventLines);
        }
    }
    return true;
}

bool GunShapeMagClearM(const char* mesh, float* clearM)
{
    const gs::GunShapeRow* r = gs::FindGunShapeRow(mesh);
    return r && gs::SeatedMagClearM(*r, clearM);
}

bool GunShapeMagStop(int armIdx, const float hand9[9], const float holderPos[3], float T[3])
{
    if (!HandCollision || g_resolved != 1 || armIdx != 1) { return false; }
    if (GunCollisionOff()) { return false; }
    Excl ex;   // RUN SG3: any thread (the pad-poll solve is the majority); the held-mag state below is shared
    float rel[12], drawn[12];
    const bool inHand = g_posed && g_row && g_row->hasMag && ReloadMagInHand(rel, drawn);
    if (!inHand) {
        g_magEdge.Update(false, NowS());
        g_haveLastH = false;
        g_escaping = false;
        return false;
    }
    // H at THIS solve's target, the reload's convention (reload.cpp Hand34: the basis columns normalised, the handholder position).
    float H[12];
    for (int c = 0; c < 3; ++c) {
        const float l = std::sqrt(hand9[c] * hand9[c] + hand9[3 + c] * hand9[3 + c] + hand9[6 + c] * hand9[6 + c]);
        if (!(l > 1e-6f) || !std::isfinite(l)) { return false; }
        for (int r = 0; r < 3; ++r) { H[4 * r + c] = hand9[3 * r + c] / l; }
    }
    for (int r = 0; r < 3; ++r) { H[4 * r + 3] = holderPos[r]; }
    // Review finding 3: the reload drew the mag from the ACHIEVED (drawn) hand; this H is the COMMANDED one. With last update's pair
    // (our commanded H, the reload's drawn mag), the mag is re-expressed in the commanded frame - the drawn-vs-commanded offset
    // (forearm roll, clamps, CorrectHandLocal) is carried; the first update of a hold falls back to the reload's own rel.
    float relCmd[12];
    const bool haveCmd = g_haveLastH && gs::RelFromLast(g_lastH, drawn, relCmd);
    if (!haveCmd) {
        // RUN SG2: the hold's FIRST update has no pair yet - the reload's rel is in the DRAWN hand frame (77 deg from this commanded H
        // in SG2) and pushed the hand up to 88 mm at every grab. No stop this one update: record H and wait for the pair.
        for (int k = 0; k < 12; ++k) { g_lastH[k] = H[k]; }
        g_haveLastH = true;
        g_magEdge.Update(false, NowS());
        return false;
    }
    float magW[12];
    gs::detail::Mul34(H, relCmd, magW);   // bone_magazine's world at this solve's target, in the commanded frame
    gs::GunShape g = g_row->shape;
    g.hiddenParts = g_hiddenParts;
    float shift[3];
    ++g_magSolves;
    const bool hit = gs::StopBoxOnHull(g, g_gun34, g_row->mag, magW, ReloadAlignDeg, shift);
    if (!g_saidFirstMag) {
        g_saidFirstMag = true;   // first sample with both frames (the second solve of a hold) - unconditional from then
        // The commanded-vs-drawn difference the correction carries: the angle between the two rels' rotations and their offset.
        float tr = 0.0f;
        for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) { tr += rel[4 * r + c] * relCmd[4 * r + c]; } }
        float ca = (tr - 1.0f) * 0.5f;
        ca = ca > 1.0f ? 1.0f : (ca < -1.0f ? -1.0f : ca);
        const float dmm = 1000.0f * std::sqrt((rel[3] - relCmd[3]) * (rel[3] - relCmd[3]) + (rel[7] - relCmd[7]) * (rel[7] - relCmd[7]) +
                                              (rel[11] - relCmd[11]) * (rel[11] - relCmd[11]));
        // [PROBE: GUNSHAPE] gate=hand_collision  site=HEADSET  log-only=yes
        LOG("gunshape: [GUNSHAPE] first held-mag stop call (row %s): %s, shift (%+.4f %+.4f %+.4f), align limit %.1f deg; commanded vs "
            "drawn hand frame: %.1f deg, %.1f mm (carried into the mag's pose).", g_row->mesh, hit ? "TOUCHING the gun" : "clear of the gun",
            hit ? shift[0] : 0.0f, hit ? shift[1] : 0.0f, hit ? shift[2] : 0.0f, ReloadAlignDeg,
            std::acos(ca) * 57.2957795f, dmm);
    }
    const bool rise = g_magEdge.Update(hit, NowS());
    // [PROBE: GUNSHAPE] gate=hand_collision  site=HEADSET  log-only=yes - RUN SG2's ESCAPE probe: the mag's top inside the well while
    // its centre is outside the footprint, AFTER this solve's push (did the walls hold it?). One line per escape (rising edge), capped.
    {
        float shifted[12];
        for (int k = 0; k < 12; ++k) { shifted[k] = magW[k]; }
        if (hit) { shifted[3] += shift[0]; shifted[7] += shift[1]; shifted[11] += shift[2]; }
        float lateral = 0.0f;
        const bool esc = gs::MagEscapingWell(g, g_gun34, g_row->mag, shifted, &lateral);
        if (esc && !g_escaping && ++g_nEscape <= kMaxEventLines) {
            LOG("gunshape: [GUNSHAPE] ESCAPE the held mag is leaving the well SIDEWAYS (row %s): centre %.1f mm off the well's axis "
                "(half-width %.1f / half-depth %.1f mm), this solve %s %.1f mm; mag solves %lld, stops %lld.", g_row->mesh,
                1000.0f * lateral, 1000.0f * g_row->shape.well.halfW, 1000.0f * g_row->shape.well.halfD, hit ? "pushed" : "did NOT push",
                hit ? 1000.0f * std::sqrt(shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]) : 0.0f, g_magSolves, g_magStops);
        }
        g_escaping = esc;
    }
    for (int k = 0; k < 12; ++k) { g_lastH[k] = H[k]; }
    if (hit) { g_lastH[3] += shift[0]; g_lastH[7] += shift[1]; g_lastH[11] += shift[2]; }   // the target the arm now goes to
    g_haveLastH = true;
    if (!hit) { return false; }
    ++g_magStops;
    T[0] += shift[0]; T[1] += shift[1]; T[2] += shift[2];
    if (rise) {
        const bool pulsed = HandCollisionBuzzAmp > 0.0f && PulseController(kLeftHand, HandCollisionBuzzAmp, static_cast<int>(HandCollisionBuzzMs));
        if (++g_nMagTouch <= kMaxEventLines) {
            // [PROBE: GUNSHAPE] gate=hand_collision  site=HEADSET  log-only=no
            LOG("gunshape: [GUNSHAPE] TOUCH the held mag on the gun (row %s): pushed %.1f mm (%+.4f %+.4f %+.4f), pulse %s; mag solves "
                "%lld, stops %lld.", g_row->mesh, 1000.0f * std::sqrt(shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]),
                shift[0], shift[1], shift[2], pulsed ? "yes" : "NO", g_magSolves, g_magStops);
        }
    }
    return true;
}

}  // namespace dlvr
