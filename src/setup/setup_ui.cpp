// See setup_ui.h. Jay 2026-10-17: "I don't want it to look like a windows UAC pop-up ... We want a proper installer UI.
// Inclusive of progress bar"; the look of the Titanfall 2 VR installer; "Use the title.png for the art in the small left
// box"; the footer "made by" line BLANK. Decisions come from the tested headers (setup_plan.h, setup_record.h,
// setup_store.h, setup_gpu.h, setup_view.h); this file draws them and runs the worker.
#include "setup_ui.h"

#include <d3d11.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "setup_gpu.h"
#include "setup_record.h"
#include "setup_view.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace dlvr::setup {

namespace {

// --------------------------------------------------------------- colours ---
// The approved mockup (.superpowers/brainstorm/.../installer-look.html).
const ImU32 kBg = IM_COL32(0x16, 0x18, 0x1d, 255);
const ImU32 kTitleBg = IM_COL32(0x11, 0x13, 0x17, 255);
const ImU32 kCardBg = IM_COL32(0x1d, 0x20, 0x27, 255);
const ImU32 kCardBorder = IM_COL32(0x2a, 0x2e, 0x36, 255);
const ImU32 kOkFill = IM_COL32(0x1e, 0x3b, 0x2a, 255);
const ImU32 kOkLine = IM_COL32(0x2c, 0x6b, 0x45, 255);
const ImU32 kOkText = IM_COL32(0x5f, 0xd4, 0x8b, 255);
const ImU32 kTodoFill = IM_COL32(0x26, 0x2a, 0x32, 255);
const ImU32 kTodoLine = IM_COL32(0x3a, 0x3f, 0x49, 255);
const ImU32 kWarnFill = IM_COL32(0x3a, 0x31, 0x18, 255);
const ImU32 kWarnLine = IM_COL32(0x8a, 0x6d, 0x24, 255);
const ImU32 kBadFill = IM_COL32(0x3b, 0x1e, 0x1e, 255);
const ImU32 kBadLine = IM_COL32(0x8a, 0x34, 0x34, 255);
const ImVec4 kText(0.90f, 0.91f, 0.93f, 1.0f);
const ImVec4 kDim(0.55f, 0.58f, 0.63f, 1.0f);
const ImVec4 kBlue(0.18f, 0.50f, 0.93f, 1.0f);
const ImVec4 kBlueHover(0.25f, 0.57f, 0.98f, 1.0f);
const ImVec4 kTag(0.62f, 0.76f, 1.0f, 1.0f);
const ImVec4 kWarnText(0.95f, 0.78f, 0.38f, 1.0f);
const ImVec4 kBadText(0.95f, 0.45f, 0.45f, 1.0f);

constexpr float kWinW = 720.0f;
constexpr float kWinH = 640.0f;
constexpr float kTitleH = 34.0f;

// ---------------------------------------------------------------- state ---

enum class CardState { Done, Todo, Warn, Bad };

struct App {
    UiArgs args;
    float scale = 1.0f;
    HWND hwnd = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* titleTex = nullptr;
    float titleAspect = 2.0f;
    ImFont* fontBody = nullptr;
    ImFont* fontBold = nullptr;
    ImFont* fontBig = nullptr;

