// Standalone unit test for src/src/setup/setup_view.h - NOT part of any DLL build.
// docs/superpowers/specs/2026-10-17-installer-design.md 8.2 / 8.5 (the window's header and its guards). Exit = failures.
#include "setup_view.h"

#include <cstdio>

using namespace dlvr::setup;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static ViewModel Vm(HeaderState h)
{
    ViewModel v;
    v.header = h;
    return v;
}

static void ui_close_blocked_while_installing()
{
    ViewModel v = Vm(HeaderState::Install);
    CHECK(CanClose(v));
    v.installing = true;
    CHECK(!CanClose(v));
    v.installing = false;
    v.done = true;
    CHECK(CanClose(v));
}
static void primary_disabled_installing_and_installed()
{
    ViewModel v = Vm(HeaderState::Install);
    CHECK(PrimaryEnabled(v));
    v.installing = true;
    CHECK(!PrimaryEnabled(v));
    CHECK(PrimaryEnabled(Vm(HeaderState::Update)));
    CHECK(!PrimaryEnabled(Vm(HeaderState::Installed)));
    ViewModel blocked = Vm(HeaderState::Install);
    blocked.blocked = true;   // Program Files / game running / no folder
    CHECK(!PrimaryEnabled(blocked));
}
static void status_texts()
{
    CHECK(HeaderStatus(Vm(HeaderState::Install)) == "Ready to install");
    CHECK(HeaderStatus(Vm(HeaderState::Update)) == "Update available");
    CHECK(HeaderStatus(Vm(HeaderState::Installed)) == "Installed - start Dying Light from your store");
    ViewModel i = Vm(HeaderState::Install);
    i.installing = true;
    CHECK(HeaderStatus(i) == "Installing...");
    ViewModel f = Vm(HeaderState::Update);
    f.failed = true;
    CHECK(HeaderStatus(f) == "Setup FAILED - see below");
    CHECK(std::string(PrimaryLabel(Vm(HeaderState::Install))) == "Install");
    CHECK(std::string(PrimaryLabel(Vm(HeaderState::Update))) == "Update");
    CHECK(std::string(PrimaryLabel(Vm(HeaderState::Installed))) == "Installed");
}
static void progress_fraction()
{
    ViewModel v = Vm(HeaderState::Install);
    CHECK(ProgressFraction(v) == 0.0f);
    v.progressNum = 21;
    v.progressDen = 42;
    CHECK(ProgressFraction(v) == 0.5f);
    v.progressNum = 50;
    CHECK(ProgressFraction(v) == 1.0f);
}

int main()
{
    ui_close_blocked_while_installing();
    primary_disabled_installing_and_installed();
    status_texts();
    progress_fraction();
    std::printf("setup_view_test: %d failure(s)\n", g_fail);
    return g_fail;
}
