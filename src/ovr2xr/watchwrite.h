// "Find what writes to this address", using a hardware data breakpoint.
//
// Writing the camera matrix from outside is a race we keep losing in different
// ways: too early and the engine recomputes it, continuously and the renderer
// sees it change mid-frame, which is what the graphical glitching is. There is
// no LibOVR entry point at the moment that matters - between the engine
// finishing its camera and the renderer reading it - so no amount of moving our
// write around will land it reliably.
//
// The way out is to stop guessing where that moment is and ask the CPU. A debug
// register set to break on writes to the matrix reports the exact instruction
// that stores it. That address, minus the module base, is the engine function
// that decides where the camera points - which is the thing worth hooking, and
// the thing all the scanning was only ever approximating.
//
// This is the same question Cheat Engine's "find out what writes to this
// address" answers, done in-process so it can be pointed at an address the
// overlay already knows.
#pragma once

#include <cstdint>

namespace dlvr {

struct WriteHit {
    uintptr_t rip;        // instruction AFTER the store - data breakpoints trap
    uintptr_t moduleBase;
    char module[64];
    uint64_t count;
};

// x86 data breakpoints cannot watch reads alone - only "write" or "read-or-
// write". Call this before WatchWritesStart() to arm read-or-write instead of
// the write-only default, for when the writer is already known (e.g. the
// engine's own camera-copy hook) and the interesting question is who else
// touches the address - a reader that never appears as a writer.
void WatchWritesSetIncludeReads(bool includeReads);

// Arms DR0 on every thread in the process to break on a 4-byte write to `addr`.
// The address is rounded down to a 4-byte boundary, which is the granularity a
// debug register can watch. False if the breakpoint could not be set.
bool WatchWritesStart(uintptr_t addr);
void WatchWritesStop();
bool WatchWritesActive();

int WatchWritesGet(WriteHit* out, int max);
const char* WatchWritesStatus();

// A second, independent tool: an EXECUTION breakpoint (DR1, coexists with the
// DR0 data watch above with no conflict) at one exact instruction address,
// logging the value of RCX every time execution reaches it - for a value
// that only ever exists in a register at that one point in the code and is
// never written to memory anywhere, which WatchWritesStart() has no way to
// see. Traps BEFORE the instruction runs, so whatever an earlier instruction
// already put in RCX is still there to read.
struct ExecHit {
    uintptr_t rcxValue;
    uintptr_t rdxValue;
    // Optional: 3 floats read from [base + stackFloatOffset] at the exact
    // moment this hit fired, if WatchExecSetStackFloatOffset() set a nonzero
    // offset. For a value that lives on the CALLER's stack (an output buffer
    // passed by address, e.g. `lea rdx,[rbp-0x48]` before a call) - RCX/RDX
    // are volatile and not reliably still valid after the callee runs, and
    // the stack slot itself gets reused by other calls within a frame or two,
    // so this has to be captured synchronously right here, not inspected
    // later. `base` is RBP by default (callee-saved, so still correct here
    // even when arming right after a `call` returns) - but NOT every function
    // establishes an RBP frame at all (confirmed 2026-09-02: ShotgunShootDir's
    // caller uses a plain `sub rsp, N` prologue and never touches RBP, so
    // reading [rbp+offset] there silently returned an unrelated ancestor
    // frame's leftover data instead of failing outright). WatchExecSetStackUseRsp()
    // switches the base to RSP for exactly that case - a `call`/`ret` pair is
    // stack-neutral, so RSP right after the call returns is identical to RSP
    // right before it, matching a `lea rdx,[rsp+N]` seen before the call.
    float stackVec[3];
    bool stackVecValid;
    // The return address read from [RSP] at the moment this hit fired. Only
    // meaningful when the watch is armed on a function's real ENTRY point,
    // where [RSP] still holds the address the CALL pushed - it then names the
    // exact calling instruction, which is the only way to identify the caller
    // of anything reached through a vtable, where no static xref exists to
    // trace back from. 0 if the read faulted.
    uintptr_t returnAddr;
    uint64_t count;

