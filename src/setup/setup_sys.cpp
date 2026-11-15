// See setup_sys.h. Ported from v1's setup_main.cpp (59808c9c), plus Epic / GOG discovery (spec 8.4) and the embedded
// payload (spec 8.3).
#include "setup_sys.h"
#include "setup_profile.h"

#include <dxgi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#pragma comment(lib, "version.lib")

namespace dlvr::setup {

// ------------------------------------------------------------------ log ---

namespace {
std::wstring g_logPath;
std::mutex g_logLock;
}

void SetLogPath(const std::wstring& path) { g_logPath = path; }
std::wstring LogPath() { return g_logPath; }

void Log(const std::string& s)
{
    std::lock_guard<std::mutex> l(g_logLock);
    if (!g_logPath.empty()) {
        std::ofstream f(fs::path(g_logPath), std::ios::binary | std::ios::app);
        f << s << "\r\n";
    }
}

std::wstring W(const std::string& s)
{
    if (s.empty()) { return L""; }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string U(const std::wstring& w)
{
    if (w.empty()) { return ""; }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool ReadAll(const fs::path& p, std::string* out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) { return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

namespace {

std::string LowerA(std::string s)
{
    for (auto& c : s) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
    return s;
}

// Write `bytes` to `p` (directories created), then read it back: the md5 of what is ON DISK must equal `md5`
// (or, for text, the bytes must be identical). A locked file reports the Windows error number.
bool WriteBytes(const fs::path& p, const std::string& bytes, std::string* why)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *why = "copy failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    size_t off = 0;
    bool ok = true;
    while (off < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - off, 1u << 24));
        DWORD wrote = 0;
        if (!WriteFile(h, bytes.data() + off, chunk, &wrote, nullptr) || wrote != chunk) {
            *why = "write failed (error " + std::to_string(GetLastError()) + ")";
            ok = false;
            break;
        }
        off += chunk;
    }
    CloseHandle(h);
    return ok;
}

bool WriteTextVerified(const fs::path& p, const std::string& text, std::string* why)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.wstring() + L".tmp";
    if (!WriteBytes(tmp, text, why)) { return false; }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *why = "cannot replace " + U(p.wstring()) + " (error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(tmp.c_str());
        return false;
    }
    std::string back;
    if (!ReadAll(p, &back) || back != text) {
        *why = "read-back differs: " + U(p.wstring());
        return false;
    }
    return true;
}

std::string Md5Disk(const fs::path& p)
{
    std::string b;
    return ReadAll(p, &b) ? Md5Hex(b) : "";
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value, DWORD flags = 0)
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD cb = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | flags, nullptr, buf, &cb) != ERROR_SUCCESS) { return L""; }
    return buf;
}

std::wstring SteamRoot()
{
    std::wstring s = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    for (auto& c : s) { if (c == L'/') { c = L'\\'; } }
    return s;
}

std::vector<fs::path> SteamLibraries(const std::wstring& steam)
{
    std::vector<fs::path> libs;
    if (steam.empty()) { return libs; }
    libs.push_back(steam);
    std::string vdf;
    if (!ReadAll(fs::path(steam) / L"steamapps" / L"libraryfolders.vdf", &vdf)) { return libs; }
    size_t p = 0;
    while ((p = vdf.find("\"path\"", p)) != std::string::npos) {
        const size_t a = vdf.find('"', p + 6);
        if (a == std::string::npos) { break; }
        std::string v;
        size_t i = a + 1;
        for (; i < vdf.size() && vdf[i] != '"'; ++i) {
            if (vdf[i] == '\\' && i + 1 < vdf.size()) { ++i; }
            v += vdf[i];
        }
        libs.push_back(W(v));
        p = i;
    }
    return libs;
}

bool IsGameFolder(const fs::path& g)
{
    std::error_code ec;
    return !g.empty() && fs::exists(g / L"DyingLightGame.exe", ec);
}

}  // namespace

// ------------------------------------------------------------ discovery ---

