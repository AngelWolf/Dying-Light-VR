#include "rtticam.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {
namespace {

// Offsets inside CCamera, from IBaseCamera's accessors in the macOS build:
//   GetViewMatrix       -> [this+8] + 0x10
//   GetInvCameraMatrix  -> [this+8] + 0x40
//   GetProjectionMatrix -> [this+8] + 0x70
// where [this+8] is the CCamera. SetPosition writes 0x4C / 0x5C / 0x6C, which
// is the fourth float of each row at +0x40 - so that matrix is camera->world
// with the position in its translation column.
// The macOS values, used as the starting guess and as the fallback.
constexpr size_t kMacViewMatrix = 0x10;
constexpr size_t kMacCamMatrix = 0x40;
constexpr size_t kMacSpacing = kMacCamMatrix - kMacViewMatrix;

// What we actually measured in this process.
size_t g_camOffset = kMacCamMatrix;
size_t g_spacing = kMacSpacing;
bool g_layoutMeasured = false;

struct Cam {
    uintptr_t object;
    XrQuaternionf last;
    bool moving;
    float moveDeg;      // decayed rotation rate, so the live camera stands out
    const char* cls;
};

uintptr_t g_vtable = 0;
char g_class[64] = "CRendererCamera";
}  // namespace

bool RttiOnlyRealCameras = true;
bool RttiSortByActivity = true;   // DEFAULT ON 2026-10-01 (Tier D: Jay's live value)

namespace {
std::vector<Cam> g_cams;
char g_status[256] = "not scanned";
const char* kEngine = "engine_x64_rwdi.dll";

bool SafeRead(uintptr_t addr, void* out, size_t bytes)
{
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

XrQuaternionf MatToQuat(const float m[9])
{
    const float t = m[0] + m[4] + m[8];
    XrQuaternionf q;
    if (t > 0.0f) {
        float s = sqrtf(t + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m[7] - m[5]) / s;
        q.y = (m[2] - m[6]) / s;
        q.z = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q.w = (m[7] - m[5]) / s;
        q.x = 0.25f * s;
        q.y = (m[1] + m[3]) / s;
        q.z = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q.w = (m[2] - m[6]) / s;
        q.x = (m[1] + m[3]) / s;
        q.y = 0.25f * s;
        q.z = (m[5] + m[7]) / s;
    } else {
        float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q.w = (m[3] - m[1]) / s;
        q.x = (m[2] + m[6]) / s;
        q.y = (m[5] + m[7]) / s;
        q.z = 0.25f * s;
    }
    const float n = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n > 1e-6f) {
        q.w /= n; q.x /= n; q.y /= n; q.z /= n;
    }
    return q;
}

float AngleBetween(const XrQuaternionf& a, const XrQuaternionf& b)
{
    float d = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    if (d > 1.0f) {
        d = 1.0f;
    }
    return 2.0f * acosf(d) * 57.2957795f;
}

// Reads the mtx34 at +0x40 as a rotation plus a position.
bool ReadCamMatrix(uintptr_t obj, XrQuaternionf& q, float& px, float& py, float& pz)
{
    float raw[12];
    if (!SafeRead(obj + g_camOffset, raw, sizeof(raw))) {
        return false;
    }
    float m[9];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            m[r * 3 + c] = raw[r * 4 + c];
        }
    }
    // Sanity: it must still be a rotation. A CCamera that has been destroyed
    // leaves its vtable pointer behind in freed memory for a while.
    for (int r = 0; r < 3; ++r) {
        const float len = m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] +
                          m[r * 3 + 2] * m[r * 3 + 2];
        if (len < 0.9f || len > 1.1f) {
            return false;
        }
    }
    q = MatToQuat(m);
    px = raw[3];
    py = raw[7];
    pz = raw[11];
    return true;
}

void ToEuler(const XrQuaternionf& q, float& yaw, float& pitch, float& roll)
{
    const float deg = 57.2957795f;
    const float sinp = 2.0f * (q.w * q.x - q.y * q.z);
    pitch = (fabsf(sinp) >= 1.0f) ? copysignf(90.0f, sinp) : asinf(sinp) * deg;
    yaw = atan2f(2.0f * (q.w * q.y + q.x * q.z),
                 1.0f - 2.0f * (q.x * q.x + q.y * q.y)) * deg;
    roll = atan2f(2.0f * (q.w * q.z + q.x * q.y),
                  1.0f - 2.0f * (q.x * q.x + q.z * q.z)) * deg;
}

