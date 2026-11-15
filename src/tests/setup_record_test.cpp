// Standalone unit test for src/src/setup/setup_record.h and the revision-2 additions to setup_plan.h - NOT part of any
// DLL build. docs/superpowers/specs/2026-10-17-installer-design.md 8.5 (the header state, the install record, the
// dropdowns on an existing install). Exit code = number of failures.
#include "setup_record.h"
#include "setup_plan.h"

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

static const std::string kMd5 = "0123456789abcdef0123456789abcdef";

static void record_roundtrip()
{
    const std::string t = RecordText("DyingLightVR_Setup_20261017", kMd5);
    CHECK(t == "package=DyingLightVR_Setup_20261017\r\nmanifest=" + kMd5 + "\r\n");
    std::string p, m;
    CHECK(ParseRecord(t, &p, &m) && p == "DyingLightVR_Setup_20261017" && m == kMd5);
    CHECK(ParseRecord("\xEF\xBB\xBFmanifest=" + kMd5 + "\npackage=X\n", &p, &m) && p == "X" && m == kMd5);
    CHECK(!ParseRecord("", &p, &m));
    CHECK(!ParseRecord("package=X\r\n", &p, &m));
}
static void header_not_present_install()
{
    CHECK(HeaderFor(false, false, "", "", "P", kMd5, true) == HeaderState::Install);
    CHECK(HeaderFor(false, true, "P", kMd5, "P", kMd5, true) == HeaderState::Install);
}
static void header_match_installed()
{
    CHECK(HeaderFor(true, true, "P", kMd5, "P", kMd5, true) == HeaderState::Installed);
}
// Review finding 2: a matching record over files that are no longer all there (a store verify, a game patch, antivirus)
// is an Update, so the user can repair without Reset.
static void header_installed_needs_intact_files()
{
    CHECK(HeaderFor(true, true, "P", kMd5, "P", kMd5, false) == HeaderState::Update);
}
static void record_other_package_is_update()
{
    CHECK(HeaderFor(true, true, "OLD", kMd5, "P", kMd5, true) == HeaderState::Update);
    CHECK(HeaderFor(true, true, "P", "ffffffffffffffffffffffffffffffff", "P", kMd5, true) == HeaderState::Update);
}
static void record_none_present_is_update()
{
    CHECK(HeaderFor(true, false, "", "", "P", kMd5, true) == HeaderState::Update);
}

