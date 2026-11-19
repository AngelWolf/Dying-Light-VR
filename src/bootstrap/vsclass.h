// vsclass.h - THE HELD-ITEM VERTEX SHADER CLASSIFIER (2026-10-07, STATE item 4, INV-467; Jay: "Do it").
//
// WHY. The rebuild's held-pose map redraws each player / held-item draw with OUR vertex shader, which must read the
// engine's vertex stream register for register. Which of ours fits was decided by a hand-read hash list, and INV-467
// showed the list cannot keep up: the same machete mesh came back under new hashes after a re-equip, 8 unread shaders in
// one run. So each vertex shader is classified ONCE, when the engine creates it, by what it IS:
//   (1) its input signature (the DXBC ISGN chunk) supplies every input OUR shader declares, at the SAME register - our
//       shader is drawn with the engine's input layout, so a missing element would be read as garbage;
//   (2) its constant buffers have the shape we read: CB2 >= 7 rows (the view-projection), skinned also CB4 >= 1536;
//   (3) it does the math we copy - rigid: `dp4 v0, v<row>` for each of the three world rows; skinned: the body palette's
//       index math (`mul rK, v2, l(765.299988 x4)` then `ishl rK, rK, l(1 x4)`). A signature alone is NOT a pairing
//       (INV-416), hence (3).
// The game's shaders carry ONLY ISGN / OSGN / SHEX (1877 of 1877 vertex shaders in the pack dump: no RDEF), so
// D3DReflect has no buffer data; (1) is read from ISGN here and (2) / (3) from D3DDisassemble's text - the same
// disassembler that produced the pack dump the rules were tested on (12 / 12 hand-read shaders, 2026-10-07).
// COST: ISGN parse per vertex shader created (microseconds); D3DDisassemble only when (1) already passed.
#pragma once
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace vsclass {

// kSkinned1 / kRigidC (2026-10-07, INV-473 / 474): the revolver's s3875 form (one bone, no weights) and s2065's form (rigid,
// no TANGENT, rows at v3..v5).
// kRigidD / kRigidE / kSkinnedC (2026-10-28, the buggy, run B4 + OUR pack read): the buggy's three large draws - forms A / C / the
// body's skinning with a COLOR input added (a68f15c7 / 4f2f75b7 / e40d02f6, pack +93A1D3 / +78931F / +A9F527).
// kRigidC1 (2026-11-19, INV-751, trace 20261119_1840): form C with TEXCOORD1 at v3, rows TEXCOORD4..6 at v4..v6 - the ONLY GBuffer
// form of a weapon material we did not move (36 shaders in OUR pack a00bdd80, 95 weapon meshes; e.g. the machete's bloody blade
// dc9f5576 +66DC4F, the pickaxe 4a006809 +86176F).
enum Class : unsigned char { kUnclassified = 0, kNone = 1, kRigidA = 2, kRigidB = 3, kSkinned = 4, kSkinned1 = 5, kRigidC = 6,
                             kRigidD = 7, kRigidE = 8, kSkinnedC = 9, kSkinnedM = 10, kRigidC1 = 11 };
// why a shader is kNone (the first test that failed for the closest form)
enum Reason : unsigned char { kOk = 0, kNotDxbc = 1, kSignature = 2, kBuffers = 3, kMath = 4, kDisasmFailed = 5 };

struct In { const char* name; unsigned idx; unsigned reg; };
// OUR shaders' inputs, copied from kRwHlsl (RIn / RInB) and kHmHlsl (VIn) in dlssflat.cpp
constexpr In kOurA[] = { { "POSITION", 0, 0 }, { "NORMAL", 0, 1 }, { "TANGENT", 0, 2 }, { "TEXCOORD", 0, 3 },
                         { "TEXCOORD", 4, 4 }, { "TEXCOORD", 5, 5 }, { "TEXCOORD", 6, 6 }, { "TEXCOORD", 7, 7 } };
constexpr In kOurB[] = { { "POSITION", 0, 0 }, { "NORMAL", 0, 1 }, { "TANGENT", 0, 2 }, { "TEXCOORD", 0, 3 },
                         { "TEXCOORD", 1, 4 }, { "TEXCOORD", 4, 5 }, { "TEXCOORD", 5, 6 }, { "TEXCOORD", 6, 7 },
                         { "TEXCOORD", 7, 8 } };
constexpr In kOurS[] = { { "POSITION", 0, 0 }, { "BLENDWEIGHT", 0, 1 }, { "BLENDINDICES", 0, 2 } };
// kHmHlsl VIn1 (s3875's form) and kRwHlsl RInC (s2065's form), copied register for register from those shaders
constexpr In kOurS1[] = { { "POSITION", 0, 0 }, { "BLENDINDICES", 0, 1 } };
constexpr In kOurC[] = { { "POSITION", 0, 0 }, { "NORMAL", 0, 1 }, { "TEXCOORD", 0, 2 }, { "TEXCOORD", 4, 3 },
                         { "TEXCOORD", 5, 4 }, { "TEXCOORD", 6, 5 }, { "TEXCOORD", 7, 6 } };
// kRwHlsl RInD / RInE and kHmHlsl VInC, copied register for register from the buggy's a68f15c7 / 4f2f75b7 / e40d02f6 (fxc /dumpbin)
constexpr In kOurD[] = { { "POSITION", 0, 0 }, { "COLOR", 0, 1 }, { "NORMAL", 0, 2 }, { "TANGENT", 0, 3 }, { "TEXCOORD", 0, 4 },
                         { "TEXCOORD", 4, 5 }, { "TEXCOORD", 5, 6 }, { "TEXCOORD", 6, 7 }, { "TEXCOORD", 7, 8 } };
constexpr In kOurE[] = { { "POSITION", 0, 0 }, { "COLOR", 0, 1 }, { "NORMAL", 0, 2 }, { "TEXCOORD", 0, 3 }, { "TEXCOORD", 4, 4 },
                         { "TEXCOORD", 5, 5 }, { "TEXCOORD", 6, 6 }, { "TEXCOORD", 7, 7 } };
constexpr In kOurSC[] = { { "POSITION", 0, 0 }, { "BLENDWEIGHT", 0, 1 }, { "COLOR", 0, 2 }, { "BLENDINDICES", 0, 3 } };
// kRwHlsl RInC1, copied register for register from the 36 C1 shaders' ISGN (OUR pack; fxc /dumpbin of +66DC4F and 35 more)
constexpr In kOurC1[] = { { "POSITION", 0, 0 }, { "NORMAL", 0, 1 }, { "TEXCOORD", 0, 2 }, { "TEXCOORD", 1, 3 },
                          { "TEXCOORD", 4, 4 }, { "TEXCOORD", 5, 5 }, { "TEXCOORD", 6, 6 }, { "TEXCOORD", 7, 7 } };

struct Sig { int n; struct E { char name[24]; unsigned idx, reg; } e[32]; };

inline unsigned Rd32(const unsigned char* b, size_t o) { unsigned v; memcpy(&v, b + o, 4); return v; }

// The ISGN chunk -> (semantic, index, register). false = not DXBC / no ISGN / malformed.
inline bool ParseIsgn(const void* code, size_t n, Sig* s)
{
    s->n = 0;
    const unsigned char* b = static_cast<const unsigned char*>(code);
    if (!b || n < 32 || memcmp(b, "DXBC", 4) != 0) { return false; }
    const unsigned chunks = Rd32(b, 28);
    if (chunks > 64 || 32 + 4ull * chunks > n) { return false; }
    for (unsigned i = 0; i < chunks; ++i) {
        const unsigned off = Rd32(b, 32 + 4 * i);
        if (off + 8ull > n || memcmp(b + off, "ISGN", 4) != 0) { continue; }
        const size_t c = off + 8;
        if (c + 8 > n) { return false; }
        const unsigned cnt = Rd32(b, c);
        if (cnt > 32 || c + 8 + 24ull * cnt > n) { return false; }
        for (unsigned k = 0; k < cnt; ++k) {
            const size_t e = c + 8 + 24ull * k;
            const unsigned nameOff = Rd32(b, e), idx = Rd32(b, e + 4), reg = Rd32(b, e + 16);
            if (c + nameOff >= n) { return false; }
            const char* nm = reinterpret_cast<const char*>(b + c + nameOff);
            size_t len = 0;
            while (c + nameOff + len < n && nm[len] && len < sizeof(s->e[0].name) - 1) { ++len; }
            memcpy(s->e[s->n].name, nm, len);
            s->e[s->n].name[len] = 0;
            _strupr_s(s->e[s->n].name, sizeof(s->e[s->n].name));
            s->e[s->n].idx = idx; s->e[s->n].reg = reg;
            ++s->n;
        }
        return true;
    }
    return false;
}

template <size_t N> bool Supplies(const Sig& s, const In (&want)[N])
{
    for (size_t i = 0; i < N; ++i) {
        bool ok = false;
        for (int k = 0; k < s.n && !ok; ++k) {
            ok = s.e[k].idx == want[i].idx && s.e[k].reg == want[i].reg && strcmp(s.e[k].name, want[i].name) == 0;
        }
        if (!ok) { return false; }
    }
    return true;
}

inline bool HasSemantic(const Sig& s, const char* name)
{
    for (int k = 0; k < s.n; ++k) { if (strcmp(s.e[k].name, name) == 0) { return true; } }
    return false;
}

// "dcl_constantbuffer CB<slot>[<rows>]" -> rows, 0 = not declared
inline int CbRows(const char* t, int slot)
{
    char pat[40];
    sprintf_s(pat, "dcl_constantbuffer CB%d[", slot);
    const char* p = strstr(t, pat);
    return p ? atoi(p + strlen(pat)) : 0;
}

// is there a line "dp4 r..., v0.xyzw, v<row>.xyzw" or "dp4 r..., v<row>.xyzw, v0.xyzw"?
inline bool HasDp4Row(const char* t, unsigned row)
{
    char a[40], b[40];
    sprintf_s(a, ", v0.xyzw, v%u.xyzw", row);
    sprintf_s(b, ", v%u.xyzw, v0.xyzw", row);
    for (const char* l = t; l && *l; ) {
        while (*l == ' ' || *l == '\t') { ++l; }
        const char* e = strchr(l, '\n');
        const size_t len = e ? static_cast<size_t>(e - l) : strlen(l);
        if (len > 5 && strncmp(l, "dp4 r", 5) == 0) {
            char line[256];
            const size_t m = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
            memcpy(line, l, m); line[m] = 0;
            if (strstr(line, a) || strstr(line, b)) { return true; }
        }
        l = e ? e + 1 : nullptr;
    }
    return false;
}

// the body palette's index math: "mul rK.xyzw, v2.xyzw, l(765.299988 x4)" then "ishl rK.xyzw, rK.xyzw, l(1, 1, 1, 1)"
// bi = the BLENDINDICES register: v2 for the body's form, v3 for the buggy's skinned-colour form (kOurSC)
inline bool HasPaletteMath(const char* t, unsigned bi = 2)
{
    char kMul[96];
    sprintf_s(kMul, ".xyzw, v%u.xyzw, l(765.299988, 765.299988, 765.299988, 765.299988)", bi);
    for (const char* p = strstr(t, kMul); p; p = strstr(p + 1, kMul)) {
        const char* q = p;                                   // back up over "mul rK"
        while (q > t && q[-1] >= '0' && q[-1] <= '9') { --q; }
        if (q - t < 5 || strncmp(q - 5, "mul r", 5) != 0 || q == p) { continue; }
        char reg[8] = {};
        const size_t rl = static_cast<size_t>(p - q) < sizeof(reg) - 1 ? static_cast<size_t>(p - q) : sizeof(reg) - 1;
        memcpy(reg, q, rl);
        char ishl[80];
        sprintf_s(ishl, "ishl r%s.xyzw, r%s.xyzw, l(1, 1, 1, 1)", reg, reg);
        if (strstr(t, ishl)) { return true; }
    }
    return false;
}

// the single-bone palette index math (s3875): "mul rK.x, v1.x, l(765.299988)" then "ishl rK.x, rK.x, l(1)"
inline bool HasPaletteMath1(const char* t)
{
    static const char kMul[] = ".x, v1.x, l(765.299988)";
    for (const char* p = strstr(t, kMul); p; p = strstr(p + 1, kMul)) {
        const char* q = p;
        while (q > t && q[-1] >= '0' && q[-1] <= '9') { --q; }
        if (q - t < 5 || strncmp(q - 5, "mul r", 5) != 0 || q == p) { continue; }
        char reg[8] = {};
        const size_t rl = static_cast<size_t>(p - q) < sizeof(reg) - 1 ? static_cast<size_t>(p - q) : sizeof(reg) - 1;
        memcpy(reg, q, rl);
        char ishl[64];
        sprintf_s(ishl, "ishl r%s.x, r%s.x, l(1)", reg, reg);
        if (strstr(t, ishl)) { return true; }
    }
    return false;
}

// AFW list item 5: skinned with morph targets (80 VS in OUR pack). Returns the row of the morph count (cb0[row].x), -1 = not this form.
// The index math writes a different register from the one it reads, so HasPaletteMath does not match these.
inline int MorphBase(const char* t)
{
    if (!strstr(t, ", vertex_id") || !strstr(t, "dcl_resource_buffer") ||
        !strstr(t, "v2.xyzw, l(765.299988, 765.299988, 765.299988, 765.299988)")) { return -1; }
    for (const char* p = strstr(t, "ige r"); p; p = strstr(p + 1, "ige r")) {
        const char* e = strchr(p, '\n');
        const char* c = strstr(p, ", cb0[");
        if (c && (!e || c < e)) {
            const int row = atoi(c + 6);
            const char* b = strchr(c, ']');
            if (b && b[1] == '.' && b[2] == 'x' && row >= 0 && row < 48) { return row; }
        }
    }
    return -1;
}

// The rules applied to an ISGN result and the disassembly text (the offline harness calls this directly).
inline Class FromText(const Sig& s, const char* t, Reason* why)
{
    const bool sigB = Supplies(s, kOurB), sigA = Supplies(s, kOurA), sigS = Supplies(s, kOurS);
    const int cb2 = CbRows(t, 2), cb4 = CbRows(t, 4);
    Reason r = kSignature;
    if (sigB) {
        if (cb2 < 7) { r = kBuffers; }
        else if (HasDp4Row(t, 5) && HasDp4Row(t, 6) && HasDp4Row(t, 7)) { *why = kOk; return kRigidB; }
        else { r = kMath; }
    } else if (sigA) {
        if (cb2 < 7) { r = kBuffers; }
        else if (HasDp4Row(t, 4) && HasDp4Row(t, 5) && HasDp4Row(t, 6)) { *why = kOk; return kRigidA; }
        else { r = kMath; }
    } else if (Supplies(s, kOurC)) {                                   // s2065's form: no TANGENT, rows at v3..v5
        if (cb2 < 7) { r = kBuffers; }
        else if (HasDp4Row(t, 3) && HasDp4Row(t, 4) && HasDp4Row(t, 5)) { *why = kOk; return kRigidC; }
        else { r = kMath; }
    } else if (Supplies(s, kOurD)) {                                   // the buggy's a68f15c7: form A + COLOR, rows at v5..v7
        if (cb2 < 7) { r = kBuffers; }
        else if (HasDp4Row(t, 5) && HasDp4Row(t, 6) && HasDp4Row(t, 7)) { *why = kOk; return kRigidD; }
        else { r = kMath; }
    } else if (Supplies(s, kOurE)) {                                   // the buggy's 4f2f75b7: COLOR for TANGENT, rows at v4..v6
        if (cb2 < 7) { r = kBuffers; }
        else if (HasDp4Row(t, 4) && HasDp4Row(t, 5) && HasDp4Row(t, 6)) { *why = kOk; return kRigidE; }
        else { r = kMath; }
    } else if (Supplies(s, kOurC1)) {                                  // INV-751: form C + TEXCOORD1 at v3, rows at v4..v6
        if (cb2 < 7) { r = kBuffers; }
        else if (HasDp4Row(t, 4) && HasDp4Row(t, 5) && HasDp4Row(t, 6)) { *why = kOk; return kRigidC1; }
        else { r = kMath; }
    }
    if (sigS) {
        const int mb = MorphBase(t);
        if (cb2 < 7 || cb4 < 1536) { if (r == kSignature) { r = kBuffers; } }
        else if (mb >= 0) { *why = static_cast<Reason>(16 + mb); return kSkinnedM; }   // why carries the morph row
        else if (HasPaletteMath(t)) { *why = kOk; return kSkinned; }
        else if (r == kSignature || r == kBuffers) { r = kMath; }
    } else if (Supplies(s, kOurS1) && !HasSemantic(s, "BLENDWEIGHT")) {  // s3875's form: one bone, no weights
        if (cb2 < 7 || cb4 < 1536) { if (r == kSignature) { r = kBuffers; } }
        else if (HasPaletteMath1(t)) { *why = kOk; return kSkinned1; }
        else if (r == kSignature || r == kBuffers) { r = kMath; }
    } else if (Supplies(s, kOurSC)) {                                    // the buggy's e40d02f6: COLOR at v2, BLENDINDICES v3
        if (cb2 < 7 || cb4 < 1536) { if (r == kSignature) { r = kBuffers; } }
        else if (HasPaletteMath(t, 3)) { *why = kOk; return kSkinnedC; }
        else if (r == kSignature || r == kBuffers) { r = kMath; }
    }
    *why = r;
    return kNone;
}

// The whole classification of one vertex shader's bytecode. *disasm = 1 when D3DDisassemble ran.
inline Class Classify(const void* code, size_t n, Reason* why, int* disasm)
{
    *disasm = 0;
    Sig s;
    if (!ParseIsgn(code, n, &s)) { *why = kNotDxbc; return kNone; }
    if (!Supplies(s, kOurA) && !Supplies(s, kOurB) && !Supplies(s, kOurS) && !Supplies(s, kOurC) &&
        !(Supplies(s, kOurS1) && !HasSemantic(s, "BLENDWEIGHT")) && !Supplies(s, kOurD) && !Supplies(s, kOurE) &&
        !Supplies(s, kOurSC) && !Supplies(s, kOurC1)) { *why = kSignature; return kNone; }
    ID3DBlob* text = nullptr;
    *disasm = 1;
    if (FAILED(D3DDisassemble(code, n, 0, nullptr, &text)) || !text) { *why = kDisasmFailed; return kNone; }
    const Class c = FromText(s, static_cast<const char*>(text->GetBufferPointer()), why);
    text->Release();
    return c;
}

// PIXEL shaders (2026-10-08, INV-486): true = the SHEX / SHDR token stream holds a `discard` (opcode 13) - an
// alpha-tested surface. A token walk, no disassembly: each instruction's length is bits 24-30 of its opcode token,
// customdata (opcode 0x35) carries it in the next dword. Checked offline over the pack dump: 3790 / 3790 pixel shaders agree
// with fxc /dumpbin's text (650 discard, 3140 not; the Volkatronix sight d7ab1613 yes, the gun's other PSs no). A malformed
// or truncated stream returns false (nothing is left out of the held-pose map on a guess). COST: microseconds, at creation.
inline bool HasDiscard(const void* code, size_t n)
{
    const unsigned char* b = static_cast<const unsigned char*>(code);
    if (!b || n < 32 || memcmp(b, "DXBC", 4) != 0) { return false; }
    const unsigned chunks = Rd32(b, 28);
    if (chunks > 64 || 32 + 4ull * chunks > n) { return false; }
    for (unsigned i = 0; i < chunks; ++i) {
        const unsigned off = Rd32(b, 32 + 4 * i);
        if (off + 16ull > n || (memcmp(b + off, "SHEX", 4) != 0 && memcmp(b + off, "SHDR", 4) != 0)) { continue; }
        const unsigned csz = Rd32(b, off + 4);
        const size_t c = off + 8;
        const unsigned ndw = Rd32(b, c + 4);
        if (4ull * ndw > csz || c + 4ull * ndw > n) { return false; }
        for (unsigned pos = 2; pos < ndw;) {
            const unsigned tok = Rd32(b, c + 4ull * pos);
            const unsigned op = tok & 0x7FF;
            unsigned len = (tok >> 24) & 0x7F;
            if (op == 0x35) { if (pos + 1 >= ndw) { return false; } len = Rd32(b, c + 4ull * (pos + 1)); }
            if (len == 0) { return false; }
            if (op == 13) { return true; }
            pos += len;
        }
        return false;
    }
    return false;
}

inline const char* Name(unsigned char c)
{
    switch (c) {
    case kRigidA: return "rigid A"; case kRigidB: return "rigid B"; case kSkinned: return "skinned";
    case kSkinned1: return "single-bone skinned"; case kRigidC: return "rigid C";
    case kRigidD: return "rigid D"; case kRigidE: return "rigid E"; case kSkinnedC: return "skinned (COLOR, BLENDINDICES v3)";
    case kSkinnedM: return "skinned, morph targets"; case kRigidC1: return "rigid C1";
    case kNone: return "none"; default: return "unclassified (created before the hook)";
    }
}
inline const char* WhyName(unsigned char r)
{
    if (r >= 16) { return "ok (morph row in why)"; }
    switch (r) {
    case kOk: return "ok"; case kNotDxbc: return "not DXBC / no ISGN"; case kSignature: return "input signature fits none of ours";
    case kBuffers: return "constant buffers not CB2>=7 (+CB4>=1536 skinned)"; case kMath: return "signature + buffers fit, math does not";
    default: return "D3DDisassemble failed";
    }
}

}  // namespace vsclass
