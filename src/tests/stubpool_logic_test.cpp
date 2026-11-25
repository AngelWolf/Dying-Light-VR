// Unit tests for bootstrap/stubpool_logic.h (compiled with cl, no game).
#include <cstdio>
#include "../bootstrap/stubpool_logic.h"

static int g_fail = 0;
#define CHECK(x) do { if (!(x)) { ++g_fail; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

int main()
{
    using namespace dlvr::stubpool_logic;
    const unsigned long long site = 0x0000029371030940ull;   // a run's low rd3d11 base + a patched site

    // reach: a call is 5 bytes, rel32 counts from its end
    CHECK(Reaches(site + 5, site));
    CHECK(Reaches(site + 5 + 0x7FFFFFFFll, site));
    CHECK(!Reaches(site + 5 + 0x80000000ull, site));
    CHECK(Reaches(site + 5 - 0x80000000ull, site));
    CHECK(!Reaches(site + 5 - 0x80000001ull, site));
    CHECK(!Reaches(0, site));                                // null is never reachable
    CHECK(Rel32(site + 5 + 0x1000, site) == 0x1000);
    CHECK(Rel32(site, site) == -5);

    // slots: 64 bytes apart, in order, none twice, none past the block
    const unsigned long long block = site + 0x1000000;
    CHECK(SlotAddr(block, 0, site) == block);
    CHECK(SlotAddr(block, 1, site) == block + 64);
    CHECK(SlotAddr(block, kSlots - 1, site) == block + 4096 - 64);
    CHECK(SlotAddr(block, kSlots, site) == 0);
    CHECK(SlotAddr(block, -1, site) == 0);
    CHECK(SlotAddr(0, 0, site) == 0);                        // no block
    CHECK(SlotAddr(block + 0x80000000ull, 0, site) == 0);    // a block out of reach hands out nothing
    static_assert(kSlots * kSlotBytes == kBlockBytes, "slots fill the block");
    static_assert(kSlots >= 12, "7 setter sites + 4 draw sites + the Unmap site");
    static_assert(kSlotBytes >= 17, "the larger stub");

    // the search: nearest first, both sides, never below 64 KB, never past the span, always 64 KB aligned
    int n = 0, below = 0, above = 0;
    unsigned long long prevD = 0;
    for (int i = 0; ; ++i) {
        const unsigned long long c = Candidate(site, i);
        if (c == kEnd) { break; }
        ++n;
        if (!c) { continue; }
        CHECK((c & 0xFFFF) == 0);
        const unsigned long long d = c > site ? c - site : site - c;
        CHECK(d <= kSpan + 0x10000);
        CHECK(d + 0x20000 >= prevD);                         // distance never shrinks (bar the rounding of a pair)
        prevD = d;
        (c < site ? below : above)++;
        CHECK(Reaches(c, site) && Reaches(c + kBlockBytes - kSlotBytes, site));
    }
    CHECK(n > 50000);
    CHECK(below > 20000 && above > 20000);
    CHECK(Candidate(0x30000, 0) != 0x30000);                 // not the DLL itself
    for (int i = 0; i < 40; ++i) {                           // near address 0: below is 0 (skipped), never wrapped round
        const auto c = Candidate(0x30000, i);
        if (i & 1) { CHECK(c > 0x30000); } else { CHECK(c == 0 || (c >= 0x10000 && c < 0x30000)); }
    }
    // a site so low that address 0 is within rel32: null and "no block" must still be refused
    CHECK(!Reaches(0, 0x100000));
    CHECK(Reaches(0x200000, 0x100000));
    CHECK(SlotAddr(0, 1, 0x100000) == 0);

    printf("stubpool_logic: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
