// Standalone unit test for src/src/ovr2xr/watchexec_orphan_logic.h - NOT part of the DLL build.
// docs/superpowers/specs/2026-10-20-watchexec-orphan-trap-design.md §3. Exit code = number of failures.
#include "watchexec_orphan_logic.h"

#include <cstdio>

using namespace dlvr::watchorphan;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

constexpr uintptr_t kA = 0x7FF807769FF0ull;   // lodoverride's exec address in the 10-20 dump
constexpr uintptr_t kB = 0x7FF808252AC0ull;   // boneprobe's

static void claims_armed_address()
{
    Ring r;
    NoteArmed(r, kA);
    CHECK(IsOrphanExecTrap(r, 0, kA, kA, 123456));
}

static void claims_released_within_window()
{
    Ring r;
    NoteArmed(r, kA);
    NoteReleased(r, kA, 1000);
    CHECK(IsOrphanExecTrap(r, 0, kA, kA, 1000 + kWindowMs));
}

static void refuses_after_window()
{
    Ring r;
    NoteArmed(r, kA);
    NoteReleased(r, kA, 1000);
    CHECK(!IsOrphanExecTrap(r, 0, kA, kA, 1000 + kWindowMs + 1));
}

static void refuses_never_armed()
{
    Ring r;
    NoteArmed(r, kA);
    CHECK(!IsOrphanExecTrap(r, 0, kB, kB, 5));
    CHECK(!IsOrphanExecTrap(r, 0, 0, 0, 5));
}

// Review Focus 1: a B-bit means a real hit - the existing path keeps it.
static void bbit_keeps_existing_path()
{
    Ring r;
    NoteArmed(r, kA);
    CHECK(!IsOrphanExecTrap(r, 0x1, kA, kA, 5));
    CHECK(!IsOrphanExecTrap(r, 0x2, kA, kA, 5));
    CHECK(!IsOrphanExecTrap(r, 0x4, kA, kA, 5));
    CHECK(!IsOrphanExecTrap(r, 0x8, kA, kA, 5));
    // Final review I2: BS (bit 14) = a genuine trap-flag single step - the spec passes it on ("a genuine single step").
    CHECK(!IsOrphanExecTrap(r, 0x4000, kA, kA, 5));
    CHECK(IsOrphanExecTrap(r, 0xFFFF0FF0ull, kA, kA, 5));   // reserved bits set, B0..B3 and BS clear (INV-75's reported shape)
}

// Final review I3: a long-lived armed watch is never evicted by another client cycling through the ring.
static void long_lived_arm_survives_cycles()
{
    Ring r;
    NoteArmed(r, kA);                                    // client 0, held for the session
    for (int i = 0; i < kRing + 1; ++i) {                // client 1 cycles: arm, release
        const uintptr_t b = kB + static_cast<uintptr_t>(i) * 16;
        NoteArmed(r, b);
        NoteReleased(r, b, 1000 + static_cast<unsigned long long>(i));
    }
    CHECK(IsOrphanExecTrap(r, 0, kA, kA, 99999));
    const uintptr_t last = kB + static_cast<uintptr_t>(kRing) * 16;
    CHECK(IsOrphanExecTrap(r, 0, last, last, 1000 + kRing + 100));   // the newest release is still in the ring
}

static void refuses_exception_address_mismatch()
{
    Ring r;
    NoteArmed(r, kA);
    CHECK(!IsOrphanExecTrap(r, 0, kA + 3, kA, 5));
    CHECK(!IsOrphanExecTrap(r, 0, kA, kA + 3, 5));
}

// Review Focus 2: an address only the OTHER DLL's copy armed is not in this copy's ring.
static void foreign_address_refused()
{
    Ring mine, theirs;
    NoteArmed(theirs, kB);
    CHECK(!IsOrphanExecTrap(mine, 0, kB, kB, 5));
}

// Review Focus 3: armed again after a release = claimable whatever the older stamp says.
static void rearm_clears_release()
{
    Ring r;
    NoteArmed(r, kA);
    NoteReleased(r, kA, 1000);
    NoteArmed(r, kA);
    CHECK(IsOrphanExecTrap(r, 0, kA, kA, 99999));
}

// All slots armed: each further arm overwrites the next-oldest, not the same slot again.
static void full_ring_overwrites_in_order()
{
    Ring r;
    for (int i = 0; i < kRing + 2; ++i) { NoteArmed(r, kA + static_cast<uintptr_t>(i) * 16); }
    CHECK(!IsOrphanExecTrap(r, 0, kA, kA, 5));
    CHECK(!IsOrphanExecTrap(r, 0, kA + 16, kA + 16, 5));
    for (int i = 2; i < kRing + 2; ++i) {
        const uintptr_t a = kA + static_cast<uintptr_t>(i) * 16;
        CHECK(IsOrphanExecTrap(r, 0, a, a, 5));
    }
}

// A slot reused after the ring wraps starts ARMED, whatever release stamp the slot held before (the 5.6 s ping-pong reuses
// slots all session).
static void reused_slot_is_armed()
{
    Ring r;
    NoteArmed(r, kB);                               // slot 0
    NoteReleased(r, kB, 1000);
    for (int i = 1; i < kRing; ++i) { NoteArmed(r, kA + static_cast<uintptr_t>(i) * 16); }
    NoteArmed(r, kB);                               // slot 0 again
    CHECK(IsOrphanExecTrap(r, 0, kB, kB, 99999));
}

// Review Focus 4: the ninth arm overwrites the OLDEST entry.
static void ring_wraps_oldest()
{
    Ring r;
    for (int i = 0; i < kRing + 1; ++i) { NoteArmed(r, kA + static_cast<uintptr_t>(i) * 16); }
    CHECK(!IsOrphanExecTrap(r, 0, kA, kA, 5));
    for (int i = 1; i < kRing + 1; ++i) {
        const uintptr_t a = kA + static_cast<uintptr_t>(i) * 16;
        CHECK(IsOrphanExecTrap(r, 0, a, a, 5));
    }
}

// Review Focus 5: a release at clock 0 is a release, not "still armed".
static void zero_release_means_armed()
{
    Ring r;
    NoteArmed(r, kA);
    NoteReleased(r, kA, 0);
    CHECK(!IsOrphanExecTrap(r, 0, kA, kA, kWindowMs + 2));
}

// A release stamps only entries still armed - an older release keeps its own time.
static void release_keeps_older_stamp()
{
    Ring r;
    NoteArmed(r, kA);
    NoteReleased(r, kA, 1000);
    NoteReleased(r, kA, 50000);
    CHECK(!IsOrphanExecTrap(r, 0, kA, kA, 50000));
}

int main()
{
    claims_armed_address();
    claims_released_within_window();
    refuses_after_window();
    refuses_never_armed();
    bbit_keeps_existing_path();
    refuses_exception_address_mismatch();
    foreign_address_refused();
    rearm_clears_release();
    ring_wraps_oldest();
    reused_slot_is_armed();
    long_lived_arm_survives_cycles();
    full_ring_overwrites_in_order();
    zero_release_means_armed();
    release_keeps_older_stamp();
    std::printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