    // discovery
    std::vector<Candidate> candidates;
    int sel = -1;
    Gpu gpu;
    Recommendation rec{};
    std::vector<AaOption> aaOptions;
    // the selected folder
    DetectedState state;
    Store store = Store::Unknown;
    bool programFiles = false;
    int running = 0;
    OnDisk disk;
    std::string layoutWarn;
    // the dropdowns
    int aaIdx = 0;
    int turnIdx = 0;   // 0 snap, 1 smooth
    int followIdx = 0; // 0 body follows head (on - the default, Jay 2026-10-19 BETA: "Correct"), 1 off
    int afwIdx = 0;    // 0 AFW off (the default, Jay 2026-10-18), 1 on
    int hudIdx = 0;    // 0 panel, 1 head
    // the run
    ViewModel vm;
    Progress progress;
    std::thread worker;
    std::mutex lock;
    std::string resultText;   // shown in the mod card after a run
    bool lastWasRemove = false;
    // Written ONLY by the worker, under `lock`; applied by the UI thread at the top of DrawFrame (review finding 3:
    // the view model is the UI thread's alone).
    bool finished = false;
    bool finishedOk = false;
    std::string finishedText;
    int shotHeight = 0;   // --shot-height (pixels)
};

App* g_app = nullptr;

// ---------------------------------------------------------- the decisions ---

void SelectGame(App& a, int idx)
{
    a.sel = idx;
    a.vm = ViewModel{};
    a.resultText.clear();
    if (idx < 0 || idx >= static_cast<int>(a.candidates.size())) {
        a.vm.blocked = true;
        return;
    }
    const Candidate& c = a.candidates[static_cast<size_t>(idx)];
    a.state = Detect(c.folder);
    a.state.nvidiaGpu = (a.gpu.vendor == 0x10DE);
    a.store = c.store;
    a.programFiles = UnderProgramFiles(c.folder);
    a.running = GameRunning();
    a.disk = ReadOnDisk(a.state);
    a.layoutWarn = LayoutWarning(a.store, a.args.steamUserdata);
    std::string recText, recPkg, recMan;
    const bool haveRec = ReadAll(c.folder / L"dlvr" / L"setup_installed.txt", &recText) && ParseRecord(recText, &recPkg, &recMan);
    // the md5 sweep only when the record matches (it is what could make the header read Installed)
    const bool recMatches = haveRec && recPkg == a.args.package && recMan == a.args.manifestMd5;
    const bool intact = recMatches && FilesIntact(c.folder, a.args.manifest);
    a.vm.header = HeaderFor(a.state.modPresent, haveRec, recPkg, recMan, a.args.package, a.args.manifestMd5, intact);
    a.vm.blocked = a.programFiles || a.running != 0;
    // dropdowns: what is on disk (added to the list if the list lacks it), else the recommendation / shipped default
    int d = 0;
    a.aaOptions = OptionsFor(a.rec, a.gpu.name, a.gpu.vendor, &d);
    a.aaIdx = SelectCurrent(&a.aaOptions, a.disk.haveAa, AaOption{a.disk.method, a.disk.quality}, d);
    a.turnIdx = (a.disk.haveTurn && !a.disk.snap) ? 1 : 0;
    a.followIdx = (a.disk.haveFollow && !a.disk.follow) ? 1 : 0;
    a.afwIdx = (a.disk.haveAfw && a.disk.afw) ? 1 : 0;
    a.hudIdx = (a.disk.haveHud && !a.disk.hudPanel) ? 1 : 0;
    Log("selected " + U(c.folder.wstring()) + " [" + StoreTag(a.store) + "], mod present " + (a.state.modPresent ? "yes" : "no") +
        ", program files " + (a.programFiles ? "YES" : "no") + ", running " + std::to_string(a.running) + ", header " +
        PrimaryLabel(a.vm));
}

void Discover(App& a)
{
    a.candidates = FindGames(a.args.discovery);
    if (!a.args.forcedGame.empty()) {
        a.candidates.insert(a.candidates.begin(), Candidate{a.args.forcedGame, StoreOfFolder(a.args.forcedGame)});
    }
    a.gpu = a.args.gpuOverride.vendor ? a.args.gpuOverride : BestGpu();
    a.rec = Recommend(a.gpu.name, a.gpu.vendor);
    Log("GPU: " + (a.gpu.name.empty() ? std::string("none found") : a.gpu.name) + " vendor " + std::to_string(a.gpu.vendor));
    SelectGame(a, a.candidates.empty() ? -1 : 0);
}

// The worker: install (keep / reset) or remove, exactly as the plan headers decide.
void StartRun(App& a, bool remove, bool keep, bool keepArmFit)
{
    if (a.vm.installing || a.sel < 0) { return; }
    const int run = GameRunning();
    if (run != 0) {
        a.running = run;
        a.vm.blocked = true;
        Log("refused: Dying Light is running (or could not be checked)");
        return;
    }
    const Candidate c = a.candidates[static_cast<size_t>(a.sel)];
    std::vector<Action> acts;
    std::vector<Moved> moved;   // keep install: lines moved from an old shipped default (spec 2026-10-25 keep-settings reset)
    if (remove) {
        acts = PlanRemove(a.state);
    } else {
        const AaOption o = a.aaOptions[static_cast<size_t>(a.aaIdx)];
        Answers ans = AnswersFromUi(a.disk, keep && a.state.modPresent, o.method, o.quality, a.turnIdx == 0,
                                    a.followIdx == 0, a.afwIdx == 1, a.hudIdx == 0);
        ans.keepArmFit = keepArmFit;
        const VcVersion have = InstalledVcRuntime();
        const VcVersion bundled = BundledVcRuntime(*a.args.payload);
        const bool needRedist = VersionLess(have, bundled) || have.major == 0;
        Log("VC++ runtime: installed " + VerText(have) + ", bundled " + VerText(bundled) + (needRedist ? " -> will install" : " -> up to date"));
        try {
            acts = PlanInstall(a.state, ans, a.args.manifest, a.args.retiredKeys, needRedist,
                               RecordText(a.args.package, a.args.manifestMd5), a.args.resets, &moved);
            for (const auto& mv : moved) { Log("reset: " + mv.key + " " + mv.from + " -> " + mv.to); }
        } catch (const std::exception& e) {
            a.vm.failed = true;
            a.resultText = std::string("Setup's file list is damaged: ") + e.what();
            return;
        }
    }
    a.vm.installing = true;
    a.vm.failed = false;
    a.vm.done = false;
    a.lastWasRemove = remove;
    a.resultText.clear();
    Log(std::string(remove ? "REMOVE" : keep ? "UPDATE (keep)" : "INSTALL / RESET") + " - " + std::to_string(acts.size()) + " actions");
    if (a.worker.joinable()) { a.worker.join(); }
    a.worker = std::thread([&a, acts, c, moved]() {
        const Outcome o = Execute(acts, c.folder, a.args.docs, *a.args.payload, &a.progress);
        Log(o.ok ? std::string("RESULT: OK") : "RESULT: FAILED - " + o.failedPath);
        std::lock_guard<std::mutex> l(a.lock);
        a.finishedOk = o.ok;
        a.finishedText = o.ok ? InstalledSummary(a.args.package, moved, 12, U(LogPath())) : "This did not complete: " + o.failedPath +
                                                         "\nThe mod is NOT correctly installed. Log: " + U(LogPath());
        a.finished = true;
    });
}

// --------------------------------------------------------------- drawing ---

float S(float v) { return v * g_app->scale; }

void TickCircle(CardState st)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = S(10);
    const ImVec2 c(p.x + r, p.y + r);
    const ImU32 fill = st == CardState::Done ? kOkFill : st == CardState::Warn ? kWarnFill : st == CardState::Bad ? kBadFill : kTodoFill;
    const ImU32 line = st == CardState::Done ? kOkLine : st == CardState::Warn ? kWarnLine : st == CardState::Bad ? kBadLine : kTodoLine;
    dl->AddCircleFilled(c, r, fill);
    dl->AddCircle(c, r, line, 0, S(1.2f));
    if (st == CardState::Done) {
        dl->AddLine(ImVec2(c.x - S(4.5f), c.y + S(0.5f)), ImVec2(c.x - S(1.2f), c.y + S(3.8f)), kOkText, S(2));
        dl->AddLine(ImVec2(c.x - S(1.2f), c.y + S(3.8f)), ImVec2(c.x + S(5), c.y - S(3.5f)), kOkText, S(2));
    } else if (st == CardState::Warn || st == CardState::Bad) {
        const ImU32 t = st == CardState::Warn ? ImGui::GetColorU32(kWarnText) : ImGui::GetColorU32(kBadText);
        dl->AddLine(ImVec2(c.x, c.y - S(5)), ImVec2(c.x, c.y + S(1.5f)), t, S(2));
        dl->AddCircleFilled(ImVec2(c.x, c.y + S(4.5f)), S(1.3f), t);
    }
    ImGui::Dummy(ImVec2(r * 2, r * 2));
}