    // THE FULL x64 ARGUMENT SET, added 2026-09-10 for the damage work.
    //
    // RCX/RDX above cover a 2-argument call. HealthModule::AddDamageToBody
    // (gamedll RVA 0x2A2830) takes TWELVE, and Hex-Rays contradicts itself about
    // their types across its two overrides (a5 is float in one, unsigned int in
    // the other) - so the arguments have to be read LIVE rather than inferred.
    //
    // Under the Microsoft x64 convention the register is chosen by POSITION, and
    // a float at position N travels in XMMn rather than the integer register:
    //
    //   pos1 -> RCX    pos2 -> RDX    pos3 -> R8 / XMM2    pos4 -> R9 / XMM3
    //   pos5.. -> stack. At a function's ENTRY [RSP] holds the return address
    //             and 0x20 of shadow space follows it, so arg5 is at [RSP+0x28],
    //             arg6 at [RSP+0x30], and so on.
    //
    // All of this was already sitting in the CONTEXT the handler holds; only the
    // struct was too small to carry it.
    uintptr_t r8Value;
    uintptr_t r9Value;
    // Positions 3 and 4 reinterpreted as floats, because that is where a float
    // argument at those positions actually travels. Captured unconditionally:
    // taking both the integer and the float view of the same slot costs nothing
    // and removes the need to know the signature in advance, which is the whole
    // reason for doing this live.
    float xmm2f;
    float xmm3f;
    // Stack arguments 5..12, both views again - the qword as an integer and its
    // low dword as a float.
    uint64_t stackArgs[8];
    float stackArgsF[8];
    bool stackArgsValid;

    // ---- SYNCHRONOUS FIELD CAPTURE, added 2026-09-15 ----
    //
    // >>> WHY THIS EXISTS, AND IT COST A RUN TO LEARN. Everything the damage
    // work reads OFF the object - hit location, direction vector, part, cut
    // types, the hit counter - was being read by the polling thread up to
    // 100 ms AFTER the breakpoint fired. Only RCX and RDX were ever captured
    // at the instruction. On 2026-09-15 21:28 four entries flushed in one poll
    // printed FOUR DIFFERENT BONES against ONE identical LOCATION, VECTOR and
    // m_HitCount, because those three were a single late snapshot shared
    // between them. That is INV-5 exactly: a field sampled at the wrong point
    // is not a weak measurement of that field, it is a measurement of a
    // different quantity. <<<
    //
    // This captures a contiguous blob at [RCX + offset] INSIDE the handler, at
    // the instant the instruction is trapped, so the fields belong to THIS hit.
    // Read in 8-byte guarded chunks; a fault part-way sets fieldBlobValid
    // false rather than leaving a half-filled buffer looking like data.
    uint8_t  fieldBlob[96];
    uint32_t fieldBlobBytes;   // how many bytes are actually valid
    bool     fieldBlobValid;

    // ---- REGISTER WRITE-BACK: what the register held, and what we put there ----
    // Same old->new discipline as pokeOrig below, for the same reason: "we wrote
    // 77" without "it was 3" cannot distinguish a working override from a no-op.
    // regValid is true only when the write passed BOTH the return-address gate
    // and the value bound AND ran.
    uint64_t regOrig;
    uint64_t regWritten;
    bool     regValid;