Store StoreOfFolder(const fs::path& game)
{
    std::error_code ec;
    FolderFacts f{};
    const std::string low = LowerA(U(game.wstring()));
    f.steamappsPath = low.find("\\steamapps\\common\\") != std::string::npos;
    f.steamApiDll = fs::exists(game / L"steam_api64.dll", ec);
    f.egstoreDir = fs::is_directory(game / L".egstore", ec);
    for (const auto& e : fs::directory_iterator(game, ec)) {
        const std::string n = LowerA(U(e.path().filename().wstring()));
        if (n.compare(0, 8, "goggame-") == 0 && n.size() > 5 && n.compare(n.size() - 5, 5, ".info") == 0) {
            f.gogInfo = true;
        }
    }
    return StoreOf(f);
}

std::vector<Candidate> FindGames(const DiscoveryOptions& o)
{
    std::vector<Candidate> out;
    std::set<std::string> seen;
    auto add = [&](const fs::path& g, const char* how) {
        if (!IsGameFolder(g)) { return; }
        std::error_code ec;
        const fs::path c = fs::weakly_canonical(g, ec);
        const std::string key = LowerA(U((ec ? g : c).wstring()));
        if (!seen.insert(key).second) { return; }
        const Store s = StoreOfFolder(g);
        out.push_back({g, s});
        Log(std::string("found Dying Light (") + how + "): " + U(g.wstring()) + " [" + StoreTag(s) + "]");
    };
    if (o.useRegistry) {
        for (const auto& lib : SteamLibraries(SteamRoot())) {
            add(lib / L"steamapps" / L"common" / L"Dying Light", "Steam library");
        }
    }
    // Epic: one JSON manifest per installed game.
    fs::path em = o.epicManifests;
    if (em.empty()) {
        wchar_t pd[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH) > 0) {
            em = fs::path(pd) / L"Epic" / L"EpicGamesLauncher" / L"Data" / L"Manifests";
        }
    }
    std::error_code ec;
    if (!em.empty() && fs::is_directory(em, ec)) {
        for (const auto& e : fs::directory_iterator(em, ec)) {
            if (LowerA(U(e.path().extension().wstring())) != ".item") { continue; }
            std::string json, loc;
            if (ReadAll(e.path(), &json) && EpicManifestDyingLight(json, &loc)) { add(fs::path(W(loc)), "Epic manifest"); }
        }
    }
    // GOG: SOFTWARE\GOG.com\Games\<id> -> path, both registry views.
    if (o.useRegistry) {
        for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
            HKEY games = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games", 0, KEY_READ | view, &games) != ERROR_SUCCESS) {
                continue;
            }
            wchar_t sub[256];
            for (DWORD i = 0;; ++i) {
                DWORD n = 256;
                if (RegEnumKeyExW(games, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) { break; }
                const std::wstring key = std::wstring(L"SOFTWARE\\GOG.com\\Games\\") + sub;
                const std::wstring path = RegString(HKEY_LOCAL_MACHINE, key.c_str(), L"path",
                                                    view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : RRF_SUBKEY_WOW6464KEY);
                if (!path.empty()) { add(path, "GOG registry"); }
            }
            RegCloseKey(games);
        }
    }
    if (out.empty()) { Log("no Dying Light folder found (Steam libraries, Epic manifests, GOG registry)"); }
    return out;
}

bool UnderProgramFiles(const fs::path& g)
{
    // review finding 13: canonicalise (junctions, 8.3 names, '..') and normalise (PathUnder) before comparing
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(g, ec);
    const std::string p = U((ec ? g : c).wstring());
    std::vector<std::string> roots;
    for (const wchar_t* v : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"}) {
        wchar_t buf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(v, buf, MAX_PATH) > 0) {
            roots.push_back(U(buf));
            const fs::path rc = fs::weakly_canonical(fs::path(buf), ec);
            if (!ec) { roots.push_back(U(rc.wstring())); }
        }
    }
    return PathUnder(p, roots) || PathUnder(U(g.wstring()), roots);
}

