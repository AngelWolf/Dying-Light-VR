// Minimal logger. Writes next to the calling DLL so the log always lands in the
// game folder regardless of the game's working directory.
//
// Truncated once per launch, on the first line written by that process. It used
// to append, so every run piled into the same file and only install.ps1 ever
// cleared it - which means reading a log involved working out where the previous
// session ended, and getting that wrong means diagnosing a run that already
// happened. A launch now starts with an empty file and a banner saying which
// build wrote it.
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dlvr {

// Serialises the write sequence below (since 2026-11-08 the file stays open; the lock covers the open-once + write).
//
// Without this, lines are LOST - not interleaved, lost. Proven on 2026-09-08:
// the bone-blend probe's install line is written unconditionally, in code order,
// between two lines that both appear in the log, and it was absent from the file
// entirely (grep -c returned zero) on two separate runs, with a line from
// another thread sitting where it should have been. It cost a wrong inference on
// the spot - "the inner hook never installed", when it had.
//
// That matters well beyond one probe. This project draws a lot of conclusions of
// the form "the log shows no X, therefore X never happened", and every one of
// those made while several subsystems were logging concurrently is weaker than
// it looks. Worth re-reading the diag_bone_dump no-hit result and the
// GetBaseShootDir caller that never fired with that in mind.
//
// FirstWriteThisRun's own bookkeeping was racy too, so it is inside the lock.
//
// A critical section rather than a named mutex: the two DLLs that log write to
// different files (bootstrap -> dlvr_bootstrap.log, layer -> dlvr_ovr2xr.log),
// so the race that actually exists is between threads within one module, and
// this is far cheaper. Deliberately never deleted - logging can happen during
// DLL unload, and a destroyed section would be worse than a leaked one.
struct LogCriticalSection {
    CRITICAL_SECTION cs;
    LogCriticalSection() { InitializeCriticalSection(&cs); }
};

inline CRITICAL_SECTION& LogLock()
{
    // Function-local static: thread-safe initialisation is guaranteed by the
    // standard and implemented by MSVC 2015+.
    static LogCriticalSection lock;
    return lock.cs;
}

// True the first time this process writes to `filename`, false afterwards.
// Per-DLL, which is what we want: two modules log here and each truncates only
// the files it writes. Callers must hold LogLock().
// [LOGCOST] - what LogTo itself costs, per module (inline statics are per DLL). Every call is timed
// from entry to exit, INCLUDING the wait for LogLock, and the lock wait is kept separately, so a
// once-a-second report can say whether logging is where a stall goes. QPC ticks; the reader
// converts. The window maxima are reset by the reader with InterlockedExchange64.
struct LogCost {
    volatile LONG64 calls;
    volatile LONG64 totalQpc;
    volatile LONG64 maxQpc;
    volatile LONG64 maxWaitQpc;
};
inline LogCost& LogCostStats()
{
    static LogCost c = {};
    return c;
}
inline void LogCostMax(volatile LONG64& slot, LONG64 v)
{
    LONG64 cur = slot;
    while (v > cur) {
        const LONG64 prev = InterlockedCompareExchange64(&slot, v, cur);
        if (prev == cur) { break; }
        cur = prev;
    }
}
inline void LogCostNote(LONG64 t0, LONG64 w0, LONG64 w1)
{
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    LogCost& c = LogCostStats();
    InterlockedIncrement64(&c.calls);
    InterlockedAdd64(&c.totalQpc, t1.QuadPart - t0);
    LogCostMax(c.maxQpc, t1.QuadPart - t0);
    LogCostMax(c.maxWaitQpc, w1 - w0);
}

inline bool FirstWriteThisRun(const char* filename)
{
    static const char* seen[8] = {};
    static int count = 0;
    for (int i = 0; i < count; ++i) {
        if (strcmp(seen[i], filename) == 0) {
            return false;
        }
    }
    if (count < 8) {
        seen[count++] = filename;
    }
    return true;
}

// >>> ⚠ THE PRINTF PROMOTION IN CMakeLists CANNOT SEE LOG() CALLS, AND THIS
// ANNOTATION DOES NOT CHANGE THAT. MEASURED, NOT ASSUMED. <<<
//
// `src/CMakeLists.txt:31-35` promotes /we4473 /we4474 /we4476 /we4477 /we4313 -
// the whole printf family - to ERRORS. **Not one of them can fire on a LOG()
// call.** MSVC only consumes _Printf_format_string_ under /analyze, which this
// build does not enable.
//
// VERIFIED 2026-09-22 by deliberately compiling `item_local_test_m=%lld` against
// a float argument WITH this annotation in place: **it built clean, exit 0.**
// The annotation is kept because it is correct and costs nothing, and it would
// arm those five errors the day /analyze is switched on - but as of today it
// enforces NOTHING.
//
// WHAT THIS COST: [LOCALSET] printed its OK counter as 4599075939685498880 - the
// IEEE-754 bit pattern of 0.3, i.e. item_local_test_m arriving where a %lld
// belonged - because the counters sat early in the format and were passed last.
// The build was clean, the run held ZERO data, and it cost a flat launch.
//
// UNTIL /analyze IS ON, FORMAT/ARGUMENT AGREEMENT IS A MANUAL REVIEW ITEM. Count
// the specifiers against the arguments before shipping a new LOG line.
//
// LogWriteV is LogTo's body: format, then lock, truncate-per-launch, banner, timestamp, [LOGCOST].
// >>> KEEP-OPEN WRITER (2026-11-08, Jay: "Yes, keep file open"). <<< Until then every line did GetModuleFileNameA (the
// loader lock), fopen, vfprintf, fclose - ~0.16 ms a line on Jay's machine (INV-249), far more on a slow disk or with an
// antivirus scan on every close, on the game / render threads, under LogLock. Now:
//  - the module's directory + filename are read ONCE (LogModulePathGet), lock-free and OUTSIDE LogLock, as before - never
//    the loader lock while holding LogLock (a DllMain thread holding the loader lock may be waiting for LogLock);
//  - each file is opened ONCE per launch with CreateFileA (first open truncates, as before) and kept open. A Win32 handle,
//    NOT a CRT FILE*: the CRT closes its own streams during DLL unload, and a line logged after that would write through
//    a closed FILE*. The OS closes the handle at process exit;
//  - each line is ONE WriteFile of the whole formatted line. No CRT buffer, so nothing waits for a flush: a game crash
//    keeps every line already written (only an OS crash can lose the cache);
//  - shared read / write / delete, so a log can be read while the game runs;
//  - text-mode output is reproduced byte for byte: every '\n' (the line end and any inside a message) becomes "\r\n".
// [LOGCOST] still times every call.
struct LogModulePath {
    char dir[MAX_PATH];      // ends in '\\'
    char module[MAX_PATH];   // this DLL's file name, for the banner
};

// The cached path once one thread has published it; until then the caller's `scratch`, filled here. nullptr = unreadable.
inline const LogModulePath* LogModulePathGet(LogModulePath& scratch)
{
    static LogModulePath s_cached = {};
    static volatile LONG s_state = 0;   // 0 none, 1 being written, 2 ready
    if (s_state == 2) { return &s_cached; }
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&LogModulePathGet), &self);
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { return nullptr; }
    char* slash = strrchr(path, '\\');
    if (!slash) { return nullptr; }
    strncpy_s(scratch.module, sizeof(scratch.module), slash + 1, _TRUNCATE);
    slash[1] = '\0';
    strncpy_s(scratch.dir, sizeof(scratch.dir), path, _TRUNCATE);
    if (InterlockedCompareExchange(&s_state, 1, 0) == 0) {
        s_cached = scratch;
        InterlockedExchange(&s_state, 2);
    }
    return &scratch;
}

