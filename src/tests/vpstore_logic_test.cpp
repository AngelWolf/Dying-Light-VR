// Standalone unit test for src/src/bootstrap/vpstore_logic.h - NOT part of the DLL build.
// docs/superpowers/specs/2026-10-20-vpstore-lru-design.md §3. Exit code = number of failures.
#include "vpstore_logic.h"

#include <cstdint>
#include <cstdio>

using namespace dlvr::vpstorelogic;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static const void* P(uintptr_t i) { return reinterpret_cast<const void*>(0x10000 + i * 0x100); }

static Table g_t;   // ~10 KB - static, not on the stack
static void Reset() { g_t = Table{}; }

static void find_misses_unknown()
{
    Reset();
    CHECK(Find(g_t, P(1)) == nullptr);
    Bind(g_t, P(1));
    CHECK(Find(g_t, P(1)) != nullptr);
    CHECK(Find(g_t, P(2)) == nullptr);
}

// Review Focus 1: a buffer bound every round survives any amount of one-off churn.
static void live_buffer_survives_churn()
{
    Reset();
    Ent* live = Bind(g_t, P(0));
    live->have = true;
    live->m[0] = 42.0f;
    for (uintptr_t i = 1; i <= 200; ++i) {
        Bind(g_t, P(1000 + i));   // one-off
        Bind(g_t, P(0));          // the live one, every round
    }
    Ent* e = Find(g_t, P(0));
    CHECK(e != nullptr);
    CHECK(e && e->have && e->m[0] == 42.0f);
}

// Review Focus 2: binding a known buffer again evicts nothing and keeps its matrix.
static void known_add_keeps_entry()
{
    Reset();
    for (uintptr_t i = 0; i < kMax; ++i) { Bind(g_t, P(i)); }
    Ent* e = Find(g_t, P(5));
    e->have = true;
    e->m[3] = 7.0f;
    const long before = g_t.evicted;
    Bind(g_t, P(5));
    CHECK(g_t.evicted == before);
    CHECK(g_t.n == kMax);
    e = Find(g_t, P(5));
    CHECK(e && e->have && e->m[3] == 7.0f);
}

// Review Focus 3: a slot handed to a new buffer serves nothing until that buffer's own store.
static void evicted_slot_has_no_matrix()
{
    Reset();
    for (uintptr_t i = 0; i < kMax; ++i) {
        Ent* e = Bind(g_t, P(i));
        e->have = true;
    }
    Ent* fresh = Bind(g_t, P(9999));
    CHECK(fresh != nullptr);
    CHECK(fresh && !fresh->have);
    CHECK(g_t.evicted == 1);
}

// The entry bound longest ago is the one evicted.
static void evicts_least_recent()
{
    Reset();
    for (uintptr_t i = 0; i < kMax; ++i) { Bind(g_t, P(i)); }
    for (uintptr_t i = 1; i < kMax; ++i) { Bind(g_t, P(i)); }   // everything but P(0) bound again
    Bind(g_t, P(9999));
    CHECK(Find(g_t, P(0)) == nullptr);
    for (uintptr_t i = 1; i < kMax; ++i) { CHECK(Find(g_t, P(i)) != nullptr); }
    CHECK(Find(g_t, P(9999)) != nullptr);
}

// Review Focus 4: live = distinct buffers bound in the window, however often; the window end resets it (and evicted).
static void live_counts_distinct()
{
    Reset();
    for (int k = 0; k < 5; ++k) {
        Bind(g_t, P(1));
        Bind(g_t, P(2));
        Bind(g_t, P(3));
    }
    CHECK(g_t.live == 3);
    EndWindow(g_t);
    CHECK(g_t.live == 0);
    CHECK(g_t.evicted == 0);
    Bind(g_t, P(2));
    CHECK(g_t.live == 1);
}

// ===== CarriesTablePhase (2026-10-20, docs/superpowers/specs/2026-10-20-vpstore-jittered-only-design.md) =====
// The real table: realjit::Phase (dlssflat.cpp) = Halton(2,3) phase i, centred, x scale; i & 7 repeats as the engine fills it.
static double Radical(int b, int i)
{
    double f = 1.0, r = 0.0;
    for (; i > 0; i /= b) { f /= b; r += f * (i % b); }
    return r;
}
static void BuildTable(float* t, int n, double scale)
{
    for (int i = 0; i < n; ++i) {
        t[i * 2] = static_cast<float>((Radical(2, i + 1) - 0.5) * scale);
        t[i * 2 + 1] = static_cast<float>((Radical(3, i + 1) - 0.5) * scale);
    }
}

