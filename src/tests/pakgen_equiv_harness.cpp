// Standalone harness - NOT part of the DLL build. docs/DESIGN-COOP-FIXED-DATA3-2026-10-10.md §4.1 (step 1).
//
// Runs the REAL pakgen (bootstrap/pakgen.cpp, compiled in unchanged) against a SCRATCH game folder, so the equivalence test
// (tools/pp_equiv_test.py) can compare today's generated render_loop / oculus / sprite50 text, per configuration, with the
// fixed merged texts resolved by that configuration's dlvr_pp.mth values. Never point it at the real game folder: it writes
// <gameDir>\DW\Data3.pak in place, exactly as the bootstrap does.
//
// usage: pakgen_equiv_harness.exe <dlvrDir> <gameDir>      exit 0 = pakgen ran (whatever it decided - read its log)
// pakgen's log goes to dlvr_bootstrap.log NEXT TO THIS EXE (common/log.h resolves the module's own folder).
#include "../bootstrap/pakgen.cpp"

#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::printf("usage: %s <dlvrDir> <gameDir>\n", argv[0]);
        return 2;
    }
    if (strstr(argv[2], "steamapps") != nullptr) {
        std::printf("REFUSED: %s looks like the real game folder - this harness only runs against a scratch copy\n", argv[2]);
        return 3;
    }
    // Step 2: on a LEGACY pak the game never writes DW\dlvr_pp.mth; the harness asks for it so the test can compare the
    // values pakgen decided on today's path with the ones it writes on the FIXED pak and with the Python spec.
    g_ppWriteInLegacy = true;
    GenerateRenderLoopPak(argv[1], argv[2]);
    std::printf("pakgen ran: dlvr=%s game=%s\n", argv[1], argv[2]);
    return 0;
}
