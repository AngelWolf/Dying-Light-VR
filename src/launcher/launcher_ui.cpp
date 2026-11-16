// See launcher_ui.h. Plan docs/superpowers/plans/2026-11-15-launcher.md Tasks 1 and 10: the window, its size rule, the drag
// area, close / minimise, the five tabs drawn from LauncherApp's snapshot, a worker thread for the long actions, Jay's art
// (art\*.png beside the exe, drawn scaled, code-drawn fallback + a log line when a file is missing) and --shot.
#include "launcher_ui.h"

#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "launcher_app.h"
#include "launcher_size.h"
#include "launcher_sys.h"
#include "launcher_update.h"
#include "launcher_view.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shcore.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace dlvr::launcher {

namespace {

const ImU32 kBg = IM_COL32(0x14, 0x15, 0x19, 255);
const ImU32 kBarBg = IM_COL32(0x0e, 0x0f, 0x12, 255);
const ImU32 kCard = IM_COL32(0x1c, 0x1e, 0x24, 255);
const ImVec4 kText(0.90f, 0.91f, 0.93f, 1.0f);
const ImVec4 kDim(0.55f, 0.58f, 0.63f, 1.0f);
const ImVec4 kAccent(0.86f, 0.36f, 0.12f, 1.0f);   // placeholder until Jay's art (spec 5)
const ImVec4 kOk(0.45f, 0.85f, 0.55f, 1.0f);
const ImVec4 kWarn(0.95f, 0.78f, 0.38f, 1.0f);
const ImVec4 kBad(0.95f, 0.45f, 0.45f, 1.0f);

constexpr float kBarH = 40.0f;
constexpr float kTabH = 44.0f;
constexpr float kBtnW = 46.0f;
constexpr float kStatusH = 34.0f;

const char* const kTabNames[] = {"PLAY", "VR COMFORT", "CONFIG", "CONTROLS", "SUPPORT"};

std::wstring ExeDir()
{
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

struct Image {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
};

struct App {
    UiArgs args;
    WinSize size{kLayoutW, kLayoutH, 1.0f};
    HWND hwnd = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ImFont* fontBody = nullptr;
    ImFont* fontBold = nullptr;
    bool styleDirty = false;
    Tab tab = Tab::Play;
    bool quit = false;
    Image background, controls;
    std::wstring controlsPath;

    LauncherApp model;
    Snapshot snap;                 // the UI's copy; taken only while no job runs
    std::thread worker;
    std::atomic<bool> busy{false};
    std::mutex lock;
    bool jobDone = false;
    Result jobResult;
    std::string busyWhat;
    Result last;                   // the status line
    ULONGLONG lastRefresh = 0;
    bool confirmUninstall = false;
    std::atomic<bool> quitRequested{false};   // self-update: the new launcher is up, this one closes
    std::map<std::string, std::string> dragging;   // a slider's value while it is held (saved on release)
};

App* g_app = nullptr;

float S(float v) { return v * (g_app ? g_app->size.scale : 1.0f); }

void MonitorFacts(POINT pt, float* dpiScale, RECT* work)
{
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    *work = mi.rcWork;
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) { dx = 96; }
    *dpiScale = static_cast<float>(dx) / 96.0f;
}

void ReleaseRtv(App& a)
{
    if (a.rtv) { a.rtv->Release(); a.rtv = nullptr; }
}

void CreateRtv(App& a)
{
    ID3D11Texture2D* bb = nullptr;
    a.swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) {
        a.dev->CreateRenderTargetView(bb, nullptr, &a.rtv);
        bb->Release();
    }
}

// Jay's art, built into the exe (src_beta/launcher_art/launcher_art.rc; Jay 2026-11-15: "In the EXE").
constexpr int kResArtBackground = 201;
constexpr int kResArtControls = 202;

bool ResourceBytes(int id, std::string* out)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));   // 10 = RT_RCDATA
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* p = g ? LockResource(g) : nullptr;
    if (!p) { return false; }
    out->assign(static_cast<const char*>(p), SizeofResource(nullptr, r));
    return true;
}

