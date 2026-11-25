// The arithmetic of the near block that holds every rd3d11 call-site stub (INV-777, trace docs/traces/20261125_1135_rd3d11_stub_pool.md).
// Pure: no Windows types, no allocation. A call site is 5 bytes (E8 rel32), so a stub must lie within rel32 of the site's end.
#pragma once

namespace dlvr {
namespace stubpool_logic {

constexpr int kBlockBytes = 4096;
constexpr int kSlotBytes  = 64;                         // the stubs are 14 and 17 bytes; one cache line each
constexpr int kSlots      = kBlockBytes / kSlotBytes;
constexpr unsigned long long kGran = 0x10000ull;        // Windows' allocation granularity
constexpr unsigned long long kSpan = 0x70000000ull;     // 1.75 GB each way, inside rel32 with room for the module itself
constexpr unsigned long long kEnd  = ~0ull;             // Candidate: no more addresses

inline long long Rel32(unsigned long long stub, unsigned long long site)
{
    return static_cast<long long>(stub - (site + 5));
}
inline bool Reaches(unsigned long long stub, unsigned long long site)
{
    if (!stub) { return false; }
    const long long r = Rel32(stub, site);
    return r <= 0x7FFFFFFFll && r >= -0x80000000ll;
}
// Slot i of the block, or 0: no block, no such slot, or out of the site's reach.
inline unsigned long long SlotAddr(unsigned long long block, int i, unsigned long long site)
{
    if (!block || i < 0 || i >= kSlots) { return 0; }
    const unsigned long long a = block + static_cast<unsigned long long>(i) * kSlotBytes;
    return Reaches(a, site) ? a : 0;
}
// The i-th address to try for the block: 64 KB steps outwards, below then above. 0 = skip this one (it would go under 64 KB).
inline unsigned long long Candidate(unsigned long long near_, int i)
{
    const unsigned long long step = (static_cast<unsigned long long>(i / 2) + 1) * kGran;
    if (step > kSpan) { return kEnd; }
    const unsigned long long at = near_ & ~(kGran - 1);
    if (i & 1) { return at + step; }
    return at > step ? at - step : 0;
}

}  // namespace stubpool_logic
}  // namespace dlvr