const uint8_t* FindBytes(const uint8_t* hay, size_t hayLen,
                         const void* needle, size_t nLen)
{
    if (nLen == 0 || hayLen < nLen) {
        return nullptr;
    }
    const uint8_t* n = static_cast<const uint8_t*>(needle);
    const uint8_t* end = hay + hayLen - nLen;
    for (const uint8_t* p = hay; p <= end; ++p) {
        if (p[0] == n[0] && memcmp(p, n, nLen) == 0) {
            return p;
        }
    }
    return nullptr;
}

// Is there a valid rotation at this offset, with its inverse `spacing` later?
bool LooksLikeCameraPair(uintptr_t obj, size_t off, size_t spacing)
{
    float a[12], b[12];
    if (!SafeRead(obj + off, a, sizeof(a)) ||
        !SafeRead(obj + off + spacing, b, sizeof(b))) {
        return false;
    }
    float ma[9], mb[9];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            ma[r * 3 + c] = a[r * 4 + c];
            mb[r * 3 + c] = b[r * 4 + c];
        }
    }
    for (int r = 0; r < 3; ++r) {
        const float la = ma[r * 3] * ma[r * 3] + ma[r * 3 + 1] * ma[r * 3 + 1] +
                         ma[r * 3 + 2] * ma[r * 3 + 2];
        const float lb = mb[r * 3] * mb[r * 3] + mb[r * 3 + 1] * mb[r * 3 + 1] +
                         mb[r * 3 + 2] * mb[r * 3 + 2];
        if (la < 0.98f || la > 1.02f || lb < 0.98f || lb > 1.02f) {
            return false;
        }
    }
    // Inverse of a rotation is its transpose.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (fabsf(ma[r * 3 + c] - mb[c * 3 + r]) > 0.01f) {
                return false;
            }
        }
    }
    // Reject identity. The test above is satisfied trivially by any two
    // identity matrices, because identity is its own transpose - so a pair of
    // untouched, zero-initialised matrices anywhere in the object votes just as
    // strongly as the real thing. Objects are full of those, which is how the
    // vote could settle on an offset holding a constant, and constants are
    // exactly what a table of numbers that never change looks like.
    const float offDiag = fabsf(ma[1]) + fabsf(ma[2]) + fabsf(ma[3]) +
                          fabsf(ma[5]) + fabsf(ma[6]) + fabsf(ma[7]);
    return offDiag > 0.02f;
}

// Finds the offset by looking for that pair inside the objects we have, and
// taking whichever offset the most of them agree on.
void MeasureLayout(const std::vector<Cam>& cams)
{
    if (cams.empty()) {
        return;
    }
    struct Vote { size_t off, spacing; int n; };
    std::vector<Vote> votes;

    const size_t spacings[] = { 0x30, 0x40 };
    for (size_t i = 0; i < cams.size() && i < 32; ++i) {
        for (size_t off = 0; off <= 0x300; off += 4) {
            for (size_t sp : spacings) {
                if (!LooksLikeCameraPair(cams[i].object, off, sp)) {
                    continue;
                }
                bool seen = false;
                for (auto& v : votes) {
                    if (v.off == off && v.spacing == sp) {
                        ++v.n;
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    votes.push_back(Vote{ off, sp, 1 });
                }
            }
        }
    }
    if (votes.empty()) {
        LOG("rtti: no matrix pair found inside any CCamera - keeping the macOS "
            "offset 0x%zX", kMacCamMatrix);
        return;
    }
    const Vote* best = &votes[0];
    for (const auto& v : votes) {
        if (v.n > best->n) {
            best = &v;
        }
    }
    // The pair is (view, camera) in that order, so the SECOND of the two is the
    // camera->world matrix with the position in its translation column.
    g_camOffset = best->off + best->spacing;
    g_spacing = best->spacing;
    g_layoutMeasured = true;
    LOG("rtti: measured layout - view at +0x%zX, camera at +0x%zX, spacing 0x%zX "
        "(%d of %zu objects agree; macOS said +0x%zX)",
        best->off, g_camOffset, g_spacing, best->n, cams.size(), kMacCamMatrix);
}

}  // namespace

int RttiMatrixOffset() { return static_cast<int>(g_camOffset); }
int RttiMatrixSpacing() { return static_cast<int>(g_spacing); }