// A PNG (the embedded bytes when `mem` is given, else the file at `path`) -> a texture. Undecodable = no texture.
Image LoadImageFile(App& a, const std::wstring& path, const std::string* mem = nullptr)
{
    Image img;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) { return img; }
    IWICStream* st = nullptr;
    IWICBitmapDecoder* d = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    const bool opened = mem ? (SUCCEEDED(f->CreateStream(&st)) &&
                               SUCCEEDED(st->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(mem->data())),
                                                                  static_cast<DWORD>(mem->size()))) &&
                               SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnLoad, &d)))
                            : SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &d));
    if (opened && SUCCEEDED(d->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
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
                if (SUCCEEDED(a.dev->CreateShaderResourceView(tex, nullptr, &img.srv))) { img.w = static_cast<int>(w); img.h = static_cast<int>(h); }
                tex->Release();
            }
        }
    }
    if (cv) { cv->Release(); }
    if (fr) { fr->Release(); }
    if (d) { d->Release(); }
    if (st) { st->Release(); }
    f->Release();
    return img;
}

// The embedded art first, then art\<file> beside the exe, then the code-drawn look - the log names which (never silent).
Image LoadArt(App& a, int id, const std::wstring& file)
{
    std::string bytes;
    if (ResourceBytes(id, &bytes)) {
        Image img = LoadImageFile(a, L"", &bytes);
        Log("art: resource " + std::to_string(id) + (img.srv ? " (embedded) " + std::to_string(img.w) + "x" + std::to_string(img.h)
                                                            : std::string(" embedded but NOT decodable")));
        if (img.srv) { return img; }
    } else {
        Log("art: resource " + std::to_string(id) + " not in this exe (a build without src_beta/launcher_art) - trying " + W2U(file));
    }
    Image img = LoadImageFile(a, file);
    Log("art: " + W2U(file) + (img.srv ? " " + std::to_string(img.w) + "x" + std::to_string(img.h) : std::string(" - not found / not a PNG; drawn by code")));
    return img;
}

void ApplyStyle()
{
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0;
    st.FrameRounding = S(4);
    st.FramePadding = ImVec2(S(10), S(6));
    st.ItemSpacing = ImVec2(S(8), S(7));
    st.ScrollbarSize = S(12);
    st.Colors[ImGuiCol_Text] = kText;
    st.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.16f, 0.20f, 1);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.21f, 0.23f, 0.28f, 1);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.27f, 0.32f, 1);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.16f, 0.20f, 1);
    st.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.21f, 0.26f, 1);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.27f, 0.32f, 1);
    st.Colors[ImGuiCol_SliderGrab] = kAccent;
    st.Colors[ImGuiCol_CheckMark] = kAccent;
    st.Colors[ImGuiCol_ChildBg] = ImGui::ColorConvertU32ToFloat4(kCard);
}