bool FilesIntact(const fs::path& game, const std::vector<ManifestEntry>& m)
{
    std::error_code ec;
    for (const auto& e : m) {
        const std::string low = LowerA(e.relPath);
        if (low == "dlvr/aa_method.txt" || low == "dlvr/dlss_quality.txt" || low == "dlvr/dlss_preset.txt" ||
            low == "dlvr/dlss_sharpness.txt" || low == "dw/dlvr_pp.mth") {
            continue;
        }
        const fs::path p = game / fs::u8path(e.relPath).make_preferred();
        if (e.dlssOnly && !fs::exists(p, ec)) { continue; }
        if (Md5Disk(p) != e.md5) {
            Log("intact check: " + e.relPath + " missing or changed");
            return false;
        }
    }
    return true;
}

int GameRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { return -1; }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    int found = 0;
    if (!Process32FirstW(snap, &pe)) {
        CloseHandle(snap);
        return -1;
    }
    do {
        if (_wcsicmp(pe.szExeFile, L"DyingLightGame.exe") == 0) { found = 1; }
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}

Gpu BestGpu()
{
    Gpu best;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) || !f) { return best; }
    SIZE_T bestMem = 0;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (best.name.empty() || d.DedicatedVideoMemory > bestMem)) {
            best.name = U(d.Description);
            best.vendor = d.VendorId;
            bestMem = d.DedicatedVideoMemory;
        }
        a->Release();
    }
    f->Release();
    return best;
}

VcVersion InstalledVcRuntime()
{
    VcVersion best{0, 0, 0};
    for (const wchar_t* k : {L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64",
                             L"SOFTWARE\\WOW6432Node\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64"}) {
        DWORD inst = 0, ma = 0, mi = 0, bl = 0;
        DWORD cb = sizeof(DWORD);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, k, L"Installed", RRF_RT_REG_DWORD, nullptr, &inst, &cb) != ERROR_SUCCESS ||
            inst != 1) {
            continue;
        }
        cb = sizeof(DWORD);
        RegGetValueW(HKEY_LOCAL_MACHINE, k, L"Major", RRF_RT_REG_DWORD, nullptr, &ma, &cb);
        cb = sizeof(DWORD);
        RegGetValueW(HKEY_LOCAL_MACHINE, k, L"Minor", RRF_RT_REG_DWORD, nullptr, &mi, &cb);
        cb = sizeof(DWORD);
        RegGetValueW(HKEY_LOCAL_MACHINE, k, L"Bld", RRF_RT_REG_DWORD, nullptr, &bl, &cb);
        const VcVersion v{ma, mi, bl};
        if (VersionLess(best, v)) { best = v; }
    }
    return best;
}

namespace {
// vc_redist written out to %TEMP% (once per run) - for its version and for running it.
fs::path ExtractVcRedist(Payload& p, std::string* why)
{
    std::string bytes;
    if (!p.Read("vc_redist.x64.exe", &bytes, why)) { return {}; }
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const fs::path out = fs::path(tmp) / L"DyingLightVR_vc_redist.x64.exe";
    if (!WriteBytes(out, bytes, why)) { return {}; }
    return out;
}
}  // namespace

VcVersion BundledVcRuntime(Payload& p)
{
    std::string why;
    const fs::path f = ExtractVcRedist(p, &why);
    if (f.empty()) { return {0, 0, 0}; }
    DWORD h = 0;
    const DWORD n = GetFileVersionInfoSizeW(f.c_str(), &h);
    if (n == 0) { return {0, 0, 0}; }
    std::vector<unsigned char> buf(n);
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(f.c_str(), 0, n, buf.data()) ||
        !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi) {
        return {0, 0, 0};
    }
    return {HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS)};
}

std::string VerText(const VcVersion& v)
{
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.bld);
}

