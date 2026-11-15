// START-UP HANG WATCH - A DIAGNOSTIC BUILD. 2026-10-21 (Jay: a separate build for one tester's diagnostics ->
// the hang-dump build -> "Yes, that's fine"). BETA-FINDINGS-2026-10-19 section 14: a tester's launches stop inside the engine's
// InitSDL right after its "SelectKeyboard" line, only with our bootstrap present; nothing on file names the call the main thread
// blocks in (the game writes no dump for a hang; the stall sampler starts at the first frame). The process was seen alive 6.4 s
// past SelectKeyboard, so this fires at 3 s.
//
// WHAT IT DOES (built with -DDLVR_HANG_WATCH=ON only; otherwise Start / OnEngineLine are empty):
//   * The engine's "SelectKeyboard" line ARMS it (that thread + the time); "InitSDL successed" ends it for the session.
//   * 3 s after arming with no success it FIRES once, into dlvr\hang_watch.log: every thread's stack (suspended one at a time,
//     nothing written while one is suspended) with its CPU time, a minidump dlvr\hang_<time>.dmp, the module list (path, base,
//     size), the raw-input devices (keyboards, mice, HID - VID / PID are in the names), then the arming thread's stack and CPU
//     time every 0.5 s for 5 s, then every thread's name.
//   * A launch that passes InitSDL writes three lines (on / armed / passed) and nothing else.
//   * dlvr\hang_watch_test.txt (Jay's PC only, never shipped) = fire as soon as it is armed: the positive control.
// SAFETY: the hung thread may hold the loader lock, so after start the watchdog never calls anything that takes it: its own file
// writer (CreateFile / WriteFile, path and handle made at start), addresses named from a module table it read itself, dbghelp loaded
// at start. Each step that MIGHT need the loader lock or a heap lock (module re-read, minidump, device list) runs on a helper thread
// with a timeout; a step that times out is logged as BLOCKED - itself an answer. Thread names (they allocate) come last.
#include "hangwatch.h"

#if defined(DLVR_HANG_WATCH)
#include <tlhelp32.h>
#include <psapi.h>
#include <dbghelp.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_bootstrap.log", __VA_ARGS__)
#endif

