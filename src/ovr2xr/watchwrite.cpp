#include "watchwrite.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../common/buildrva.h"   // the EGL build's rva for an allowlisted exec-watch address
#include "../common/log.h"
#include "watchexec_orphan_logic.h"   // U-26: the exec addresses this copy armed, for the orphan-trap claim

// The layer logs to dlvr_ovr2xr.log. The BOOTSTRAP also compiles this file (for
// the flat-mode jitter watch, which needs the exec breakpoint and cannot reach
// the layer - LibOVRRT64_1.dll is not loaded in flatscreen), and it must log to
// its own file: log.h truncates a file once per module on first write, so two
// modules writing the same file would each truncate it and silently eat the
// other's lines. Overridden from CMake for the xinput1_3 target.
#ifndef DLVR_WATCHWRITE_LOG
#define DLVR_WATCHWRITE_LOG "dlvr_ovr2xr.log"
#endif

#define LOG(...) ::dlvr::LogTo(DLVR_WATCHWRITE_LOG, __VA_ARGS__)

namespace dlvr {
namespace {

struct Hit {
    uintptr_t rip;
    uint64_t count;
};

CRITICAL_SECTION g_cs;
bool g_csReady = false;
PVOID g_veh = nullptr;
uintptr_t g_addr = 0;
bool g_active = false;
// x86 data breakpoints only offer "write" or "read-or-write", not a pure
// read - so finding who READS an address (with no interest in the already-
// known writer) means arming read-or-write and filtering the known writer
// out of the results by eye.
bool g_includeReads = false;
volatile LONG g_total = 0;
volatile LONG g_expired = 0;
constexpr LONG kMaxHits = 400;
std::vector<Hit> g_hits;
char g_status[256] = "off";

// A second, independent breakpoint on DR1, for a value that only ever exists
// in a register at one exact point in the code and is never written to
// memory anywhere - a data breakpoint has nothing to watch in that case. An
// execution breakpoint (RW=00) traps BEFORE the instruction at the address
// runs, so whichever register already holds the value we want (set by an
// earlier instruction, read here before it's clobbered) is still there to
// read out of the exception context. Coexists with the DR0 write-watch above
// - different register, no conflict.
uintptr_t g_execAddr = 0;
bool g_execActive = false;
volatile LONG g_execTotal = 0;
volatile LONG g_execExpired = 0;
// The hit cap as a VARIABLE rather than kMaxHits directly. 0 == unlimited.
// Defaults to kMaxHits, so every existing client is unchanged and only a caller
// that explicitly calls WatchExecSetMaxHits() behaves differently.
//
// >>> THE 400 DEFAULT ENDED THE 2026-09-16 10:52 OVERRIDE MID-FIGHT. <<< The
// handler below clears DR7/DR1 on the 401st hit; the only code that turns
// g_execExpired into words is WatchExecActive(), which the ELEM poll never calls
// (camhook.cpp goes straight to WatchExecGet). So the watch disarmed itself
// silently and the run's behavioural observations covered an override that was
// no longer running. INV-6: make the negative path SAY something.
volatile LONG g_execMaxHits = kMaxHits;
// Distinct (rcx,rdx,ret) keys the 64-entry table turned away. Was silent, and
// INV-59 already records that exact silence costing a reading once.
volatile LONG g_execRefusedNew = 0;
// Threads carrying the breakpoint at the moment of arming. Threads created after
// that never get it, so a caller can compare against the live count and SEE
// drift rather than infer it.
volatile LONG g_execArmedThreads = 0;
std::vector<ExecHit> g_execHits;

// How ArmThread programs the debug registers. This used to be a `bool execMode`;
// there are three modes now because DR0 can be either a data watch or - for
// client 1 - an execution breakpoint, and those need different DR7 fields.
enum ArmMode {
    kArmDataDr0 = 0,   // WatchWrite: DR0, RW=write/read-write, LEN=4 bytes
    kArmExecDr1 = 1,   // client 0:   DR1, RW=00 execute, LEN=00 one byte
    kArmExecDr0 = 2,   // client 1:   DR0, RW=00 execute, LEN=00 one byte
};

// ---- CLIENT 1: the second exec client, on DR0. See watchwrite.h. ----
// Fixed-size table, not a vector: this is read and written from the handler and
// a fixed array needs no allocation there. Client 0's vector stays exactly as it
// is - it is correctly guarded and rewriting working locking while adding a
// feature is how this kind of change goes wrong.
constexpr int kExec2Max = 32;
uintptr_t     g_exec2Addr = 0;
volatile LONG g_exec2Active = 0;
volatile LONG g_exec2Total = 0;
volatile LONG g_exec2Expired = 0;
volatile LONG g_exec2MaxHits = 0;      // 0 = unlimited, like client 0's opt-in
ExecHit       g_exec2Hits[kExec2Max] = {};
volatile LONG g_exec2Count = 0;
// Set once THIS copy arms the register (any client), never cleared - see the handler's top comment.
volatile LONG g_everDr0 = 0;
volatile LONG g_everDr1 = 0;
// U-26 (docs/superpowers/specs/2026-10-20-watchexec-orphan-trap-design.md): every exec address THIS copy armed, with its
// release time, and the claims the handler made. The handler only reads the ring and counts; SayOrphans logs outside it.
watchorphan::Ring g_orphanRing;
volatile LONG    g_orphanClaims = 0;
volatile LONG64  g_orphanFirstAddr = 0;
volatile LONG    g_orphanFirstTid = 0;
volatile LONG64  g_orphanFirstDr7 = 0;
LONG             g_orphanSaid = -1;   // -1 = nothing said yet: the first SayOrphans prints the count, 0 included (review I1)
SRWLOCK          g_orphanWrite = SRWLOCK_INIT;   // review I4: serialises ring WRITERS (two client threads); the handler never takes it

void OrphanArmed(uintptr_t addr)
{
    AcquireSRWLockExclusive(&g_orphanWrite);
    watchorphan::NoteArmed(g_orphanRing, addr);
    ReleaseSRWLockExclusive(&g_orphanWrite);
}

void OrphanReleased(uintptr_t addr)
{
    AcquireSRWLockExclusive(&g_orphanWrite);
    watchorphan::NoteReleased(g_orphanRing, addr, GetTickCount64());
    ReleaseSRWLockExclusive(&g_orphanWrite);
}
char g_execStatus[256] = "off";
volatile LONG g_execStackFloatOffset = 0;   // see WatchExecSetStackFloatOffset
// Synchronous field capture at [RCX + offset] - see WatchExecSetFieldCapture.
// Bytes == 0 means disabled, which is the default, so nothing changes for any
// existing client of this file.
volatile LONG g_execFieldOffset = 0;
volatile LONG g_execFieldBytes  = 0;

// ===== THE REGISTER WRITE-BACK (2026-09-17) - see the header for the safety
// model and for why the poke cannot do this job (D-56). =====
// Every one of these defaults to the REFUSING state: disabled, no gate entries,
// and a bound of [0,0] that nothing satisfies. Configuring one of the three and
// forgetting another must not produce a write.
volatile LONG   g_regWriteEnabled = 0;
volatile LONG   g_regWriteReg     = 1;         // 0=RCX 1=RDX 2=R8 3=R9
volatile LONG64 g_regWriteValue   = 0;
volatile LONG64 g_regWriteMask    = 0;         // 0 = writes nothing, counted
volatile LONG64 g_regWriteLo      = 0;
volatile LONG64 g_regWriteHi      = 0;         // hi == 0 => every write refused
volatile LONG   g_regWriteApplied = 0;
volatile LONG   g_regWriteGatedOut = 0;
volatile LONG   g_regWriteRefused = 0;
volatile LONG64 g_regWriteOrigLast = 0;
uintptr_t     g_regGate[kRegWriteGateMax] = {};
volatile LONG g_regGateCount = 0;

// ===== THE RETURN-ADDRESS CENSUS (2026-09-17) =====
ReturnHit     g_retCensus[kReturnCensusMax] = {};
volatile LONG g_retCensusOn      = 0;
volatile LONG g_retCensusCount   = 0;
volatile LONG g_retCensusTotal   = 0;
volatile LONG g_retCensusRefused = 0;
// The poke - see WatchExecSetPoke. Default DISABLED, which is the safe state.
// g_pokeCapture arms the READ of the original dword; g_pokeEnabled arms the
// WRITE. They are separate on purpose: observe-only mode must still report the
// value at the site, or "nothing written" and "nothing seen" become the same
// log line - which is the absence-of-output trap this project keeps paying for.
volatile LONG g_pokeCapture = 0;
volatile LONG g_pokeEnabled = 0;
volatile LONG g_pokeReg     = 2;   // 0=RCX 1=RDX 2=R8 3=R9
volatile LONG g_pokeOffset  = 0;
volatile LONG g_pokeValue   = 0;
// The per-model poke map - see the header. The table is written from outside the
// handler and only READ inside it, so a torn read is the worst case: entries are
// published value-first, then key, and the handler matches on key, so a
// half-written entry can never be selected.
struct PokeMapEntry { volatile uintptr_t key; volatile LONG value; volatile LONG bound; };
PokeMapEntry  g_pokeMap[kPokeMapMax] = {};

// ---- the TYPE map, D-44's fix. See watchwrite.h for why the key is the
// ---- element count and why reading it here needs no call.
constexpr int kPokeTypeMax = 32;
struct PokeTypeEntry {
    volatile LONG count;      // the element count = the type key. 0 = empty
    volatile LONG value;      // the element to write for that type
    volatile LONG bound;      // fail-closed shape check, same rule as above
    volatile LONG poisoned;   // two different values for one count -> never write
};
PokeTypeEntry g_pokeType[kPokeTypeMax] = {};
volatile LONG g_pokeTypeN        = 0;
volatile LONG g_pokeTypeArmed    = 0;
volatile LONG g_pokeTypePtrOff   = 0;
volatile LONG g_pokeTypeContOff  = 0;
volatile LONG g_pokeTypeCountOff = 0;
volatile LONG g_pokeTypeHits     = 0;
volatile LONG g_pokeTypeMisses   = 0;

volatile LONG g_pokeMapN       = 0;
volatile LONG g_pokeMapArmed   = 0;
volatile LONG g_pokeMapKeyOff  = 0;
volatile LONG g_pokeMapWriteOff = 0;
volatile LONG g_pokeMapHits    = 0;
volatile LONG g_pokeMapMisses  = 0;
volatile LONG g_pokeMapRejected = 0;

// Guarded 4-byte WRITE. Mirrors ReadStackQword's shape: a fault is swallowed and
// reported as failure rather than taking the process down. Note what this CANNOT
// protect against - a successful write to the WRONG address.
bool WriteDwordGuarded(uintptr_t addr, uint32_t v)
{
    __try {
        *reinterpret_cast<volatile uint32_t*>(addr) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
volatile LONG g_execStackUseRsp = 0;        // see WatchExecSetStackUseRsp

// Our own writes trip the same breakpoint, and reporting ourselves as the
// culprit would be worse than useless. The layer's own range is excluded.
uintptr_t g_selfLo = 0, g_selfHi = 0;

struct Guard {
    Guard() { EnterCriticalSection(&g_cs); }
    ~Guard() { LeaveCriticalSection(&g_cs); }
};

void EnsureCs()
{
    if (!g_csReady) {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
}

// __try cannot share a function with C++ objects that need unwinding (the
// Guard lock, std::vector) - MSVC C2712. Isolated here, same reason rtticam.cpp's
// SafeRead() is its own small function rather than inlined where it's used.
bool ReadStackFloats3(uintptr_t rbp, intptr_t offset, float out[3])
{
    __try {
        const auto* p = reinterpret_cast<const float*>(rbp + offset);
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// At a breakpoint armed on a function's ENTRY point, [RSP] still holds the
// return address the CALL pushed - i.e. the instruction immediately after the
// call site. That names the caller outright, which matters for any target
// reached through a vtable, where no static xref exists to trace back from
// (the recurring wall in this project: CLineListEx's add-line factory,
// SRendererAPI's slots, IGSObject::GetLevel...). Only meaningful when the
// watch is armed at a real entry point - arming mid-function reads whatever
// happens to be on the stack there instead. Same C2712 isolation as above.
bool ReadReturnAddress(uintptr_t rsp, uintptr_t* out)
{
    __try {
        *out = *reinterpret_cast<const uintptr_t*>(rsp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One guarded qword read, for the stack argument slots. Same __try shape as
// ReadReturnAddress above and for the same reason: an unmapped or shifted stack
// must fail cleanly rather than take the process down inside a debug handler.
bool ReadStackQword(uintptr_t addr, uint64_t* out)
{
    __try {
        *out = *reinterpret_cast<const uint64_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A 16-bit read of its own rather than a masked qword: the element count is a
// u16 and an 8-byte read of it could straddle the end of a page that the 2-byte
// read would never touch. In a VEH handler that is not a theoretical difference.
bool ReadWordGuarded(uintptr_t addr, uint16_t* out)
{
    __try {
        *out = *reinterpret_cast<const uint16_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ===== THE POKE ITSELF, LIFTED OUT OF THE VEH HANDLER SO IT HAS ONE BODY =====
//
// >>> WHY THIS IS A SEPARATE FUNCTION AND NOT A COPY. <<< There are now TWO ways
// to reach this moment - the DR1 execution breakpoint, and the inline hook at
// gamedll+0xC4D920 - and U-27 exists to tell them apart: six of six in-play
// face-2 crashes were trap-armed, three of them with the write OFF (INV-120).
// The whole point of the comparison is that the MECHANISM is the only thing
// that differs. Two near-identical implementations would make it a comparison
// of implementations instead, and the difference would be unattributable.
//
// `pbase` is whatever register the record arrived in - R8 for the current
// configuration, at the breakpoint or at the hook, identically.
//
// Every read and every write is guarded. A faulting write is swallowed and
// leaves `false`, which is what the caller reports as pokeValid.
// Defined inside this file's anonymous namespace, so it has internal linkage;
// WatchExecPokeApplyAt below (outside it) is the one the inline hook links to.
bool PokeApplyAtImpl(uintptr_t pbase, uint32_t* origOut)
{
    if (origOut) { *origOut = 0; }
    if (!pbase || !g_pokeMapArmed) { return false; }

    const uintptr_t kaddr =
        static_cast<uintptr_t>(static_cast<intptr_t>(pbase) +
                               static_cast<intptr_t>(g_pokeMapKeyOff));
    const uintptr_t waddr =
        static_cast<uintptr_t>(static_cast<intptr_t>(pbase) +
                               static_cast<intptr_t>(g_pokeMapWriteOff));
    uint64_t kq = 0, wq = 0;
    uint32_t orig = 0;
    bool wrote = false;

    // Read the original FIRST, then write. "we wrote 7" without "it was 3"
    // cannot tell a working override from a no-op.
    if (ReadStackQword(waddr, &wq)) {
        orig = static_cast<uint32_t>(wq & 0xFFFFFFFFull);
    }
    if (origOut) { *origOut = orig; }

    if (!ReadStackQword(kaddr, &kq) || !kq) {
        InterlockedIncrement(&g_pokeMapMisses);
        return false;
    }

    const LONG n = g_pokeMapN;
    bool found = false;
    for (LONG i = 0; i < n && i < kPokeMapMax; ++i) {
        if (g_pokeMap[i].key != static_cast<uintptr_t>(kq)) { continue; }
        found = true;
        // FAIL-CLOSED SHAPE CHECK. The original value at the write offset must
        // be a plausible element for this model. A record of a different shape
        // (INV-67) matching the key by coincidence will almost certainly fail
        // here - and we must NOT write into it.
        const LONG bound = g_pokeMap[i].bound;
        if (bound > 0 && (static_cast<LONG>(orig) < 0 ||
                          static_cast<LONG>(orig) >= bound)) {
            InterlockedIncrement(&g_pokeMapRejected);
            return false;                 // nothing written, deliberately
        }
        wrote = WriteDwordGuarded(waddr, static_cast<uint32_t>(g_pokeMap[i].value));
        InterlockedIncrement(&g_pokeMapHits);
        return wrote;
    }
    (void)found;

    // ===== D-44's FIX: fall back to the TYPE map =====
    // The instance pointer is unknown - a body we have not dumped, or one whose
    // pointer has churned. The element index is a property of the TYPE, so ask
    // the type key instead of writing nothing. Three plain guarded reads, no
    // calls: watchwrite.h has why that is possible and what D-1 licenses.
    if (g_pokeTypeArmed) {
        uint64_t p1 = 0, cont = 0;
        uint16_t cnt = 0;
        if (ReadStackQword(static_cast<uintptr_t>(kq) +
                               static_cast<uintptr_t>(g_pokeTypePtrOff), &p1) &&
            p1 &&
            ReadStackQword(static_cast<uintptr_t>(p1) +
                               static_cast<uintptr_t>(g_pokeTypeContOff), &cont) &&
            cont &&
            ReadWordGuarded(static_cast<uintptr_t>(cont) +
                                static_cast<uintptr_t>(g_pokeTypeCountOff), &cnt) &&
            cnt) {
            const LONG tn = g_pokeTypeN;
            for (LONG t = 0; t < tn && t < kPokeTypeMax; ++t) {
                if (g_pokeType[t].count != static_cast<LONG>(cnt)) { continue; }
                // POISONED: two skeletons share this element count and disagree
                // on the element. Write NOTHING - a guess here is a
                // wrong-but-valid index, which is the failure D-42 names.
                if (g_pokeType[t].poisoned) { break; }
                const LONG tb = g_pokeType[t].bound;
                if (tb > 0 && (static_cast<LONG>(orig) < 0 ||
                               static_cast<LONG>(orig) >= tb)) {
                    InterlockedIncrement(&g_pokeMapRejected);
                    return false;
                }
                wrote = WriteDwordGuarded(waddr,
                                          static_cast<uint32_t>(g_pokeType[t].value));
                InterlockedIncrement(&g_pokeTypeHits);
                return wrote;
            }
        }
    }
    // Counted in BOTH: typeMisses says the type fallback did not save it,
    // mapMisses keeps its existing meaning of "wrote nothing".
    InterlockedIncrement(&g_pokeTypeMisses);
    InterlockedIncrement(&g_pokeMapMisses);
    return false;
}

void ComputeSelfRange()
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ComputeSelfRange), &self);
    if (!self) {
        return;
    }
    g_selfLo = reinterpret_cast<uintptr_t>(self);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_selfLo);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_selfLo + dos->e_lfanew);
    g_selfHi = g_selfLo + nt->OptionalHeader.SizeOfImage;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = ep->ContextRecord;
    // [LOADBEARING: WATCHEXEC-ORPHAN] U-26: an exec trap with NO breakpoint bit in Dr6, exactly on an address this copy armed
    //      (now, or released within 5 s), is ours - the arm / disarm hand-over rewrote that thread's debug registers between the
    //      trap and its dispatch. Without this nobody claims it and the game dies (0x80000004 at gamedll+0x179FF0, 2026-10-20).
    //      Claimed: RF so the instruction runs once, resume. No lock, no allocation, no LOG here (SayOrphans logs).
    if (watchorphan::IsOrphanExecTrap(g_orphanRing, ctx->Dr6, reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress),
                                      static_cast<uintptr_t>(ctx->Rip), GetTickCount64())) {
        ctx->EFlags |= 0x10000;
        if (InterlockedCompareExchange64(&g_orphanFirstAddr, static_cast<LONG64>(ctx->Rip), 0) == 0) {
            InterlockedExchange(&g_orphanFirstTid, static_cast<LONG>(GetCurrentThreadId()));
            InterlockedExchange64(&g_orphanFirstDr7, static_cast<LONG64>(ctx->Dr7));
        }
        InterlockedIncrement(&g_orphanClaims);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // DR6 bit 0 says DR0 fired, bit 1 says DR1 fired. Either, both, or
    // neither (some other single-step reason entirely) can be true.
    //
    // 2026-10-13: THIS FILE IS COMPILED INTO TWO DLLs (layer and bootstrap), each with its own handler in the same process.
    // A register THIS copy has never armed belongs to the OTHER copy, and is passed on untouched (Dr6 intact) - before this,
    // whichever handler ran first treated the other DLL's live breakpoint as stale and cleared it on that thread. A copy that
    // has armed a register keeps the full self-heal for it, so the 2026-09-09 stale-register crash path is unchanged.
    const bool dr0Hit = (ctx->Dr6 & 0x1) != 0 && g_everDr0 != 0;
    const bool dr1Hit = (ctx->Dr6 & 0x2) != 0 && g_everDr1 != 0;
    if (!dr0Hit && !dr1Hit) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ctx->Dr6 = 0;

    // SELF-HEAL A STALE REGISTER.
    //
    // A thread whose disarm failed still has DR0 or DR1 live after the watch has
    // been stopped globally. Without this it would fall through every branch
    // below to CONTINUE_SEARCH - an unhandled debug exception, which is a process
    // kill, and was exactly the 2026-09-09 crash. Clearing the offending register
    // on THIS thread and resuming turns that into a single wasted trap.
    // DR0 now has TWO possible owners - the data watch, or exec client 1. A hit
    // is only stale if NEITHER claims it. Getting this wrong would clear a live
    // client-1 breakpoint on first fire and silently stop the watch, which is
    // indistinguishable from "the address never executed".
    const bool dr0IsExec2 = (InterlockedCompareExchange(&g_exec2Active, 0, 0) != 0)
                            && g_exec2Addr != 0;
    const bool staleDr0 = dr0Hit && !dr0IsExec2 && (!g_active || g_addr == 0);
    const bool staleDr1 = dr1Hit && (!g_execActive || g_execAddr == 0);
    if ((staleDr0 || !dr0Hit) && (staleDr1 || !dr1Hit)) {
        if (dr0Hit) {
            ctx->Dr7 &= ~static_cast<DWORD64>(0xF0003);
            ctx->Dr0 = 0;
        }
        if (dr1Hit) {
            ctx->Dr7 &= ~static_cast<DWORD64>(0xF00004);
            ctx->Dr1 = 0;
            // Fault-class, so without RF the same instruction re-traps forever.
            ctx->EFlags |= 0x10000;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Stop on its own once we have seen enough.
    //
    // A data breakpoint on a matrix the engine rewrites every frame fires
    // constantly, and every hit is an exception dispatched through this handler.
    // On the render thread that is enough to stop frames reaching the compositor
    // - the headset goes black while the flat window carries on, which is
    // exactly what a starved submit loop looks like. A few hundred hits name
    // every writer there is; staying armed past that only costs the picture.
    if (dr0Hit && dr0IsExec2) {
        // DR0 PROGRAMMED AS AN EXECUTION BREAKPOINT IS FAULT-CLASS, exactly like
        // DR1 - it fires BEFORE the instruction runs, RIP still on it. The data
        // watch below is trap-class and fires after. Without EFLAGS.RF the same
        // still-current RIP re-matches immediately and re-raises forever, and
        // the instruction never executes. This is the same x86 gotcha the DR1
        // branch documents; it applies per-REGISTER-MODE, not per register.
        ctx->EFlags |= 0x10000;

        const LONG fires = InterlockedIncrement(&g_exec2Total);
        const LONG cap = g_exec2MaxHits;
        if (cap > 0 && fires > cap) {
            ctx->Dr7 &= ~static_cast<DWORD64>(0xF0003);
            ctx->Dr0 = 0;
            InterlockedExchange(&g_exec2Expired, 1);
        } else {
            // Aggregate by RCX, same contract as client 0 (INV-59): the table
            // COPIES AND AGGREGATES, so one entry per distinct call site with a
            // running count - never one entry per firing.
            const uintptr_t rcx = static_cast<uintptr_t>(ctx->Rcx);
            Guard lk;
            const LONG n = g_exec2Count;
            bool found = false;
            for (LONG i = 0; i < n && i < kExec2Max; ++i) {
                if (g_exec2Hits[i].rcxValue == rcx) {
                    ++g_exec2Hits[i].count;
                    found = true;
                    break;
                }
            }
            if (!found && n < kExec2Max) {
                g_exec2Hits[n].rcxValue = rcx;
                g_exec2Hits[n].rdxValue = static_cast<uintptr_t>(ctx->Rdx);
                g_exec2Hits[n].r8Value  = static_cast<uintptr_t>(ctx->R8);
                g_exec2Hits[n].count    = 1;
                g_exec2Count = n + 1;
            }
        }
    } else if (dr0Hit) {
        if (InterlockedIncrement(&g_total) > kMaxHits) {
            ctx->Dr7 &= ~static_cast<DWORD64>(0xF0003);
            ctx->Dr0 = 0;
            InterlockedExchange(&g_expired, 1);
        } else {
            const uintptr_t rip = static_cast<uintptr_t>(ctx->Rip);
            if (rip < g_selfLo || rip >= g_selfHi) {
                Guard lk;
                bool found = false;
                for (auto& h : g_hits) {
                    if (h.rip == rip) {
                        ++h.count;
                        found = true;
                        break;
                    }
                }
                if (!found && g_hits.size() < 64) {
                    g_hits.push_back(Hit{ rip, 1 });
                }
            }
        }
    }

    if (dr1Hit) {
        // Execution breakpoints are fault-class (fire BEFORE the flagged
        // instruction runs, RIP still pointing at it) - unlike the DR0 data
        // watch above, which is trap-class and fires after, with RIP already
        // past it. Resuming without EFLAGS.RF (bit 16) set lets the debug
        // hardware immediately re-match the same still-current RIP and
        // re-raise the exception again before that instruction has ever
        // actually executed - an x86 gotcha. See the matching comment and
        // CLAUDE.md note in aimhook.cpp's handler, where this was originally
        // tracked down; likely explanation for every "this fires an enormous
        // number of times" reading taken from this exec-watch tool too.
        ctx->EFlags |= 0x10000;

        // g_execTotal is incremented UNCONDITIONALLY and is the denominator a
        // poke client needs: it is exactly how many times the override ran.
        // A cap of 0 means unlimited - the watch then never disarms itself and
        // the caller is responsible for stopping it.
        const LONG fires = InterlockedIncrement(&g_execTotal);
        const LONG cap = g_execMaxHits;
        if (cap > 0 && fires > cap) {
            ctx->Dr7 &= ~static_cast<DWORD64>(0xF00004);
            ctx->Dr1 = 0;
            InterlockedExchange(&g_execExpired, 1);
        } else if (InterlockedCompareExchange(&g_retCensusOn, 0, 0) != 0) {
            // ===== CENSUS MODE: THE LEAN PATH =====
            //
            // One guarded [RSP] read and a linear scan of at most 64 entries.
            // The stack-argument set, the field capture, the poke and the
            // ExecHit table are ALL SKIPPED - see the header. This exists to
            // measure the call rate of a site whose rate is [UNKNOWN], and
            // twelve stack reads per hit would be measuring our own instrument.
            //
            // The Guard is the same critical section the full path takes, kept
            // deliberately rather than swapped for a lock-free find-or-add: a
            // racing duplicate entry would be silent, and if this lock turns out
            // to cost anything at this site then that IS the finding the census
            // was armed to produce.
            InterlockedIncrement(&g_retCensusTotal);
            uintptr_t ret = 0;
            ReadReturnAddress(static_cast<uintptr_t>(ctx->Rsp), &ret);
            Guard lk;
            bool found = false;
            const LONG n = g_retCensusCount;
            for (LONG i = 0; i < n && i < kReturnCensusMax; ++i) {
                if (g_retCensus[i].ret == ret) {
                    ++g_retCensus[i].count;
                    g_retCensus[i].lastRcx = static_cast<uintptr_t>(ctx->Rcx);
                    g_retCensus[i].lastRdx = static_cast<uintptr_t>(ctx->Rdx);
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (n < kReturnCensusMax) {
                    g_retCensus[n].ret     = ret;
                    g_retCensus[n].count   = 1;
                    g_retCensus[n].lastRcx = static_cast<uintptr_t>(ctx->Rcx);
                    g_retCensus[n].lastRdx = static_cast<uintptr_t>(ctx->Rdx);
                    g_retCensusCount = n + 1;
                } else {
                    // The table is full and this is a call site it will never
                    // show. Counted, never dropped silently: a gate list built
                    // from an incomplete census is an incomplete gate list.
                    InterlockedIncrement(&g_retCensusRefused);
                }
            }
        } else {
            // The breakpoint address is the call site itself, and execution
            // breakpoints trap BEFORE the instruction runs - so rcx/rdx
            // already hold whatever the instructions just before it put
            // there, untouched by the call about to happen. Grabbing them
            // here is the only place a register-only value exists at all;
            // it is never written to memory anywhere in this function.
            const uintptr_t rcx = static_cast<uintptr_t>(ctx->Rcx);
            const uintptr_t rdx = static_cast<uintptr_t>(ctx->Rdx);

            // Optional: a value that lives on the CALLER's stack, addressed
            // relative to RBP (callee-saved, so still correct even if this
            // site is armed right after a `call` returns rather than before
            // it). Has to be read right here, synchronously - by the time
            // any later code looks at it, that stack slot has almost
            // certainly been reused for something else entirely.
            float stackVec[3] = {};
            bool stackVecValid = false;
            const LONG offset = g_execStackFloatOffset;
            const bool useRsp = g_execStackUseRsp != 0;
            if (offset != 0) {
                const uintptr_t base = useRsp ? static_cast<uintptr_t>(ctx->Rsp)
                                               : static_cast<uintptr_t>(ctx->Rbp);
                stackVecValid = ReadStackFloats3(base, static_cast<intptr_t>(offset), stackVec);
                // Logged every hit, not throttled - unlike the camera matrix
                // or the WeaponFireControllerRight write site, this fires at
                // most once per shot, nowhere near flood volume, and the
                // whole point is watching it change shot to shot with a real
                // timestamp attached, which a single de-duplicated UI row
                // cannot show.
                if (stackVecValid) {
                    LOG("watchexec: stack vec at %s%+ld = %.4f %.4f %.4f",
                        useRsp ? "rsp" : "rbp", offset,
                        stackVec[0], stackVec[1], stackVec[2]);
                }
            }

            // ---- SYNCHRONOUS FIELD CAPTURE, at the instruction ----
            //
            // Read a contiguous blob at [RCX + offset] HERE, while the trapped
            // instruction has not yet run, so the values belong to THIS hit. The
            // polling client reads them up to 100 ms later otherwise, which on
            // 2026-09-15 made four bones share one location - INV-5's failure.
            //
            // Cost: at most twelve guarded 8-byte reads, on a breakpoint that
            // fired 59 times in three minutes. This is NOT a per-frame path.
            //
            // Chunked rather than one memcpy so a fault part-way through marks
            // the blob INVALID instead of leaving half a buffer that reads like
            // data - INV-6, fail closed.
            uint8_t  fieldBlob[96] = {};
            uint32_t fieldBlobBytes = 0;
            bool     fieldBlobValid = false;
            {
                const LONG fbytes = g_execFieldBytes;
                if (fbytes > 0) {
                    const LONG foff = g_execFieldOffset;
                    uint32_t want = static_cast<uint32_t>(fbytes);
                    if (want > sizeof(fieldBlob)) { want = sizeof(fieldBlob); }
                    const uintptr_t src =
                        static_cast<uintptr_t>(
                            static_cast<intptr_t>(ctx->Rcx) + static_cast<intptr_t>(foff));
                    bool ok = (ctx->Rcx != 0);
                    uint32_t got = 0;
                    while (ok && got < want) {
                        uint64_t q = 0;
                        if (!ReadStackQword(src + got, &q)) { ok = false; break; }
                        const uint32_t n =
                            (want - got) < 8u ? (want - got) : 8u;
                        memcpy(fieldBlob + got, &q, n);
                        got += n;
                    }
                    fieldBlobBytes = got;
                    fieldBlobValid = ok && (got == want);
                }
            }

            // ---- THE POKE: mutate the game's memory before the instruction runs ----
            //
            // Read the original first, then write. The original goes into the
            // ExecHit so the log can say old -> new: "we wrote 7" without "it was
            // 3" cannot tell a working override from a no-op, and that is exactly
            // the distinction this exists to make.
            uint32_t pokeOrig = 0;
            bool     pokeValid = false;
            if (g_pokeCapture) {
                uintptr_t pbase = 0;
                switch (g_pokeReg) {
                    case 0:  pbase = static_cast<uintptr_t>(ctx->Rcx); break;
                    case 1:  pbase = static_cast<uintptr_t>(ctx->Rdx); break;
                    case 2:  pbase = static_cast<uintptr_t>(ctx->R8);  break;
                    default: pbase = static_cast<uintptr_t>(ctx->R9);  break;
                }
                if (pbase) {
                    // THE PER-MODEL MAP takes precedence when armed. It reads a
                    // key (the model) from the SAME base register, looks it up,
                    // and writes only on a hit. A miss leaves the original value
                    // untouched and is counted - see the header for why writing
                    // a default would destroy the result.
                    if (g_pokeMapArmed) {
                        // >>> ONE BODY, TWO MECHANISMS. <<< The map logic lived
                        // here until 2026-09-18 and now lives in
                        // WatchExecPokeApplyAt, because the inline hook at
                        // gamedll+0xC4D920 has to run EXACTLY this and U-27's
                        // comparison is only valid if the mechanism is the sole
                        // difference. See the function's own comment.
                        pokeValid = PokeApplyAtImpl(pbase, &pokeOrig);
                    } else {
                        const uintptr_t paddr =
                            static_cast<uintptr_t>(static_cast<intptr_t>(pbase) +
                                                   static_cast<intptr_t>(g_pokeOffset));
                        uint64_t q = 0;
                        if (ReadStackQword(paddr, &q)) {
                            pokeOrig = static_cast<uint32_t>(q & 0xFFFFFFFFull);
                            // The READ always happens; the WRITE only when armed.
                            pokeValid = g_pokeEnabled
                                            ? WriteDwordGuarded(
                                                  paddr, static_cast<uint32_t>(g_pokeValue))
                                            : true;
                        }
                    }
                }
            }

            uintptr_t returnAddr = 0;
            ReadReturnAddress(static_cast<uintptr_t>(ctx->Rsp), &returnAddr);

            // THE FULL ARGUMENT SET - see the ExecHit comment for the x64
            // position->register mapping. Read unconditionally: a few register
            // copies plus eight guarded stack reads, and which slot holds what
            // is precisely the question being asked.
            const uintptr_t r8v = static_cast<uintptr_t>(ctx->R8);
            const uintptr_t r9v = static_cast<uintptr_t>(ctx->R9);
            // Xmm2/Xmm3 are M128A; a float argument occupies the low 32 bits.
            float xmm2f = 0.0f, xmm3f = 0.0f;
            {
                const unsigned int lo2 =
                    static_cast<unsigned int>(ctx->Xmm2.Low & 0xFFFFFFFFull);
                const unsigned int lo3 =
                    static_cast<unsigned int>(ctx->Xmm3.Low & 0xFFFFFFFFull);
                memcpy(&xmm2f, &lo2, sizeof(float));
                memcpy(&xmm3f, &lo3, sizeof(float));
            }
            // Stack arguments 5..12. At a function ENTRY the return address
            // occupies [RSP] and 0x20 of shadow space follows, so arg5 lands at
            // [RSP+0x28]. If this site is NOT an entry point that layout does
            // not hold - hence the validity flag, so the numbers cannot quietly
            // pass themselves off as arguments when they are some other frame's
            // leftovers. That exact mistake is already on record in this file's
            // stackVec comment.
            uint64_t stackArgs[8] = {};
            float    stackArgsF[8] = {};
            bool     stackArgsValid = true;
            {
                const uintptr_t sp = static_cast<uintptr_t>(ctx->Rsp);
                for (int i = 0; i < 8; ++i) {
                    uint64_t q = 0;
                    if (!ReadStackQword(sp + 0x28 + static_cast<uintptr_t>(i) * 8, &q)) {
                        stackArgsValid = false;
                        break;
                    }
                    stackArgs[i] = q;
                    const unsigned int lo = static_cast<unsigned int>(q & 0xFFFFFFFFull);
                    memcpy(&stackArgsF[i], &lo, sizeof(float));
                }
            }

            Guard lk;
            bool found = false;
            for (auto& h : g_execHits) {
                if (h.rcxValue == rcx && h.rdxValue == rdx && h.returnAddr == returnAddr) {
                    ++h.count;
                    // Refresh to the latest capture, not the first - the
                    // whole point is watching this change shot to shot.
                    h.stackVec[0] = stackVec[0];
                    h.stackVec[1] = stackVec[1];
                    h.stackVec[2] = stackVec[2];
                    h.stackVecValid = stackVecValid;
                    // Refresh the argument set too, for the same reason the
                    // stack vec is refreshed: the point is watching it change
                    // hit to hit, not preserving the first one.
                    h.r8Value = r8v;
                    h.r9Value = r9v;
                    h.xmm2f = xmm2f;
                    h.xmm3f = xmm3f;
                    for (int i = 0; i < 8; ++i) {
                        h.stackArgs[i] = stackArgs[i];
                        h.stackArgsF[i] = stackArgsF[i];
                    }
                    h.stackArgsValid = stackArgsValid;
                    // Refresh the synchronous blob too - same reason as the stack
                    // vec above: the point is watching these change blow to blow.
                    memcpy(h.fieldBlob, fieldBlob, sizeof(h.fieldBlob));
                    h.fieldBlobBytes = fieldBlobBytes;
                    h.fieldBlobValid = fieldBlobValid;
                    h.pokeOrig = pokeOrig;
                    h.pokeValid = pokeValid;
                    found = true;
                    break;
                }
            }
            // INV-59: this refusal was SILENT. A caller seeing no new lines
            // could not tell "no new call sites" from "the table is full", and
            // that ambiguity has already cost one reading.
            if (!found && g_execHits.size() >= 64) {
                InterlockedIncrement(&g_execRefusedNew);
            }
            if (!found && g_execHits.size() < 64) {
                ExecHit hit{};
                hit.rcxValue = rcx;
                hit.rdxValue = rdx;
                hit.returnAddr = returnAddr;
                hit.stackVec[0] = stackVec[0];
                hit.stackVec[1] = stackVec[1];
                hit.stackVec[2] = stackVec[2];
                hit.stackVecValid = stackVecValid;
                hit.r8Value = r8v;
                hit.r9Value = r9v;
                hit.xmm2f = xmm2f;
                hit.xmm3f = xmm3f;
                for (int i = 0; i < 8; ++i) {
                    hit.stackArgs[i] = stackArgs[i];
                    hit.stackArgsF[i] = stackArgsF[i];
                }
                hit.stackArgsValid = stackArgsValid;
                memcpy(hit.fieldBlob, fieldBlob, sizeof(hit.fieldBlob));
                hit.fieldBlobBytes = fieldBlobBytes;
                hit.fieldBlobValid = fieldBlobValid;
                hit.pokeOrig = pokeOrig;
                hit.pokeValid = pokeValid;
                // The register write-back runs AFTER this table is filled, so a
                // fresh entry always records what the GAME chose. These three
                // are updated by the write-back below when it lands.
                hit.regOrig = 0;
                hit.regWritten = 0;
                hit.regValid = false;
                hit.count = 1;
                g_execHits.push_back(hit);
            }
        }

        // ===== THE REGISTER WRITE-BACK =====
        //
        // LAST, after every capture above has been taken, immediately before the
        // resume. The placement is load-bearing rather than incidental: rcx,
        // rdx, the stack vec, the return address and the field blob must all
        // record what the game chose, NOT what we substituted. An override that
        // also falsifies its own observation is INV-5's failure with a write
        // attached, and no counter could reveal it afterwards.
        //
        // Runs on the expired path too - deliberately. If the watch has just
        // self-disarmed, `enabled` is still set and the caller is entitled to
        // know from `applied` that this hit was NOT written.
        if (InterlockedCompareExchange(&g_regWriteEnabled, 0, 0) != 0) {
            const LONG64 mask = g_regWriteMask;
            const LONG64 val  = g_regWriteValue;
            const LONG64 lo   = g_regWriteLo;
            const LONG64 hi   = g_regWriteHi;
            // Read BEFORE the write: with reg == 0 the write changes RCX itself,
            // and the ExecHit entry this hit created is keyed on the original.
            const uintptr_t rcxAtEntry = static_cast<uintptr_t>(ctx->Rcx);

            // 1. THE CALL-SITE GATE. An empty list refuses everything; it never
            //    means "allow all". See the header.
            uintptr_t ret = 0;
            ReadReturnAddress(static_cast<uintptr_t>(ctx->Rsp), &ret);
            bool gateOk = false;
            {
                Guard lk;
                const LONG gn = g_regGateCount;
                for (LONG i = 0; i < gn && i < kRegWriteGateMax; ++i) {
                    if (g_regGate[i] == ret) { gateOk = true; break; }
                }
            }
            if (!gateOk) {
                InterlockedIncrement(&g_regWriteGatedOut);
            } else if (mask == 0 ||
                       static_cast<uint64_t>(hi) == 0ull ||
                       static_cast<uint64_t>(val) < static_cast<uint64_t>(lo) ||
                       static_cast<uint64_t>(val) > static_cast<uint64_t>(hi)) {
                // 2. THE VALUE BOUND, and the mask. Never clamped - a value out
                //    of range is a bug on our side, and clamping would hide it
                //    behind a plausible result.
                InterlockedIncrement(&g_regWriteRefused);
            } else {
                DWORD64* target = nullptr;
                switch (g_regWriteReg) {
                    case 0:  target = &ctx->Rcx; break;
                    case 1:  target = &ctx->Rdx; break;
                    case 2:  target = &ctx->R8;  break;
                    default: target = &ctx->R9;  break;
                }
                const uint64_t orig = static_cast<uint64_t>(*target);
                const uint64_t m = static_cast<uint64_t>(mask);
                const uint64_t written = (orig & ~m) |
                                         (static_cast<uint64_t>(val) & m);
                *target = static_cast<DWORD64>(written);
                InterlockedExchange64(&g_regWriteOrigLast,
                                      static_cast<LONG64>(orig));
                InterlockedIncrement(&g_regWriteApplied);
                // Record old->new on the entry this hit just created or
                // refreshed, so a log line can state both without a second
                // sampling point.
                Guard lk;
                for (auto& h : g_execHits) {
                    if (h.rcxValue == rcxAtEntry && h.returnAddr == ret) {
                        h.regOrig = orig;
                        h.regWritten = written;
                        h.regValid = true;
                        break;
                    }
                }
            }
        }
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

// U-26: the handler's orphan-trap claims, logged here - outside the handler - when the count has moved. Called by the exec
// clients' start / stop / active paths. COST: one interlocked read when nothing changed.
void SayOrphans()
{
    const LONG n = InterlockedCompareExchange(&g_orphanClaims, 0, 0);
    if (n == g_orphanSaid) { return; }
    g_orphanSaid = n;
    // [PROBE: WATCHEXEC] gate=none (always on)  site=HEADSET  log-only=yes
    LOG("watchexec: [WATCHEXEC] ORPHAN TRAP claims: %ld so far (an exec trap with an empty Dr6 at an address this copy armed "
        "or had just released; resumed instead of crashing; 0 = none yet) | first: address %p, thread %lu, Dr7 0x%llx",
        static_cast<long>(n),
        reinterpret_cast<void*>(static_cast<uintptr_t>(g_orphanFirstAddr)), static_cast<unsigned long>(g_orphanFirstTid),
        static_cast<unsigned long long>(g_orphanFirstDr7));
}

// Sets or clears DR0 (data) or DR1 (execute) on one thread.
bool ArmThread(DWORD tid, uintptr_t addr, bool on, int mode)
{
    if (tid == GetCurrentThreadId()) {
        return true;   // cannot Get/SetThreadContext on self reliably
    }
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                              THREAD_SUSPEND_RESUME,
                          FALSE, tid);
    if (!t) {
        return false;
    }
    SuspendThread(t);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = false;
    if (GetThreadContext(t, &ctx)) {
        if (mode == kArmExecDr0) {
            // CLIENT 1. DR0 as an EXECUTION breakpoint: L0 enable, and RW0 and
            // LEN0 must BOTH be zero - identical rule to DR1 below, applied to
            // register 0's field. Clearing the field is the whole job.
            if (on) {
                ctx.Dr0 = addr;
                ctx.Dr7 &= ~static_cast<DWORD64>(0xF0003);
                ctx.Dr7 |= (1ull << 0);
            } else {
                ctx.Dr0 = 0;
                ctx.Dr7 &= ~static_cast<DWORD64>(0xF0003);
            }
        } else if (mode == kArmExecDr1) {
            if (on) {
                ctx.Dr1 = addr;
                // L1 enable; RW1 = 00 (execute) and LEN1 = 00 (one byte) are
                // both required to be zero for an execution breakpoint -
                // clearing the field is the whole job, nothing to OR in
                // beyond the enable bit.
                ctx.Dr7 &= ~static_cast<DWORD64>(0xF00004);
                ctx.Dr7 |= (1ull << 2);
            } else {
                ctx.Dr1 = 0;
                ctx.Dr7 &= ~static_cast<DWORD64>(0xF00004);
            }
        } else if (on) {
            ctx.Dr0 = addr;
            // L0 enable, RW0 = 01 (write) or 11 (read-or-write), LEN0 = 11 (four bytes).
            const DWORD64 rw = g_includeReads ? 0b11ull : 0b01ull;
            ctx.Dr7 &= ~static_cast<DWORD64>(0xF0003);
            ctx.Dr7 |= (1ull << 0) | (rw << 16) | (0b11ull << 18);
        } else {
            ctx.Dr0 = 0;
            ctx.Dr7 &= ~static_cast<DWORD64>(0xF0003);
        }
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        ok = SetThreadContext(t, &ctx) != 0;
    }
    ResumeThread(t);
    CloseHandle(t);
    return ok;
}

int ArmAll(uintptr_t addr, bool on, int mode)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    int n = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid &&
                ArmThread(te.th32ThreadID, addr, on, mode)) {
                ++n;
            }
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

struct SelfArmArgs {
    uintptr_t addr;
    DWORD tid;
    bool on;
    int mode;
};

DWORD WINAPI ArmSelfFromOutside(LPVOID p)
{
    const auto* a = static_cast<const SelfArmArgs*>(p);
    ArmThread(a->tid, a->addr, a->on, a->mode);
    return 0;
}

// ArmAll() walks every thread and arms it, but ArmThread() deliberately
// refuses to touch the thread it is called FROM - you cannot SuspendThread
// yourself without deadlocking. That thread is whichever one clicked the
// "Watch writes" button, which is the same thread the D3D11 Present hook
// (and this overlay) runs on - almost certainly the game's main/render
// thread, and exactly where gameplay state like AimModule is most likely to
// be written. Silently skipping it made every prior watch here report
// success while missing the one thread that mattered. A short-lived helper
// thread suspends and arms the caller from the outside instead, which is
// safe because it is genuinely a different thread doing the suspending.
bool ArmSelf(uintptr_t addr, bool on, int mode)
{
    SelfArmArgs args{ addr, GetCurrentThreadId(), on, mode };
    HANDLE helper = CreateThread(nullptr, 0, ArmSelfFromOutside, &args, 0, nullptr);
    if (!helper) {
        return false;
    }
    const bool ok = WaitForSingleObject(helper, 2000) == WAIT_OBJECT_0;
    CloseHandle(helper);
    return ok;
}

// Shared by both watches - either can be active on its own, or both at once
// (different debug registers, no conflict), so the handler is only installed
// once and only removed once neither needs it any more.
bool EnsureVeh()
{
    if (g_veh) {
        return true;
    }
    g_veh = AddVectoredExceptionHandler(1, Handler);
    return g_veh != nullptr;
}

// DELIBERATELY DOES NOT REMOVE THE HANDLER ANY MORE.
//
// This killed the game on 2026-09-09: an unhandled STATUS_SINGLE_STEP inside
// engine+0x672459, the world-matrix writer, while a DR0 watch was cycling.
//
// The mechanism. Stop() disarms every thread through ArmThread, which returns
// false whenever OpenThread or SetThreadContext fails - a thread exiting between
// the Toolhelp snapshot and the OpenThread, a protected thread, any of it. Stop
// ignored that failure and removed the VEH anyway, so a thread that kept DR0 live
// had no handler for its next write. The CPU raises the exception regardless of
// whether anyone is listening, and nobody was.
//
// Keeping the handler installed costs essentially nothing - it returns
// CONTINUE_SEARCH immediately for anything that is not its own breakpoint - while
// removing it converts a stale debug register from a harmless spurious trap into
// a process kill. Combined with the stale-hit self-heal in Handler(), a leftover
// register now cleans itself up on its next fault instead of taking the game down.
//
// This is the second crash from exactly this pattern. The first was the
// streaming-batch watch, fixed the same way in dllmain.cpp's own diag namespace
// and never back-ported here, where the identical bug had been sitting the whole
// time. Do not reintroduce the removal as a tidy-up.
void MaybeRemoveVeh()
{
    // Intentionally empty. See above.
}

}  // namespace

void WatchWritesSetIncludeReads(bool includeReads)
{
    g_includeReads = includeReads;
}

bool WatchWritesStart(uintptr_t addr)
{
    EnsureCs();
    WatchWritesStop();
    if (!addr) {
        return false;
    }
    ComputeSelfRange();

    {
        Guard lk;
        g_hits.clear();
    }
    InterlockedExchange(&g_total, 0);
    InterlockedExchange(&g_expired, 0);
    // A debug register watches 4 bytes on a 4-byte boundary.
    g_addr = addr & ~static_cast<uintptr_t>(3);

    if (!EnsureVeh()) {
        strncpy_s(g_status, sizeof(g_status), "could not install the handler",
                  _TRUNCATE);
        return false;
    }
    // >>> THE OTHER HALF OF THE MUTUAL EXCLUSION (see WatchExec2Start). DR0 has
    // two possible owners now. Whichever is second refuses and says why - it
    // must never quietly reprogram a live execution breakpoint out from under
    // client 1, because that reads downstream as "the address never executed".
    if (WatchExec2Active()) {
        LOG("watchwrites: REFUSING TO ARM at %p - exec CLIENT 1 holds DR0 "
            "(watching execution at %p). DR0 cannot be a data watch and an "
            "execution breakpoint at once. Stop client 1 first "
            "(WatchExec2Stop). NOTHING WAS ARMED.",
            reinterpret_cast<void*>(g_addr),
            reinterpret_cast<void*>(g_exec2Addr));
        g_addr = 0;
        return false;
    }

    InterlockedExchange(&g_everDr0, 1);
    const int n = ArmAll(g_addr, true, kArmDataDr0);
    const bool selfArmed = ArmSelf(g_addr, true, kArmDataDr0);
    g_active = (n > 0) || selfArmed;
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                g_active ? "watching %p (%s) on %d threads (+ caller: %s)"
                         : "could not arm any thread",
                reinterpret_cast<void*>(g_addr), g_includeReads ? "read+write" : "write-only",
                n, selfArmed ? "yes" : "no");
    LOG("watchwrite: %s", g_status);
    if (!g_active) {
        MaybeRemoveVeh();
    }
    return g_active;
}

void WatchWritesStop()
{
    if (g_addr) {
        ArmAll(g_addr, false, kArmDataDr0);
        ArmSelf(g_addr, false, kArmDataDr0);
    }
    g_active = false;
    g_addr = 0;
    strncpy_s(g_status, sizeof(g_status), "off", _TRUNCATE);
    MaybeRemoveVeh();
}

bool WatchWritesActive()
{
    // The handler disarms its own thread's register once the budget is spent;
    // clean the rest up here, on a thread that is allowed to take locks.
    if (g_active && g_expired) {
        WatchWritesStop();
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "stopped after %d writes - the list below is complete",
                    static_cast<int>(kMaxHits));
    }
    return g_active;
}
const char* WatchWritesStatus() { return g_status; }

int WatchWritesGet(WriteHit* out, int max)
{
    if (!g_csReady || !out || max <= 0) {
        return 0;
    }
    Guard lk;
    const int n = static_cast<int>(g_hits.size()) < max
                      ? static_cast<int>(g_hits.size())
                      : max;
    for (int i = 0; i < n; ++i) {
        out[i].rip = g_hits[i].rip;
        out[i].count = g_hits[i].count;
        out[i].moduleBase = 0;
        out[i].module[0] = '\0';

        HMODULE m = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(g_hits[i].rip), &m) && m) {
            out[i].moduleBase = reinterpret_cast<uintptr_t>(m);
            char path[MAX_PATH];
            if (GetModuleFileNameA(m, path, sizeof(path))) {
                const char* leaf = strrchr(path, '\\');
                strncpy_s(out[i].module, sizeof(out[i].module),
                          leaf ? leaf + 1 : path, _TRUNCATE);
            }
        }
    }
    return n;
}

void WatchExecSetMaxHits(long maxHits)
{
    if (maxHits < 0) { maxHits = 0; }
    InterlockedExchange(&g_execMaxHits, static_cast<LONG>(maxHits));
    if (maxHits == 0) {
        LOG("watchexec: HIT CAP LIFTED - this watch will NOT self-disarm. The "
            "default %d exists so a runaway site cannot trap forever; a client "
            "that lifts it owns stopping the watch. Licensed by INV-53, which "
            "measured a session-long hold at 0 of 600 frames late across 17 "
            "windows.", static_cast<int>(kMaxHits));
    } else {
        LOG("watchexec: hit cap set to %ld (default %d). The watch self-disarms "
            "after that many hits and ONLY the stats line will say so.",
            maxHits, static_cast<int>(kMaxHits));
    }
}

// Counts this process's threads the same way ArmAll() enumerates them, so the
// live figure and the armed figure are directly comparable rather than two
// different notions of "thread".
int CountThreadsNow()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return -1;   // NOT zero: "could not count" must not read as "no threads"
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    int n = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) { ++n; }
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

void WatchExecStats(unsigned long* fires, unsigned long* refusedNew,
                    bool* expired, int* armedThreads, int* liveThreads)
{
    if (fires)        { *fires = static_cast<unsigned long>(g_execTotal); }
    if (refusedNew)   { *refusedNew = static_cast<unsigned long>(g_execRefusedNew); }
    if (expired)      { *expired = (g_execExpired != 0); }
    if (armedThreads) { *armedThreads = static_cast<int>(g_execArmedThreads); }
    if (liveThreads)  { *liveThreads = CountThreadsNow(); }
}

void WatchExecPokeMapSet(uintptr_t key, unsigned int value)
{
    if (!key) { return; }
    const LONG n = g_pokeMapN;

    // Already present: refresh in place. This is the STALENESS fix - a recycled
    // pointer re-resolves to whatever object now lives there.
    for (LONG i = 0; i < n && i < kPokeMapMax; ++i) {
        if (g_pokeMap[i].key == key) {
            if (g_pokeMap[i].value != static_cast<LONG>(value)) {
                LOG("watchexec: POKE MAP ~ model %p element %ld -> %u. The pointer "
                    "was REUSED by a different model (D-44/INV-59) - the stale "
                    "value has been replaced rather than left to write the wrong "
                    "index.", reinterpret_cast<void*>(key),
                    static_cast<long>(g_pokeMap[i].value), value);
            }
            InterlockedExchange(&g_pokeMap[i].value, static_cast<LONG>(value));
            return;
        }
    }

    if (n < kPokeMapMax) {
        // Value first, THEN key. The handler matches on key, so an entry is
        // never selectable until its value is already in place.
        InterlockedExchange(&g_pokeMap[n].value, static_cast<LONG>(value));
        g_pokeMap[n].key = key;
        InterlockedExchange(&g_pokeMapN, n + 1);
        LOG("watchexec: POKE MAP + model %p -> element %u  (%ld of %d)",
            reinterpret_cast<void*>(key), value, n + 1, kPokeMapMax);
        return;
    }

    // Full: replace round-robin. A full map must not mean "everything from here
    // on misses" - that was D-44's measured failure at 16 entries.
    static LONG s_next = 0;
    const LONG slot = s_next % kPokeMapMax;
    s_next = (s_next + 1) % kPokeMapMax;
    InterlockedExchange(&g_pokeMap[slot].value, static_cast<LONG>(value));
    g_pokeMap[slot].key = key;
    LOG("watchexec: POKE MAP replaced slot %ld with model %p -> element %u "
        "(map is at its %d-entry cap; oldest evicted, NOT dropped).",
        slot, reinterpret_cast<void*>(key), value, kPokeMapMax);
}

void WatchExecPokeMapArm(int reg, int keyOffset, int writeOffset, bool enabled)
{
    if (reg < 0 || reg > 3) { reg = 2; }
    InterlockedExchange(&g_pokeReg, reg);
    InterlockedExchange(&g_pokeMapKeyOff, keyOffset);
    InterlockedExchange(&g_pokeMapWriteOff, writeOffset);
    InterlockedExchange(&g_pokeMapArmed, enabled ? 1 : 0);
    InterlockedExchange(&g_pokeMapHits, 0);
    InterlockedExchange(&g_pokeMapMisses, 0);
    InterlockedExchange(&g_pokeMapRejected, 0);
    // The capture must be on for pokeOrig to be recorded either way.
    InterlockedExchange(&g_pokeCapture, 1);
    static const char* kRegName[4] = { "RCX", "RDX", "R8", "R9" };
    if (enabled) {
        LOG("watchexec: POKE MAP ARMED - key = [%s%+d] (the object), write = "
            "[%s%+d]. The value written is chosen PER OBJECT from the table. A "
            "hit on an object with no mapping writes NOTHING and counts a MISS - "
            "read the miss count before believing any behavioural result.",
            kRegName[reg], keyOffset, kRegName[reg], writeOffset);
    } else {
        LOG("watchexec: POKE MAP DISARMED.");
    }
}

void WatchExecPokeMapSetBound(uintptr_t key, unsigned int bound)
{
    if (!key) { return; }
    const LONG n = g_pokeMapN;
    for (LONG i = 0; i < n && i < kPokeMapMax; ++i) {
        if (g_pokeMap[i].key == key) {
            InterlockedExchange(&g_pokeMap[i].bound, static_cast<LONG>(bound));
            return;
        }
    }
}

void WatchExecPokeMapStats(unsigned long* hits, unsigned long* misses, int* entries,
                           unsigned long* rejected)
{
    if (hits)     { *hits = static_cast<unsigned long>(g_pokeMapHits); }
    if (misses)   { *misses = static_cast<unsigned long>(g_pokeMapMisses); }
    if (entries)  { *entries = static_cast<int>(g_pokeMapN); }
    if (rejected) { *rejected = static_cast<unsigned long>(g_pokeMapRejected); }
}

// ===================== THE TYPE MAP - D-44's FIX =====================

// The externally-linked entry point. The body lives in PokeApplyAtImpl inside
// this file's anonymous namespace; this exists so the inline hook at
// gamedll+0xC4D920 runs EXACTLY what the VEH handler runs - U-27's comparison
// is of MECHANISMS, and two implementations would make it a comparison of
// implementations instead.
bool WatchExecPokeApplyAt(uintptr_t pbase, uint32_t* origOut)
{
    return PokeApplyAtImpl(pbase, origOut);
}

void WatchExecPokeTypeArm(int ptrOffset, int containerOffset, int countOffset,
                          bool enabled)
{
    InterlockedExchange(&g_pokeTypePtrOff, static_cast<LONG>(ptrOffset));
    InterlockedExchange(&g_pokeTypeContOff, static_cast<LONG>(containerOffset));
    InterlockedExchange(&g_pokeTypeCountOff, static_cast<LONG>(countOffset));
    InterlockedExchange(&g_pokeTypeArmed, enabled ? 1 : 0);
}

// Register count -> value, or POISON the count if it is already registered with
// a different one.
//
// >>> POISONING IS THE WHOLE SAFETY PROPERTY AND IT IS WHY THIS IS NOT D-42
// AGAIN. <<< Two skeletons can share an element count and not share a layout.
// If that happens, writing EITHER value is writing a wrong-but-valid index into
// a body it does not describe - which is exactly the failure mode D-42 recorded
// for a constant. So a conflict disables the count permanently for the session
// and every later hit on that type writes NOTHING. The caller logs it; a silent
// poisoning would be indistinguishable from a type nobody ever saw.
void WatchExecPokeTypeSet(unsigned int count, unsigned int value,
                          unsigned int bound)
{
    if (count == 0) { return; }          // 0 is the empty marker, never a key
    const LONG n = g_pokeTypeN;
    for (LONG i = 0; i < n && i < kPokeTypeMax; ++i) {
        if (g_pokeType[i].count != static_cast<LONG>(count)) { continue; }
        if (g_pokeType[i].poisoned) { return; }
        if (g_pokeType[i].value != static_cast<LONG>(value)) {
            InterlockedExchange(&g_pokeType[i].poisoned, 1);
        }
        return;                          // already known, and consistent
    }
    if (n >= kPokeTypeMax) { return; }   // full: the caller reports it
    // value and bound BEFORE the count, because the count is what the handler
    // matches on - a reader that sees the key must be looking at a complete
    // entry. Same ordering rule as PublishHitBody's module pointer.
    InterlockedExchange(&g_pokeType[n].value, static_cast<LONG>(value));
    InterlockedExchange(&g_pokeType[n].bound, static_cast<LONG>(bound));
    InterlockedExchange(&g_pokeType[n].poisoned, 0);
    InterlockedExchange(&g_pokeType[n].count, static_cast<LONG>(count));
    InterlockedExchange(&g_pokeTypeN, n + 1);
}

void WatchExecPokeTypeStats(unsigned long* typeHits, unsigned long* typeMisses,
                            int* entries, int* poisoned)
{
    if (typeHits)   { *typeHits = static_cast<unsigned long>(g_pokeTypeHits); }
    if (typeMisses) { *typeMisses = static_cast<unsigned long>(g_pokeTypeMisses); }
    if (entries)    { *entries = static_cast<int>(g_pokeTypeN); }
    if (poisoned) {
        int p = 0;
        const LONG n = g_pokeTypeN;
        for (LONG i = 0; i < n && i < kPokeTypeMax; ++i) {
            if (g_pokeType[i].poisoned) { ++p; }
        }
        *poisoned = p;
    }
}

void WatchExecSetPoke(int reg, int32_t offset, uint32_t value, bool enabled)
{
    if (reg < 0 || reg > 3) { reg = 2; }
    InterlockedExchange(&g_pokeReg, reg);
    InterlockedExchange(&g_pokeOffset, offset);
    InterlockedExchange(&g_pokeValue, static_cast<LONG>(value));
    InterlockedExchange(&g_pokeEnabled, enabled ? 1 : 0);
    // Configuring the site always arms the READ, so observe-only still reports
    // the value. Only a zero-offset/disabled call turns the capture off.
    InterlockedExchange(&g_pokeCapture, 1);
    static const char* kRegName[4] = { "RCX", "RDX", "R8", "R9" };
    LOG("watchexec: POKE %s - [%s%+ld] := %u (0x%X). >>> THIS WRITES INTO THE "
        "GAME'S MEMORY at the breakpoint, before the trapped instruction runs. "
        "Everything else in this file only observes. The original dword is "
        "recorded per hit so the log can state old -> new. <<<",
        enabled ? "ARMED" : "DISABLED", kRegName[reg], static_cast<long>(offset),
        value, value);
}

void WatchExecSetFieldCapture(int32_t rcxOffset, int bytes)
{
    if (bytes < 0) { bytes = 0; }
    if (bytes > 96) { bytes = 96; }
    InterlockedExchange(&g_execFieldOffset, rcxOffset);
    InterlockedExchange(&g_execFieldBytes, bytes);
    LOG("watchexec: synchronous field capture %s - [rcx%+ld], %d byte(s), read "
        "INSIDE the handler so the values belong to the trapped hit rather than "
        "to whenever the poller next looks.",
        bytes ? "ARMED" : "DISABLED", static_cast<long>(rcxOffset), bytes);
}

void WatchExecSetStackFloatOffset(int32_t rbpOffset)
{
    InterlockedExchange(&g_execStackFloatOffset, rbpOffset);
}

void WatchExecSetStackUseRsp(bool useRsp)
{
    InterlockedExchange(&g_execStackUseRsp, useRsp ? 1 : 0);
}

// ===========================================================================
// CLIENT 1 - the second execution watch, on DR0. See watchwrite.h for why it
// exists and what it deliberately does not have.
// ===========================================================================

bool WatchExec2Active()
{
    return InterlockedCompareExchange(&g_exec2Active, 0, 0) != 0;
}

void WatchExec2Stop()
{
    SayOrphans();
    if (!WatchExec2Active()) {
        return;
    }
    InterlockedExchange(&g_exec2Active, 0);
    const uintptr_t a = g_exec2Addr;   // review I4: this call's address, read once
    ArmAll(a, false, kArmExecDr0);
    ArmSelf(a, false, kArmExecDr0);
    OrphanReleased(a);   // U-26: after the disarm pass
    LOG("watchexec2: stopped (was watching execution at %p, %lu fires)",
        reinterpret_cast<void*>(a),
        static_cast<unsigned long>(g_exec2Total));
    g_exec2Addr = 0;
}

bool WatchExec2Start(uintptr_t addr)
{
    EnsureCs();
    if (!addr) {
        LOG("watchexec2: REFUSING TO ARM - null address.");
        return false;
    }
    // [LOADBEARING: buildrva watches] same as WatchExecStart: an ALLOWLISTED engine / gamedll address is moved to its
    // EGL rva on the Epic build; Steam and unknown builds pass unchanged.
    {
        bool moved = false;
        const uintptr_t steamAddr = addr;
        addr = ::dlvr::buildrva::Abs(addr, &moved);
        // [PROBE: BUILDRVA] gate=none (one line per arm of a translated address, only on the EGL build)  site=HEADSET  log-only=yes
        if (moved) {
            LOG("watchexec2: [BUILDRVA] EGL build - exec watch %p moved to %p (allowlisted table entry)",
                reinterpret_cast<void*>(steamAddr), reinterpret_cast<void*>(addr));
        }
    }

    // >>> THE MUTUAL EXCLUSION. DR0 cannot be a data watch and an execution
    // breakpoint at the same time, and a silent overwrite here is exactly the
    // bug the one-slot rule exists for. Refuse, loudly, and name the holder.
    if (WatchWritesActive()) {
        LOG("watchexec2: REFUSING TO ARM at %p - the DATA WATCH holds DR0, and "
            "DR0 cannot be a data watch and an execution breakpoint at once. "
            "Stop the data watch first (WatchWritesStop), or arm this client "
            "before it. NOTHING WAS ARMED - this is a refusal, not a silent "
            "no-op.", reinterpret_cast<void*>(addr));
        return false;
    }
    if (WatchExec2Active() && g_exec2Addr != addr) {
        LOG("watchexec2: REFUSING TO ARM at %p - client 1 is already watching "
            "%p. One address per client; stop it first.",
            reinterpret_cast<void*>(addr), reinterpret_cast<void*>(g_exec2Addr));
        return false;
    }

    WatchExec2Stop();
    ComputeSelfRange();
    {
        Guard lk;
        for (int i = 0; i < kExec2Max; ++i) { g_exec2Hits[i] = ExecHit{}; }
        g_exec2Count = 0;
    }
    InterlockedExchange(&g_exec2Total, 0);
    InterlockedExchange(&g_exec2Expired, 0);
    g_exec2Addr = addr;

    if (!EnsureVeh()) {
        LOG("watchexec2: REFUSING - could not install the vectored handler.");
        g_exec2Addr = 0;
        return false;
    }
    // Set active BEFORE arming: the first trap can land while ArmAll is still
    // walking threads, and the handler's stale-DR0 self-heal would clear a live
    // breakpoint if it could not see that client 1 owns it.
    InterlockedExchange(&g_exec2Active, 1);

    InterlockedExchange(&g_everDr0, 1);
    OrphanArmed(addr);   // U-26: BEFORE arming - a trap can land while ArmAll walks (this call's own address, review I4)
    const int n = ArmAll(addr, true, kArmExecDr0);
    const bool selfArmed = ArmSelf(addr, true, kArmExecDr0);
    if (n <= 0 && !selfArmed) {
        OrphanReleased(addr);
        InterlockedExchange(&g_exec2Active, 0);
        g_exec2Addr = 0;
        LOG("watchexec2: REFUSING - could not arm any thread at %p.",
            reinterpret_cast<void*>(addr));
        return false;
    }
    LOG("watchexec2: watching execution at %p on %d threads (+ caller: %s) "
        "- CLIENT 1, on DR0. Client 0 (DR1) is unaffected and they now coexist.",
        reinterpret_cast<void*>(g_exec2Addr), n, selfArmed ? "yes" : "no");
    return true;
}

int WatchExec2Get(ExecHit* out, int max)
{
    if (!g_csReady || !out || max <= 0) {
        return 0;
    }
    Guard lk;
    const int n = (g_exec2Count < max) ? static_cast<int>(g_exec2Count) : max;
    for (int i = 0; i < n; ++i) {
        out[i] = g_exec2Hits[i];
    }
    return n;
}

void WatchExec2Stats(unsigned long* fires, bool* expired)
{
    if (fires) {
        *fires = static_cast<unsigned long>(
            InterlockedCompareExchange(&g_exec2Total, 0, 0));
    }
    if (expired) {
        *expired = InterlockedCompareExchange(&g_exec2Expired, 0, 0) != 0;
    }
}

bool WatchExecStart(uintptr_t addr)
{
    EnsureCs();
    WatchExecStop();
    if (!addr) {
        return false;
    }
    // [LOADBEARING: buildrva watches] on the Epic build an exec watch on a Steam gamedll / engine rva lands mid-
    // instruction and never fires (boneprobe's CorrectItemPosition capture: 0 hits on every attempt, EGL run
    // 2026-10-18) - an ALLOWLISTED address is moved to its EGL rva here. Steam and unknown builds pass unchanged.
    bool moved = false;
    const uintptr_t steamAddr = addr;
    addr = ::dlvr::buildrva::Abs(addr, &moved);
    // [PROBE: BUILDRVA] gate=none (one line per arm of a translated address, only on the EGL build)  site=HEADSET  log-only=yes
    if (moved) {
        LOG("watchexec: [BUILDRVA] EGL build - exec watch %p moved to %p (allowlisted table entry)",
            reinterpret_cast<void*>(steamAddr), reinterpret_cast<void*>(addr));
    }
    ComputeSelfRange();

    {
        Guard lk;
        g_execHits.clear();
    }
    InterlockedExchange(&g_execTotal, 0);
    InterlockedExchange(&g_execExpired, 0);
    InterlockedExchange(&g_execRefusedNew, 0);
    InterlockedExchange(&g_execArmedThreads, 0);
    // Byte-exact, unlike the 4-byte-rounded data watch above - an execution
    // breakpoint's LEN field is required to be zero.
    g_execAddr = addr;

    if (!EnsureVeh()) {
        strncpy_s(g_execStatus, sizeof(g_execStatus), "could not install the handler",
                  _TRUNCATE);
        return false;
    }
    InterlockedExchange(&g_everDr1, 1);
    OrphanArmed(addr);   // U-26: BEFORE arming - a trap can land while ArmAll walks (this call's own address, review I4)
    const int n = ArmAll(addr, true, kArmExecDr1);
    const bool selfArmed = ArmSelf(addr, true, kArmExecDr1);
    InterlockedExchange(&g_execArmedThreads, static_cast<LONG>(n));
    g_execActive = (n > 0) || selfArmed;
    if (!g_execActive) { OrphanReleased(addr); }
    _snprintf_s(g_execStatus, sizeof(g_execStatus), _TRUNCATE,
                g_execActive ? "watching execution at %p on %d threads (+ caller: %s)"
                             : "could not arm any thread",
                reinterpret_cast<void*>(g_execAddr), n, selfArmed ? "yes" : "no");
    LOG("watchexec: %s", g_execStatus);
    if (!g_execActive) {
        MaybeRemoveVeh();
    }
    return g_execActive;
}

void WatchExecStop()
{
    SayOrphans();
    const uintptr_t a = g_execAddr;   // review I4: this call's address, read once
    if (a) {
        ArmAll(a, false, kArmExecDr1);
        ArmSelf(a, false, kArmExecDr1);
        OrphanReleased(a);   // U-26: after the disarm pass
    }
    g_execActive = false;
    g_execAddr = 0;
    strncpy_s(g_execStatus, sizeof(g_execStatus), "off", _TRUNCATE);
    MaybeRemoveVeh();
}

void WatchExecSayOrphans() { SayOrphans(); }

uintptr_t WatchExecAddress() { return g_execAddr; }
const char* WatchExecStatus() { return g_execStatus; }

int WatchExecCount()
{
    if (!g_csReady) {
        return 0;
    }
    Guard lk;
    return static_cast<int>(g_execHits.size());
}

int WatchExecGet(ExecHit* out, int max)
{
    if (!g_csReady || !out || max <= 0) {
        return 0;
    }
    Guard lk;
    const int n = static_cast<int>(g_execHits.size()) < max
                      ? static_cast<int>(g_execHits.size())
                      : max;
    for (int i = 0; i < n; ++i) {
        out[i] = g_execHits[i];
    }
    return n;
}

// ===================== THE REGISTER WRITE-BACK =====================

void WatchExecSetRegWrite(int reg, uint64_t value, uint64_t mask, bool enabled)
{
    if (reg < 0 || reg > 3) {
        // Not silently clamped to RCX. A caller that asks for a register this
        // does not have has a bug, and turning that into a write somewhere else
        // is the worst available outcome.
        LOG("watchexec: regwrite REFUSED - reg=%d is not 0..3 (RCX/RDX/R8/R9). "
            "NOTHING is armed.", reg);
        InterlockedExchange(&g_regWriteEnabled, 0);
        return;
    }
    InterlockedExchange(&g_regWriteReg, static_cast<LONG>(reg));
    InterlockedExchange64(&g_regWriteValue, static_cast<LONG64>(value));
    InterlockedExchange64(&g_regWriteMask, static_cast<LONG64>(mask));
    InterlockedExchange(&g_regWriteEnabled, enabled ? 1 : 0);
    if (enabled) {
        // Say out loud which of the two filters is still unset, because an armed
        // write that silently refuses every hit looks exactly like a site that
        // never executes.
        const LONG gn = InterlockedCompareExchange(&g_regGateCount, 0, 0);
        const uint64_t hi = static_cast<uint64_t>(g_regWriteHi);
        LOG("watchexec: regwrite ARMED reg=%d value=0x%llX mask=0x%llX | gate "
            "entries=%ld bound=[0x%llX..0x%llX]%s%s%s",
            reg, static_cast<unsigned long long>(value),
            static_cast<unsigned long long>(mask), gn,
            static_cast<unsigned long long>(g_regWriteLo),
            static_cast<unsigned long long>(hi),
            gn == 0 ? "  >>> GATE LIST EMPTY - EVERY WRITE WILL BE REFUSED <<<" : "",
            hi == 0 ? "  >>> BOUND UNSET - EVERY WRITE WILL BE REFUSED <<<" : "",
            mask == 0 ? "  >>> MASK IS 0 - NOTHING WOULD BE WRITTEN <<<" : "");
    } else {
        LOG("watchexec: regwrite DISARMED (reg=%d) - observe only", reg);
    }
}

void WatchExecRegWriteGateByReturn(const uintptr_t* allowedReturns, int count)
{
    EnsureCs();
    Guard lk;
    if (count > kRegWriteGateMax) {
        LOG("watchexec: regwrite gate TRUNCATED - %d addresses offered, room for "
            "%d. The census that produced them is larger than the gate; the "
            "write would cover only part of it.", count, kRegWriteGateMax);
        count = kRegWriteGateMax;
    }
    if (count < 0) { count = 0; }
    for (int i = 0; i < count; ++i) {
        g_regGate[i] = allowedReturns ? allowedReturns[i] : 0;
    }
    g_regGateCount = count;
    LOG("watchexec: regwrite gate set - %d return address(es)%s", count,
        count == 0 ? "  >>> EMPTY: NOTHING WILL BE WRITTEN <<<" : "");
}

void WatchExecSetRegWriteBound(uint64_t lo, uint64_t hi)
{
    InterlockedExchange64(&g_regWriteLo, static_cast<LONG64>(lo));
    InterlockedExchange64(&g_regWriteHi, static_cast<LONG64>(hi));
    LOG("watchexec: regwrite bound = [0x%llX .. 0x%llX]%s",
        static_cast<unsigned long long>(lo),
        static_cast<unsigned long long>(hi),
        hi == 0 ? "  >>> hi == 0 REFUSES EVERY WRITE <<<" : "");
}

void WatchExecRegWriteStats(unsigned long* applied, unsigned long* gatedOut,
                            unsigned long* refused, uint64_t* lastOrig)
{
    if (applied) {
        *applied = static_cast<unsigned long>(
            InterlockedCompareExchange(&g_regWriteApplied, 0, 0));
    }
    if (gatedOut) {
        *gatedOut = static_cast<unsigned long>(
            InterlockedCompareExchange(&g_regWriteGatedOut, 0, 0));
    }
    if (refused) {
        *refused = static_cast<unsigned long>(
            InterlockedCompareExchange(&g_regWriteRefused, 0, 0));
    }
    if (lastOrig) {
        *lastOrig = static_cast<uint64_t>(
            InterlockedCompareExchange64(&g_regWriteOrigLast, 0, 0));
    }
}

// ===================== THE RETURN-ADDRESS CENSUS =====================

void WatchExecReturnCensusEnable(bool on)
{
    EnsureCs();
    InterlockedExchange(&g_retCensusOn, on ? 1 : 0);
    LOG("watchexec: return census %s%s", on ? "ON" : "off",
        on ? "  - the FULL per-hit capture is suppressed while this is on, so "
             "an empty hit list means THIS, not an absent site"
           : "");
}

void WatchExecReturnCensusReset()
{
    EnsureCs();
    Guard lk;
    for (int i = 0; i < kReturnCensusMax; ++i) {
        g_retCensus[i] = ReturnHit{};
    }
    g_retCensusCount = 0;
    InterlockedExchange(&g_retCensusTotal, 0);
    InterlockedExchange(&g_retCensusRefused, 0);
}

int WatchExecReturnCensusGet(ReturnHit* out, int max)
{
    if (!g_csReady || !out || max <= 0) {
        return 0;
    }
    Guard lk;
    const LONG have = g_retCensusCount;
    int n = static_cast<int>(have);
    if (n > max) { n = max; }
    if (n > kReturnCensusMax) { n = kReturnCensusMax; }
    for (int i = 0; i < n; ++i) {
        out[i] = g_retCensus[i];
    }
    return n;
}

void WatchExecReturnCensusStats(unsigned long* total, unsigned long* distinct,
                                unsigned long* refusedNew)
{
    if (total) {
        *total = static_cast<unsigned long>(
            InterlockedCompareExchange(&g_retCensusTotal, 0, 0));
    }
    if (distinct) {
        *distinct = static_cast<unsigned long>(
            InterlockedCompareExchange(&g_retCensusCount, 0, 0));
    }
    if (refusedNew) {
        *refusedNew = static_cast<unsigned long>(
            InterlockedCompareExchange(&g_retCensusRefused, 0, 0));
    }
}

}  // namespace dlvr