// The open handle for `filename` (caller holds LogLock). `first` = this launch's first write to it (the file was just
// created empty). `closeAfter` = more than 8 file names in this DLL: not cached, the caller closes it after the line.
struct LogOpenSlot {
    const char* name;
    HANDLE h;
};
inline HANDLE LogFileLocked(const char* dir, const char* filename, bool* first, bool* closeAfter)
{
    static LogOpenSlot s_open[8] = {};
    static int s_count = 0;
    *first = false;
    *closeAfter = false;
    int slot = -1;
    for (int i = 0; i < s_count; ++i) {
        if (strcmp(s_open[i].name, filename) == 0) {
            if (s_open[i].h != INVALID_HANDLE_VALUE && s_open[i].h) { return s_open[i].h; }
            slot = i;   // an earlier open failed: retry it (append, as the old per-line open did)
            break;
        }
    }
    char path[MAX_PATH];
    if (strlen(dir) + strlen(filename) >= MAX_PATH) { return INVALID_HANDLE_VALUE; }
    strcpy_s(path, sizeof(path), dir);
    strcat_s(path, sizeof(path), filename);
    *first = FirstWriteThisRun(filename);
    const HANDLE h = CreateFileA(path, *first ? GENERIC_WRITE : FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 *first ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return INVALID_HANDLE_VALUE; }
    if (!*first) { SetFilePointer(h, 0, nullptr, FILE_END); }
    if (slot < 0 && s_count < 8) {
        slot = s_count++;
        s_open[slot].name = filename;
    }
    if (slot >= 0) { s_open[slot].h = h; }
    else { *closeAfter = true; }
    return h;
}