namespace dlvr {
namespace hangwatch {

#if !defined(DLVR_HANG_WATCH)

void Start(HMODULE) {}
void OnEngineLine(const char*) {}

#else

namespace {

constexpr DWORD kFireAfterMs  = 3000;
constexpr DWORD kNeverArmedMs = 180000;   // no SelectKeyboard in 3 min of the watch: stop watching
constexpr int   kSamples      = 10;
constexpr DWORD kSampleMs     = 500;
constexpr int   kMaxFrames    = 32;
constexpr int   kMaxMods      = 512;
constexpr int   kMaxThreads   = 512;

volatile LONG   g_armed = 0;
volatile LONG   g_passed = 0;
volatile LONG   g_armTid = 0;
volatile LONG64 g_armQpc = 0;
volatile LONG64 g_passQpc = 0;
double          g_tickMs = 0.0;
bool            g_test = false;
char            g_dlvrDir[MAX_PATH] = {};
char            g_realXInput[MAX_PATH] = "(none loaded)";
HANDLE          g_log = INVALID_HANDLE_VALUE;

typedef BOOL(WINAPI* MiniDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                 PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpFn g_miniDump = nullptr;
typedef HRESULT(WINAPI* GetThreadDescriptionFn)(HANDLE, PWSTR*);
GetThreadDescriptionFn g_getDesc = nullptr;

struct Mod { DWORD64 base; DWORD64 size; char name[64]; };
Mod  g_mods[kMaxMods];                 // the watchdog's naming table (start, then refreshed at arm)
int  g_nMods = 0;
Mod  g_jobMods[kMaxMods];              // filled by the module helper
char g_jobPaths[kMaxMods][MAX_PATH];
int  g_nJobMods = 0;
char g_devText[16384];                 // filled by the device helper
char g_dumpPath[MAX_PATH] = {};
volatile LONG g_dumpOk = -1;           // -1 not run, 0 failed, 1 written
volatile LONG g_dumpErr = 0;
volatile LONG g_moduleJobStuck = 0;    // a module helper timed out: never start another (it may still fill the buffers)

double NowMs()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * g_tickMs;
}

// The watchdog's own writer: no CRT FILE, no loader lock. Only the watchdog thread calls it.
void W(const char* fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) { return; }
    char buf[4096];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    if (n < 0) { n = 0; }
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf_s(buf + n, sizeof(buf) - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m < 0) { m = static_cast<int>(strlen(buf + n)); }
    n += m;
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD wr = 0;
    WriteFile(g_log, buf, static_cast<DWORD>(n), &wr, nullptr);
    FlushFileBuffers(g_log);
}

void Name(DWORD64 a, char* out, size_t cap)
{
    for (int i = 0; i < g_nMods; ++i) {
        if (a >= g_mods[i].base && a < g_mods[i].base + g_mods[i].size) {
            _snprintf_s(out, cap, _TRUNCATE, "%s+%llX", g_mods[i].name, static_cast<unsigned long long>(a - g_mods[i].base));
            return;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "?%llX", static_cast<unsigned long long>(a));
}

// Read every loaded module into g_jobMods / g_jobPaths. Helper thread (or the watchdog at its own start).
void ReadModules()
{
    HMODULE h[kMaxMods];
    DWORD need = 0;
    g_nJobMods = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), h, sizeof(h), &need)) { return; }
    int n = static_cast<int>(need / sizeof(HMODULE));
    if (n > kMaxMods) { n = kMaxMods; }
    for (int i = 0; i < n; ++i) {
        MODULEINFO mi = {};
        K32GetModuleInformation(GetCurrentProcess(), h[i], &mi, sizeof(mi));
        g_jobPaths[i][0] = 0;
        K32GetModuleFileNameExA(GetCurrentProcess(), h[i], g_jobPaths[i], MAX_PATH);
        const char* b = strrchr(g_jobPaths[i], '\\');
        strncpy_s(g_jobMods[i].name, sizeof(g_jobMods[i].name), b ? b + 1 : g_jobPaths[i], _TRUNCATE);
        g_jobMods[i].base = reinterpret_cast<DWORD64>(mi.lpBaseOfDll);
        g_jobMods[i].size = mi.SizeOfImage;
    }
    g_nJobMods = n;
}

void TakeJobModules()
{
    memcpy(g_mods, g_jobMods, sizeof(Mod) * g_nJobMods);
    g_nMods = g_nJobMods;
}

void ReadDevices()
{
    size_t at = 0;
    g_devText[0] = 0;
    UINT n = 0;
    if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || n == 0) {
        _snprintf_s(g_devText, sizeof(g_devText), _TRUNCATE, "(no raw-input device list: count %u, error %lu)", n, GetLastError());
        return;
    }
    RAWINPUTDEVICELIST list[128];
    if (n > 128) { n = 128; }
    const UINT got = GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
    for (UINT i = 0; i < got && i < 128 && at + 400 < sizeof(g_devText); ++i) {
        char name[300] = {};
        UINT cch = sizeof(name);
        GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICENAME, name, &cch);
        RID_DEVICE_INFO info = {};
        info.cbSize = sizeof(info);
        UINT cb = sizeof(info);
        GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICEINFO, &info, &cb);
        const char* type = list[i].dwType == RIM_TYPEKEYBOARD ? "keyboard" : (list[i].dwType == RIM_TYPEMOUSE ? "mouse" : "hid");
        int w = 0;
        if (list[i].dwType == RIM_TYPEHID) {
            w = _snprintf_s(g_devText + at, sizeof(g_devText) - at, _TRUNCATE, "  device %u: %s usage page 0x%X usage 0x%X | %s\n", i,
                            type, info.hid.usUsagePage, info.hid.usUsage, name);
        } else {
            w = _snprintf_s(g_devText + at, sizeof(g_devText) - at, _TRUNCATE, "  device %u: %s | %s\n", i, type, name);
        }
        if (w < 0) { break; }
        at += static_cast<size_t>(w);
    }
}

