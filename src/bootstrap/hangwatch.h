// START-UP HANG WATCH - A DIAGNOSTIC BUILD (CMake -DDLVR_HANG_WATCH=ON). Without that option
// both functions are empty and nothing is started. See hangwatch.cpp.
#pragma once

#include <windows.h>

namespace dlvr {
namespace hangwatch {

// DllMain attach, after LoadRealXInput. Starts the watchdog thread (built with DLVR_HANG_WATCH only).
void Start(HMODULE realXInput);

// Every engine log line (the bootstrap's three _CLog hooks). Two substring tests until InitSDL has passed, then one flag test.
void OnEngineLine(const char* line);

}  // namespace hangwatch
}  // namespace dlvr