// Loaded once: ImGui 1.92 draws a font at any size given to PushFont, so a scale change needs no rebuild.
void BuildFonts(App& a)
{
    ImGuiIO& io = ImGui::GetIO();
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::string reg = W2U(win) + "\\Fonts\\segoeui.ttf";
    const std::string bold = W2U(win) + "\\Fonts\\segoeuib.ttf";
    if (GetFileAttributesA(reg.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesA(bold.c_str()) != INVALID_FILE_ATTRIBUTES) {
        a.fontBody = io.Fonts->AddFontFromFileTTF(reg.c_str(), 16.0f);
        a.fontBold = io.Fonts->AddFontFromFileTTF(bold.c_str(), 16.0f);
    }
    if (!a.fontBody || !a.fontBold) {
        Log("font: Segoe UI not found - using ImGui's default font");
        a.fontBody = a.fontBold = io.Fonts->AddFontDefault();
    }
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

// ------------------------------------------------------------------------------------------------- the jobs ---

template <typename F>
void StartJob(App& a, const char* what, F fn)
{
    if (a.busy) { return; }
    if (a.worker.joinable()) { a.worker.join(); }
    a.busy = true;
    a.busyWhat = what;
    a.worker = std::thread([&a, fn]() {
        Result r = fn();
        std::lock_guard<std::mutex> l(a.lock);
        a.jobResult = r;
        a.jobDone = true;
        a.busy = false;
    });
}

void PollJob(App& a)
{
    bool done = false;
    {
        std::lock_guard<std::mutex> l(a.lock);
        if (a.jobDone) { done = true; a.jobDone = false; a.last = a.jobResult; }
    }
    if (done) {
        if (a.worker.joinable()) { a.worker.join(); }
        a.snap = a.model.Snap();
        if (a.quitRequested && a.hwnd) { PostMessageW(a.hwnd, WM_CLOSE, 0, 0); }
    }
    // the game process and the files can change under us: refresh every 2 s while idle and no slider is held
    const ULONGLONG now = GetTickCount64();
    if (!a.busy && a.dragging.empty() && now - a.lastRefresh > 2000) {
        a.lastRefresh = now;
        a.model.Refresh();
        a.snap = a.model.Snap();
    }
}

// ------------------------------------------------------------------------------------------------ the draws ---

void Heading(App& a, const char* t)
{
    ImGui::PushFont(a.fontBold, S(17));
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
}

void Colored(const ImVec4& c, const std::string& t)
{
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", t.c_str());
    ImGui::PopStyleColor();
}

bool BigButton(const char* label, bool enabled, bool accent = false, float w = 0.0f)
{
    ImGui::BeginDisabled(!enabled);
    if (accent) { ImGui::PushStyleColor(ImGuiCol_Button, kAccent); }
    const bool p = ImGui::Button(label, ImVec2(w > 0 ? w : S(260), S(48)));
    if (accent) { ImGui::PopStyleColor(); }
    ImGui::EndDisabled();
    return p && enabled;
}

void DrawTitleBar(App& a)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float w = static_cast<float>(a.size.w);
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + S(kBarH)), kBarBg);
    ImGui::PushFont(a.fontBold, S(16));
    ImGui::SetCursorPos(ImVec2(S(16), (S(kBarH) - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextUnformatted("DYING LIGHT VR");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::SetCursorPos(ImVec2(w - S(kBtnW) * 2, 0));
    if (ImGui::Button("_##min", ImVec2(S(kBtnW), S(kBarH))) && a.hwnd) { ShowWindow(a.hwnd, SW_MINIMIZE); }
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.16f, 0.16f, 1));
    if (ImGui::Button("X##close", ImVec2(S(kBtnW), S(kBarH))) && a.hwnd) { PostMessageW(a.hwnd, WM_CLOSE, 0, 0); }
    ImGui::PopStyleColor(2);
}

void DrawTabs(App& a)
{
    ImGui::SetCursorPos(ImVec2(S(16), S(kBarH) + S(8)));
    for (int i = 0; i < static_cast<int>(Tab::Count); ++i) {
        const bool sel = static_cast<int>(a.tab) == i;
        if (sel) { ImGui::PushStyleColor(ImGuiCol_Button, kAccent); }
        if (ImGui::Button(kTabNames[i], ImVec2(0, S(kTabH) - S(12)))) { a.tab = static_cast<Tab>(i); }
        if (sel) { ImGui::PopStyleColor(); }
        ImGui::SameLine();
    }
    ImGui::NewLine();
}

