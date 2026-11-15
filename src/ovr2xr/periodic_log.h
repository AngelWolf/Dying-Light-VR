// diag_periodic_logs - the switch for the layer's TIMER-DRIVEN report lines. 2026-10-01.
//
// Jay: "Anything that is currently not required to be enabled, any probe that is going
// unused, anything that's unnecessarily using resources whilst playing ... is to be turned
// off for the time being until it's required again." Chose option A: one switch, shipped
// OFF, that silences the periodic report lines and nothing else.
//
// WHY: with every probe off, the log still ran 109.6 lines/s (backups/
// 20261001_headside_test_run1, 122 s after the rig latched) - the rig's 2 s report pass,
// the 1 Hz per-side lines and a handful of other timers. INV-249 timed LogTo at ~0.16 ms
// per line (every line opens and closes the file) and found logging IS the once-a-second
// tick stall: ~17.5 ms/s at that rate, worst second ~37 ms.
//
// WHAT IT TOUCHES: only the LOG statement at each site, by changing `LOG(` to
// `LOG_PERIODIC(`. Nothing around it moves - every counter reset, every throttle stamp,
// every engine read and ResolveAccessors() itself run exactly as before. It is a do/while
// statement, so it is safe as the body of an unbraced if/else.
// The sites were chosen MECHANICALLY: the timer-driven ones that EMITTED in a session with
// no probe switched on. A line behind its own diag_ switch is NOT converted - that would
// make a probe someone switches on stay silent unless this switch were on too.
// KEPT as plain LOG on purpose: `boneblend: hooks -` (the rig liveness / IK-solve count,
// INV-138), the frame-pacing report (INV-8's quotable fps instrument), every install,
// ARMED, FIRST and latch line ([SHOULDER] latched, INV-2), every event line, and every
// error / refusal outside the report pass.
//
// TURN IT ON (diag_periodic_logs=1) for any run whose question reads a converted line -
// docs/PROBE_DEPS.md lists them. It is a live setting: the overlay toggle takes effect at
// once.
#pragma once

namespace dlvr {
// [DEP: DiagPeriodicLogs] declared and documented in boneblend.cpp (the definition).
#if defined(DLVR_BETA_LEAN)   // compiled out of BETA (docs/BETA_CENSUS.md): every LOG_PERIODIC body is dropped
inline constexpr bool DiagPeriodicLogs = false;
#else
extern bool DiagPeriodicLogs;
#endif
}  // namespace dlvr

// Expands to THIS file's LOG(), so each translation unit keeps writing to the log it
// already writes to.
#define LOG_PERIODIC(...)                                                                    \
    do {                                                                                     \
        if (::dlvr::DiagPeriodicLogs) {                                                      \
            LOG(__VA_ARGS__);                                                                \
        }                                                                                    \
    } while (0)

// LOG_PERIODIC that still prints a report's FIRST sample (`first` true) - for timer lines whose first window is the
// "it is running" proof. 2026-11-08 log trim (Jay: 5 s summaries "Out of BETA, kept in DEV"). Format = argument 1
// (tools/logdecode.py knows it).
#define LOG_WINDOW(first, ...)                                                               \
    do {                                                                                     \
        if ((first) || ::dlvr::DiagPeriodicLogs) {                                           \
            LOG(__VA_ARGS__);                                                                \
        }                                                                                    \
    } while (0)