void WriteDump()
{
    if (!g_miniDump) { InterlockedExchange(&g_dumpOk, 0); InterlockedExchange(&g_dumpErr, -1); return; }
    HANDLE f = CreateFileA(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        InterlockedExchange(&g_dumpErr, static_cast<LONG>(GetLastError()));
        InterlockedExchange(&g_dumpOk, 0);
        return;
    }
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                          MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithFullMemoryInfo |
                                                          MiniDumpWithHandleData | MiniDumpWithProcessThreadData);
    const BOOL ok = g_miniDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, nullptr, nullptr, nullptr);
    InterlockedExchange(&g_dumpErr, ok ? 0 : static_cast<LONG>(GetLastError()));
    CloseHandle(f);
    InterlockedExchange(&g_dumpOk, ok ? 1 : 0);
}

enum JobKind { kJobModules, kJobDevices, kJobDump };

DWORD WINAPI Helper(LPVOID p)
{
    switch (static_cast<JobKind>(reinterpret_cast<INT_PTR>(p))) {
    case kJobModules: ReadModules(); break;
    case kJobDevices: ReadDevices(); break;
    case kJobDump: WriteDump(); break;
    }
    return 0;
}

bool RunWithTimeout(JobKind kind, DWORD ms, const char* what)
{
    HANDLE h = CreateThread(nullptr, 0, Helper, reinterpret_cast<LPVOID>(static_cast<INT_PTR>(kind)), 0, nullptr);
    if (!h) {
        W("%s: could not start a helper thread (error %lu)", what, GetLastError());
        return false;
    }
    const DWORD r = WaitForSingleObject(h, ms);
    CloseHandle(h);
    if (r != WAIT_OBJECT_0) {
        W("%s: BLOCKED - not finished after %lu ms. A new thread cannot run, or that call waits, while another thread holds the "
          "loader lock (or the lock that call needs).", what, ms);
        return false;
    }
    return true;
}

// Unwind a captured context into frames[]. Reads the suspended thread's stack; SEH-guarded.
int Unwind(CONTEXT* c, DWORD64* frames)
{
    int n = 0;
    __try {
        while (n < kMaxFrames && c->Rip) {
            frames[n++] = c->Rip;
            const DWORD64 prevRsp = c->Rsp;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c->Rip, &base, nullptr);
            if (!f) {
                c->Rip = *reinterpret_cast<const DWORD64*>(c->Rsp);
                c->Rsp += 8;
            } else {
                PVOID hd = nullptr;
                DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, f, c, &hd, &est, nullptr);
            }
            if (c->Rsp <= prevRsp) { break; }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

// Suspend, read, unwind, resume - nothing written or allocated in between.
int Capture(HANDLE h, DWORD64* frames)
{
    if (SuspendThread(h) == static_cast<DWORD>(-1)) { return -1; }
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    int n = -2;
    if (GetThreadContext(h, &ctx)) { n = Unwind(&ctx, frames); }
    ResumeThread(h);
    return n;
}

void FramesText(const DWORD64* f, int n, char* out, size_t cap)
{
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < n && at + 120 < cap; ++i) {
        char m[96];
        Name(f[i], m, sizeof(m));
        const int w = _snprintf_s(out + at, cap - at, _TRUNCATE, "%s%s", i ? " < " : "", m);
        if (w < 0) { break; }
        at += static_cast<size_t>(w);
    }
    if (n <= 0) { _snprintf_s(out, cap, _TRUNCATE, "(not read: %d)", n); }
}