void RttiSetClass(const char* name)
{
    if (!name || !*name) {
        return;
    }
    if (strcmp(name, g_class) == 0) {
        return;
    }
    strncpy_s(g_class, sizeof(g_class), name, _TRUNCATE);
    g_vtable = 0;            // a different class means a different vtable
    g_cams.clear();
    g_layoutMeasured = false;
    LOG("rtti: now looking for %s", g_class);
}

namespace {

// The vtable for one named class. Cached per name, because "both" needs two.
uintptr_t VtableFor(const char* cls)
{
    static struct { char name[64]; uintptr_t vt; } cache[4] = {};
    for (auto& e : cache) {
        if (e.vt && strcmp(e.name, cls) == 0) {
            return e.vt;
        }
    }
    const uintptr_t vt = RttiVtableOf(cls);
    if (vt) {
        for (auto& e : cache) {
            if (!e.vt) {
                strncpy_s(e.name, sizeof(e.name), cls, _TRUNCATE);
                e.vt = vt;
                break;
            }
        }
    }
    return vt;
}

}  // namespace

uintptr_t RttiFindCCameraVtable()
{
    return VtableFor(g_class);
}

// Searched module by module because RTTI-visible classes are not all in the
// same DLL: CCamera and friends live in engine_x64_rwdi.dll, but gameplay
// classes (weapon/aim modules, HUD elements) live in gamedll_x64_rwdi.dll
// instead. Hardcoding just the engine here silently found nothing for those -
// no crash, no obviously-wrong number, just an empty list - which is exactly
// the kind of failure HANDOVER.md's "never add a silent cap" lesson is about.
uintptr_t TryVtableInModule(HMODULE mod, const char* modName, const char* cls)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    const uint8_t* img = reinterpret_cast<const uint8_t*>(base);

    // 1. The type descriptor, found by name. Its name field sits 16 bytes in,
    //    after the vftable pointer and the spare. MSVC mangles a `class` as
    //    V and a `struct` as U - try both, since e.g. I_Reticle turned up as
    //    a plain string in the module but never matched the class form, which
    //    is exactly what you'd see if it were declared as a struct.
    char decorated[80];
    const uint8_t* nameAt = nullptr;
    for (const char* tag : { "V", "U" }) {
        _snprintf_s(decorated, sizeof(decorated), _TRUNCATE, ".?A%s%s@@", tag, cls);
        nameAt = FindBytes(img, imageSize, decorated, strlen(decorated));
        if (nameAt) {
            break;
        }
    }
    if (!nameAt) {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "no RTTI type descriptor for %s in %s", cls, modName);
        return 0;
    }
    const uint32_t tdRva = static_cast<uint32_t>(
        (reinterpret_cast<uintptr_t>(nameAt) - 16) - base);

    // 2. The complete object locator that names it, for the primary base.
    uint32_t colRva = 0;
    for (size_t off = 0; off + 24 <= imageSize; off += 4) {
        const auto* p = reinterpret_cast<const uint32_t*>(img + off);
        if (p[3] != tdRva || p[0] != 1 || p[1] != 0) {
            continue;
        }
        colRva = static_cast<uint32_t>(off);
        break;
    }
    if (!colRva) {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "%s type descriptor found but no locator", cls);
        return 0;
    }

    // 3. The qword holding that locator's address sits immediately before the
    //    vtable.
    // There can be more than one reference to the locator, and only the one
    // immediately before the vtable is the vtable. Check each: a real vtable's
    // first slots point into the module's own code.
    const uintptr_t colVa = base + colRva;
    const uintptr_t codeLo = base;
    const uintptr_t codeHi = base + imageSize;
    // The result is a LOCAL (trace docs/traces/20261118_1830_rtti_lookup_race.md): this used to be the namespace global g_vtable,
    // returned after the scan - two threads at once (lodoverride's LevelDI lookup and the swing install's WeaponMeleeController,
    // RUN WF1 18:25:52.850) handed the swing LevelDI's vtable and melee never installed.
    uintptr_t found = 0;
    for (size_t off = 0; off + 8 <= imageSize; off += 8) {
        if (*reinterpret_cast<const uintptr_t*>(img + off) != colVa) {
            continue;
        }
        const auto* slots = reinterpret_cast<const uintptr_t*>(img + off + 8);
        int good = 0;
        for (int k = 0; k < 4; ++k) {
            if (slots[k] > codeLo && slots[k] < codeHi) {
                ++good;
            }
        }
        if (good >= 3) {
            found = reinterpret_cast<uintptr_t>(slots);
            break;
        }
    }
    if (!found) {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "%s locator found but no vtable references it", cls);
        return 0;
    }
    LOG("rtti: %s vtable at %p (RVA 0x%zX in %s), td RVA 0x%X, col RVA 0x%X", cls,
        reinterpret_cast<void*>(found), found - base, modName, tdRva, colRva);
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "%s vtable RVA 0x%zX in %s", cls, found - base, modName);
    return found;
}

