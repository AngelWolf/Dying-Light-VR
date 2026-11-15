// VR video force (docs/superpowers/specs/2026-10-21-vr-video-force-design.md 3.1). Pure and header-only - tested by
// src/src/tests/videoforce_logic_test.cpp.
//
// The engine's Oculus bring-up (engine +2E6010, VR only) holds the parsed video.scr values in rdi when it reaches its
// VSync store at +2E63F2 (C7 47 18 02 00 00 00). That one instruction becomes a jmp to the stub built here, which copies
// the six original values into a Record, zeroes them through rdi and jumps back to +2E63F9. Parsed-struct offsets (engine
// parser +23D970, ida/vs1 + fc2): VSync +0x18 (dword), MaxFPS +0x20 (dword), NvidiaEffects HBAO+ / DOF / PCSS +0x60 /
// +0x61 / +0x62 (bytes), MotionBlur +0x76 (byte). The stub clobbers eax and flags only; both are dead at +2E63F2 (fc2).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace dlvr::videoforce {

struct Record {
    volatile uint8_t ran;   // 1 once the stub has run (the bring-up reached the store)
    uint8_t pad[3];
    uint32_t vsync;         // the values video.scr loaded with, before the stub zeroed them
    uint32_t maxFps;
    uint8_t hbao, dof, pcss, motionBlur;
};
static_assert(sizeof(Record) == 16, "the stub writes Record fields at fixed offsets");

constexpr size_t kStubMax = 128;

namespace detail {

inline bool Rel32(uintptr_t from, uintptr_t to, int32_t* out)
{
    const int64_t d = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    if (d > 0x7FFF0000LL || d < -0x7FFF0000LL) { return false; }
    *out = static_cast<int32_t>(d);
    return true;
}

struct Emitter {
    uint8_t* out;
    uintptr_t code;
    size_t n;
    bool ok;
    void raw(std::initializer_list<uint8_t> xs) { for (uint8_t x : xs) { out[n++] = x; } }
    // opcode, then a rel32 to `target` measured from the end of the instruction (which includes `tail`)
    void rip(std::initializer_list<uint8_t> op, uintptr_t target, std::initializer_list<uint8_t> tail = {})
    {
        raw(op);
        int32_t d = 0;
        if (!Rel32(code + n + 4 + tail.size(), target, &d)) { ok = false; }
        std::memcpy(out + n, &d, 4);
        n += 4;
        raw(tail);
    }
};

}  // namespace detail

// Builds the stub for `codeAddr`, recording into the Record at `recAddr`, returning to `retAddr`. Returns the byte count,
// or 0 when `cap` < kStubMax or any rel32 is out of reach.
inline size_t BuildStub(uint8_t* out, size_t cap, uintptr_t codeAddr, uintptr_t recAddr, uintptr_t retAddr)
{
    if (!out || cap < kStubMax) { return 0; }
    detail::Emitter e{out, codeAddr, 0, true};
    e.raw({0x8B, 0x47, 0x18}); e.rip({0x89, 0x05}, recAddr + offsetof(Record, vsync));        // VSync
    e.raw({0x8B, 0x47, 0x20}); e.rip({0x89, 0x05}, recAddr + offsetof(Record, maxFps));       // MaxFPS
    e.raw({0x8A, 0x47, 0x60}); e.rip({0x88, 0x05}, recAddr + offsetof(Record, hbao));         // HBAO+
    e.raw({0x8A, 0x47, 0x61}); e.rip({0x88, 0x05}, recAddr + offsetof(Record, dof));          // DOF
    e.raw({0x8A, 0x47, 0x62}); e.rip({0x88, 0x05}, recAddr + offsetof(Record, pcss));         // PCSS
    e.raw({0x8A, 0x47, 0x76}); e.rip({0x88, 0x05}, recAddr + offsetof(Record, motionBlur));   // MotionBlur
    e.rip({0xC6, 0x05}, recAddr + offsetof(Record, ran), {0x01});                             // ran = 1
    e.raw({0xC7, 0x47, 0x18, 0, 0, 0, 0});                                                     // VSync = 0
    e.raw({0xC7, 0x47, 0x20, 0, 0, 0, 0});                                                     // MaxFPS = 0
    e.raw({0xC6, 0x47, 0x60, 0});                                                              // HBAO+ = 0
    e.raw({0xC6, 0x47, 0x61, 0});                                                              // DOF = 0
    e.raw({0xC6, 0x47, 0x62, 0});                                                            // PCSS = 0
    e.raw({0xC6, 0x47, 0x76, 0});                                                              // MotionBlur = 0
    e.rip({0xE9}, retAddr);                                                                  // back to the bring-up
    return e.ok ? e.n : 0;
}

// The 7-byte replacement for the store: jmp rel32 to the stub, then two nops. False when out of rel32 reach.
inline bool BuildSiteJump(uint8_t out[7], uintptr_t siteAddr, uintptr_t stubAddr)
{
    int32_t d = 0;
    if (!detail::Rel32(siteAddr + 5, stubAddr, &d)) { return false; }
    out[0] = 0xE9;
    std::memcpy(out + 1, &d, 4);
    out[5] = 0x90;
    out[6] = 0x90;
    return true;
}

}  // namespace dlvr::videoforce
