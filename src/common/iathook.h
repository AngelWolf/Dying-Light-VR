// Import Address Table patching.
//
// Safe to call from DllMain. The Windows loader runs in three phases for a
// dependency graph - map every module, snap every import, then call DllMains in
// dependency order - so by the time our DllMain executes, the importing
// module's IAT is already populated and will not be rewritten underneath us.
#pragma once

#include <windows.h>

// uint8_t is used below. This header only ever compiled because every existing
// includer happened to pull in <cstdint> first; a new translation unit that did
// not made it fail on its own.
#include <cstdint>
#include <cstring>

namespace dlvr {

// Replaces the IAT slot in `module` for `importName` imported from `fromDll`.
// Returns the original entry, or nullptr if the import was not found.
inline void* PatchIat(HMODULE module, const char* fromDll, const char* importName,
                      void* replacement)
{
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return nullptr;
    }

    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* dll = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dll, fromDll) != 0) {
            continue;
        }

        // OriginalFirstThunk keeps the names; FirstThunk is the live IAT.
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            auto imp = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(imp->Name), importName) != 0) {
                continue;
            }

            void* original = reinterpret_cast<void*>(iat->u1.Function);
            DWORD old = 0;
            if (!VirtualProtect(iat, sizeof(*iat), PAGE_READWRITE, &old)) {
                return nullptr;
            }
            iat->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(iat, sizeof(*iat), old, &old);
            return original;
        }
    }
    return nullptr;
}

// READ-ONLY: the address of the IAT slot in `module` for `importName` imported from `fromDll`, or nullptr. Same walk as
// PatchIat; nothing is written. For reporting what a slot holds after the fact (2026-10-18: the Epic build's rd3d11
// D3D11CreateDevice slot, which our hook never saw called).
inline void** FindIatSlot(HMODULE module, const char* fromDll, const char* importName)
{
    auto base = reinterpret_cast<uint8_t*>(module);
    if (!base) {
        return nullptr;
    }
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return nullptr;
    }
    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), fromDll) != 0) {
            continue;
        }
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            auto imp = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(imp->Name), importName) == 0) {
                return reinterpret_cast<void**>(&iat->u1.Function);
            }
        }
    }
    return nullptr;
}

}  // namespace dlvr