void Fire(DWORD self)
{
    const DWORD armTid = static_cast<DWORD>(g_armTid);
    W("FIRED (%s) - no 'InitSDL successed' %.0f ms after the engine's SelectKeyboard line (thread %lu)",
      g_test ? "TEST - dlvr\\hang_watch_test.txt present" : "a start-up HANG", NowMs() - static_cast<double>(g_armQpc) * g_tickMs,
      armTid);
    W("real XInput the proxy loaded: %s", g_realXInput);

    // 1. every thread's stack, one at a time
    DWORD tids[kMaxThreads];
    int nt = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        const DWORD pid = GetCurrentProcessId();
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != self && nt < kMaxThreads) { tids[nt++] = te.th32ThreadID; }
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
    W("THREADS: %d (the arming thread is marked <<)", nt);
    for (int i = 0; i < nt; ++i) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tids[i]);
        if (!h) { W("  thread %lu: cannot open (error %lu)", tids[i], GetLastError()); continue; }
        ULONG64 cyc = 0;
        QueryThreadCycleTime(h, &cyc);
        DWORD64 fr[kMaxFrames];
        const int n = Capture(h, fr);
        CloseHandle(h);
        char txt[3200];
        FramesText(fr, n, txt, sizeof(txt));
        W("  thread %lu%s cycles %llu: %s", tids[i], tids[i] == armTid ? " <<" : "", static_cast<unsigned long long>(cyc), txt);
    }

    // 2. the minidump
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snprintf_s(g_dumpPath, sizeof(g_dumpPath), _TRUNCATE, "%s\\hang_%04d%02d%02d_%02d%02d%02d.dmp", g_dlvrDir, t.wYear, t.wMonth,
                t.wDay, t.wHour, t.wMinute, t.wSecond);
    W("minidump: writing %s", g_dumpPath);
    if (RunWithTimeout(kJobDump, 30000, "minidump")) {
        W("minidump: %s (error %ld)", g_dumpOk == 1 ? "WRITTEN" : "FAILED", static_cast<long>(g_dumpErr));
    }

    // 3. modules (refreshed now if possible, else the table read when armed)
    if (!g_moduleJobStuck && RunWithTimeout(kJobModules, 3000, "module list")) {
        W("MODULES: %d loaded now", g_nJobMods);
        for (int i = 0; i < g_nJobMods; ++i) {
            W("  %016llX %9llu  %s", static_cast<unsigned long long>(g_jobMods[i].base),
              static_cast<unsigned long long>(g_jobMods[i].size), g_jobPaths[i]);
        }
        TakeJobModules();
    } else {
        InterlockedExchange(&g_moduleJobStuck, 1);
        W("MODULES (the table read when armed - the module list could not be read now): %d", g_nMods);
        for (int i = 0; i < g_nMods; ++i) {
            W("  %016llX %9llu  %s", static_cast<unsigned long long>(g_mods[i].base),
              static_cast<unsigned long long>(g_mods[i].size), g_mods[i].name);
        }
    }

    // 4. input devices
    if (RunWithTimeout(kJobDevices, 3000, "raw-input devices")) {
        W("RAW-INPUT DEVICES:");
        char* p = g_devText;
        while (*p) {
            char* nl = strchr(p, '\n');
            if (nl) { *nl = 0; }
            W("%s", p);
            if (!nl) { break; }
            p = nl + 1;
        }
    }

    // 5. the arming thread, sampled
    HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, armTid);
    if (!ht) {
        W("arming thread %lu: cannot open (error %lu) - it may have exited", armTid, GetLastError());
    } else {
        ULONG64 prev = 0;
        QueryThreadCycleTime(ht, &prev);
        for (int s = 1; s <= kSamples; ++s) {
            Sleep(kSampleMs);
            ULONG64 cyc = 0;
            QueryThreadCycleTime(ht, &cyc);
            DWORD64 fr[kMaxFrames];
            const int n = Capture(ht, fr);
            char txt[3200];
            FramesText(fr, n, txt, sizeof(txt));
            W("sample %d/%d thread %lu, cycles +%llu in %lu ms, passed InitSDL %s: %s", s, kSamples, armTid,
              static_cast<unsigned long long>(cyc - prev), kSampleMs, g_passed ? "YES" : "no", txt);
            prev = cyc;
        }
        CloseHandle(ht);
    }

    // 6. thread names (they allocate - last)
    if (g_getDesc) {
        for (int i = 0; i < nt; ++i) {
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tids[i]);
            if (!h) { continue; }
            PWSTR wname = nullptr;
            char name[128] = {};
            if (SUCCEEDED(g_getDesc(h, &wname)) && wname) {
                WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof(name), nullptr, nullptr);
                LocalFree(wname);
            }
            CloseHandle(h);
            if (name[0]) { W("  name of thread %lu: %s", tids[i], name); }
        }
    }
    W("DONE - send the whole dlvr folder (this log and the .dmp) and the game folder's dlvr_*.log files");
}

