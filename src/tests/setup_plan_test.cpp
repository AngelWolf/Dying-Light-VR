// Standalone unit test for src/src/setup/setup_manifest.h + setup_plan.h - NOT part of any DLL build.
// docs/superpowers/specs/2026-10-17-installer-design.md 5.2, 5.7, 5.8, 5.9, 5.10. Exit code = number of failures.
#include "setup_plan.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

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

static std::vector<ManifestEntry> Manifest()
{
    return ParseManifest("# test manifest\n" + kMd5 + "  LibOVRRT64_1.dll\n" + kMd5 + "  xinput1_3.dll\n" + kMd5 +
                         "  nvngx_dlss.dll dlss-only\n" + kMd5 + "  amd_fidelityfx_upscaler_dx12.dll\n" + kMd5 +
                         "  DW/Data3.pak\n" + kMd5 + "  dlvr/dlss_preset.txt\n" + kMd5 + "  dlvr/force_oculus.txt\n");
}

static Answers AllAnswered(bool keep, AaWord m, int q)
{
    Answers a{};
    a.keep = keep;
    a.keepArmFit = true;
    a.method = m;
    a.quality = q;
    a.snap = true;
    a.follow = true;
    a.afw = false;
    a.hudPanel = true;
    a.haveGraphics = a.haveTurn = a.haveFollow = a.haveAfw = a.haveHud = true;
    return a;
}

static const Action* Find(const std::vector<Action>& v, ActKind k, const std::string& path)
{
    for (const auto& a : v) {
        if (a.kind == k && a.relPath == path) {
            return &a;
        }
    }
    return nullptr;
}

static DetectedState Existing()
{
    DetectedState s;
    s.modPresent = true;
    s.haveOvr2xr = true;
    s.ovr2xrText = "comfort_turn_mode=0\r\nfoo=3\r\n";
    s.aaMethodText = "dlss\r\n";
    s.present = {"libovrrt64_1.dll", "dlvr/aa_method.txt", "dlvr/dlss_quality.txt", "dlvr/dlss_preset.txt",
                 "dlvr/dlss_sharpness.txt", "dlvr/eye_res.txt", "dlvr/hmd_cache.txt", "dlvr/arm_fit.txt",
                 "dlvr/gpu_frame.txt", "dlvr/ovr2xr.txt", "nvngx_dlss.dll"};
    return s;
}