bool BeginCard(const char* id, CardState st)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(st == CardState::Warn ? kWarnFill : st == CardState::Bad ? kBadFill : kCardBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(st == CardState::Warn ? kWarnLine : st == CardState::Bad ? kBadLine : kCardBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    const bool open = ImGui::BeginChild(id, ImVec2(-1, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    TickCircle(st);
    ImGui::SameLine(0, S(10));
    ImGui::BeginGroup();
    return open;
}

void EndCard()
{
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0, S(2)));
}

void Heading(const char* t)
{
    ImGui::PushFont(g_app->fontBold, S(15));
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
}

void Dim(const std::string& t)
{
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - S(4));
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void Colored(const ImVec4& c, const std::string& t)
{
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - S(4));
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// The card's right edge in window-local x (its width minus the padding BeginCard pushed).
float RightEdge() { return ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x; }

// Right-aligned buttons on the current line: returns which (0..n-1) was clicked, -1 none.
int RightButtons(const std::vector<const char*>& labels, bool enabled = true)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    float w = 0;
    for (const char* l : labels) { w += ImGui::CalcTextSize(l).x + st.FramePadding.x * 2; }
    w += st.ItemSpacing.x * static_cast<float>(labels.size() - 1);
    ImGui::SameLine();
    ImGui::SetCursorPosX(RightEdge() - w);   // window-local: SameLine(x) would add the group's indent (found: clipped buttons)
    int clicked = -1;
    ImGui::BeginDisabled(!enabled);
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i) { ImGui::SameLine(); }
        if (ImGui::Button(labels[i])) { clicked = static_cast<int>(i); }
    }
    ImGui::EndDisabled();
    return clicked;
}

std::wstring BrowseFolder(HWND owner)
{
    std::wstring out;
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
        DWORD o = 0;
        d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(L"Choose your Dying Light folder (the one with DyingLightGame.exe)");
        if (SUCCEEDED(d->Show(owner))) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(d->GetResult(&it))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    out = p;
                    CoTaskMemFree(p);
                }
                it->Release();
            }
        }
        d->Release();
    }
    return out;
}

std::wstring MethodLabelW(const AaOption& o)
{
    static const char* kQ[] = {"DLAA / native", "Quality", "Balanced", "Performance"};
    const char* qn = (o.quality >= 0 && o.quality <= 3) ? kQ[o.quality] : "?";
    std::string s;
    if (o.method == AaWord::Dlss) { s = o.quality == 0 ? "DLAA" : std::string("DLSS ") + qn; }
    else { s = std::string(o.method == AaWord::Fsr4 ? "FSR 4 " : "FSR 3.1 ") + qn; }
    return W(s);
}