// Tries engine_x64_rwdi.dll first (where every class used so far has lived),
// then gamedll_x64_rwdi.dll (where gameplay-side classes like weapon/aim
// modules live instead). First hit wins; if neither has it, the last module's
// status message is what is left on screen.
uintptr_t RttiVtableOf(const char* cls)
{
    const char* const modules[] = { kEngine, "gamedll_x64_rwdi.dll" };
    for (const char* modName : modules) {
        HMODULE mod = GetModuleHandleA(modName);
        if (!mod) {
            _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "%s not loaded", modName);
            continue;
        }
        const uintptr_t vt = TryVtableInModule(mod, modName, cls);
        if (vt) {
            return vt;
        }
    }
    return 0;
}

// The reverse of VtableFor()/TryVtableInModule(): those start from a class
// name and go looking for a vtable. This starts from a live object address
// found some other way - a caught RCX value, an execution breakpoint, a
// pointer read out of another object's fields - and reads the class name
// back out of it. Same RTTI layout, walked backward: object -> vtable
// pointer -> (vtable - 8) is the Complete Object Locator's address -> its
// type-descriptor RVA -> the mangled name at +16.
// The MSVC x64 RTTICompleteObjectLocator:
//     +0x00 signature (1 on x64)   <-- already validated below
//     +0x04 offset                 <-- THIS: subobject -> complete object
//     +0x08 cdOffset
//     +0x0C pTypeDescriptor (RVA)
//     +0x10 pClassDescriptor (RVA)
//     +0x14 pSelf (RVA, x64 only)
// The name lookup already walks to the COL and checks the signature, so the
// offset is four bytes away from something we were reading anyway.
bool RttiClassNameAndOffsetOf(uintptr_t objectAddr, char* out, size_t outLen,
                              uint32_t& offsetInClass)
{
    offsetInClass = 0;
    if (!objectAddr || !out || outLen == 0) {
        return false;
    }
    uintptr_t vtable = 0;
    if (!SafeRead(objectAddr, &vtable, sizeof(vtable)) || !vtable) {
        return false;
    }
    uintptr_t colVa = 0;
    if (!SafeRead(vtable - sizeof(uintptr_t), &colVa, sizeof(colVa)) || !colVa) {
        return false;
    }
    uint32_t sig = 0;
    if (!SafeRead(colVa, &sig, sizeof(sig)) || sig != 1) {
        return false;
    }
    if (!SafeRead(colVa + 4, &offsetInClass, sizeof(offsetInClass))) {
        return false;
    }
    return RttiClassNameOf(objectAddr, out, outLen);
}

