// Standalone unit test for src_beta/src/bootstrap/vsclass.h's form C1 (INV-751, trace docs/traces/20261119_1840_weapon_stage_rekey_c1.md)
// - NOT part of the DLL build. Runs the REAL classifier (vsclass::Classify, D3DDisassemble) over REAL vertex shaders cut from OUR shader
// pack (optimized_dx11.mp a00bdd80; cut by backups/20261119_weapon_census/vs_cut.py). Exit code = number of failures.
//   vsclass_c1_test <c1 dir> <none dir> <ctl dir>
//     <c1 dir>   : every *.dxbc must classify kRigidC1 (the 36 C1 shaders reachable by weapon materials)
//     <none dir> : every *.dxbc must stay kNone (ems / zfill / lpp / rfr / trn_last / opq_clr shaders - not GBuffer, INV-751 (3))
//     <ctl dir>  : vs_BAF917 (the machete's clean blade a90f4b13) must stay kRigidC; vs_6E6D9B (wn_axe_g opq) and vs_52A9D7 (the
//                  buggy's rigid A) must stay kRigidA - no existing class moves
// Build: cl /nologo /EHsc /W4 /std:c++17 /I <tree>\src\bootstrap vsclass_c1_test.cpp d3dcompiler.lib
#include "vsclass.h"

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

static int g_fail = 0;

static std::vector<std::string> Dxbc(const std::string& dir)
{
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.dxbc").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { return out; }
    do { out.push_back(dir + "\\" + fd.cFileName); } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

static bool Load(const std::string& p, std::vector<unsigned char>* b)
{
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") != 0 || !f) { return false; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b->resize(n > 0 ? static_cast<size_t>(n) : 0);
    const size_t got = n > 0 ? fread(b->data(), 1, b->size(), f) : 0;
    fclose(f);
    return n > 0 && got == b->size();
}

static int Expect(const std::string& p, vsclass::Class want)
{
    std::vector<unsigned char> b;
    if (!Load(p, &b)) { std::printf("FAIL cannot read %s\n", p.c_str()); ++g_fail; return 0; }
    vsclass::Reason why = vsclass::kOk;
    int dis = 0;
    const vsclass::Class c = vsclass::Classify(b.data(), b.size(), &why, &dis);
    if (c != want) {
        std::printf("FAIL %s: %s (%s), want %s\n", p.c_str(), vsclass::Name(c), vsclass::WhyName(why), vsclass::Name(want));
        ++g_fail;
    }
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 4) { std::printf("usage: vsclass_c1_test <c1 dir> <none dir> <ctl dir>\n"); return 99; }
    const auto c1 = Dxbc(argv[1]);
    const auto none = Dxbc(argv[2]);
    // the sets must be the ones the census found - an empty or short directory is a FAILURE, not a pass
    if (c1.size() != 36) { std::printf("FAIL c1 dir holds %zu .dxbc, want 36\n", c1.size()); ++g_fail; }
    if (none.size() != 13) { std::printf("FAIL none dir holds %zu .dxbc, want 13\n", none.size()); ++g_fail; }
    int n = 0;
    for (const auto& p : c1) { n += Expect(p, vsclass::kRigidC1); }
    for (const auto& p : none) { n += Expect(p, vsclass::kNone); }
    const std::string ctl = argv[3];
    n += Expect(ctl + "\\vs_BAF917.dxbc", vsclass::kRigidC);
    n += Expect(ctl + "\\vs_6E6D9B.dxbc", vsclass::kRigidA);
    n += Expect(ctl + "\\vs_52A9D7.dxbc", vsclass::kRigidA);
    std::printf("vsclass_c1_test: %d shaders classified, %d failure(s)\n", n, g_fail);
    return g_fail;
}