void DrawFrame(App& a)
{
    {
        bool finished = false, ok = false;
        std::string text;
        {
            std::lock_guard<std::mutex> l(a.lock);
            finished = a.finished;
            ok = a.finishedOk;
            text = a.finishedText;
            a.finished = false;
        }
        if (finished) {   // the worker is done: apply its result on THIS thread
            if (a.worker.joinable()) { a.worker.join(); }
            const bool wasRemove = a.lastWasRemove;
            SelectGame(a, a.sel);   // re-read the folder (record, files, markers)
            a.vm.failed = !ok;
            a.vm.done = ok;
            a.resultText = text;
            if (wasRemove && ok) {
                a.resultText = "The mod was removed. Now let your store put back the game's own files - Steam: right-click "
                               "Dying Light -> Properties -> Installed Files -> \"Verify integrity of game files\". Epic / GOG: "
                               "use the game's Verify / Repair option.";
            }
        }
        a.vm.progressNum = a.progress.done;
        a.vm.progressDen = a.progress.total;
        a.vm.currentFile = a.progress.Current();
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(kBg));
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // ---- title bar (drag area handled by WM_NCHITTEST)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + S(kTitleH)), kTitleBg);
    dl->AddLine(ImVec2(vp->Pos.x, vp->Pos.y + S(kTitleH)), ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + S(kTitleH)), kCardBorder);
    ImGui::SetCursorPos(ImVec2(S(12), S(9)));
    ImGui::PushFont(a.fontBold, S(15));
    ImGui::TextUnformatted("Dying Light VR Setup");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text("%s", a.args.package.c_str());
    ImGui::PopStyleColor();
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        ImGui::SetCursorPos(ImVec2(vp->Size.x - S(84), S(4)));
        if (ImGui::Button(" _ ##min", ImVec2(S(38), S(26)))) { ShowWindow(a.hwnd, SW_MINIMIZE); }
        ImGui::SameLine(0, S(2));
        ImGui::BeginDisabled(!CanClose(a.vm));
        if (ImGui::Button(" X ##close", ImVec2(S(38), S(26)))) { PostMessageW(a.hwnd, WM_CLOSE, 0, 0); }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
    }

    // ---- header
    ImGui::SetCursorPos(ImVec2(S(16), S(kTitleH + 14)));
    ImGui::BeginGroup();
    const float artH = S(52);
    const float artW = artH * a.titleAspect;
    if (a.titleTex) {
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(a.titleTex)), ImVec2(artW, artH));
    } else {
        ImGui::Dummy(ImVec2(artW, artH));
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, S(12));
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::PushFont(a.fontBig, S(20));
    ImGui::TextUnformatted("Dying Light VR");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, a.vm.failed ? kBadText : kDim);
    ImGui::TextUnformatted(HeaderStatus(a.vm).c_str());
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    {
        const char* label = PrimaryLabel(a.vm);
        const bool installed = a.vm.header == HeaderState::Installed && !a.vm.installing;
        const float bw = S(130);
        ImGui::SameLine(vp->Size.x - bw - S(16));
        ImGui::SetCursorPosY(S(kTitleH + 14) + artH / 2 - S(16));
        if (installed) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kOkFill));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(kOkFill));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(kOkFill));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kOkText));
            ImGui::Button("Installed##hdr", ImVec2(bw, S(32)));
            ImGui::PopStyleColor(4);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, kBlue);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBlueHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kBlue);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            ImGui::BeginDisabled(!PrimaryEnabled(a.vm));
            if (ImGui::Button(a.vm.installing ? "Installing...##hdr" : (std::string(label) + "##hdr").c_str(), ImVec2(bw, S(32)))) {
                StartRun(a, false, a.vm.header == HeaderState::Update, true);
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor(4);
        }
    }

    // ---- the cards
    ImGui::SetCursorPos(ImVec2(S(16), S(kTitleH + 14) + artH + S(16)));
    ImGui::BeginChild("##cards", ImVec2(vp->Size.x - S(32), vp->Size.y - ImGui::GetCursorPosY() - S(30)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    const bool busy = a.vm.installing;

    // 1. Find Dying Light
    {
        const bool have = a.sel >= 0;
        const CardState st = !have ? CardState::Todo : (a.programFiles || a.running != 0) ? CardState::Bad : CardState::Done;
        BeginCard("##find", st);
        Heading("Find Dying Light");
        if (have) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kTag);
            ImGui::Text("  %s", StoreTag(a.store));
            ImGui::PopStyleColor();
        }
        const int b = RightButtons(have ? std::vector<const char*>{"Open", "Change"} : std::vector<const char*>{"Choose folder"}, !busy);
        if (b >= 0) {
            if (have && b == 0) {
                ShellExecuteW(nullptr, L"open", a.candidates[static_cast<size_t>(a.sel)].folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else {
                const std::wstring f = BrowseFolder(a.hwnd);
                if (!f.empty()) {
                    std::error_code ec;
                    if (fs::exists(fs::path(f) / L"DyingLightGame.exe", ec)) {
                        a.candidates.insert(a.candidates.begin(), Candidate{f, StoreOfFolder(f)});
                        SelectGame(a, 0);
                    } else {
                        a.resultText.clear();
                        Log("chosen folder has no DyingLightGame.exe: " + U(f));
                        a.layoutWarn.clear();
                        a.sel = -1;
                        a.vm.blocked = true;
                        a.candidates.clear();
                    }
                }
            }
        }
        if (!have) {
            Dim("Dying Light was not found. Choose the folder that has DyingLightGame.exe in it.");
        } else {
            if (a.candidates.size() > 1) {
                ImGui::SetNextItemWidth(-S(8));
                const std::string cur = U(a.candidates[static_cast<size_t>(a.sel)].folder.wstring());
                ImGui::BeginDisabled(busy);
                if (ImGui::BeginCombo("##games", cur.c_str())) {
                    for (size_t i = 0; i < a.candidates.size(); ++i) {
                        const std::string l = std::string("[") + StoreTag(a.candidates[i].store) + "]  " + U(a.candidates[i].folder.wstring());
                        if (ImGui::Selectable(l.c_str(), static_cast<int>(i) == a.sel)) { SelectGame(a, static_cast<int>(i)); }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
            } else {
                Dim(U(a.candidates[static_cast<size_t>(a.sel)].folder.wstring()));
            }
            const std::string un = UntestedNotice(a.store);
            if (!un.empty()) { Colored(kWarnText, un); }
            if (a.programFiles) { Colored(kBadText, ProgramFilesRefusal(a.store, U(a.candidates[static_cast<size_t>(a.sel)].folder.wstring()))); }
            if (a.running != 0) {
                Colored(kBadText, a.running > 0 ? "Dying Light is running. Close the game completely, then press Check again."
                                                : "Setup could not check whether Dying Light is running. Close the game, then press Check again.");
                if (ImGui::Button("Check again")) { SelectGame(a, a.sel); }
            }
        }
        EndCard();
    }

    const bool canInstall = a.sel >= 0 && !a.programFiles && a.running == 0;
    if (canInstall) {
        // 2. Graphics
        BeginCard("##gfx", CardState::Done);
        Heading("Graphics");
        Dim(a.gpu.name.empty() ? std::string("Graphics card not found") : a.gpu.name);
        if (!a.rec.recognised) { Dim("This card is not in Setup's list - the suggestion is a safe default."); }
        ImGui::SetNextItemWidth(S(300));
        ImGui::BeginDisabled(busy);
        const std::string curAa = U(MethodLabelW(a.aaOptions[static_cast<size_t>(a.aaIdx)]));
        if (ImGui::BeginCombo("##aa", curAa.c_str())) {
            int dflt = 0;
            OptionsFor(a.rec, a.gpu.name, a.gpu.vendor, &dflt);
            for (size_t i = 0; i < a.aaOptions.size(); ++i) {
                std::string l = U(MethodLabelW(a.aaOptions[i]));
                if (static_cast<int>(i) == dflt && !a.rec.askDlaa) { l += "  (recommended)"; }
                if (a.rec.askDlaa && a.aaOptions[i].method == AaWord::Dlss && a.aaOptions[i].quality <= 1) { l += "  (your preference)"; }
                if (ImGui::Selectable(l.c_str(), static_cast<int>(i) == a.aaIdx)) { a.aaIdx = static_cast<int>(i); }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        Dim(std::string("Virtual Desktop: set the graphics quality to ") + a.rec.vdAdvice + ". Setup can't change this for you.");
        EndCard();

        // 3. Comfort
        BeginCard("##comfort", CardState::Done);
        Heading("Comfort");
        ImGui::BeginDisabled(busy);
        const char* turns[] = {"Turning: Snap 30 degrees", "Turning: Smooth"};
        const char* huds[] = {"HUD: Panel in 3D space", "HUD: Following your head"};
        ImGui::SetNextItemWidth(S(240));
        ImGui::Combo("##turn", &a.turnIdx, turns, 2);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(240));
        ImGui::Combo("##hud", &a.hudIdx, huds, 2);
        const char* follows[] = {"Body follows head: On (recommended)", "Body follows head: Off"};
        ImGui::SetNextItemWidth(S(488));
        ImGui::Combo("##follow", &a.followIdx, follows, 2);
        Dim("On: turning your head or body turns your character, and the stick walks you where you look.");
        const char* afws[] = {"Alternate Frame Warp: Off", "Alternate Frame Warp: On"};
        ImGui::SetNextItemWidth(S(488));
        ImGui::Combo("##afw", &a.afwIdx, afws, 2);
        Dim("Renders one eye per frame and warps the other. You can change it later on the VR Comfort tab.");
        ImGui::EndDisabled();
        EndCard();

        // 3b. controller layout warning
        if (!a.layoutWarn.empty()) {
            BeginCard("##layout", CardState::Warn);
            Heading("Controller layout");
            Colored(kWarnText, a.layoutWarn);
            EndCard();
        }
    }

    // 4. Dying Light VR
    {
        const bool installedNow = a.vm.header == HeaderState::Installed;
        const CardState st = a.vm.failed ? CardState::Bad : (installedNow || a.vm.done) && !busy ? CardState::Done : CardState::Todo;
        BeginCard("##mod", st);
        Heading("Dying Light VR");
        if (a.sel >= 0 && a.state.modPresent && !busy && canInstall) {
            const ImGuiStyle& sty = ImGui::GetStyle();
            const float w = ImGui::CalcTextSize("Uninstall").x + ImGui::CalcTextSize("Reset  v").x + sty.FramePadding.x * 4 + sty.ItemSpacing.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(RightEdge() - w);
            if (ImGui::Button("Uninstall")) { ImGui::OpenPopup("Remove Dying Light VR?"); }
            ImGui::SameLine();
            if (ImGui::Button("Reset  v")) { ImGui::OpenPopup("##resetmenu"); }
            if (ImGui::BeginPopup("##resetmenu")) {
                if (ImGui::Selectable("Reset settings")) { StartRun(a, false, false, true); }
                if (ImGui::Selectable("Reset settings and arm calibration")) { StartRun(a, false, false, false); }
                ImGui::EndPopup();
            }
            if (ImGui::BeginPopupModal("Remove Dying Light VR?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("This deletes the mod's files, your mod settings and your arm calibration.");
                if (ImGui::Button("Remove")) {
                    StartRun(a, true, false, false);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
        }
        if (busy) {
            Dim(a.vm.currentFile.empty() ? std::string("Working...") : a.vm.currentFile);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kBlue);
            ImGui::ProgressBar(ProgressFraction(a.vm), ImVec2(-S(8), S(8)), "");
            ImGui::PopStyleColor();
        } else if (!a.resultText.empty()) {
            Colored(a.vm.failed ? kBadText : ImGui::ColorConvertU32ToFloat4(kOkText), a.resultText);
        } else if (!a.state.modPresent || a.sel < 0) {
            Dim("Not installed. Installing also sets the game's video settings for VR.");
        } else if (installedNow) {
            Dim("Installed: " + a.args.package + " - every file checked.");
        } else {
            Dim("An older or hand-made install was found. Update keeps your settings; Reset puts them back to the defaults.");
        }
        EndCard();
    }
    ImGui::EndChild();
    // footer: BLANK (Jay: "leave it blank")
    ImGui::End();
}

// -------------------------------------------------------------- plumbing ---

void CreateRtv(App& a)
{
    ID3D11Texture2D* bb = nullptr;
    a.swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) {
        a.dev->CreateRenderTargetView(bb, nullptr, &a.rtv);
        bb->Release();
    }
}

bool LoadTitle(App& a)
{
    if (a.args.titlePng.empty()) {
        Log("title.png: not embedded - the art box stays empty");
        return false;
    }
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) { return false; }
    IWICStream* s = nullptr;
    IWICBitmapDecoder* d = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;
    if (SUCCEEDED(f->CreateStream(&s)) &&
        SUCCEEDED(s->InitializeFromMemory(reinterpret_cast<BYTE*>(a.args.titlePng.data()), static_cast<DWORD>(a.args.titlePng.size()))) &&
        SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnLoad, &d)) && SUCCEEDED(d->GetFrame(0, &fr)) &&
        SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        cv->GetSize(&w, &h);
        std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
        if (SUCCEEDED(cv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()))) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4, 0};
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(a.dev->CreateTexture2D(&td, &sd, &tex))) {
                ok = SUCCEEDED(a.dev->CreateShaderResourceView(tex, nullptr, &a.titleTex));
                tex->Release();
                a.titleAspect = h ? static_cast<float>(w) / static_cast<float>(h) : 2.0f;
                if (a.titleAspect > 3.0f) { a.titleAspect = 3.0f; }
                if (a.titleAspect < 1.0f) { a.titleAspect = 1.0f; }
            }
            Log("title.png: " + std::to_string(w) + "x" + std::to_string(h) + (ok ? " loaded" : " texture FAILED"));
        }
    } else {
        Log("title.png: could not be decoded");
    }
    if (cv) { cv->Release(); }
    if (fr) { fr->Release(); }
    if (d) { d->Release(); }
    if (s) { s->Release(); }
    f->Release();
    return ok;
}

