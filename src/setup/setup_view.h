// The setup window's header and guards, as a pure model (docs/superpowers/specs/2026-10-17-installer-design.md 8.2 / 8.5).
// setup_ui.cpp draws from it; tested by src/src/tests/setup_view_test.cpp.
#pragma once

#include <string>
#include <vector>

#include "setup_record.h"

namespace dlvr::setup {

struct ViewModel {
    HeaderState header = HeaderState::Install;
    bool installing = false;   // the worker thread is running
    bool done = false;         // the last run finished OK
    bool failed = false;       // the last run FAILED
    bool blocked = false;      // nothing may be installed: no folder, Program Files, game running
    std::string status;
    int progressNum = 0;
    int progressDen = 0;
    std::string currentFile;
    std::vector<std::string> errors;
};

// Close is ignored while files are being written: a half-written install never goes unreported.
inline bool CanClose(const ViewModel& v) { return !v.installing; }

// The header button: never twice at once, never on a blocked folder, not on an install that already matches.
inline bool PrimaryEnabled(const ViewModel& v)
{
    return !v.installing && !v.blocked && v.header != HeaderState::Installed;
}

inline const char* PrimaryLabel(const ViewModel& v)
{
    switch (v.header) {
    case HeaderState::Update: return "Update";
    case HeaderState::Installed: return "Installed";
    default: return "Install";
    }
}

inline std::string HeaderStatus(const ViewModel& v)
{
    if (v.installing) { return "Installing..."; }
    if (v.failed) { return "Setup FAILED - see below"; }
    switch (v.header) {
    case HeaderState::Update: return "Update available";
    case HeaderState::Installed: return "Installed - start Dying Light from your store";
    default: return "Ready to install";
    }
}

inline float ProgressFraction(const ViewModel& v)
{
    if (v.progressDen <= 0) { return 0.0f; }
    const float f = static_cast<float>(v.progressNum) / static_cast<float>(v.progressDen);
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

}  // namespace dlvr::setup