bool RttiClassNameOf(uintptr_t objectAddr, char* out, size_t outLen)
{
    if (!objectAddr || !out || outLen == 0) {
        return false;
    }
    uintptr_t vtable = 0;
    if (!SafeRead(objectAddr, &vtable, sizeof(vtable)) || !vtable) {
        return false;
    }

    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(vtable), &mod) ||
        !mod) {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(mod);

    uintptr_t colVa = 0;
    if (!SafeRead(vtable - sizeof(uintptr_t), &colVa, sizeof(colVa)) || !colVa) {
        return false;
    }
    uint32_t sig = 0;
    if (!SafeRead(colVa, &sig, sizeof(sig)) || sig != 1) {
        return false;   // not a Complete Object Locator - this vtable has no RTTI
    }
    uint32_t tdRva = 0;
    if (!SafeRead(colVa + 12, &tdRva, sizeof(tdRva)) || !tdRva) {
        return false;
    }

    char raw[128] = {};
    if (!SafeRead(base + tdRva + 16, raw, sizeof(raw) - 1)) {
        return false;
    }
    // Strip the ".?AV"/".?AU" mangling prefix and the trailing "@@", same
    // convention as the demangling done by hand elsewhere in this project.
    const char* p = raw;
    if (p[0] == '.' && p[1] == '?' && p[2] == 'A' && (p[3] == 'V' || p[3] == 'U')) {
        p += 4;
    }
    strncpy_s(out, outLen, p, _TRUNCATE);
    if (char* at = strstr(out, "@@")) {
        *at = '\0';
    }

    // 2026-11-08 log trim (Jay: "Yes"): ONE line per (object, class). The same pointer was re-described ~once a second
    // (IPoseElement, 168 lines in EZ1) and every HUD re-show printed its ~80 classes again (an 83-line burst). A repeat
    // also skips GetModuleFileNameA (the loader lock). The last 256 keys are kept; an evicted one prints again.
    static SRWLOCK s_seenLock = SRWLOCK_INIT;
    static uintptr_t s_seen[256] = {};
    static unsigned s_seenNext = 0;
    const uintptr_t key = objectAddr ^ (vtable * 0x9E3779B97F4A7C15ull);
    bool repeat = false;
    AcquireSRWLockExclusive(&s_seenLock);
    for (const uintptr_t k : s_seen) {
        if (k == key) { repeat = true; break; }
    }
    if (!repeat) { s_seen[s_seenNext++ & 255u] = key; }
    ReleaseSRWLockExclusive(&s_seenLock);
    if (repeat) { return true; }

    char modPath[MAX_PATH] = {};
    GetModuleFileNameA(mod, modPath, sizeof(modPath));
    const char* leaf = strrchr(modPath, '\\');
    LOG("rtti: %p is %s (%s)", reinterpret_cast<void*>(objectAddr), out,
        leaf ? leaf + 1 : modPath);
    return true;
}

// Scans one committed region for either vtable.
//
// Split out because it uses __try and the caller holds std::vectors, which MSVC
// will not compile together. It also earns its keep on its own: the scan used to
// allocate a buffer the size of each region and ReadProcessMemory the whole
// thing into it - on our own process, across gigabytes of heap. That is a
// syscall and a full copy per region, and it is why pressing scan froze the game
// for a moment. The memory is already ours to read directly; the copy only ever
// bought safety against a region being freed mid-scan, and the SEH guard here
// buys that for nothing.
constexpr int kRegionHits = 256;

struct RegionHit {
    uintptr_t object;
};