DWORD WINAPI Watch(LPVOID)
{
    const DWORD self = GetCurrentThreadId();
    HMODULE dbg = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    g_miniDump = dbg ? reinterpret_cast<MiniDumpFn>(GetProcAddress(dbg, "MiniDumpWriteDump")) : nullptr;
    g_getDesc = reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription"));
    ReadModules();
    TakeJobModules();
    W("hang watch ON (watchdog thread %lu) | fires %lu ms after the engine's SelectKeyboard line unless InitSDL passes | test "
      "marker %s | minidump %s | modules at start %d | real XInput: %s",
      self, kFireAfterMs, g_test ? "PRESENT - fires as soon as armed" : "absent", g_miniDump ? "available" : "NOT AVAILABLE",
      g_nMods, g_realXInput);
    const double start = NowMs();
    bool armSaid = false;
    for (;;) {
        Sleep(50);
        if (g_passed && !g_test) {
            W("InitSDL passed %.0f ms after SelectKeyboard - no hang; the watch ends",
              static_cast<double>(g_passQpc - g_armQpc) * g_tickMs);
            return 0;
        }
        if (!g_armed) {
            if (NowMs() - start > kNeverArmedMs) {
                W("no SelectKeyboard line in %lu ms - the watch ends without arming", kNeverArmedMs);
                return 0;
            }
            continue;
        }
        if (!armSaid) {
            armSaid = true;
            W("armed: SelectKeyboard logged on thread %lu", static_cast<DWORD>(g_armTid));
            if (RunWithTimeout(kJobModules, 2000, "module list at arming")) {
                TakeJobModules();
            } else {
                InterlockedExchange(&g_moduleJobStuck, 1);
            }
        }
        if (g_test || NowMs() - static_cast<double>(g_armQpc) * g_tickMs >= kFireAfterMs) {
            Fire(self);
            return 0;
        }
    }
}

}  // namespace

void Start(HMODULE realXInput)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_tickMs = f.QuadPart > 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
    if (realXInput) { GetModuleFileNameA(realXInput, g_realXInput, MAX_PATH); }
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (char* slash = strrchr(exe, '\\')) { *slash = 0; }
    _snprintf_s(g_dlvrDir, sizeof(g_dlvrDir), _TRUNCATE, "%s\\dlvr", exe);
    CreateDirectoryA(g_dlvrDir, nullptr);
    char path[MAX_PATH] = {};
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\hang_watch_test.txt", g_dlvrDir);
    g_test = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\hang_watch.log", g_dlvrDir);
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE th = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
    // [PROBE: HANGWATCH] gate=compile option DLVR_HANG_WATCH (diagnostic build)  site=FLAT  log-only=yes
    LOG("hang watch: %s - writes dlvr\\hang_watch.log%s", th ? "started" : "COULD NOT START its thread",
        g_log == INVALID_HANDLE_VALUE ? " (COULD NOT OPEN it)" : "");
    if (th) { CloseHandle(th); }
}

void OnEngineLine(const char* line)
{
    if (g_passed || !line) { return; }
    if (!g_armed && strstr(line, "SelectKeyboard")) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        InterlockedExchange(&g_armTid, static_cast<LONG>(GetCurrentThreadId()));
        InterlockedExchange64(&g_armQpc, t.QuadPart);
        InterlockedExchange(&g_armed, 1);
        return;
    }
    if (g_armed && strstr(line, "InitSDL successed")) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        InterlockedExchange64(&g_passQpc, t.QuadPart);
        InterlockedExchange(&g_passed, 1);
    }
}

#endif

}  // namespace hangwatch
}  // namespace dlvr
