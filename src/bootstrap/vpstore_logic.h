// FIX A's PER-BUFFER VIEW-PROJECTION STORE - the table, with no D3D in it (docs/superpowers/specs/2026-10-20-vpstore-lru-design.md).
// Tested offline by src/src/tests/vpstore_logic_test.cpp; used by dlssflat.cpp's vpstore namespace.
//
// Every VS slot-2 bind goes through Bind. The 2026-10-19 table (32, never pruned) filled at the main menu ~10 s after launch and
// then REFUSED every new buffer for the session (~1,500-2,000 binds / s in play, BETA-FINDINGS-2026-10-19 addendum 2026-10-20). Now
// a full table evicts the entry bound longest ago: live buffers are bound every frame and stay, dead and menu-only ones age out.
// Buffers are compared by pointer only - never dereferenced. Render thread only (the hooks and the replay detour).
#pragma once

namespace dlvr {
namespace vpstorelogic {

constexpr int kMax = 128;

struct Ent {
    const void* buf = nullptr;
    float m[16] = {};
    bool have = false;                  // m holds a whole-buffer store of THIS buffer
    unsigned long long lastBound = 0;   // the table clock at its last bind
    unsigned long long liveWin = 0;     // the window it was last counted live in
    unsigned long long storedEval = 0;  // the table's eval count when m was stored (2026-11-22, StoredSinceEval)
};

struct Table {
    Ent e[kMax];
    int n = 0;                          // slots in use
    unsigned long long clock = 0;       // bumped on every bind
    unsigned long long window = 1;      // the 5 s window number
    long evicted = 0;                   // this window
    int live = 0;                       // distinct buffers bound this window
    unsigned long long evals = 0;       // evals so far (NoteEval, one per eye eval) - 2026-11-22
};

inline Ent* Find(Table& t, const void* buf)
{
    for (int i = 0; i < t.n; ++i) {
        if (t.e[i].buf == buf) { return &t.e[i]; }
    }
    return nullptr;
}

// A slot-2 bind of `buf`: its entry (added - evicting the least recently bound when full - if new), marked bound now.
inline Ent* Bind(Table& t, const void* buf)
{
    Ent* e = Find(t, buf);
    if (!e) {
        if (t.n < kMax) {
            e = &t.e[t.n++];
        } else {
            e = &t.e[0];
            for (int i = 1; i < kMax; ++i) {
                if (t.e[i].lastBound < e->lastBound) { e = &t.e[i]; }
            }
            ++t.evicted;
        }
        *e = Ent{};
        e->buf = buf;
    }
    e->lastBound = ++t.clock;
    if (e->liveWin != t.window) {
        e->liveWin = t.window;
        ++t.live;
    }
    return e;
}

// FRESH SERVES ONLY (2026-11-22, trace docs/traces/20261122_1505_vpstore_fresh_serve.md). Every rd3d11 command list opens with a
// bind of the context's slot-2 buffer BEFORE that list's upload (ida/rord1-rord4_rd3d11_out.txt), so at the bind the store still
// holds the buffer's PREVIOUS upload - under AFW the other eye's, one frame old; with the doubled AFW table it carries this frame's
// phase and lands in this frame's vote bucket (the AFW gate's "STALE camera" frames). An entry may be served only when its matrix
// was stored since the most recent eval: this eye's own uploads always are (they are replayed after the other eye's eval).
inline void NoteEval(Table& t) { ++t.evals; }
inline void StoreMatrix(Table& t, Ent& e, const float* m)
{
    for (int i = 0; i < 16; ++i) { e.m[i] = m[i]; }
    e.have = true;
    e.storedEval = t.evals;
}
inline bool StoredSinceEval(const Table& t, const Ent& e) { return e.have && e.storedEval == t.evals; }

// 2026-10-20 (docs/superpowers/specs/2026-10-20-vpstore-jittered-only-design.md): does a block's jitter offset (render px, from
// realjit::Offsets) equal a phase of the active jitter table (n entries of x, y)? Worker-context blocks carry NO jitter (INV-560)
// and no Halton(2,3) phase is near (0, 0), so they match none. No table / no entries = cannot judge = false.
// The FIRST table index whose phase the offset matches (a doubled AFW table's two copies share it), or -1.
inline int PhaseIndex(double px, double py, const float* table, int n, double tol)
{
    if (!table) { return -1; }
    for (int i = 0; i < n; ++i) {   // n <= 0: no entries, no match
        const double dx = px - table[i * 2], dy = py - table[i * 2 + 1];
        if (dx * dx + dy * dy <= tol * tol) { return i; }
    }
    return -1;
}

inline bool CarriesTablePhase(double px, double py, const float* table, int n, double tol)
{
    return PhaseIndex(px, py, table, n, tol) >= 0;
}

// The per-eye vote (docs/superpowers/specs/2026-10-20-vpstore-majority-phase-design.md): each served block adds its phase; the
// snapshot is the LATEST matrix of the phase with the most votes (a tie keeps the earlier leader). A minority of other-frame
// jittered blocks can no longer decide it. Reset after each eye's eval.
constexpr int kVoteMax = 8;

struct Vote {
    int   idx[kVoteMax] = {};
    int   count[kVoteMax] = {};
    float m[kVoteMax][16] = {};
    int   n = 0;
    int   lead = -1;          // slot of the current winner
    long  overflow = 0;       // phases ignored because the vote was full
};

inline void VoteReset(Vote& v)
{
    v.n = 0;
    v.lead = -1;
}

// Adds a block of phase `idx`; returns the winner's matrix (never null once anything was added).
inline const float* VoteAdd(Vote& v, int idx, const float* mat)
{
    int s = -1;
    for (int i = 0; i < v.n; ++i) {
        if (v.idx[i] == idx) { s = i; break; }
    }
    if (s < 0) {
        if (v.n >= kVoteMax) {
            ++v.overflow;
            return v.lead >= 0 ? v.m[v.lead] : nullptr;
        }
        s = v.n++;
        v.idx[s] = idx;
        v.count[s] = 0;
    }
    ++v.count[s];
    for (int i = 0; i < 16; ++i) { v.m[s][i] = mat[i]; }
    if (v.lead < 0 || v.count[s] > v.count[v.lead]) { v.lead = s; }
    return v.m[v.lead];
}

// The cross-eye guard (docs/superpowers/specs/2026-10-21-snapshot-cross-eye-guard-design.md, INV-271 addendum 4). The snapshot is
// ONE matrix for both eyes; an eye whose own pass wrote nothing would read the OTHER eye's. Every write bumps gen; an eval takes
// the matrix fresh (gen moved -> it becomes the owner) or, with no write since the previous eval, only if it already owns it.
// A consumer called once per frame (the AFW gate push, the AFW depth copy) asks "written since MY previous call?" instead -
// it carries no eye label with upscaling on, and its place relative to the eval does not matter then.
enum WriteSrc { kSrcVpstore = 0, kSrcVtable = 1 };

struct PassTally {
    long serves = 0;    // vpstore writes
    long vtable = 0;    // vtable-capture writes
    long noPhase = 0;   // vpstore serves refused: no jitter phase
    long noData = 0;    // vpstore serves refused: the bound buffer had no stored matrix
};

struct SnapGuard {
    unsigned long long gen = 0;       // bumped by every write of the snapshot
    unsigned long long evalGen = 0;   // gen at the previous eval
    int owner = -1;                   // the eye whose eval last took the matrix fresh
    PassTally pass;                   // since the previous eval
    PassTally lastPass;               // the pass before the most recent eval
    // Amendment 1, the run cap: run[e] = consecutive evals of eye e that found the OTHER eye's matrix (no write since the
    // previous eval). Only the first of a run is refused; from the second on the eye TAKES it (the behaviour before the guard),
    // because a refused eye repeats its last camera motion and a long run would freeze it. A fresh write for the eye ends it.
    int run[2] = {};
    int longest[2] = {};              // the longest run this window (the caller resets it with EndGuardWindow)
    bool crossTaken = false;          // the most recent eval took the other eye's matrix (run >= 2)
};

struct ConsumerSeen {
    unsigned long long gen = ~0ull;   // gen at this consumer's previous call (never = gen before the first)
};

inline void OnWrite(SnapGuard& g, WriteSrc s)
{
    ++g.gen;
    if (s == kSrcVpstore) { ++g.pass.serves; } else { ++g.pass.vtable; }
}

inline void OnServeRefused(SnapGuard& g, bool noData)
{
    if (noData) { ++g.pass.noData; } else { ++g.pass.noPhase; }
}

// true = this eye may take the snapshot (eye 0 or 1). The pass tally moves to lastPass either way.
inline bool OnEval(SnapGuard& g, int eye)
{
    bool ok;
    g.crossTaken = false;
    if (g.gen != g.evalGen) {
        g.owner = eye;
        g.run[eye] = 0;
        ok = true;
    } else if (g.owner == eye) {
        ok = true;
    } else {
        ++g.run[eye];
        if (g.run[eye] > g.longest[eye]) { g.longest[eye] = g.run[eye]; }
        ok = (g.run[eye] > 1);   // the first of a run is refused; later ones take the other eye's matrix
        g.crossTaken = ok;
    }
    g.evalGen = g.gen;
    g.lastPass = g.pass;
    g.pass = PassTally{};
    return ok;
}

// The 5 s window ended: the longest-run maxima start again (the runs themselves carry on).
inline void EndGuardWindow(SnapGuard& g)
{
    g.longest[0] = 0;
    g.longest[1] = 0;
}

inline bool ConsumerFresh(const SnapGuard& g, ConsumerSeen& c)
{
    const bool fresh = (g.gen != c.gen);
    c.gen = g.gen;
    return fresh;
}

// The 5 s line was printed: the next window starts.
inline void EndWindow(Table& t)
{
    ++t.window;
    t.evicted = 0;
    t.live = 0;
}

}  // namespace vpstorelogic
}  // namespace dlvr