void SetupImGui(App& a)
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::string reg = U(win) + "\\Fonts\\segoeui.ttf";
    const std::string bold = U(win) + "\\Fonts\\segoeuib.ttf";
    std::error_code ec;
    if (fs::exists(fs::u8path(reg), ec) && fs::exists(fs::u8path(bold), ec)) {
        a.fontBody = io.Fonts->AddFontFromFileTTF(reg.c_str(), S(15));
        a.fontBold = io.Fonts->AddFontFromFileTTF(bold.c_str(), S(15));
        a.fontBig = io.Fonts->AddFontFromFileTTF(bold.c_str(), S(20));
    }
    if (!a.fontBody || !a.fontBold || !a.fontBig) {
        Log("font: Segoe UI not found - using ImGui's default font");
        ImFontConfig c;
        c.SizePixels = S(15);
        a.fontBody = a.fontBold = a.fontBig = io.Fonts->AddFontDefault(&c);
    }
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.FrameRounding = S(5);
    st.PopupRounding = S(6);
    st.WindowRounding = S(6);
    st.GrabRounding = S(4);
    st.FramePadding = ImVec2(S(10), S(5));
    st.ItemSpacing = ImVec2(S(8), S(6));
    st.Colors[ImGuiCol_Text] = kText;
    st.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.16f, 0.20f, 1);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.21f, 0.23f, 0.28f, 1);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.27f, 0.32f, 1);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.16f, 0.20f, 1);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.22f, 0.27f, 1);
    st.Colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.16f, 1);
    st.Colors[ImGuiCol_Header] = ImVec4(0.18f, 0.30f, 0.50f, 1);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.36f, 0.60f, 1);
    st.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    ImGui_ImplWin32_Init(a.hwnd);
    ImGui_ImplDX11_Init(a.dev, a.ctx);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) { return 1; }
    switch (msg) {
    case WM_NCHITTEST: {
        // the custom title bar drags the window (except the two buttons at its right end)
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        RECT rc;
        GetClientRect(h, &rc);
        if (g_app && p.y >= 0 && p.y < static_cast<LONG>(S(kTitleH)) && p.x < rc.right - static_cast<LONG>(S(90))) { return HTCAPTION; }
        return HTCLIENT;
    }
    case WM_CLOSE:
        if (g_app && !CanClose(g_app->vm)) { return 0; }   // never mid-install
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool CreateWindowAndDevice(App& a, bool visible)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DyingLightVRSetup";
    RegisterClassExW(&wc);
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    // never larger than the screen's work area (a 1366x768 laptop at 150%): the cards region scrolls instead
    int w = static_cast<int>(S(kWinW));
    int hgt = static_cast<int>(S(kWinH));
    if (a.shotHeight > 0) { hgt = std::min(hgt, a.shotHeight); }   // --shot-height: render the clamped (small-screen) layout
    if (visible) {
        w = std::min(w, static_cast<int>(wa.right - wa.left) - 16);
        hgt = std::min(hgt, static_cast<int>(wa.bottom - wa.top) - 16);
    }
    a.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Dying Light VR Setup", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                             wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - hgt) / 2, w, hgt, nullptr,
                             nullptr, wc.hInstance, nullptr);
    if (!a.hwnd) { return false; }
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = a.hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                               &a.swap, &a.dev, &fl, &a.ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &a.swap,
                                           &a.dev, &fl, &a.ctx);
    }
    if (FAILED(hr)) {
        Log("D3D11 device could not be created (" + std::to_string(hr) + ")");
        return false;
    }
    CreateRtv(a);
    if (visible) {
        ShowWindow(a.hwnd, SW_SHOWDEFAULT);
        UpdateWindow(a.hwnd);
    }
    return true;
}