DetectedState Detect(const fs::path& game)
{
    DetectedState s;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(game, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { break; }
        const fs::path rel = fs::relative(it->path(), game, ec);
        const std::string r = LowerA(U(rel.generic_wstring()));
        const bool want = r.find('/') == std::string::npos || r.compare(0, 5, "dlvr/") == 0 ||
                          (r.compare(0, 3, "dw/") == 0 && r.find('/', 3) == std::string::npos) ||
                          (r.compare(0, 9, "dw_dlc24/") == 0 && r.find('/', 9) == std::string::npos);
        if (it->is_directory(ec)) {
            if (!want && it.depth() > 0) { it.disable_recursion_pending(); }
            continue;
        }
        if (want) { s.present.insert(r); }
    }
    s.modPresent = s.present.count("libovrrt64_1.dll") > 0 || fs::exists(game / L"dlvr", ec);
    s.haveOvr2xr = ReadAll(game / L"dlvr" / L"ovr2xr.txt", &s.ovr2xrText);
    s.haveWritten = ReadAll(game / L"dlvr" / L"setup_settings_written.txt", &s.writtenText);
    ReadAll(game / L"dlvr" / L"aa_method.txt", &s.aaMethodText);
    ReadAll(game / L"dlvr" / L"dlss_quality.txt", &s.dlssQualityText);
    return s;
}

std::string LayoutWarning(Store store, const fs::path& userdata)
{
    if (store != Store::Steam) {
        Log(std::string("controller layout: NOT CHECKED - only Steam's profile location is known (") + StoreTag(store) + ")");
        return "Setup can't check your controller layout on this version of the game. Please check in the game that "
               "Options -> Controls -> controller layout is Default.";
    }
    std::error_code ec;
    if (userdata.empty() || !fs::is_directory(userdata, ec)) {
        Log("controller layout: NOT CHECKED - Steam's userdata folder was not found (" +
            (userdata.empty() ? std::string("Steam not found") : U(userdata.wstring())) + ")");
        return "Setup could not look for your controller layout (Steam's userdata folder was not found). Please check in "
               "the game that Options -> Controls -> controller layout is Default.";
    }
    std::vector<fs::path> files;
    for (const auto& id : fs::directory_iterator(userdata, ec)) {
        const fs::path p = id.path() / L"239140" / L"remote" / L"out" / L"profiles" / L"default.pro";
        if (fs::exists(p, ec)) { files.push_back(p); }
    }
    if (files.empty()) {
        Log("controller layout: no Dying Light profile in " + U(userdata.wstring()) + " yet (new PC) - the game starts on Default");
    }
    std::string warn;
    for (const auto& p : files) {
        std::string bytes;
        LayoutResult r{LayoutVerdict::Unreadable, {}, 0};
        if (ReadAll(p, &bytes)) { r = ReadLayout(std::vector<unsigned char>(bytes.begin(), bytes.end())); }
        Log("profile " + U(p.wstring()) + ": " +
            (r.verdict == LayoutVerdict::AllDefault ? "all layout keys 0 (Default)"
             : r.verdict == LayoutVerdict::NonDefault ? std::to_string(r.nonDefaultKeys.size()) + " layout key(s) not 0"
                                                      : "could not read") +
            " (" + std::to_string(r.layoutKeysFound) + " layout keys)");
        if (r.verdict == LayoutVerdict::NonDefault) {
            warn += "Your game's controller layout is not Default (Steam profile " + U(p.parent_path().wstring()) +
                    "). In the game: Options -> Controls -> set the controller layout to Default. The mod's buttons are "
                    "made for it.\n";
        } else if (r.verdict == LayoutVerdict::Unreadable) {
            warn += "Setup could not read your controller layout (" + U(p.wstring()) +
                    "). Please check in the game that Options -> Controls -> controller layout is Default.\n";
        }
    }
    if (!warn.empty() && warn.back() == '\n') { warn.pop_back(); }
    return warn;
}

// --------------------------------------------------------------- execute ---