// Every phase of the active table is recognised (8 / 16 / 64 / 128 entries, at the Halton scales the marker allows).
static void every_phase_matches()
{
    static float t[128 * 2];
    const int ns[4] = { 8, 16, 64, 128 };
    const double scales[3] = { 0.1, 1.0, 4.0 };
    for (double s : scales) {
        for (int n : ns) {
            BuildTable(t, n, s);
            for (int i = 0; i < n; ++i) { CHECK(CarriesTablePhase(t[i * 2], t[i * 2 + 1], t, n, 0.02 * s)); }
        }
    }
}

// An unjittered block (offset 0, plus float noise) matches no phase at any scale or table size.
static void unjittered_matches_none()
{
    static float t[128 * 2];
    const int ns[4] = { 8, 16, 64, 128 };
    const double scales[3] = { 0.1, 1.0, 4.0 };
    for (double s : scales) {
        for (int n : ns) {
            BuildTable(t, n, s);
            CHECK(!CarriesTablePhase(0.0, 0.0, t, n, 0.02 * s));
            CHECK(!CarriesTablePhase(1e-4, -1e-4, t, n, 0.02 * s));
        }
    }
}

// Tolerance edges: inside matches, outside does not.
static void tolerance_edges()
{
    static float t[8 * 2];
    BuildTable(t, 8, 1.0);
    CHECK(CarriesTablePhase(t[0] + 0.019, t[1], t, 8, 0.02));
    CHECK(!CarriesTablePhase(t[0] + 0.03, t[1] + 0.03, t, 8, 0.02));
    CHECK(!CarriesTablePhase(t[0], t[1], nullptr, 8, 0.02));   // no table -> cannot judge -> no
    CHECK(!CarriesTablePhase(t[0], t[1], t, 0, 0.02));
}

// ===== The per-eye majority vote (2026-10-20, docs/superpowers/specs/2026-10-20-vpstore-majority-phase-design.md) =====
static void Mat(float* m, float tag) { for (int i = 0; i < 16; ++i) { m[i] = tag; } }

// A doubled AFW table (t[2j] == t[2j+1]) groups both copies under the FIRST index.
static void phase_index_first_match_for_duplicates()
{
    static float t[16 * 2];
    for (int j = 0; j < 8; ++j) {
        const float x = static_cast<float>((Radical(2, j + 1) - 0.5));
        const float y = static_cast<float>((Radical(3, j + 1) - 0.5));
        t[(2 * j) * 2] = x; t[(2 * j) * 2 + 1] = y;
        t[(2 * j + 1) * 2] = x; t[(2 * j + 1) * 2 + 1] = y;
    }
    for (int j = 0; j < 8; ++j) { CHECK(PhaseIndex(t[(2 * j) * 2], t[(2 * j) * 2 + 1], t, 16, 0.02) == 2 * j); }
    CHECK(PhaseIndex(0.0, 0.0, t, 16, 0.02) == -1);
    CHECK(PhaseIndex(0.0, 0.0, nullptr, 16, 0.02) == -1);
}

// Four blocks of phase 3 then one of phase 5: the snapshot stays phase 3's, and it is phase 3's LATEST matrix.
static void majority_beats_latest_minority()
{
    Vote v;
    float a[16], b[16], c[16];
    Mat(a, 1.0f); Mat(b, 2.0f); Mat(c, 3.0f);
    VoteAdd(v, 3, a);
    VoteAdd(v, 3, a);
    VoteAdd(v, 3, b);
    const float* w = VoteAdd(v, 5, c);
    CHECK(w && w[0] == 2.0f);
    w = VoteAdd(v, 3, a);
    CHECK(w && w[0] == 1.0f);   // the majority's latest
}

// A tie keeps the earlier leader.
static void tie_keeps_leader()
{
    Vote v;
    float a[16], c[16];
    Mat(a, 1.0f); Mat(c, 3.0f);
    VoteAdd(v, 3, a);
    const float* w = VoteAdd(v, 5, c);
    CHECK(w && w[0] == 1.0f);
}

static void reset_starts_a_new_pass()
{
    Vote v;
    float a[16], b[16], c[16];
    Mat(a, 1.0f); Mat(b, 2.0f); Mat(c, 3.0f);
    VoteAdd(v, 3, a);   // slot 0
    VoteAdd(v, 4, b);   // slot 1 ...
    VoteAdd(v, 4, b);   // ... becomes the leader
    VoteReset(v);
    const float* w = VoteAdd(v, 5, c);   // the new pass's first block - the old leader (slot 1) must not answer
    CHECK(w && w[0] == 3.0f);
    CHECK(v.n == 1);
}

// More than kVoteMax distinct phases: the extra is ignored (counted) and the winner stands.
static void full_vote_ignores_new_phase()
{
    Vote v;
    float a[16], c[16];
    Mat(a, 1.0f); Mat(c, 9.0f);
    VoteAdd(v, 0, a);
    VoteAdd(v, 0, a);
    for (int i = 1; i < kVoteMax; ++i) { VoteAdd(v, i, a); }
    const float* w = VoteAdd(v, 99, c);
    CHECK(w && w[0] == 1.0f);
    CHECK(v.overflow == 1);
}