    // ---- POKE: the ORIGINAL dword at the poke site, before we changed it ----
    // >>> THIS IS THE FIRST THING IN THIS PROJECT THAT WRITES INTO THE GAME'S
    // OWN MEMORY RATHER THAN READING IT. Everything else here observes. <<<
    // Captured so a log line can state old -> new, because "we wrote 7" without
    // "it used to be 3" cannot distinguish a working override from a no-op.
    uint32_t pokeOrig;
    bool     pokeValid;   // the read-back succeeded AND the write was attempted
};

// Arm a WRITE at an exec breakpoint: replace the dword at [<reg> + offset] with
// `value` at the moment the trapped instruction is reached, before it runs.
//
//   reg: 0 = RCX, 1 = RDX, 2 = R8, 3 = R9   (x64 integer argument order)
//
// enabled == false disables it, which is the DEFAULT and the safe state. The
// original dword is recorded in ExecHit::pokeOrig so the log can show old->new.
//
// >>> USE DELIBERATELY. This mutates game state on a hot path; a wrong offset
// corrupts whatever it lands on. It is guarded (a faulting write is swallowed
// and pokeValid stays false) but a SUCCESSFUL write to the wrong place is not
// something a guard can catch. <<<
void WatchExecSetPoke(int reg, int32_t offset, uint32_t value, bool enabled);

// Arm a synchronous read of `bytes` (<= 96) at [RCX + rcxOffset] on every
// subsequent exec-breakpoint hit. bytes == 0 disables it. Sticky across
// WatchExecStart() calls, like the stack float offset above.
//
// The blob is REFRESHED on a repeat hit of the same (rcx, rdx, returnAddr)
// entry, for the same reason stackVec is: the point is watching a field change
// blow to blow, not preserving the first one forever.
void WatchExecSetFieldCapture(int32_t rcxOffset, int bytes);

// 0 (default) disables the stack-relative float read. Nonzero arms it for
// every subsequent WatchExecStart() until changed again - e.g. -0x48 to read
// the vec3 at [rbp-0x48], matching a `lea rdx,[rbp-0x48]` seen a few
// instructions before the site being armed.
void WatchExecSetStackFloatOffset(int32_t rbpOffset);

// false (default) reads relative to RBP; true reads relative to RSP instead -
// needed for a callee whose prologue never sets up an RBP frame at all (see
// the ExecHit::stackVec comment above). Sticky across WatchExecStart() calls
// the same way the offset is - set explicitly before arming, every time, so
// switching between diagnostic targets never silently reuses the wrong base.
void WatchExecSetStackUseRsp(bool useRsp);

// The exec watch SELF-DISARMS after this many hits: the handler clears DR7/DR1
// and sets the expired flag. 400 is the default and is right for a bounded
// census, where a runaway site would otherwise trap forever.
//
// >>> IT IS FATAL FOR A POKE CLIENT, AND IT KILLED THE 2026-09-16 10:52 RUN. <<<
// The element override armed at 10:52:39, burned 400 fires within seconds of the
// first fight, disarmed itself, and said NOTHING for the remaining 3m 44s - so
// the run's behavioural observations covered an override that was no longer
// running, and there was no number anywhere to reveal that.
//
// 0 == UNLIMITED. Sticky across WatchExecStart() like the other setters, and
// DEFAULTS TO 400, so no existing client changes behaviour.
void WatchExecSetMaxHits(long maxHits);

// ---------------------------------------------------------------------------
// THE SECOND EXEC CLIENT. Everything above this is CLIENT 0, on DR1, unchanged -
// its seven existing callers do not move.
//
// WHY IT EXISTS: VR-directed melee needs the bone probe AND the melee trap armed
// AT THE SAME TIME, and they cannot share DR1. Proven by a run on 2026-09-16,
// not inferred - the trap logged `REFUSING TO ARM - the bone probe holds the
// single process-wide WatchExec slot` every 5 s while the rig held it, and
// neither produced usable output.
//
// WHICH REGISTER: DR0. The CPU has four; aimhook owns DR2/DR3 with its own
// programming, client 0 owns DR1, so DR0 is the only one left. It is normally
// WatchWrite's data-watch register, and the minidumps show `Dr0=0` in the melee
// configuration - measured in two crash dumps, not assumed - so it is free
// there.
//
// >>> THE MUTUAL EXCLUSION IS THE POINT OF THIS API, NOT A CAVEAT. <<<
// DR0 cannot be a data watch and an execution breakpoint at once. Whichever of
// WatchWritesStart / WatchExec2Start is second REFUSES, returns false, and SAYS
// WHY. It never silently overwrites - silent slot theft is the exact bug the
// one-slot rule was written for, and it has already cost runs.
//
// WHAT CLIENT 1 DELIBERATELY DOES NOT HAVE: the poke map, field capture, stack
// float capture, RSP/RBP selection. Client 0 keeps all of that. Client 1 is a
// plain execution watch that records RCX/RDX/R8 per distinct call site, because
// that is what the second client actually needs. Symmetry we do not need is
// surface area we would have to keep correct in a VEH handler.
// ---------------------------------------------------------------------------

// Arm the second exec client on DR0. Returns false (and logs why) if the data
// watch holds DR0, or if it is already armed at a different address.
bool WatchExec2Start(uintptr_t addr);
void WatchExec2Stop();
bool WatchExec2Active();

// Copies out up to `max` distinct call sites seen by client 1, newest counts
// included. Same COPY-AND-AGGREGATE contract as WatchExecGet (INV-59): the
// table aggregates by RCX, so a returned entry is a running total and a real
// increment - not one entry per firing.
int  WatchExec2Get(ExecHit* out, int max);

// fires = how many times the breakpoint hit, unconditionally. expired = whether
// it self-disarmed on the hit cap. Same shape as WatchExecStats.
void WatchExec2Stats(unsigned long* fires, bool* expired);

// ===== THE PER-MODEL POKE MAP =====
//
// WatchExecSetPoke writes a CONSTANT. That is the wrong shape for melee: D-42
// and INV-72 establish that the same element index is `r_hand` on one zombie
// model and a jacket sleeve with NO BONE on another, so one value cannot mean
// one body part.
//
// This writes a value CHOSEN BY THE OBJECT BEING HIT. At the breakpoint the
// handler reads a key from [reg + keyOffset], looks it up in a small table, and
// writes the matching value to [reg + writeOffset]. For the melee trap the key
// is the model at [R8+0x0] and the target is the element at [R8+0x8].
//
// >>> NO ENGINE CALL HAPPENS IN THE HANDLER. <<< The table is filled OUTSIDE, by
// whoever can call GetElementID safely (the [ELEMDUMP] thread). The handler only
// does a guarded read and a linear scan of at most kPokeMapMax entries. INV-53
// licenses event-rate exec work; it does not license calling into the engine
// from a VEH, and this deliberately does not.
//
// >>> A MISS WRITES NOTHING, AND IS COUNTED. <<< If the model is not in the
// table the original value is left alone. Writing some default would make "we
// hit a model we have no mapping for" indistinguishable from "we redirected it",
// which is the absence-of-output trap this project keeps paying for (INV-6).
// Read the miss count before believing any behavioural result.
// 64, not 16. D-44: at 16 the map filled and later bodies MISSED - measured
// hits=90 misses=66, i.e. 42% of blows wrote nothing. The handler scan is linear
// but runs at EVENT rate (a melee hit), not per frame, and INV-53 measured a
// session-long exec hold at 0 of 600 frames late.
constexpr int kPokeMapMax = 64;

// Add or update one mapping, keyed on the pointer.
//
// >>> CALL THIS EVERY TIME THE OBJECT IS SEEN, NOT ONCE. <<< D-44's second half:
// an instance pointer goes STALE when a body dies and its memory is reused, and
// a recycled pointer would otherwise match an entry describing a different
// model. Re-resolving on every sighting overwrites the stale value, so the
// exposure is one sighting rather than the rest of the session. INV-59 records
// this hazard biting this project already.
//
// When the table is full the OLDEST entry is replaced rather than the new one
// being dropped - a full map must not mean "everything from here on misses".
void WatchExecPokeMapSet(uintptr_t key, unsigned int value);

// Set the per-key SANITY BOUND on the field being written.
//
// >>> THIS IS A FAIL-CLOSED SHAPE CHECK, AND IT IS ALSO THE TEST. <<< INV-67:
// different callers pass differently-shaped records. If a caller's record has a
// valid model pointer at the key offset but something OTHER than the element at
// the write offset, the map would match and write an element index into the
// wrong field. That is the only mechanism visible from this build's change to
// the two crashes on 2026-09-16 (both on the map path; the constant path, which
// writes MORE often, was clean).
//
// Before writing, the handler requires the ORIGINAL value at the write offset to
// be a plausible element for that model: 0 <= orig < bound. A wrong-shaped
// record almost certainly fails that, the write is skipped, and it is COUNTED.
//
// So the rejection count answers the question directly:
//   rejections > 0  -> wrong-shaped records WERE being written into. Cause found.
//   rejections == 0 and it still crashes -> that hypothesis is dead, look elsewhere.
void WatchExecPokeMapSetBound(uintptr_t key, unsigned int bound);

// rejected - matched the map but the original value failed the shape check, so
//            NOTHING was written. Read this before believing any result.
void WatchExecPokeMapStats(unsigned long* hits, unsigned long* misses,
                           int* entries, unsigned long* rejected);

// Arm the map. reg: 0=RCX 1=RDX 2=R8 3=R9. enabled=false returns the poke to
// whatever WatchExecSetPoke last configured. Sticky across WatchExecStart.
void WatchExecPokeMapArm(int reg, int keyOffset, int writeOffset, bool enabled);

// ===================== THE TYPE MAP - D-44's ACTUAL FIX =====================
//
// THE PROBLEM, MEASURED. The map above is keyed on the model POINTER, which
// identifies an INSTANCE. The element index is a property of the model TYPE.
// D-44 recorded that on 2026-09-16 and nobody built the fix; run 4 on
// 2026-09-18 13:53 measured what it costs: 19 models mapped, 9 keys held,
// `PART MAP hits=13 misses=48`. Instances churn faster than a pointer-keyed map
// can track them, so 79% of blows wrote nothing.
//
// THE KEY IS THE ELEMENT COUNT, and D-44 named it: "the element count plus a
// name check identifies the skeleton layout". The name check lives on the dump
// thread, which may call virtuals. The COUNT has to be readable INSIDE the VEH
// handler, where a call is out of the question - so this is only possible
// because of what the count's accessor turned out to be:
//
//   IModelObject::GetElementsNumber(this):          (our own engine, +0x141F60)
//       v1 = *(u64*)(this + 8);
//       return *(u16*)(*(u64*)dl_PickThreadBuffer(v1 + 904) + 40);
//
//   dl_PickThreadBuffer(a1):                        (our own engine, +0x9A520)
//       ... TlsGetValue, may malloc ...
//       return (*tlsValue < 2) ? a1 + 16 : a1 + 32;
//
// The pick is SLOT SELECTION between two fixed offsets 16 bytes apart - it
// cannot be called from a handler (TlsGetValue, malloc), but it does not have
// to be: >>> D-1 MEASURED BOTH SLOTS HOLDING THE SAME CONTAINER POINTER, 54
// samples, ONE distinct value, and called it structural rather than a sampling
// coincidence. <<< So the handler reads slot 0 directly: three plain guarded
// reads, no calls, no locks.
//
// ⚠ AND THAT RELIANCE IS CHECKED RATHER THAN TRUSTED. The dump thread computes
// the count BOTH ways - through the real export and through these three reads -
// and REFUSES to register the type if they disagree, naming both values. If
// D-1's finding ever stops holding, this says so instead of writing an element
// index derived from the wrong container.
//
// Registers count -> value. A second registration of the same count with a
// DIFFERENT value POISONS that count: two skeletons sharing an element count
// but not a layout must write nothing rather than guess. Poisoning is logged by
// the caller and is permanent for the session.
void WatchExecPokeTypeSet(unsigned int count, unsigned int value,
                          unsigned int bound);

// The three offsets above, so watchwrite.cpp never has to know what a model is.
// enabled=false disables the type fallback and leaves the instance map alone.
void WatchExecPokeTypeArm(int ptrOffset, int containerOffset, int countOffset,
                          bool enabled);

// typeHits   - writes that MISSED the instance map, matched on the TYPE, and
//              landed. >>> READ THIS SEPARATELY FROM hits: it is the only way
//              to tell whether the D-44 fix is doing anything. <<<
// poisoned   - counts registered by two different values and therefore refused
void WatchExecPokeTypeStats(unsigned long* typeHits, unsigned long* typeMisses,
                            int* entries, int* poisoned);

// >>> THE POKE ITSELF, WITH ONE BODY AND TWO CALLERS. <<< The VEH handler calls
// it, and so does the inline hook at gamedll+0xC4D920. U-27 asks which
// MECHANISM causes the face-2 crash, and that comparison is only valid if the
// mechanism is the sole difference - two near-identical implementations would
// make it a comparison of implementations instead.
//
// `pbase` is whatever register the contact record arrived in (R8 today), at the
// breakpoint or at the hook, identically. Returns whether a write landed;
// `origOut` receives the value that was there first, because "we wrote 7"
// without "it was 3" cannot tell an override from a no-op.
bool WatchExecPokeApplyAt(uintptr_t pbase, uint32_t* origOut);

// hits   - writes that found a mapping, passed the shape check, and landed
// misses - hits on an object with NO mapping; NOTHING was written for these
// entries- how many mappings are currently loaded

bool WatchExecStart(uintptr_t addr);
void WatchExecStop();
// U-26: log the handler's orphan-trap claims if the count moved (the first call prints it, 0 included). The layer calls this
// from its 600-frame line so a claim is never left unlogged. COST: one interlocked read when nothing changed.
void WatchExecSayOrphans();
uintptr_t WatchExecAddress();

int WatchExecCount();
int WatchExecGet(ExecHit* out, int max);
const char* WatchExecStatus();

// The numbers a caller needs to know whether its own silence means "nothing
// happened" or "the instrument stopped". Every one of these was unavailable on
// 2026-09-16 and that is why that run could not be interpreted.
//
//   fires       - TOTAL handler entries since arming. The denominator. A poke
//                 client's override ran exactly this many times.
//   refusedNew  - distinct (rcx,rdx,ret) keys the 64-entry table REFUSED. Was
//                 silent before; INV-59 records the same silence costing a read.
//   expired     - the hit cap disarmed the watch. Reporting this is the whole
//                 point: it is otherwise indistinguishable from "no more hits".
//   armedThreads- thread count at arming, so drift is visible rather than
//                 inferred (threads created later never carry the breakpoint).
//   liveThreads - thread count NOW, counted the same way ArmAll() enumerates.
//                 liveThreads > armedThreads means threads exist that the
//                 breakpoint was never set on, and any work they do is invisible
//                 to this watch.
//
// Any pointer may be null. Safe to call from a poll loop; the counters are
// interlocked and only liveThreads takes a snapshot, so pass null for it on a
// hot path.
void WatchExecStats(unsigned long* fires, unsigned long* refusedNew,
                    bool* expired, int* armedThreads, int* liveThreads);

// ===========================================================================
// THE REGISTER WRITE-BACK, added 2026-09-17. CLIENT 0 ONLY.
// ===========================================================================
//
// >>> WHY IT EXISTS, AND THE POKE CANNOT DO IT - D-56. <<< WatchExecSetPoke
// writes MEMORY at [reg + offset]. At a fault-class exec trap the instruction
// has NOT run yet, so at SetLastHitBone (INV-103, `mov byte ptr [rcx+0x697], dl`
// then `ret`) the byte at that address still holds the PREVIOUS hit's bone and
// the value we want to change exists only in DL. There is no address to poke.
//
// INV-102 established the mechanism by measurement: a write into the trapped
// thread's CONTEXT reaches the instruction. Proven for RDX (a scalar) and RCX
// (a pointer - the store went to a different object).
//
// >>> THE SAFETY MODEL IS DIFFERENT FROM THE POKE'S, AND THIS IS THE DESIGN
// CONSTRAINT RATHER THAN A CAVEAT. <<< The poke is GUARDED (a faulting write is
// swallowed, pokeValid stays false) and SHAPE-CHECKED (WatchExecPokeMapSetBound,
// INV-67). A register write has no fault to swallow and nothing to check the
// original against: there is no page to be unmapped and no address to be wrong
// about. IT ALWAYS LANDS. So the safety comes from two things that must BOTH be
// configured before anything is written, and each of which REFUSES when it is
// not:
//
//   1. WatchExecRegWriteGateByReturn - the call site filter. INV-104: the melee
//      bone argument lives on an ENGINE-WIDE export reached from 372 call sites
//      in gamedll alone, six of which are melee. Writing on all of them
//      overrides the bone for every system in the game.
//   2. WatchExecSetRegWriteBound  - the value filter.
//
// >>> AN EMPTY GATE LIST REFUSES. IT NEVER MEANS "EVERYTHING". <<< A predicate
// that selects the universe when unconfigured is the absence-of-output trap with
// the sign flipped, and this project has already paid for it once
// (`if (g_colourSdr) { pair test }` making "nothing latched" mean "match
// everything" - INV-6). An unset bound refuses the same way.

// reg: 0=RCX 1=RDX 2=R8 3=R9 - the x64 integer argument order, the same mapping
// WatchExecSetPoke and WatchExecPokeMapArm already use. Nothing else is
// writable: those four are the argument registers, and writing anything else at
// an ENTRY is not an argument override, it is corruption of a live local.
//
// `mask` selects WHICH BYTES are replaced. 0xFF writes the low byte and leaves
// the other seven alone; ~0ull writes the whole register. It is not decoration -
// GetMeshElemFromBoneID takes `unsigned char` and reads DL (INV-104), and a
// 64-bit store into a register whose upper bytes the callee also uses is a
// different act from replacing a byte argument. The mask makes the two
// impossible to confuse and makes the log line say which one happened.
// A mask of 0 writes nothing and is counted as a refusal, not as a success.
//
// enabled == false is the DEFAULT and the safe state.
void WatchExecSetRegWrite(int reg, uint64_t value, uint64_t mask, bool enabled);

// THE CALL-SITE GATE. `allowedReturns` are return addresses as read from [RSP]
// at the trap - i.e. the instruction AFTER the `call`, which is what
// ExecHit::returnAddr already captures and what the census below enumerates.
// count == 0 clears the list, and with the list empty NOTHING is ever written.
//
// Build this list from a MEASURED census, never from a decompile's caller list:
// INV-65 names the six caller FUNCTIONS, and the difference between a function
// and the return address inside it is a `call` instruction nobody has located.
constexpr int kRegWriteGateMax = 32;
void WatchExecRegWriteGateByReturn(const uintptr_t* allowedReturns, int count);

// The value must satisfy lo <= value <= hi or nothing is written and `refused`
// counts it. Never clamped - a bone id outside the model's range is a bug in our
// code, and silently clamping it would hide that behind a plausible result.
// Until this is called, hi == 0 and EVERY write is refused.
void WatchExecSetRegWriteBound(uint64_t lo, uint64_t hi);

//   applied  - writes that passed the gate AND the bound and landed. THE
//              DENOMINATOR for any behavioural claim (INV-69).
//   gatedOut - hits whose return address was not in the list. Nothing written.
//   refused  - gate passed, bound or mask failed. Nothing written.
//   lastOrig - the register's value immediately before the last applied write.
// Any pointer may be null.
void WatchExecRegWriteStats(unsigned long* applied, unsigned long* gatedOut,
                            unsigned long* refused, uint64_t* lastOrig);

// ===========================================================================
// THE RETURN-ADDRESS CENSUS, added 2026-09-17. CLIENT 0 ONLY.
// ===========================================================================
//
// It exists because the gate above has to be built from measurement, and
// WatchExecGet CANNOT produce it: that table aggregates by (rcx, rdx,
// returnAddr), and at GetMeshElemFromBoneID `rcx` is the MODEL - a different
// pointer per body - so 64 entries fill with distinct models within one fight
// and `refusedNew` climbs forever while the handful of distinct call sites stay
// invisible. This aggregates by return address ALONE.
//
// >>> IT ALSO SUPPRESSES THE FULL CAPTURE, DELIBERATELY. <<< While the census is
// on, the handler does RF, the counter, one guarded [RSP] read and a linear scan
// - and skips the stack-argument set, the field capture, the poke and the
// ExecHit table entirely. Two reasons: the site's call rate is `[UNKNOWN]` and
// this is the measurement that decides it (DESIGN_REGWRITE.md section 5), and a
// census does not need twelve stack reads per hit. THE CONSEQUENCE, which must
// be stated wherever the census output is read: there are NO `[BONE HIT]` lines
// during a census, by design - an empty ExecHit list means THIS, not "nothing
// happened".
struct ReturnHit {
    uintptr_t ret;        // [RSP] at the trap = the instruction after the call
    uint64_t  count;
    uintptr_t lastRcx;    // the model, at the most recent hit from this site
    uintptr_t lastRdx;    // the bone id, ditto - the value a write would replace
};

constexpr int kReturnCensusMax = 64;

void WatchExecReturnCensusEnable(bool on);
void WatchExecReturnCensusReset();
int  WatchExecReturnCensusGet(ReturnHit* out, int max);

//   total      - hits seen while the census was on. The denominator.
//   distinct   - how many return addresses are in the table.
//   refusedNew - distinct return addresses the table had no room for. NON-ZERO
//                MEANS THE CENSUS IS INCOMPLETE and the gate list built from it
//                would be too, which is why it is reported rather than counted
//                silently.
void WatchExecReturnCensusStats(unsigned long* total, unsigned long* distinct,
                                unsigned long* refusedNew);

}  // namespace dlvr