namespace {

fs::path Target(const Action& a, const fs::path& game, const fs::path& docs)
{
    if (a.relPath == kVideoScrPath) { return docs / L"DyingLight" / L"out" / L"settings" / L"video.scr"; }
    return game / fs::u8path(a.relPath).make_preferred();
}

bool RunVcRedist(Payload& payload, std::string* why)
{
    const fs::path exe = ExtractVcRedist(payload, why);
    if (exe.empty()) { return false; }
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    const std::wstring file = exe.wstring();
    sei.lpFile = file.c_str();
    sei.lpParameters = L"/install /quiet /norestart";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        *why = "could not start the Visual C++ runtime installer (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0 && code != 1638 && code != 3010) {   // 1638 newer already installed, 3010 restart needed
        *why = "the Visual C++ runtime installer returned " + std::to_string(code);
        return false;
    }
    return true;
}

}  // namespace

Outcome Execute(const std::vector<Action>& acts, const fs::path& game, const fs::path& docs, Payload& payload,
                Progress* progress)
{
    if (progress) {
        progress->total = static_cast<int>(acts.size());
        progress->done = 0;
    }
    std::vector<bool> ok;
    std::vector<const Action*> deferred;   // run only after the 5 s re-check passed (the install record)
    for (const auto& a : acts) {
        if (a.afterRecheck) {
            deferred.push_back(&a);
            ok.push_back(true);   // counted when it runs below
            continue;
        }
        const fs::path t = Target(a, game, docs);
        if (progress) { progress->SetCurrent(a.relPath); }
        std::string why;
        bool good = false;
        std::error_code ec;
        switch (a.kind) {
        case ActKind::RunVcRedist:
            good = RunVcRedist(payload, &why);
            break;
        case ActKind::Copy: {
            std::string bytes;
            if (!payload.Read(a.relPath, &bytes, &why)) { break; }
            if (!WriteBytes(t, bytes, &why)) { break; }
            const std::string got = Md5Disk(t);
            good = (got == a.md5);
            if (!good) { why = "md5 " + (got.empty() ? std::string("unreadable") : got) + ", manifest " + a.md5; }
            break;
        }
        case ActKind::WriteText:
            good = WriteTextVerified(t, a.text, &why);
            break;
        case ActKind::Delete:
            if (!a.relPath.empty() && a.relPath.back() == '/') {
                fs::remove_all(t, ec);
            } else {
                fs::remove(t, ec);
            }
            good = !fs::exists(t, ec);
            if (!good) { why = "could not delete (" + ec.message() + ")"; }
            break;
        }
        static const char* kKind[] = {"RUNVCREDIST", "COPY", "WRITE", "DELETE"};
        Log(std::string(good ? "ok    " : "FAIL  ") + kKind[static_cast<int>(a.kind)] + " " + a.relPath +
            (good ? "" : "  - " + why));
        ok.push_back(good);
        if (progress) { progress->done++; }
        if (!good) {
            Outcome o = Summarise(acts, ok);
            o.failedPath += " - " + why;
            return o;
        }
    }
    Outcome o = Summarise(acts, ok);

    // Antivirus quarantine check: every copied file again, 5 s later - EVERY one that vanished or changed is named.
    if (progress) { progress->SetCurrent("checking every file again..."); }
    Sleep(5000);
    std::string bad;
    for (const auto& a : acts) {
        if (a.kind != ActKind::Copy) { continue; }
        const std::string got = Md5Disk(Target(a, game, docs));
        if (got != a.md5) {
            Log("FAIL  RECHECK " + a.relPath + " - " + (got.empty() ? "missing (quarantined?)" : "changed to " + got));
            bad += "\n  " + a.relPath + (got.empty() ? "  (gone)" : "  (changed)");
        }
    }
    if (!bad.empty()) {
        return {false, "these files were removed or changed 5 seconds after copying - check your antivirus:" + bad};
    }
    Log("ok    RECHECK every copied file after 5 s");
    for (const Action* a : deferred) {   // the install record: only now that every file has held for 5 s
        std::string why;
        if (!WriteTextVerified(Target(*a, game, docs), a->text, &why)) {
            Log("FAIL  WRITE " + a->relPath + "  - " + why);
            return {false, a->relPath + " - " + why};
        }
        Log("ok    WRITE " + a->relPath + " (after the re-check)");
        if (progress) { progress->done++; }
    }
    return o;
}

}  // namespace dlvr::setup
