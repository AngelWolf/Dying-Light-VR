// rtti_vtable - find a gamedll vtable by its MSVC x64 RTTI class name and the object-locator offset of the sub-object (2026-10-25,
// cinlook.cpp / cinbars.cpp; the same walk dlgview.cpp does for DialogController). No rva table, so every build either finds it or the
// caller refuses by name. COST: three linear scans of the module's readable data sections - call it once, never per frame.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace dlvr {

// stage out: 0 = the class name is not in the module, 1 = no locator names it with that offset, 2 = no vtable points at the locator,
// 3 = found, -1 = the scan faulted. Returns the vtable (the address of slot 0) or 0.
inline uintptr_t RttiFindVtable(uintptr_t base, const char* name, uint32_t colOffset, int* stage)
{
    const size_t nlen = strlen(name) + 1;   // the terminator is part of the match
    uintptr_t td = 0, col = 0, vt = 0;
    *stage = 0;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* first = IMAGE_FIRST_SECTION(nt);
        const WORD ns = nt->FileHeader.NumberOfSections;
        for (int pass = 0; pass < 3; ++pass) {
            const IMAGE_SECTION_HEADER* s = first;
            for (WORD i = 0; i < ns; ++i, ++s) {
                const DWORD c = s->Characteristics;
                if ((c & IMAGE_SCN_MEM_EXECUTE) || !(c & IMAGE_SCN_MEM_READ)) { continue; }
                const uintptr_t p = base + s->VirtualAddress;
                const size_t n = static_cast<size_t>(s->Misc.VirtualSize);
                if (pass == 0) {
                    for (size_t k = 0; !td && k + nlen <= n; ++k) {
                        if (memcmp(reinterpret_cast<const void*>(p + k), name, nlen) == 0) { td = p + k - 16; }
                    }
                } else if (pass == 1) {
                    const uint32_t tdRva = static_cast<uint32_t>(td - base);
                    for (size_t k = 0; !col && k + 24 <= n; k += 4) {
                        const uint32_t* q = reinterpret_cast<const uint32_t*>(p + k);
                        if (q[0] == 1 && q[1] == colOffset && q[3] == tdRva && q[5] == static_cast<uint32_t>(p + k - base)) { col = p + k; }
                    }
                } else {
                    for (size_t k = 0; !vt && k + 8 <= n; k += 8) {
                        if (*reinterpret_cast<const uintptr_t*>(p + k) == col) { vt = p + k + 8; }
                    }
                }
            }
            if (pass == 0 && !td) { return 0; }
            if (pass == 1 && !col) { *stage = 1; return 0; }
        }
        *stage = vt ? 3 : 2;
        return vt;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *stage = -1;
        return 0;
    }
}

}  // namespace dlvr