// One setting row. A checkbox / dropdown saves at once; a slider saves when it is released (its value is held in
// a.dragging meanwhile). Every save goes through LauncherApp::SaveSettings (game closed, ApplyEdits' clamps + rules).
void SettingRow(App& a, const SchemaRow& r)
{
    const std::string cur = CurrentValue(r, a.snap.ovr2xr);
    const std::string id = "##" + r.key;
    std::string next;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(DisplayName(r).c_str());
    if (!r.help.empty() && ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", r.help.c_str()); }
    ImGui::SameLine(S(380));
    ImGui::SetNextItemWidth(S(360));
    if (r.type == "bool") {
        bool b = std::atoi(cur.c_str()) != 0;
        if (ImGui::Checkbox(id.c_str(), &b)) { next = b ? "1" : "0"; }
    } else if (!r.items.empty()) {
        const int lo = static_cast<int>(r.lo);
        const int idx = std::atoi(cur.c_str()) - lo;
        const char* shown = (idx >= 0 && idx < static_cast<int>(r.items.size())) ? r.items[static_cast<size_t>(idx)].c_str() : "?";
        if (ImGui::BeginCombo(id.c_str(), shown)) {
            for (size_t i = 0; i < r.items.size(); ++i) {
                if (ImGui::Selectable(r.items[i].c_str(), static_cast<int>(i) == idx)) { next = std::to_string(static_cast<int>(i) + lo); }
            }
            ImGui::EndCombo();
        }
    } else {
        const auto held = a.dragging.find(r.key);
        const std::string shown = held != a.dragging.end() ? held->second : cur;
        std::string now;
        if (r.type == "int") {
            int v = std::atoi(shown.c_str());
            ImGui::SliderInt(id.c_str(), &v, static_cast<int>(r.lo), static_cast<int>(r.hi));
            now = std::to_string(v);
        } else {
            float v = static_cast<float>(std::atof(shown.c_str()));
            ImGui::SliderFloat(id.c_str(), &v, static_cast<float>(r.lo), static_cast<float>(r.hi), "%.2f");
            char b[32];
            std::snprintf(b, sizeof(b), "%g", v);
            now = b;
        }
        if (ImGui::IsItemActive()) { a.dragging[r.key] = now; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { next = now; }
        if (!ImGui::IsItemActive()) { a.dragging.erase(r.key); }
    }
    if (r.restart) {
        ImGui::SameLine();
        ImGui::TextDisabled("(next launch)");
    }
    if (!next.empty() && next != cur) {
        a.last = a.model.SaveSettings({{r.key, next}});
        a.snap = a.model.Snap();
    }
}

void SettingsSection(App& a, const char* tab, const char* section, bool settledOnly, bool skipSettled)
{
    for (int i : RowsIn(a.snap.schema, tab, section)) {
        const SchemaRow& r = a.snap.schema[static_cast<size_t>(i)];
        if ((settledOnly && !r.settled) || (skipSettled && r.settled)) { continue; }
        if (!Visible(r, a.snap.ovr2xr, a.snap.schema)) { continue; }
        ImGui::PushID(i);
        SettingRow(a, r);
        ImGui::PopID();
    }
}

bool SettingsReady(App& a)
{
    if (!a.snap.schemaOk) {
        Colored(kDim, "Install the mod first - its settings appear here once it is installed.");
        return false;
    }
    if (!a.snap.schemaMatchesLayer) {
        Colored(kWarn, "These settings were described for a different build of the mod than the one installed. Repair or "
                       "Update the mod so they match.");
    }
    if (a.snap.view.gameRunning) { Colored(kWarn, "Dying Light is running - settings are read-only until it is closed."); }
    return true;
}

void DrawPlay(App& a)
{
    const Snapshot& s = a.snap;
    const LauncherView& v = s.view;
    Heading(a, "Dying Light");
    ImGui::BeginChild("game", ImVec2(0, S(118)), ImGuiChildFlags_Borders);
    if (s.game.empty()) {
        Colored(kWarn, "Dying Light was not found. Choose its folder.");
    } else {
        ImGui::Text("%s", W2U(s.game.wstring()).c_str());
        ImGui::SameLine();
        ImGui::TextColored(kDim, "[%s]", setup::StoreTag(s.store));
        if (!s.storeNote.empty()) { Colored(v.programFiles ? kBad : kWarn, s.storeNote); }
        if (!s.gpuName.empty()) { ImGui::TextColored(kDim, "%s  -  Virtual Desktop: %s (advice)", s.gpuName.c_str(), s.vdAdvice.c_str()); }
    }
    ImGui::BeginDisabled(a.busy);
    if (ImGui::Button("Change folder...")) {
        const std::wstring f = BrowseFolder(a.hwnd);
        if (!f.empty()) { a.model.SelectGame(f); a.snap = a.model.Snap(); }
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    ImGui::Spacing();
    Colored(v.mode == Mode::Flat ? kWarn : (Installed(v) ? kOk : kDim), ModeLine(v));
    ImGui::Spacing();
    if (BigButton("LAUNCH IN VR", CanLaunchVr(v) && !a.busy, true)) {
        StartJob(a, "Switching to VR", [&a]() {
            Result r = a.model.ToVr();
            return r.ok ? a.model.Launch() : r;
        });
    }
    ImGui::SameLine();
    if (BigButton(FlatButtonLabel(v), CanLaunchFlat(v) && !a.busy)) {
        StartJob(a, "Switching to flat", [&a]() {
            Result r = a.model.ToFlat();
            return r.ok ? a.model.Launch() : r;
        });
    }
    ImGui::Spacing();
    ImGui::Separator();
    Heading(a, "The mod");
    const char* inst = s.installedName.empty() ? (v.match == InstallMatch::Foreign ? "an older install (not from the launcher)" : "none")
                                               : s.installedName.c_str();
    ImGui::Text("Installed: %s", inst);
    ImGui::Text("Available: %s", s.releaseName.empty() ? "-" : s.releaseName.c_str());
    if (!s.releaseWhy.empty()) { Colored(kDim, s.releaseWhy); }
    if (v.match == InstallMatch::Modified) {
        Colored(kWarn, std::to_string(s.differing.size()) + " file(s) differ from the installed version - " +
                           (v.newerRelease ? "Update" : "Repair") + " puts them right.");
    }
    if (v.match == InstallMatch::Foreign) { Colored(kWarn, "Press Uninstall (Support tab), verify the game files in your store, then install with the launcher."); }
    const bool canInstall = CanWrite(v) && !a.busy &&
                            ((v.match == InstallMatch::None && v.haveRelease) || OffersUpdate(v) || v.match == InstallMatch::Modified);
    if (BigButton(InstallButtonLabel(v), canInstall)) {
        if (v.match == InstallMatch::Modified && !v.newerRelease) {
            StartJob(a, "Repairing", [&a]() { return a.model.Repair(); });
        } else if (v.match == InstallMatch::None) {
            StartJob(a, "Installing", [&a]() { return a.model.Install(); });
        } else {
            StartJob(a, "Updating", [&a]() {
                Result r = a.model.Update();
                if (!r.ok) { return r; }
                bool restarting = false;
                const Result s = a.model.SelfUpdate(&restarting);
                if (restarting) { a.quitRequested = true; }
                return s.message.empty() ? r : (s.ok ? s : Result{true, r.message + " " + s.message});
            });
        }
    }
    ImGui::SameLine();
    if (BigButton("Check for updates", !a.busy)) { StartJob(a, "Checking for updates", [&a]() { return a.model.CheckForUpdates(); }); }
    if (!s.releaseNotes.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kDim, "What's new in %s:", s.releaseName.c_str());
        ImGui::BeginChild("notes", ImVec2(0, 0), ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", s.releaseNotes.c_str());
        ImGui::EndChild();
    }
}

void DrawComfort(App& a)
{
    if (!SettingsReady(a)) { return; }
    ImGui::BeginDisabled(!CanWrite(a.snap.view) || a.busy);
    SettingsSection(a, "comfort", "main", false, true);
    if (ImGui::CollapsingHeader("Hands & Arms")) { SettingsSection(a, "comfort", "hands_arms", false, false); }
    if (ImGui::CollapsingHeader("Hand Steadiness")) { SettingsSection(a, "comfort", "hand_steady", false, false); }
    if (ImGui::CollapsingHeader("Settled")) { SettingsSection(a, "comfort", "main", true, false); }
    ImGui::EndDisabled();
}

void DrawConfig(App& a)
{
    const Snapshot& s = a.snap;
    Heading(a, "Graphics");
    ImGui::BeginDisabled(!CanWrite(s.view) || a.busy);
    if (!s.aaOptions.empty()) {
        int cur = s.aaDefault;
        for (size_t i = 0; i < s.aaOptions.size(); ++i) {
            if (s.haveAa && s.aaOptions[i].method == s.aaMethod && s.aaOptions[i].quality == s.aaQuality) { cur = static_cast<int>(i); }
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Upscaler");
        ImGui::SameLine(S(380));
        ImGui::SetNextItemWidth(S(360));
        if (ImGui::BeginCombo("##aa", AaLabel(s.aaOptions[static_cast<size_t>(cur)]).c_str())) {
            for (size_t i = 0; i < s.aaOptions.size(); ++i) {
                const std::string l = AaLabel(s.aaOptions[i]) + (static_cast<int>(i) == s.aaDefault ? "  (recommended)" : "");
                if (ImGui::Selectable(l.c_str(), static_cast<int>(i) == cur)) {
                    a.last = a.model.SaveGraphics(s.aaOptions[i].method, s.aaOptions[i].quality);
                    a.snap = a.model.Snap();
                    break;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    Heading(a, "HUD");
    if (!SettingsReady(a)) { return; }
    ImGui::BeginDisabled(!CanWrite(a.snap.view) || a.busy);
    if (ImGui::CollapsingHeader("Menu panel")) { SettingsSection(a, "hud", "menu_panel", false, false); }
    if (ImGui::CollapsingHeader("HUD panel")) { SettingsSection(a, "hud", "hud_panel", false, false); }
    if (ImGui::CollapsingHeader("Wrist HUD")) {
        Colored(kDim, "The layout presets and placing pieces by hand are in the in-game menu (HUD tab).");
        SettingsSection(a, "hud", "wrist", false, false);
    }
    ImGui::EndDisabled();
}

void DrawControls(App& a)
{
    if (!a.controls.srv) {
        Colored(kWarn, "The controls picture was not found: " + W2U(a.controlsPath));
        return;
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float btnH = S(40);
    const FitRect r = FitImage(avail.x, avail.y - btnH - S(8), a.controls.w, a.controls.h);
    const ImVec2 c = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(c.x + r.x, c.y));
    ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(a.controls.srv)), ImVec2(r.w, r.h));
    ImGui::SetCursorPos(ImVec2(c.x + r.x, c.y + r.h + S(8)));
    if (ImGui::Button("View full size", ImVec2(S(200), btnH))) { ShellExecuteW(nullptr, L"open", a.controlsPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
}

void DrawSupport(App& a)
{
    const LauncherView& v = a.snap.view;
    Heading(a, "Logs");
    Colored(kDim, "Saves one zip to your Desktop with the game's crash logs, the mod's logs and settings. "
                  "Your Windows user name is masked in every file. Nothing is uploaded - send the zip yourself.");
    if (BigButton("Collect logs", !a.busy)) {
        StartJob(a, "Collecting logs", [&a]() {
            fs::path z;
            Result r = a.model.CollectLogs(&z);
            if (r.ok) { ShellExecuteW(nullptr, L"open", z.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
            return r;
        });
    }
    ImGui::Spacing();
    Heading(a, "Fix or remove");
    if (BigButton("Repair", CanWrite(v) && !a.busy && v.match == InstallMatch::Modified)) { StartJob(a, "Repairing", [&a]() { return a.model.Repair(); }); }
    ImGui::SameLine();
    if (BigButton("Roll back", CanWrite(v) && !a.busy && a.snap.havePrevious)) { StartJob(a, "Rolling back", [&a]() { return a.model.RollBack(); }); }
    ImGui::SameLine();
    if (BigButton("Uninstall", v.haveGame && !v.gameRunning && !a.busy && v.match != InstallMatch::None)) { a.confirmUninstall = true; }
    if (a.confirmUninstall) {
        Colored(kWarn, "Remove Dying Light VR from this folder? Your settings and arm calibration are deleted with it.");
        if (BigButton("Yes, uninstall", !a.busy, true)) {
            a.confirmUninstall = false;
            StartJob(a, "Uninstalling", [&a]() { return a.model.Uninstall(); });
        }
        ImGui::SameLine();
        if (BigButton("Cancel", true)) { a.confirmUninstall = false; }
    }
}

void DrawStatus(App& a)
{
    const float y = static_cast<float>(a.size.h) - S(kStatusH);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    dl->AddRectFilled(ImVec2(o.x, o.y + y), ImVec2(o.x + static_cast<float>(a.size.w), o.y + static_cast<float>(a.size.h)), kBarBg);
    ImGui::SetCursorPos(ImVec2(S(16), y + (S(kStatusH) - ImGui::GetTextLineHeight()) * 0.5f));
    if (a.busy) {
        ImGui::TextColored(kWarn, "%s...", a.busyWhat.c_str());
    } else if (!a.last.message.empty()) {
        std::string m = a.last.message;
        const size_t nl = m.find('\n');
        if (nl != std::string::npos) { m = m.substr(0, nl) + " (more in launcher.log)"; }
        ImGui::TextColored(a.last.ok ? kOk : kBad, "%s", m.c_str());
    } else if (a.snap.view.gameRunning) {
        ImGui::TextColored(kWarn, "Dying Light is running - close it to change anything.");
    } else {
        ImGui::Dummy(ImVec2(0, 0));   // ImGui needs an item after SetCursorPos (it flags the frame otherwise)
    }
}

void DrawFrame(App& a)
{
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(a.size.w), static_cast<float>(a.size.h)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(kBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    if (a.background.srv) {
        const ImVec2 p = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(a.background.srv)), p,
                                             ImVec2(p.x + static_cast<float>(a.size.w), p.y + static_cast<float>(a.size.h)));
    }
    DrawTitleBar(a);
    DrawTabs(a);
    const float top = S(kBarH + kTabH) + S(12);
    ImGui::SetCursorPos(ImVec2(S(24), top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(4), S(4)));
    ImGui::BeginChild("content", ImVec2(static_cast<float>(a.size.w) - S(48), static_cast<float>(a.size.h) - top - S(kStatusH) - S(10)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    switch (a.tab) {
        case Tab::Play: DrawPlay(a); break;
        case Tab::Comfort: DrawComfort(a); break;
        case Tab::Config: DrawConfig(a); break;
        case Tab::Controls: DrawControls(a); break;
        case Tab::Support: DrawSupport(a); break;
        case Tab::Count: break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    DrawStatus(a);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void RenderOnce(App& a)
{
    if (a.styleDirty) {
        a.styleDirty = false;
        ApplyStyle();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::PushFont(a.fontBody, S(16));
    DrawFrame(a);
    ImGui::PopFont();
    ImGui::Render();
    const float clear[4] = {0.078f, 0.082f, 0.098f, 1.0f};
    a.ctx->OMSetRenderTargets(1, &a.rtv, nullptr);
    a.ctx->ClearRenderTargetView(a.rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) { return 1; }
    switch (msg) {
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        RECT rc;
        GetClientRect(h, &rc);
        if (g_app && p.y >= 0 && p.y < static_cast<LONG>(S(kBarH)) && p.x < rc.right - static_cast<LONG>(S(kBtnW) * 2)) { return HTCAPTION; }
        return HTCLIENT;
    }
    case WM_DPICHANGED:
        if (g_app) {
            const RECT* sug = reinterpret_cast<const RECT*>(lp);
            POINT c{(sug->left + sug->right) / 2, (sug->top + sug->bottom) / 2};
            float dpi = 1.0f;
            RECT work{};
            MonitorFacts(c, &dpi, &work);
            g_app->size = WindowSize(dpi, work.right - work.left, work.bottom - work.top);
            SetWindowPos(h, nullptr, sug->left, sug->top, g_app->size.w, g_app->size.h, SWP_NOZORDER | SWP_NOACTIVATE);
            g_app->styleDirty = true;
            Log("monitor change: scale " + std::to_string(dpi) + " -> window " + std::to_string(g_app->size.w) + "x" + std::to_string(g_app->size.h));
        }
        return 0;
    case WM_SIZE:
        if (g_app && g_app->swap && wp != SIZE_MINIMIZED) {
            ReleaseRtv(*g_app);
            g_app->swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            CreateRtv(*g_app);
        }
        return 0;
    case WM_CLOSE:
        if (g_app && g_app->busy) { return 0; }   // never mid-job
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool CreateWindowAndDevice(App& a, bool visible, const RECT& work)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DyingLightVRLauncher";
    RegisterClassExW(&wc);
    const int x = work.left + (work.right - work.left - a.size.w) / 2;
    const int y = work.top + (work.bottom - work.top - a.size.h) / 2;
    a.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Dying Light VR", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU, x, y, a.size.w, a.size.h,
                             nullptr, nullptr, wc.hInstance, nullptr);
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
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &a.swap,
                                               &a.dev, &fl, &a.ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &a.swap, &a.dev,
                                           &fl, &a.ctx);
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

void SetupImGui(App& a)
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    BuildFonts(a);
    ApplyStyle();
    ImGui_ImplWin32_Init(a.hwnd);
    ImGui_ImplDX11_Init(a.dev, a.ctx);
    const std::wstring art = ExeDir() + L"\\art\\";
    a.background = LoadArt(a, kResArtBackground, art + L"background.png");
    a.controlsPath = art + L"controls.png";
    a.controls = LoadArt(a, kResArtControls, a.controlsPath);
    std::string embedded;
    if (ResourceBytes(kResArtControls, &embedded)) {   // "View full size" opens a file: the embedded picture, copied to %TEMP%
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        a.controlsPath = std::wstring(tmp) + L"DyingLightVR_controls.png";
        FILE* f = nullptr;
        if (_wfopen_s(&f, a.controlsPath.c_str(), L"wb") == 0 && f) {
            std::fwrite(embedded.data(), 1, embedded.size(), f);
            std::fclose(f);
        }
    }
}

void Shutdown(App& a)
{
    if (a.worker.joinable()) { a.worker.join(); }
    for (Image* i : {&a.background, &a.controls}) {
        if (i->srv) { i->srv->Release(); i->srv = nullptr; }
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ReleaseRtv(a);
    if (a.swap) { a.swap->Release(); }
    if (a.ctx) { a.ctx->Release(); }
    if (a.dev) { a.dev->Release(); }
    if (a.hwnd && IsWindow(a.hwnd)) { DestroyWindow(a.hwnd); }
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
            std::vector<BYTE> bgra(static_cast<size_t>(td.Width) * td.Height * 4);   // RGBA -> BGRA for WIC (setup_ui.cpp SavePng)
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
            IWICImagingFactory* f = nullptr;
            IWICStream* s = nullptr;
            IWICBitmapEncoder* e = nullptr;
            IWICBitmapFrameEncode* fr = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
                SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) && SUCCEEDED(e->Initialize(s, WICBitmapEncoderNoCache)) &&
                SUCCEEDED(e->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(td.Width, td.Height))) {
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

void Log(const std::string& line)
{
    const std::wstring p = ExeDir() + L"\\launcher.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"ab") != 0 || !f) { return; }
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, line.c_str());
    std::fclose(f);
}

int RunUi(const UiArgs& args)
{
    App a;
    g_app = &a;
    a.args = args;
    POINT cur{};
    GetCursorPos(&cur);
    float dpi = 1.0f;
    RECT work{};
    MonitorFacts(cur, &dpi, &work);
    a.size = WindowSize(dpi, work.right - work.left, work.bottom - work.top);
    Log("start: monitor scale " + std::to_string(dpi) + ", work area " + std::to_string(work.right - work.left) + "x" +
        std::to_string(work.bottom - work.top) + " -> window " + std::to_string(a.size.w) + "x" + std::to_string(a.size.h));
    if (!CreateWindowAndDevice(a, true, work)) { return 3; }
    SetupImGui(a);
    a.model.Init(args.gameDir, args.releaseDir);
    a.snap = a.model.Snap();
    if (args.afterUpdate) {   // the new exe's window is up: tell the old exe it can go (launcher_update.h, self-update)
        FILE* f = nullptr;
        if (_wfopen_s(&f, (ExeDir() + L"\\launcher_update_ok.txt").c_str(), L"wb") == 0 && f) { std::fclose(f); }
        Log("self-update: the new launcher is up - signalled the old one");
    }
    LauncherApp::CleanupOldExe();   // a left-over .old.exe from an earlier self-update (the old process may still be exiting)
    while (!a.quit) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
            if (m.message == WM_QUIT) { a.quit = true; }
        }
        if (a.quit) { break; }
        PollJob(a);
        RenderOnce(a);
        a.swap->Present(1, 0);
    }
    a.hwnd = nullptr;   // destroyed by WM_CLOSE
    Shutdown(a);
    g_app = nullptr;
    return 0;
}

int RenderShot(const UiArgs& args, const std::string& state, const std::wstring& png, float dpiScale, int workW, int workH)
{
    App a;
    g_app = &a;
    a.args = args;
    a.size = WindowSize(dpiScale, workW, workH);
    const RECT work{0, 0, workW, workH};
    if (!CreateWindowAndDevice(a, false, work)) { return 3; }
    SetupImGui(a);
    bool known = state == "blank";
    for (int i = 0; i < static_cast<int>(Tab::Count); ++i) {
        std::string n = kTabNames[i];
        for (char& c : n) { c = (c == ' ') ? '_' : static_cast<char>(tolower(static_cast<unsigned char>(c))); }
        if (state == n) { a.tab = static_cast<Tab>(i); known = true; }
    }
    if (!known) {
        Log("--shot: unknown state " + state);
        Shutdown(a);
        g_app = nullptr;
        return 4;
    }
    if (state != "blank") {   // READ-ONLY: the model only reads the folder; a shot never runs an action
        a.model.Init(args.gameDir, args.releaseDir);
        a.snap = a.model.Snap();
    }
    for (int i = 0; i < 4; ++i) { RenderOnce(a); }
    const bool ok = SavePng(a, png);
    Log("--shot " + state + " " + std::to_string(a.size.w) + "x" + std::to_string(a.size.h) + " -> " + (ok ? "written" : "FAILED"));
    Shutdown(a);
    g_app = nullptr;
    return ok ? 0 : 5;
}

}  // namespace dlvr::launcher