void RenderOnce(App& a)
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::PushFont(a.fontBody, S(15));
    DrawFrame(a);
    ImGui::PopFont();
    ImGui::Render();
    const float clear[4] = {0.086f, 0.094f, 0.114f, 1.0f};
    a.ctx->OMSetRenderTargets(1, &a.rtv, nullptr);
    a.ctx->ClearRenderTargetView(a.rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void Shutdown(App& a)
{
    if (a.worker.joinable()) { a.worker.join(); }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (a.titleTex) { a.titleTex->Release(); }
    if (a.rtv) { a.rtv->Release(); }
    if (a.swap) { a.swap->Release(); }
    if (a.ctx) { a.ctx->Release(); }
    if (a.dev) { a.dev->Release(); }
    if (a.hwnd) { DestroyWindow(a.hwnd); }
}

float MonitorScale()
{
    HDC dc = GetDC(nullptr);
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

bool SavePng(App& a, const std::wstring& path)
{
    ID3D11Texture2D* bb = nullptr;
    a.swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (!bb) { return false; }
    D3D11_TEXTURE2D_DESC td{};
    bb->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    bool ok = false;
    if (SUCCEEDED(a.dev->CreateTexture2D(&td, nullptr, &st))) {
        a.ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(a.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            IWICImagingFactory* f = nullptr;
            IWICStream* s = nullptr;
            IWICBitmapEncoder* e = nullptr;
            IWICBitmapFrameEncode* fr = nullptr;
            // The back buffer is RGBA; WIC's PNG encoder takes BGRA - swap R and B into a tight copy (found: the first
            // shots had red and blue exchanged).
            std::vector<BYTE> bgra(static_cast<size_t>(td.Width) * td.Height * 4);
            for (UINT y = 0; y < td.Height; ++y) {
                const BYTE* src = static_cast<const BYTE*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                BYTE* dst = bgra.data() + static_cast<size_t>(y) * td.Width * 4;
                for (UINT x = 0; x < td.Width; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 255;
                }
            }
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
                SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) && SUCCEEDED(e->Initialize(s, WICBitmapEncoderNoCache)) &&
                SUCCEEDED(e->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
                SUCCEEDED(fr->SetSize(td.Width, td.Height))) {
                WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
                fr->SetPixelFormat(&pf);
                const bool fmtOk = (pf == GUID_WICPixelFormat32bppBGRA);
                if (!fmtOk) { Log("--shot: the PNG encoder refused 32bpp BGRA - no file"); }
                ok = fmtOk && SUCCEEDED(fr->WritePixels(td.Height, td.Width * 4, static_cast<UINT>(bgra.size()), bgra.data())) &&
                     SUCCEEDED(fr->Commit()) && SUCCEEDED(e->Commit());
            }
            if (fr) { fr->Release(); }
            if (e) { e->Release(); }
            if (s) { s->Release(); }
            if (f) { f->Release(); }
            a.ctx->Unmap(st, 0);
        }
        st->Release();
    }
    bb->Release();
    return ok;
}

}  // namespace