static DetectedState Disk()
{
    DetectedState s;
    s.modPresent = true;
    s.haveOvr2xr = true;
    s.ovr2xrText = "comfort_turn_mode=0\r\ncomfort_body_follows_head=1\r\nafw_enable=0\r\nui_hud_panel=1\r\nfoo=3\r\n";
    s.aaMethodText = "\xEF\xBB\xBF" "dlss\r\n";
    s.dlssQualityText = "0\r\n";
    s.present = {"dlvr/aa_method.txt", "dlvr/dlss_quality.txt", "dlvr/ovr2xr.txt"};
    return s;
}
static const Action* Find(const std::vector<Action>& v, ActKind k, const std::string& path)
{
    for (const auto& a : v) {
        if (a.kind == k && a.relPath == path) { return &a; }
    }
    return nullptr;
}
static void ondisk_reads_markers()
{
    const OnDisk d = ReadOnDisk(Disk());
    CHECK(d.haveAa && d.method == AaWord::Dlss && d.quality == 0);
    CHECK(d.haveTurn && !d.snap);
    CHECK(d.haveHud && d.hudPanel);
    DetectedState e{};
    const OnDisk n = ReadOnDisk(e);
    CHECK(!n.haveAa && !n.haveTurn && !n.haveHud);
    DetectedState f = Disk();
    f.aaMethodText = "fsr4\n";
    f.dlssQualityText = "garbage";
    const OnDisk g = ReadOnDisk(f);
    CHECK(!g.haveAa);   // a method without a readable quality is not a usable on-disk answer
}
static void ui_untouched_keep_writes_nothing()
{
    const DetectedState s = Disk();
    const Answers a = AnswersFromUi(ReadOnDisk(s), true, AaWord::Dlss, 0, false, true, false, true);
    CHECK(a.keep && !a.haveGraphics && !a.haveTurn && !a.haveFollow && !a.haveAfw && !a.haveHud);
    const auto v = PlanInstall(s, a, {}, {}, false);
    CHECK(Find(v, ActKind::WriteText, "dlvr/aa_method.txt") == nullptr);
    const Action* o = Find(v, ActKind::WriteText, "dlvr/ovr2xr.txt");
    CHECK(o && o->text == s.ovr2xrText);
}
static void ui_changed_turn_writes_only_turn()
{
    const DetectedState s = Disk();
    const Answers a = AnswersFromUi(ReadOnDisk(s), true, AaWord::Dlss, 0, true, true, false, true);
    CHECK(!a.haveGraphics && a.haveTurn && a.snap && !a.haveFollow && !a.haveAfw && !a.haveHud);
    const auto v = PlanInstall(s, a, {}, {}, false);
    const Action* o = Find(v, ActKind::WriteText, "dlvr/ovr2xr.txt");
    CHECK(o && o->text == "comfort_turn_mode=1\r\ncomfort_body_follows_head=1\r\nafw_enable=0\r\nui_hud_panel=1\r\nfoo=3\r\n");
}
static void ui_missing_value_written()
{
    DetectedState s = Disk();
    s.ovr2xrText = "comfort_turn_mode=0\r\n";
    const Answers a = AnswersFromUi(ReadOnDisk(s), true, AaWord::Dlss, 0, false, true, false, true);
    CHECK(a.haveHud && a.hudPanel && !a.haveTurn);
    CHECK(a.haveAfw && !a.afw);   // afw_enable missing on disk: written on a keep, with the shown value
    CHECK(a.haveFollow && a.follow);   // comfort_body_follows_head missing on disk: written on a keep
}
static void reset_all_writes_all()
{
    const Answers a = AnswersFromUi(ReadOnDisk(Disk()), false, AaWord::Dlss, 0, false, true, false, true);
    CHECK(!a.keep && a.haveGraphics && a.haveTurn && a.haveFollow && a.haveAfw && a.haveHud);
}
static void plan_writes_record_last_write()
{
    const Answers a = AnswersFromUi(ReadOnDisk(Disk()), false, AaWord::Dlss, 1, true, true, false, true);
    const auto v = PlanInstall(Disk(), a, {}, {}, false, "package=P\r\nmanifest=" + kMd5 + "\r\n");
    int lastWrite = -1;
    int recordAt = -1;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].kind == ActKind::WriteText) { lastWrite = static_cast<int>(i); }
        if (v[i].kind == ActKind::WriteText && v[i].relPath == "dlvr/setup_installed.txt") { recordAt = static_cast<int>(i); }
    }
    CHECK(recordAt >= 0 && recordAt == lastWrite);
    CHECK(v[static_cast<size_t>(recordAt)].text == "package=P\r\nmanifest=" + kMd5 + "\r\n");
    // review finding 1: the old record is deleted FIRST (an interrupted or failed run must never leave a matching
    // record), whether or not it is present now
    CHECK(!v.empty() && v.front().kind == ActKind::Delete && v.front().relPath == "dlvr/setup_installed.txt");
    // ... and the record write is marked to happen only after the 5 s re-check (Execute defers it)
    CHECK(v[static_cast<size_t>(recordAt)].afterRecheck);
    // remove deletes it first too
    const auto r = PlanRemove(Disk());
    CHECK(!r.empty() && r.front().kind == ActKind::Delete && r.front().relPath == "dlvr/setup_installed.txt");
    // no record text = no record action (the v1 call shape)
    CHECK(Find(PlanInstall(Disk(), a, {}, {}, false), ActKind::WriteText, "dlvr/setup_installed.txt") == nullptr);
}

// An install made by an older package carries a third line `tester=<code>`: it still parses, and nothing writes one now.
static void old_record_with_tester_line_parses()
{
    std::string p, m;
    CHECK(ParseRecord("package=P\r\nmanifest=" + kMd5 + "\r\ntester=DLVR-07-K3QX\r\n", &p, &m) && p == "P" && m == kMd5);
    CHECK(RecordText("P", kMd5) == "package=P\r\nmanifest=" + kMd5 + "\r\n");
    Answers a = AnswersFromUi(ReadOnDisk(Disk()), false, AaWord::Dlss, 1, true, true, false, true);
    CHECK(Find(PlanInstall(Disk(), a, {}, {}, false), ActKind::WriteText, "dlvr/tester.txt") == nullptr);
}

int main()
{
    record_roundtrip();
    header_not_present_install();
    header_match_installed();
    header_installed_needs_intact_files();
    record_other_package_is_update();
    record_none_present_is_update();
    ondisk_reads_markers();
    ui_untouched_keep_writes_nothing();
    ui_changed_turn_writes_only_turn();
    ui_missing_value_written();
    reset_all_writes_all();
    plan_writes_record_last_write();
    old_record_with_tester_line_parses();
    std::printf("setup_record_test: %d failure(s)\n", g_fail);
    return g_fail;
}