static void manifest_parses()
{
    const auto m = Manifest();
    CHECK(m.size() == 7);
    CHECK(m[2].relPath == "nvngx_dlss.dll" && m[2].dlssOnly);
    CHECK(m[4].relPath == "DW/Data3.pak" && !m[4].dlssOnly);
    CHECK(ParseManifest(kMd5 + "  DW\\Data3.pak\r\n")[0].relPath == "DW/Data3.pak");
}
static void manifest_malformed_throws()
{
    bool threw = false;
    try { ParseManifest("nothex  file.dll\n"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { ParseManifest(kMd5 + "file.dll\n"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}
static void questions_fresh_all_four()
{
    DetectedState s{};
    const auto q = QuestionsToAsk(s, true);
    CHECK(q == std::vector<std::string>({"graphics", "turn", "follow", "afw", "hud"}));
    CHECK(QuestionsToAsk(Existing(), false) == std::vector<std::string>({"graphics", "turn", "follow", "afw", "hud"}));
}
static void questions_keep_only_missing()
{
    CHECK(QuestionsToAsk(Existing(), true) == std::vector<std::string>({"follow", "afw", "hud"}));
    DetectedState s = Existing();
    s.present.erase("dlvr/aa_method.txt");
    s.aaMethodText.clear();
    CHECK(QuestionsToAsk(s, true) == std::vector<std::string>({"graphics", "follow", "afw", "hud"}));
    s.ovr2xrText = "comfort_turn_mode=0\r\ncomfort_body_follows_head=0\r\nafw_enable=1\r\nfoo=3\r\n";
    CHECK(QuestionsToAsk(s, true) == std::vector<std::string>({"graphics", "hud"}));
}
// Jay 2026-10-18: "Make sure AFW is also now a switch upon the installer" - afw_enable, default OFF (Jay's answer), read and
// written like the turn question.
static void afw_read_from_disk()
{
    DetectedState s = Existing();
    CHECK(!ReadOnDisk(s).haveAfw);
    s.ovr2xrText = "afw_enable=0\r\n";
    CHECK(ReadOnDisk(s).haveAfw && !ReadOnDisk(s).afw);
    s.ovr2xrText = "afw_enable=1\r\n";
    CHECK(ReadOnDisk(s).haveAfw && ReadOnDisk(s).afw);
    s.ovr2xrText = "afw_enable=7\r\n";
    CHECK(!ReadOnDisk(s).haveAfw);
}
static void afw_default_is_off()
{
    CHECK(!Answers{}.afw);
    CHECK(!OnDisk{}.afw);
}
static void afw_answers_from_ui()
{
    OnDisk d{};
    d.haveAfw = true;
    d.afw = true;
    CHECK(!AnswersFromUi(d, true, AaWord::Dlss, 1, true, true, true, true).haveAfw);    // keep, unchanged: not written
    CHECK(AnswersFromUi(d, true, AaWord::Dlss, 1, true, true, false, true).haveAfw);    // keep, changed: written
    CHECK(AnswersFromUi(d, false, AaWord::Dlss, 1, true, true, true, true).haveAfw);    // reset: always written
    CHECK(AnswersFromUi(OnDisk{}, true, AaWord::Dlss, 1, true, true, false, true).haveAfw);   // keep, missing: written
    CHECK(!AnswersFromUi(d, false, AaWord::Dlss, 1, true, true, false, true).afw);
}
// SPEC-BODY-FOLLOWS-HEAD-2026-10-18 5.2: the installer offers comfort_body_follows_head (Jay: "ON. But have an option
// for it in the installer."; BETA 2026-10-19: "Package that with the toggle available for the beta build" / "Correct" = ON)
// - read like the turn question, written only when answered.
static void follow_read_from_disk()
{
    DetectedState s = Existing();
    CHECK(!ReadOnDisk(s).haveFollow);
    s.ovr2xrText = "comfort_body_follows_head=0\r\n";
    CHECK(ReadOnDisk(s).haveFollow && !ReadOnDisk(s).follow);
    s.ovr2xrText = "comfort_body_follows_head=1\r\n";
    CHECK(ReadOnDisk(s).haveFollow && ReadOnDisk(s).follow);
    s.ovr2xrText = "comfort_body_follows_head=7\r\n";
    CHECK(!ReadOnDisk(s).haveFollow);
}
static void follow_default_is_on()
{
    CHECK(Answers{}.follow);
}
static void follow_answers_from_ui()
{
    OnDisk d{};
    d.haveFollow = true;
    d.follow = false;
    CHECK(!AnswersFromUi(d, true, AaWord::Dlss, 1, true, false, false, true).haveFollow);   // keep, unchanged: not written
    CHECK(AnswersFromUi(d, true, AaWord::Dlss, 1, true, true, false, true).haveFollow);     // keep, changed: written
    CHECK(AnswersFromUi(d, false, AaWord::Dlss, 1, true, false, false, true).haveFollow);   // reset: always written
    CHECK(AnswersFromUi(OnDisk{}, true, AaWord::Dlss, 1, true, true, false, true).haveFollow);   // keep, missing: written
    CHECK(AnswersFromUi(d, false, AaWord::Dlss, 1, true, true, false, true).follow);
}
static void follow_written_when_answered()
{
    Answers a = AllAnswered(false, AaWord::Dlss, 1);
    a.follow = false;
    const auto v = PlanInstall(Existing(), a, Manifest(), {}, false);
    const Action* o = Find(v, ActKind::WriteText, "dlvr/ovr2xr.txt");
    CHECK(o && o->text.find("comfort_body_follows_head=0\r\n") != std::string::npos);
}
static void afw_written_when_answered()
{
    Answers a = AllAnswered(false, AaWord::Dlss, 1);
    a.afw = true;
    const auto v = PlanInstall(Existing(), a, Manifest(), {}, false);
    const Action* o = Find(v, ActKind::WriteText, "dlvr/ovr2xr.txt");
    CHECK(o && o->text.find("afw_enable=1\r\n") != std::string::npos);
    Answers k{};
    k.keep = true;
    k.haveAfw = true;
    k.afw = false;
    const auto w = PlanInstall(Existing(), k, Manifest(), {}, false);
    const Action* p = Find(w, ActKind::WriteText, "dlvr/ovr2xr.txt");
    CHECK(p && p->text == "comfort_turn_mode=0\r\nfoo=3\r\nafw_enable=0\r\n");
}
static void keep_skips_user_markers()
{
    Answers a{};
    a.keep = true;
    a.haveHud = true;
    a.hudPanel = false;
    const auto v = PlanInstall(Existing(), a, Manifest(), {}, false);
    CHECK(Find(v, ActKind::Copy, "dlvr/dlss_preset.txt") == nullptr);
    CHECK(Find(v, ActKind::Copy, "dlvr/force_oculus.txt") != nullptr);
    CHECK(Find(v, ActKind::WriteText, "dlvr/aa_method.txt") == nullptr);
    CHECK(Find(v, ActKind::WriteText, "dlvr/dlss_quality.txt") == nullptr);
    const Action* o = Find(v, ActKind::WriteText, "dlvr/ovr2xr.txt");
    CHECK(o && o->text == "comfort_turn_mode=0\r\nfoo=3\r\nui_hud_panel=0\r\n");
    CHECK(Find(v, ActKind::Delete, "dlvr/dlss_sharpness.txt") == nullptr);
    CHECK(Find(v, ActKind::Delete, "dlvr/arm_fit.txt") == nullptr);
    // kept method is dlss (aa_method.txt), so the DLSS dll is copied and not deleted
    CHECK(Find(v, ActKind::Copy, "nvngx_dlss.dll") != nullptr);
    CHECK(Find(v, ActKind::Delete, "nvngx_dlss.dll") == nullptr);
}
static void reset_keeps_caches_deletes_sharpness()
{
    const auto v = PlanInstall(Existing(), AllAnswered(false, AaWord::Dlss, 0), Manifest(), {}, false);
    CHECK(Find(v, ActKind::Delete, "dlvr/dlss_sharpness.txt") != nullptr);
    CHECK(Find(v, ActKind::Delete, "dlvr/gpu_frame.txt") != nullptr);
    CHECK(Find(v, ActKind::Delete, "dlvr/eye_res.txt") == nullptr);
    CHECK(Find(v, ActKind::Delete, "dlvr/hmd_cache.txt") == nullptr);
    const Action* o = Find(v, ActKind::WriteText, "dlvr/ovr2xr.txt");
    CHECK(o && o->text == FreshSettings({{"comfort_turn_mode", "1"}, {"comfort_body_follows_head", "1"},
                                         {"afw_enable", "0"}, {"ui_hud_panel", "1"}}));
    const Action* am = Find(v, ActKind::WriteText, "dlvr/aa_method.txt");
    const Action* dq = Find(v, ActKind::WriteText, "dlvr/dlss_quality.txt");
    CHECK(am && am->text == "dlss\r\n");
    CHECK(dq && dq->text == "0\r\n");
    CHECK(Find(v, ActKind::Copy, "dlvr/dlss_preset.txt") != nullptr);   // reset re-ships the mod's preset
}
static void reset_drop_armfit_deletes_it()
{
    Answers a = AllAnswered(false, AaWord::Dlss, 1);
    a.keepArmFit = false;
    CHECK(Find(PlanInstall(Existing(), a, Manifest(), {}, false), ActKind::Delete, "dlvr/arm_fit.txt") != nullptr);
}
static void reset_keep_armfit_leaves_it()
{
    CHECK(Find(PlanInstall(Existing(), AllAnswered(false, AaWord::Dlss, 1), Manifest(), {}, false), ActKind::Delete,
               "dlvr/arm_fit.txt") == nullptr);
}
static void amd_skips_dlss_dll_and_deletes_stale()
{
    const auto v = PlanInstall(Existing(), AllAnswered(false, AaWord::Fsr4, 1), Manifest(), {}, false);
    CHECK(Find(v, ActKind::Copy, "nvngx_dlss.dll") == nullptr);
    CHECK(Find(v, ActKind::Delete, "nvngx_dlss.dll") != nullptr);
    CHECK(Find(v, ActKind::Copy, "amd_fidelityfx_upscaler_dx12.dll") != nullptr);
    const Action* am = Find(v, ActKind::WriteText, "dlvr/aa_method.txt");
    CHECK(am && am->text == "fsr4\r\n");
}
// Spec 5.10: "nvngx_dlss.dll goes only onto NVIDIA cards" - by CARD, not by the chosen method: an RTX user who picks
// FSR in Setup can still switch to DLSS in the game's Anti-Aliasing tab (review finding 1).
static void nvidia_keeps_dlss_dll_with_fsr()
{
    DetectedState s = Existing();
    s.nvidiaGpu = true;
    const auto v = PlanInstall(s, AllAnswered(false, AaWord::Fsr3, 1), Manifest(), {}, false);
    CHECK(Find(v, ActKind::Copy, "nvngx_dlss.dll") != nullptr);
    CHECK(Find(v, ActKind::Delete, "nvngx_dlss.dll") == nullptr);
    // Keep with an FSR marker on an NVIDIA card: still copied, never deleted
    DetectedState k = Existing();
    k.nvidiaGpu = true;
    k.aaMethodText = "\xEF\xBB\xBF" "fsr3\r\n";
    Answers none{};
    none.keep = true;
    const auto w = PlanInstall(k, none, Manifest(), {}, false);
    CHECK(Find(w, ActKind::Copy, "nvngx_dlss.dll") != nullptr);
    CHECK(Find(w, ActKind::Delete, "nvngx_dlss.dll") == nullptr);
}
static void vc_runtime_version_compare()
{
    CHECK(VersionLess({14, 29, 30133}, {14, 44, 35207}));
    CHECK(VersionLess({14, 44, 35206}, {14, 44, 35207}));
    CHECK(!VersionLess({14, 44, 35207}, {14, 44, 35207}));
    CHECK(!VersionLess({14, 50, 1}, {14, 44, 35207}));
    CHECK(VersionLess({0, 0, 0}, {14, 44, 35207}));   // unreadable registry = outdated
}
static void forbidden_in_manifest_throws()
{
    auto m = Manifest();
    m.push_back({kMd5, "dlvr/arm_fit.txt", false});
    bool threw = false;
    try { PlanInstall(DetectedState{}, AllAnswered(false, AaWord::Dlss, 1), m, {}, false); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}
// BETA ships dlvr/hud_layer.txt (Jay 2026-10-19: "add the HUD layer marker for the beta") so the AFW HUD layer exists whatever
// afw_enable is at launch - the installer defaults AFW OFF, and a tester who ticks it later got ghosting menus until a restart
// (a tester, BETA-FINDINGS-2026-10-19). The plan must COPY it, not refuse the manifest; a still-forbidden name must still throw.
static void hud_layer_marker_ships_in_beta()
{
    auto m = Manifest();
    m.push_back({kMd5, "dlvr/hud_layer.txt", false});
    bool threw = false;
    std::vector<Action> v;
    try { v = PlanInstall(DetectedState{}, AllAnswered(false, AaWord::Dlss, 1), m, {}, false); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(!threw);
    const Action* c = Find(v, ActKind::Copy, "dlvr/hud_layer.txt");
    CHECK(c && c->md5 == kMd5);
    auto m2 = Manifest();
    m2.push_back({kMd5, "dlvr/eye_res.txt", false});
    bool threw2 = false;
    try { PlanInstall(DetectedState{}, AllAnswered(false, AaWord::Dlss, 1), m2, {}, false); }
    catch (const std::runtime_error&) { threw2 = true; }
    CHECK(threw2);
}
// BETA ships dlvr/extra_args.txt = "-nologos" (Jay 2026-10-20: "Ship -nologos in extra_args.txt for BETA"): the bootstrap appends
// it to the command line at attach, and the game then skips the intro movies, the press-start screen and the save-icon notice,
// as Jay's own Steam launch option does. A tester sat in that sequence with the mod menu open and never reached the main menu
// (BETA-FINDINGS-2026-10-19 10a). The plan must COPY it on a fresh install AND on Keep; a still-forbidden name must still throw.
static void extra_args_ships_in_beta()
{
    auto m = Manifest();
    m.push_back({kMd5, "dlvr/extra_args.txt", false});
    for (int keep = 0; keep < 2; ++keep) {
        DetectedState s;
        if (keep) {
            s.modPresent = true;
            s.present.insert("dlvr/extra_args.txt");
        }
        bool threw = false;
        std::vector<Action> v;
        try { v = PlanInstall(s, AllAnswered(keep != 0, AaWord::Dlss, 1), m, {}, false); }
        catch (const std::runtime_error&) { threw = true; }
        CHECK(!threw);
        const Action* c = Find(v, ActKind::Copy, "dlvr/extra_args.txt");
        CHECK(c && c->md5 == kMd5);
    }
    auto m2 = Manifest();
    m2.push_back({kMd5, "dlvr/hmd_cache.txt", false});
    bool threw2 = false;
    try { PlanInstall(DetectedState{}, AllAnswered(false, AaWord::Dlss, 1), m2, {}, false); }
    catch (const std::runtime_error&) { threw2 = true; }
    CHECK(threw2);
}
static void video_scr_always_written_and_order()
{
    const auto v = PlanInstall(DetectedState{}, AllAnswered(false, AaWord::Dlss, 1), Manifest(), {}, true);
    const Action* vs = Find(v, ActKind::WriteText, kVideoScrPath);
    CHECK(vs && vs->text == VideoScrText());
    CHECK(!v.empty() && v.front().kind == ActKind::RunVcRedist);
    // order: RunVcRedist, Copy..., WriteText..., Delete...
    int phase = 0;
    for (const auto& a : v) {
        const int p = static_cast<int>(a.kind);
        CHECK(p >= phase);
        phase = p;
    }
    const Action* c = Find(v, ActKind::Copy, "DW/Data3.pak");
    CHECK(c && c->md5 == kMd5);
}
static void plan_keep_twice_is_idempotent()
{
    DetectedState s = Existing();
    s.ovr2xrText = "comfort_turn_mode=1\r\ncomfort_body_follows_head=1\r\nafw_enable=1\r\nui_hud_panel=1\r\nfoo=3\r\n";
    Answers none{};
    none.keep = true;
    const auto v = PlanInstall(s, none, Manifest(), {}, false);
    const Action* o = Find(v, ActKind::WriteText, "dlvr/ovr2xr.txt");
    CHECK(o && o->text == s.ovr2xrText);
    CHECK(QuestionsToAsk(s, true).empty());
}
static void retired_key_removed_on_keep()
{
    DetectedState s = Existing();
    s.ovr2xrText = "old_key=1\r\ncomfort_turn_mode=0\r\nui_hud_panel=1\r\n";
    Answers none{};
    none.keep = true;
    const auto v = PlanInstall(s, none, Manifest(), {"old_key"}, false);   // held: Find returns a pointer into it
    const Action* o = Find(v, ActKind::WriteText, "dlvr/ovr2xr.txt");
    CHECK(o && o->text == "comfort_turn_mode=0\r\nui_hud_panel=1\r\n");
}
static void plan_copy_failure_reports_failed()
{
    const std::vector<Action> v = {{ActKind::Copy, "a.dll", "", kMd5}, {ActKind::Copy, "b.dll", "", kMd5},
                                   {ActKind::WriteText, "c.txt", "x", ""}};
    const Outcome ok = Summarise(v, {true, true, true});
    CHECK(ok.ok && ok.failedPath.empty());
    const Outcome bad = Summarise(v, {true, false, false});
    CHECK(!bad.ok && bad.failedPath == "b.dll");
    const Outcome shortList = Summarise(v, {true});
    CHECK(!shortList.ok && shortList.failedPath == "b.dll");
}
static void remove_lists_spec_files()
{
    DetectedState s = Existing();
    const char* spec[] = {"libovrrt64_1.dll", "xinput1_3.dll", "nvngx_dlss.dll", "amd_fidelityfx_loader_dx12.dll",
                          "amd_fidelityfx_upscaler_dx12.dll", "amd_fidelityfx_license.md", "dw/data3.pak",
                          "dw/dlvr_pp.mth", "dw_dlc24/datadlc24_2.pak"};
    for (const char* p : spec) {
        s.present.insert(p);
    }
    const auto v = PlanRemove(s);
    for (const char* p : spec) {
        bool found = false;
        for (const auto& a : v) {
            std::string r = a.relPath;
            std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            found = found || (a.kind == ActKind::Delete && r == p);
        }
        CHECK(found);
    }
    CHECK(Find(v, ActKind::Delete, "dlvr/") != nullptr);   // the whole dlvr folder
    for (const auto& a : v) {
        CHECK(a.kind == ActKind::Delete);
    }
}

int main()
{
    manifest_parses();
    manifest_malformed_throws();
    questions_fresh_all_four();
    questions_keep_only_missing();
    afw_read_from_disk();
    afw_default_is_off();
    afw_answers_from_ui();
    afw_written_when_answered();
    follow_read_from_disk();
    follow_default_is_on();
    follow_answers_from_ui();
    follow_written_when_answered();
    keep_skips_user_markers();
    reset_keeps_caches_deletes_sharpness();
    reset_drop_armfit_deletes_it();
    reset_keep_armfit_leaves_it();
    amd_skips_dlss_dll_and_deletes_stale();
    nvidia_keeps_dlss_dll_with_fsr();
    vc_runtime_version_compare();
    forbidden_in_manifest_throws();
    hud_layer_marker_ships_in_beta();
    extra_args_ships_in_beta();
    video_scr_always_written_and_order();
    plan_keep_twice_is_idempotent();
    retired_key_removed_on_keep();
    plan_copy_failure_reports_failed();
    remove_lists_spec_files();
    std::printf("setup_plan_test: %d failure(s)\n", g_fail);
    return g_fail;
}