// Append printf output to buf[*len], growing it on the heap when the stack buffer is too small. false = out of memory.
inline bool LogAppendV(char*& buf, size_t& cap, size_t& len, bool& heap, const char* fmt, va_list ap)
{
    va_list probe;
    va_copy(probe, ap);
    const int need = _vscprintf(fmt, probe);
    va_end(probe);
    if (need < 0) { return false; }
    if (len + static_cast<size_t>(need) + 1 > cap) {
        const size_t ncap = len + static_cast<size_t>(need) + 256;
        char* nb = static_cast<char*>(malloc(ncap));
        if (!nb) { return false; }
        memcpy(nb, buf, len);
        if (heap) { free(buf); }
        buf = nb; cap = ncap; heap = true;
    }
    vsnprintf(buf + len, cap - len, fmt, ap);
    len += static_cast<size_t>(need);
    return true;
}
inline bool LogAppend(char*& buf, size_t& cap, size_t& len, bool& heap, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const bool ok = LogAppendV(buf, cap, len, heap, fmt, ap);
    va_end(ap);
    return ok;
}

// One WriteFile of `buf` with every '\n' written as "\r\n" (what the old text-mode FILE* produced).
inline void LogWriteCrlf(HANDLE h, const char* buf, size_t len)
{
    char out[4096];
    size_t o = 0;
    DWORD wrote = 0;
    for (size_t i = 0; i < len; ++i) {
        if (o + 2 > sizeof(out)) { WriteFile(h, out, static_cast<DWORD>(o), &wrote, nullptr); o = 0; }
        if (buf[i] == '\n') { out[o++] = '\r'; }
        out[o++] = buf[i];
    }
    if (o) { WriteFile(h, out, static_cast<DWORD>(o), &wrote, nullptr); }
}

inline void LogWriteV(const char* filename, const char* fmt, va_list ap)
{
    LARGE_INTEGER lcT0;
    QueryPerformanceCounter(&lcT0);

    LogModulePath scratch;
    const LogModulePath* mp = LogModulePathGet(scratch);
    if (!mp) { return; }

    // Format OUTSIDE the lock: only the write is serialised.
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stackBuf[2048];
    char* buf = stackBuf;
    size_t cap = sizeof(stackBuf), len = 0;
    bool heap = false;
    bool ok = LogAppend(buf, cap, len, heap, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    if (ok) { ok = LogAppendV(buf, cap, len, heap, fmt, ap); }
    if (ok) { ok = LogAppend(buf, cap, len, heap, "\n"); }
    if (!ok) {
        if (heap) { free(buf); }
        LogCostNote(lcT0.QuadPart, lcT0.QuadPart, lcT0.QuadPart);
        return;
    }

    LARGE_INTEGER lcW0, lcW1;
    QueryPerformanceCounter(&lcW0);
    EnterCriticalSection(&LogLock());
    QueryPerformanceCounter(&lcW1);

    bool first = false, closeAfter = false;
    const HANDLE h = LogFileLocked(mp->dir, filename, &first, &closeAfter);
    if (h == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&LogLock());
        if (heap) { free(buf); }
        LogCostNote(lcT0.QuadPart, lcW0.QuadPart, lcW1.QuadPart);
        return;
    }

    if (first) {
        char banner[512];
        const int bn = _snprintf_s(banner, sizeof(banner), _TRUNCATE,
                                   "==== dlvr  %04d-%02d-%02d %02d:%02d:%02d  pid %lu  %s  "
                                   "built " __DATE__ " " __TIME__ " ====\n",
                                   t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                                   GetCurrentProcessId(), mp->module);
        if (bn > 0) { LogWriteCrlf(h, banner, static_cast<size_t>(bn)); }
    }
    LogWriteCrlf(h, buf, len);
    if (closeAfter) { CloseHandle(h); }

    LeaveCriticalSection(&LogLock());
    if (heap) { free(buf); }
    LogCostNote(lcT0.QuadPart, lcW0.QuadPart, lcW1.QuadPart);
}

inline void LogTo(const char* filename, _Printf_format_string_ const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LogWriteV(filename, fmt, ap);
    va_end(ap);
}

}  // namespace dlvr