// ===== The cross-eye guard (2026-10-21, docs/superpowers/specs/2026-10-21-snapshot-cross-eye-guard-design.md) =====
// A pass = the writes / refusals before an eval. Eyes alternate 0, 1 unless a test says otherwise.

static void fresh_write_allowed()
{
    SnapGuard g;
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 0));
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 1));
}

static void cross_eye_stale_refused()
{
    SnapGuard g;
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 0));
    CHECK(!OnEval(g, 1));   // eye 1's pass wrote nothing - the snapshot is eye 0's
}

// Review Focus 1: AFW 0,1,0,1,... with ONE empty pass refuses exactly that one eval.
static void afw_one_empty_pass_refused_once()
{
    SnapGuard g;
    int refused = 0;
    for (int f = 0; f < 20; ++f) {
        const int eye = f & 1;
        if (f != 7) { OnWrite(g, kSrcVpstore); }
        if (!OnEval(g, eye)) { ++refused; CHECK(f == 7); }
    }
    CHECK(refused == 1);
}

// Review Focus 2: after eye 1 is refused, eye 0 with no new write still owns the snapshot (its own matrix).
static void owner_keeps_own_after_refusal()
{
    SnapGuard g;
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 0));
    CHECK(!OnEval(g, 1));
    CHECK(OnEval(g, 0));
    CHECK(!g.crossTaken);
    OnWrite(g, kSrcVtable);
    CHECK(OnEval(g, 1));   // a fresh write - eye 1 takes it and becomes the owner
    CHECK(!OnEval(g, 0));
}

// Amendment 1 (run cap): eye 1's pass writes nothing for 10 frames - only the FIRST of its evals is refused, the rest take
// eye 0's matrix (the behaviour before the guard), flagged crossTaken.
static void sustained_burst_refuses_first_only()
{
    SnapGuard g;
    int refused = 0, taken = 0;
    for (int f = 0; f < 20; ++f) {
        const int eye = f & 1;
        if (eye == 0) { OnWrite(g, kSrcVpstore); }
        const bool ok = OnEval(g, eye);
        if (!ok) { ++refused; CHECK(eye == 1); }
        if (g.crossTaken) { ++taken; CHECK(eye == 1 && ok); }
    }
    CHECK(refused == 1);
    CHECK(taken == 9);
    CHECK(g.longest[1] == 10);
    CHECK(g.longest[0] == 0);
}

// Amendment 1: a fresh write for the eye ends its run - the next miss is refused again.
static void run_resets_on_own_write()
{
    SnapGuard g;
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 0));
    CHECK(!OnEval(g, 1));          // run 1
    CHECK(OnEval(g, 0));
    CHECK(OnEval(g, 1));           // run 2 - taken
    CHECK(g.crossTaken);
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 1));           // its own write - run back to 0
    CHECK(!g.crossTaken);
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 0));
    CHECK(!OnEval(g, 1));          // a new miss is refused again
    CHECK(g.longest[1] == 2);
}

// Amendment 1: longest is the window maximum; the caller resets it at the window end.
static void longest_is_window_max()
{
    SnapGuard g;
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 0));
    for (int i = 0; i < 3; ++i) { OnEval(g, 1); OnEval(g, 0); }   // eye 1 run 3
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 1));
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 0));
    OnEval(g, 1);                                                  // eye 1 run 1
    CHECK(g.longest[1] == 3);
    EndGuardWindow(g);
    CHECK(g.longest[0] == 0 && g.longest[1] == 0);
    CHECK(g.run[1] == 1);                                          // the run itself survives the window end
}

// Review Focus 3: the same eye twice with no write between (flat, or AFW giving one eye two frames) is allowed.
static void same_eye_no_write_allowed()
{
    SnapGuard g;
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 0));
    CHECK(OnEval(g, 0));
    CHECK(OnEval(g, 0));
}

// Review Focus 4: a consumer refuses only when nothing was written since ITS previous call - eval order does not matter.
static void consumer_fresh_per_call()
{
    SnapGuard g;
    ConsumerSeen c;
    OnWrite(g, kSrcVpstore);
    CHECK(ConsumerFresh(g, c));
    CHECK(OnEval(g, 0));          // an eval between consumer calls changes nothing for the consumer
    CHECK(!ConsumerFresh(g, c));  // no write since its previous call
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 1));          // eval BEFORE the consumer this time
    CHECK(ConsumerFresh(g, c));
    CHECK(!ConsumerFresh(g, c));
}

