// WATCHEXEC ORPHAN TRAP - the decision, with no Windows in it (docs/superpowers/specs/2026-10-20-watchexec-orphan-trap-design.md,
// U-26). Tested offline by src/src/tests/watchexec_orphan_logic_test.cpp; used by watchwrite.cpp's Handler.
//
// An exec breakpoint is fault-class: its trap arrives with RIP ON the armed instruction. U-26's three crashes are exactly that
// shape with Dr6 = 0, at an address watchexec had armed and was handing over - so no handler claimed them. This copy remembers
// the exec addresses IT armed (and when each was released) and claims such a trap instead of letting it kill the process.
// The ring is written by the arming caller and read by the handler on any thread: plain aligned 64-bit fields, no lock.
#pragma once

#include <cstdint>

namespace dlvr {
namespace watchorphan {

constexpr int kRing = 8;
constexpr unsigned long long kWindowMs = 5000;   // a released address stays claimable this long (spec §3)

struct Entry {
    uintptr_t addr = 0;
    unsigned long long releasedMs = 0;   // 0 = still armed
};

struct Ring {
    Entry e[kRing];
    int next = 0;
};

// An exec arm of `addr` by this copy. Final review I3: the slot is the first NOT-armed one (empty or released) from `next`
// on, so a watch still armed is never evicted by another client cycling; only when all kRing are armed is `next` overwritten.
// The caller serialises arms / releases (watchwrite.cpp's writer lock); the handler only reads.
inline void NoteArmed(Ring& r, uintptr_t addr)
{
    int slot = r.next;
    for (int k = 0; k < kRing; ++k) {
        const int i = (r.next + k) % kRing;
        if (r.e[i].addr == 0 || r.e[i].releasedMs != 0) { slot = i; break; }
    }
    Entry& s = r.e[slot];
    s.releasedMs = 0;
    s.addr = addr;
    r.next = (slot + 1) % kRing;
}

// `addr` stopped or replaced at `nowMs`: every entry for it still armed gets the time (0 is stored as 1 - 0 means armed).
inline void NoteReleased(Ring& r, uintptr_t addr, unsigned long long nowMs)
{
    const unsigned long long t = nowMs ? nowMs : 1;
    for (Entry& s : r.e) {
        if (s.addr == addr && s.releasedMs == 0) { s.releasedMs = t; }
    }
}

// True = a fault-class exec trap with no breakpoint bit in Dr6, exactly on an address this copy armed now or released within
// kWindowMs. Anything with a B0..B3 bit keeps the handler's existing path; BS (bit 14, a genuine trap-flag single step) is
// passed on (final review I2 - spec §3).
inline bool IsOrphanExecTrap(const Ring& r, unsigned long long dr6, uintptr_t exceptionAddress, uintptr_t rip, unsigned long long nowMs)
{
    if ((dr6 & 0x400F) != 0 || rip == 0 || exceptionAddress != rip) { return false; }
    for (const Entry& s : r.e) {
        if (s.addr != rip) { continue; }
        if (s.releasedMs == 0 || nowMs - s.releasedMs <= kWindowMs) { return true; }
    }
    return false;
}

}  // namespace watchorphan
}  // namespace dlvr