int RunUi(const UiArgs& args)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    App a;
    g_app = &a;
    a.args = args;
    a.scale = MonitorScale();
    if (!CreateWindowAndDevice(a, true)) { return 3; }
    SetupImGui(a);
    LoadTitle(a);
    Discover(a);
    bool quit = false;
    while (!quit) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
            if (m.message == WM_QUIT) { quit = true; }
        }
        if (quit) { break; }
        RenderOnce(a);
        a.swap->Present(1, 0);
    }
    const bool failed = a.vm.failed;
    a.hwnd = nullptr;   // already destroyed by WM_CLOSE
    Shutdown(a);
    g_app = nullptr;
    return failed ? 2 : 0;
}

int RenderShot(const UiArgs& args, const std::string& state, const std::wstring& png, float scale, int height)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    App a;
    g_app = &a;
    a.args = args;
    a.scale = scale;
    a.shotHeight = height;
    if (!CreateWindowAndDevice(a, false)) { return 3; }
    SetupImGui(a);
    LoadTitle(a);
    // a fixed fake state - no discovery, nothing read from this PC except fonts
    a.gpu = {"NVIDIA GeForce RTX 4080", 0x10DE};
    a.rec = Recommend(a.gpu.name, a.gpu.vendor);
    int d = 0;
    a.aaOptions = OptionsFor(a.rec, a.gpu.name, a.gpu.vendor, &d);
    a.aaIdx = d;
    a.candidates = {{fs::path(L"D:\\SteamLibrary\\steamapps\\common\\Dying Light"), Store::Steam}};
    a.sel = 0;
    a.store = Store::Steam;
    if (state == "ready") {
    } else if (state == "installing") {
        a.vm.installing = true;
        a.progress.total = 49;
        a.progress.done = 18;
        a.progress.SetCurrent("DW/Data3.pak");
    } else if (state == "installed") {
        a.state.modPresent = true;
        a.vm.header = HeaderState::Installed;
    } else if (state == "update") {
        a.state.modPresent = true;
        a.vm.header = HeaderState::Update;
    } else if (state == "failed") {
        a.state.modPresent = true;
        a.vm.header = HeaderState::Update;
        a.vm.failed = true;
        a.resultText = "This did not complete: these files were removed or changed 5 seconds after copying - check your "
                       "antivirus:\n  LibOVRRT64_1.dll  (gone)\n  xinput1_3.dll  (gone)\nThe mod is NOT correctly installed.";
    } else if (state == "programfiles-epic") {
        a.candidates = {{fs::path(L"C:\\Program Files\\Epic Games\\DyingLight"), Store::Epic}};
        a.store = Store::Epic;
        a.programFiles = true;
        a.vm.blocked = true;
    } else if (state == "epic-untested") {
        a.candidates = {{fs::path(L"D:\\Epic Games\\DyingLight"), Store::Epic}};
        a.store = Store::Epic;
        a.layoutWarn = "Setup can't check your controller layout on this version of the game. Please check in the game "
                       "that Options -> Controls -> controller layout is Default.";
    } else if (state == "layout-warning") {
        // BETA (2026-10-18): the preview text named a real-looking Steam userdata id and an absolute path - generic here
        a.layoutWarn = "Your game's controller layout is not Default (Steam profile Steam\\userdata\\<your id>\\239140\\"
                       "remote\\out\\profiles). In the game: Options -> Controls -> set the controller "
                       "layout to Default. The mod's buttons are made for it.";
    } else if (state == "two-candidates") {
        a.candidates = {{fs::path(L"D:\\SteamLibrary\\steamapps\\common\\Dying Light"), Store::Steam},
                        {fs::path(L"E:\\GOG Games\\Dying Light"), Store::Gog}};
    } else {
        Log("--shot: unknown state " + state);
        Shutdown(a);
        return 4;
    }
    for (int i = 0; i < 4; ++i) { RenderOnce(a); }   // let ImGui settle its layout
    const bool ok = SavePng(a, png);
    Log("--shot " + state + " -> " + U(png) + (ok ? " written" : " FAILED"));
    Shutdown(a);
    g_app = nullptr;
    return ok ? 0 : 5;
}

}  // namespace dlvr::setup
