// Standalone unit test for src_beta/src/common/binds_logic.h - NOT part of the DLL build.
// docs/superpowers/plans/2026-10-28-controls-rebind.md Task 2. Exit code = number of failures.
// argv[1] = the golden dlvr_binds.default.scr written by `py -3 tools/binds_template.py gen <dir>` (EmitScr must match it).
#include "binds_logic.h"

#include <cstdio>
#include <string>

using namespace dlvr::binds;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static bool SameBinds(const Binds& a, const Binds& b)
{
    for (int i = 0; i < gen::kActionCount; ++i) {
        for (int s = 0; s < 2; ++s) {
            if (!Equal(a.s[i][s], b.s[i][s])) { return false; }
        }
    }
    return true;
}

static std::string ReadFile(const char* path)
{
    std::string out;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) { return out; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) { out.append(buf, n); }
    fclose(f);
    return out;
}

int main(int argc, char** argv)
{
    const Binds def = Defaults();
    const int kick = FindAction("_ACTION_KICK");
    const int tackle = FindAction("_ACTION_TACLE");
    const int duck = FindAction("_ACTION_DUCK");
    const int lookUp = FindAction("_ACTION_ROTATE_UP");
    const int lshoulder = FindInput("EJoy__BUTTON_LEFTSHOULDER");
    const int r3 = FindInput("EJoy__BUTTON_RIGHTTHUMB");
    CHECK(kick >= 0 && tackle >= 0 && duck >= 0 && lookUp >= 0 && lshoulder >= 0 && r3 >= 0);
    CHECK(FindAction("_ACTION_NOPE") == -1);
    CHECK(FindInput("EJoy__BUTTON_NOPE") == -1);

    // defaults: kick on R3, slot 2 unbound; look up starts unbound; the physical-crouch hold is hidden
    CHECK(def.s[kick][0].bound && def.s[kick][0].input == r3 && !def.s[kick][0].analog);
    CHECK(!def.s[kick][1].bound);
    CHECK(!def.s[lookUp][0].bound);
    CHECK(Hidden(duck) && !Hidden(kick));

    // EmitScr(Defaults) == the generator's golden file, byte for byte (C++ and Python agree)
    if (argc > 1) {
        const std::string golden = ReadFile(argv[1]);
        CHECK(!golden.empty());
        CHECK(EmitScr(def) == golden);
    } else {
        std::printf("FAIL: no golden file argument\n");
        ++g_fail;
    }

    // round trip of the defaults; empty text = defaults, nothing rejected
    {
        Binds b = Defaults();
        std::string rep;
        CHECK(Serialize(def).empty());
        CHECK(Parse(Serialize(def).c_str(), &b, &rep) == 0);
        CHECK(SameBinds(b, def));
        CHECK(Parse("", &b, &rep) == 0 && SameBinds(b, def));
    }
    // kick on the left grip, slot 2 unbound: only that row changes
    {
        Binds b = Defaults();
        std::string rep;
        CHECK(Parse("_ACTION_KICK EJoy__BUTTON_LEFTSHOULDER,0,0,0 -\n", &b, &rep) == 0);
        CHECK(b.s[kick][0].bound && b.s[kick][0].input == lshoulder);
        CHECK(!b.s[kick][1].bound);
        Binds want = def;
        want.s[kick][0] = b.s[kick][0];
        CHECK(SameBinds(b, want));
        CHECK(Equal(b.s[tackle][0], def.s[tackle][0]));                     // tackle stays on L1 (rule 3: both fire)
        CHECK(Serialize(b) == "_ACTION_KICK EJoy__BUTTON_LEFTSHOULDER,0,0,0 -\n");
        const std::string scr = EmitScr(b);
        CHECK(scr.find("AddAction(_ACTION_KICK, EInputTarget_Player, EInputDevice_Analog, false, EJoy__BUTTON_LEFTSHOULDER, false, false);")
              != std::string::npos);
        CHECK(scr.find("_ACTION_KICK, EInputTarget_Player, EInputDevice_Analog, false, EJoy__BUTTON_RIGHTTHUMB") == std::string::npos);
    }
    // a bad line is rejected by name, every other line still applies (Review Focus 1)
    {
        Binds b = Defaults();
        std::string rep;
        const int bad = Parse("_ACTION_NOPE EJoy__BUTTON_A,0,0,0 -\n"
                              "_ACTION_KICK EJoy__BUTTON_LEFTSHOULDER,0,0,0 -\n"
                              "_ACTION_TACLE EJoy__BUTTON_A,2 -\n"
                              "_ACTION_JUMP EJoy__BUTTON_NOPE,0,0,0 -\n"
                              "_ACTION_DUCK EJoy__BUTTON_A,0,0,0 -\n", &b, &rep);
        CHECK(bad == 4);
        CHECK(rep.find("_ACTION_NOPE") != std::string::npos);
        CHECK(rep.find("EJoy__BUTTON_NOPE") != std::string::npos);
        CHECK(rep.find("_ACTION_DUCK") != std::string::npos);
        CHECK(b.s[kick][0].input == lshoulder);                               // the good line applied
        CHECK(Equal(b.s[tackle][0], def.s[tackle][0]));                       // the malformed one did not
    }
    // slot 2 equal to slot 1 is stored once (Review Focus 3); comments and blank lines are ignored
    {
        Binds b = Defaults();
        std::string rep;
        CHECK(Parse("# comment\n\n_ACTION_KICK EJoy__BUTTON_LEFTSHOULDER,0,0,0 EJoy__BUTTON_LEFTSHOULDER,0,0,0\r\n", &b, &rep) == 0);
        CHECK(b.s[kick][0].bound && !b.s[kick][1].bound);
    }
    // a second slot: kick on R3 AND the left grip - both lines emitted, slot 2 right after slot 1
    {
        Binds b = Defaults();
        std::string rep;
        CHECK(Parse("_ACTION_KICK EJoy__BUTTON_RIGHTTHUMB,0,0,0 EJoy__BUTTON_LEFTSHOULDER,0,0,0\n", &b, &rep) == 0);
        const std::string scr = EmitScr(b);
        const size_t p1 = scr.find("_ACTION_KICK, EInputTarget_Player, EInputDevice_Analog, false, EJoy__BUTTON_RIGHTTHUMB");
        const size_t p2 = scr.find("_ACTION_KICK, EInputTarget_Player, EInputDevice_Analog, false, EJoy__BUTTON_LEFTSHOULDER");
        CHECK(p1 != std::string::npos && p2 != std::string::npos && p2 > p1);
        CHECK(scr.find("_ACTION_TACLE") > p2);
    }
    // a stick direction: right stick up for sprint (analog flag as given, reverse 0 = up, half 1)
    {
        Binds b = Defaults();
        std::string rep;
        CHECK(Parse("_ACTION_SPRINT_TOGGLE EJoy__AXIS_RY,0,0,1 -\n", &b, &rep) == 0);
        CHECK(EmitScr(b).find("AddAction(_ACTION_SPRINT_TOGGLE, EInputTarget_Player, EInputDevice_Analog, false, EJoy__AXIS_RY, false, true);")
              != std::string::npos);
    }
    // a duplicate action line: the second is rejected, the first stands
    {
        Binds b = Defaults();
        std::string rep;
        CHECK(Parse("_ACTION_KICK EJoy__BUTTON_A,0,0,0 -\n_ACTION_KICK EJoy__BUTTON_B,0,0,0 -\n", &b, &rep) == 1);
        CHECK(b.s[kick][0].input == FindInput("EJoy__BUTTON_A"));
    }

    std::printf("%s - %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