// Review Focus 5: the first eval and the first consumer call of the session, after a write, are fresh.
static void first_calls_fresh()
{
    SnapGuard g;
    ConsumerSeen c;
    OnWrite(g, kSrcVtable);
    CHECK(ConsumerFresh(g, c));
    SnapGuard g2;
    OnWrite(g2, kSrcVtable);
    CHECK(OnEval(g2, 1));
}

// Review Focus 6: each eval moves the preceding pass's tallies to lastPass and starts a new pass.
static void pass_tallies_reset_at_eval()
{
    SnapGuard g;
    OnWrite(g, kSrcVpstore);
    OnWrite(g, kSrcVpstore);
    OnWrite(g, kSrcVtable);
    OnServeRefused(g, false);
    CHECK(OnEval(g, 0));
    CHECK(g.lastPass.serves == 2 && g.lastPass.vtable == 1 && g.lastPass.noPhase == 1 && g.lastPass.noData == 0);
    CHECK(g.pass.serves == 0 && g.pass.vtable == 0 && g.pass.noPhase == 0 && g.pass.noData == 0);
}

static void refusal_reports_the_pass()
{
    SnapGuard g;
    OnWrite(g, kSrcVpstore);
    CHECK(OnEval(g, 0));
    OnServeRefused(g, false);
    OnServeRefused(g, false);
    OnServeRefused(g, true);
    CHECK(!OnEval(g, 1));
    CHECK(g.lastPass.serves == 0 && g.lastPass.vtable == 0 && g.lastPass.noPhase == 2 && g.lastPass.noData == 1);
}

// ===== Fresh serves only (2026-11-22, docs/traces/20261122_1505_vpstore_fresh_serve.md) =====
// Before any eval, a store is fresh (the first pass of the session).
static void store_before_first_eval_is_fresh()
{
    Reset();
    float a[16];
    Mat(a, 1.0f);
    Ent* e = Bind(g_t, P(1));
    StoreMatrix(g_t, *e, a);
    CHECK(StoredSinceEval(g_t, *e));
    CHECK(e->have && e->m[0] == 1.0f);
}

// THE CASE: a buffer stored in the previous eye's pass, bound again (a list's opening bind) after that eye's eval - old.
static void store_before_eval_is_old_after_it()
{
    Reset();
    float a[16];
    Mat(a, 1.0f);
    Ent* e = Bind(g_t, P(1));
    StoreMatrix(g_t, *e, a);   // eye A's upload
    NoteEval(g_t);             // eye A's eval
    e = Bind(g_t, P(1));       // eye B's list opens: the bind, before its own upload
    CHECK(e && !StoredSinceEval(g_t, *e));
    CHECK(e && e->have);       // the bytes are still there - only no longer served
    StoreMatrix(g_t, *e, a);   // eye B's own upload
    CHECK(StoredSinceEval(g_t, *e));
}

// AFW frames 0..19 with one eval each: an upload in a frame is fresh until that frame's eval, old after it.
static void every_frame_own_upload_fresh()
{
    Reset();
    float a[16];
    Mat(a, 1.0f);
    int fresh = 0, old = 0;
    for (int f = 0; f < 20; ++f) {
        Ent* e = Bind(g_t, P(1));                     // the opening bind
        if (StoredSinceEval(g_t, *e)) { ++fresh; } else { ++old; }
        StoreMatrix(g_t, *e, a);                      // the upload
        CHECK(StoredSinceEval(g_t, *e));
        NoteEval(g_t);
    }
    CHECK(fresh == 0);    // every opening bind sees the previous frame's upload (frame 0: nothing stored yet, have false)
    CHECK(old == 20);
}

// A new / evicted entry carries no matrix and is never fresh.
static void new_entry_never_fresh()
{
    Reset();
    NoteEval(g_t);
    Ent* e = Bind(g_t, P(7));
    CHECK(e && !e->have && !StoredSinceEval(g_t, *e));
}

int main()
{
    find_misses_unknown();
    live_buffer_survives_churn();
    known_add_keeps_entry();
    evicted_slot_has_no_matrix();
    evicts_least_recent();
    live_counts_distinct();
    every_phase_matches();
    unjittered_matches_none();
    tolerance_edges();
    phase_index_first_match_for_duplicates();
    majority_beats_latest_minority();
    tie_keeps_leader();
    reset_starts_a_new_pass();
    full_vote_ignores_new_phase();
    fresh_write_allowed();
    cross_eye_stale_refused();
    afw_one_empty_pass_refused_once();
    owner_keeps_own_after_refusal();
    same_eye_no_write_allowed();
    consumer_fresh_per_call();
    first_calls_fresh();
    pass_tallies_reset_at_eval();
    refusal_reports_the_pass();
    sustained_burst_refuses_first_only();
    run_resets_on_own_write();
    longest_is_window_max();
    store_before_first_eval_is_fresh();
    store_before_eval_is_old_after_it();
    every_frame_own_upload_fresh();
    new_entry_never_fresh();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