int ScanRegion(uintptr_t base, size_t size, const uintptr_t* vts,
               RegionHit* out, int* which, int max)
{
    int n = 0;
    __try {
        const auto* q = reinterpret_cast<const uintptr_t*>(base);
        const size_t count = size / sizeof(uintptr_t);
        for (size_t i = 0; i < count && n < max; ++i) {
            int w = -1;
            if (vts[0] && q[i] == vts[0]) {
                w = 0;
            } else if (vts[1] && q[i] == vts[1]) {
                w = 1;
            } else {
                continue;
            }
            out[n].object = base + i * sizeof(uintptr_t);
            which[n] = w;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Freed under us mid-scan. Whatever was found before the fault stands.
    }
    return n;
}

int RttiScanCameras()
{
    const bool both = (strcmp(g_class, "both") == 0);
    const char* classes[2] = { both ? "CRendererCamera" : g_class,
                               both ? "CCamera" : nullptr };
    uintptr_t vts[2] = { VtableFor(classes[0]),
                         classes[1] ? VtableFor(classes[1]) : 0 };
    if (!vts[0] && !vts[1]) {
        return 0;
    }
    g_cams.clear();

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t hi = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

    for (uintptr_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi))) {
            break;
        }
        const uintptr_t bbase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool ok = mbi.State == MEM_COMMIT &&
                        !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                        (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                         prot == PAGE_EXECUTE_READWRITE) &&
                        mbi.Type == MEM_PRIVATE;   // objects live on the heap

        if (ok && size >= sizeof(uintptr_t)) {
            RegionHit hits[kRegionHits];
            int which[kRegionHits];
            const int got = ScanRegion(bbase, size, vts, hits, which, kRegionHits);
            for (int i = 0; i < got; ++i) {
                Cam c{};
                c.object = hits[i].object;
                c.cls = classes[which[i]];
                g_cams.push_back(c);
            }
        }
        a = bbase + size;
    }

    // Measure the layout from the objects before reading anything out of them.
    MeasureLayout(g_cams);

    // Now drop the ones that do not hold a usable matrix at the measured offset:
    // a freed object leaves its vtable pointer behind for a while. That check
    // only makes sense for the camera family, whose layout g_camOffset/
    // g_spacing were measured against - a class like AimModule has no reason
    // to have a rotation matrix at +0x40, so it would always fail here and
    // vanish from the list even though the object itself is real. For anyone
    // outside the camera family, keep the object and just show it with no
    // orientation, so the address is still there for the float inspector.
    std::vector<Cam> keep;
    keep.reserve(g_cams.size());
    const size_t viewOff = g_camOffset - g_spacing;
    for (auto& c : g_cams) {
        const bool cameraFamily = strcmp(c.cls, "CRendererCamera") == 0 ||
                                  strcmp(c.cls, "CCamera") == 0 ||
                                  strcmp(c.cls, "CBaseCamera") == 0;
        XrQuaternionf rot{0.0f, 0.0f, 0.0f, 1.0f};
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        const bool haveMatrix = ReadCamMatrix(c.object, rot, px, py, pz);
        if (cameraFamily) {
            if (!haveMatrix) {
                continue;
            }
            // An object with the vtable but no matrix pair was constructed
            // and never set up. Its numbers are whatever the allocator left
            // behind.
            if (RttiOnlyRealCameras &&
                !LooksLikeCameraPair(c.object, viewOff, g_spacing)) {
                continue;
            }
        }
        c.last = rot;
        keep.push_back(c);
    }
    g_cams.swap(keep);

    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "%zu %s objects, matrix at +0x%zX (%s)", g_cams.size(), g_class,
                g_camOffset, g_layoutMeasured ? "measured" : "macOS default");
    LOG("rtti: %zu %s instances, matrix +0x%zX", g_cams.size(), g_class, g_camOffset);
    // The addresses themselves, so whether they survive a rescan or a level
    // load is a matter of comparing two log lines rather than of guessing.
    for (size_t i = 0; i < g_cams.size() && i < 16; ++i) {
        LOG("rtti:   [%zu] %p", i, reinterpret_cast<void*>(g_cams[i].object));
    }
    return static_cast<int>(g_cams.size());
}

int RttiCameraCount() { return static_cast<int>(g_cams.size()); }

void RttiUpdateCameras()
{
    for (auto& c : g_cams) {
        XrQuaternionf rot;
        float px, py, pz;
        if (!ReadCamMatrix(c.object, rot, px, py, pz)) {
            continue;
        }
        const float d = AngleBetween(c.last, rot);
        if (d > 0.05f) {
            c.moving = true;
        }
        // A decaying rate rather than a flag. Nearly every camera in the list
        // moves at some point, so "moves: yes" barely narrows anything - what
        // picks the player camera out is that it is moving NOW, by degrees,
        // while the rest sit still.
        c.moveDeg = c.moveDeg * 0.9f + d;
        c.last = rot;
    }
}

int RttiGetCameras(RttiCamera* out, int max)
{
    if (!out || max <= 0) {
        return 0;
    }
    const int n = static_cast<int>(g_cams.size()) < max
                      ? static_cast<int>(g_cams.size())
                      : max;
    if (RttiSortByActivity) {
        std::sort(g_cams.begin(), g_cams.end(),
                  [](const Cam& a, const Cam& b) { return a.moveDeg > b.moveDeg; });
    } else {
        // A fixed order, so a row stays where it is long enough to be clicked.
        std::sort(g_cams.begin(), g_cams.end(),
                  [](const Cam& a, const Cam& b) { return a.object < b.object; });
    }

    for (int i = 0; i < n; ++i) {
        const Cam& c = g_cams[i];
        out[i].object = c.object;
        out[i].camMatrix = c.object + g_camOffset;
        out[i].moving = c.moving;
        out[i].moveDeg = c.moveDeg;
        out[i].className = c.cls ? c.cls : "?";
        XrQuaternionf rot;
        float px = 0, py = 0, pz = 0;
        if (ReadCamMatrix(c.object, rot, px, py, pz)) {
            ToEuler(rot, out[i].yawDeg, out[i].pitchDeg, out[i].rollDeg);
        } else {
            out[i].yawDeg = out[i].pitchDeg = out[i].rollDeg = 0.0f;
        }
        out[i].px = px;
        out[i].py = py;
        out[i].pz = pz;
    }
    return n;
}


}  // namespace dlvr
