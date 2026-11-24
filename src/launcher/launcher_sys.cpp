// See launcher_sys.h.
#include "launcher_sys.h"

#include <windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <cstdio>
#include <fstream>

#include "launcher_ui.h"

#pragma comment(lib, "bcrypt.lib")

namespace dlvr::launcher {

namespace {

std::string LastError()
{
    const DWORD e = GetLastError();
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring m = buf ? buf : L"";
    if (buf) { LocalFree(buf); }
    while (!m.empty() && (m.back() == L'\n' || m.back() == L'\r' || m.back() == L' ')) { m.pop_back(); }
    return W2U(m) + " (error " + std::to_string(e) + ")";
}

fs::path Known(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) { out = p; }
    if (p) { CoTaskMemFree(p); }
    return out;
}

bool EnsureParent(const fs::path& p, std::string* why)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec && !fs::is_directory(p.parent_path())) { *why = "could not create " + W2U(p.parent_path().wstring()) + " (" + ec.message() + ")"; return false; }
    return true;
}

}  // namespace

std::string W2U(const std::wstring& w)
{
    if (w.empty()) { return {}; }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring U2W(const std::string& u)
{
    if (u.empty()) { return {}; }
    const int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), static_cast<int>(u.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u.data(), static_cast<int>(u.size()), w.data(), n);
    return w;
}

// TEST ONLY: %DLVR_LAUNCHER_TEST_DOCS% / %DLVR_LAUNCHER_TEST_DESKTOP% redirect these for the end-to-end on a fake folder, so
// the test never writes the developer's real video.scr or Desktop (trace docs/traces/20261115_2110_launcher_t10_testdirs.md).
static fs::path TestOverride(const wchar_t* var, bool* logged)
{
    wchar_t v[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(var, v, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { return {}; }
    if (!*logged) { *logged = true; Log("TEST OVERRIDE active: " + W2U(var) + " = " + W2U(v)); }
    return fs::path(v);
}

fs::path DocumentsDir()
{
    static bool logged = false;
    const fs::path t = TestOverride(L"DLVR_LAUNCHER_TEST_DOCS", &logged);
    return t.empty() ? Known(FOLDERID_Documents) : t;
}

fs::path DesktopDir()
{
    static bool logged = false;
    const fs::path t = TestOverride(L"DLVR_LAUNCHER_TEST_DESKTOP", &logged);
    return t.empty() ? Known(FOLDERID_Desktop) : t;
}

fs::path VideoScrPath()
{
    const fs::path d = DocumentsDir();
    return d.empty() ? fs::path() : d / L"DyingLight" / L"out" / L"settings" / L"video.scr";
}

bool ReadAllBytes(const fs::path& p, std::string* out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) { return false; }
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string Md5File(const fs::path& p)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return ""; }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hh = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD got = 0;
        bool ok = true;
        while (ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0) {
            if (BCryptHashData(hh, buf.data(), got, 0) != 0) { ok = false; break; }
        }
        unsigned char d[16] = {};
        if (ok && BCryptFinishHash(hh, d, 16, 0) == 0) {
            char s[33] = {};
            for (int i = 0; i < 16; ++i) { std::snprintf(s + 2 * i, 3, "%02x", d[i]); }
            hex = s;
        }
    }
    if (hh) { BCryptDestroyHash(hh); }
    if (alg) { BCryptCloseAlgorithmProvider(alg, 0); }
    CloseHandle(h);
    return hex;
}

bool GameIsRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { return true; }   // "I could not check" is not "it is closed"
    PROCESSENTRY32W pe{sizeof(pe)};
    bool running = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"DyingLightGame.exe") == 0) { running = true; break; }
    }
    CloseHandle(snap);
    return running;
}

setup::DetectedState Detect(const fs::path& game, int* unreadable)
{
    setup::DetectedState s;
    *unreadable = 0;
    std::error_code ec;
    auto it = fs::recursive_directory_iterator(game, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { break; }
        std::error_code e2;
        const fs::path rel = fs::relative(it->path(), game, e2);
        if (e2) { ++*unreadable; continue; }
        std::string r = setup::detail::Lower(W2U(rel.generic_wstring()));
        const bool want = detail::ScanWants(r);   // launcher_plan.h
        if (it->is_directory(e2)) {
            if (!want && it.depth() > 0) { it.disable_recursion_pending(); }
            continue;
        }
        if (e2) { ++*unreadable; continue; }
        if (want) { s.present.insert(r); }
    }
    if (ec) { ++*unreadable; Log("detect: the folder scan stopped early (" + ec.message() + ")"); }
    s.modPresent = s.present.count("libovrrt64_1.dll") > 0 || fs::exists(game / L"dlvr", ec);
    s.haveOvr2xr = ReadAllBytes(game / L"dlvr" / L"ovr2xr.txt", &s.ovr2xrText);
    s.haveWritten = ReadAllBytes(game / L"dlvr" / L"setup_settings_written.txt", &s.writtenText);
    ReadAllBytes(game / L"dlvr" / L"aa_method.txt", &s.aaMethodText);
    ReadAllBytes(game / L"dlvr" / L"dlss_quality.txt", &s.dlssQualityText);
    if (*unreadable) { Log("detect: " + std::to_string(*unreadable) + " unreadable entries skipped"); }
    return s;
}

std::map<std::string, std::string> DiskMd5(const fs::path& game, const std::vector<std::string>& relPaths)
{
    std::map<std::string, std::string> out;
    for (const auto& r : relPaths) {
        const std::string h = Md5File(game / fs::u8path(r));
        if (!h.empty()) { out[detail::LowerPath(r)] = h; }
    }
    return out;
}

std::vector<std::string> ListFiles(const fs::path& dir)
{
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { return out; }
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) { out.push_back(W2U(fs::relative(it->path(), dir, e2).generic_wstring())); }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------- RealOps ---

fs::path RealOps::Resolve(const std::string& rel) const
{
    if (rel == setup::kVideoScrPath) { return VideoScrPath(); }
    return game_ / fs::u8path(rel);
}

bool RealOps::GameRunning() { return GameIsRunning(); }

bool RealOps::CopyFromRelease(const std::string& releasePath, const std::string& to, std::string* why)
{
    const fs::path src = staged_ / fs::u8path(releasePath);
    const fs::path dst = Resolve(to);
    if (!EnsureParent(dst, why)) { return false; }
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) { *why = LastError(); return false; }
    return true;
}

bool RealOps::Copy(const std::string& from, const std::string& to, std::string* why)
{
    const fs::path dst = Resolve(to);
    if (!EnsureParent(dst, why)) { return false; }
    if (!CopyFileW(Resolve(from).c_str(), dst.c_str(), FALSE)) { *why = LastError(); return false; }
    return true;
}

bool RealOps::Move(const std::string& from, const std::string& to, std::string* why)
{
    const fs::path dst = Resolve(to);
    if (!EnsureParent(dst, why)) { return false; }
    if (!MoveFileExW(Resolve(from).c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
        *why = LastError();
        return false;
    }
    return true;
}

bool RealOps::Write(const std::string& path, const std::string& text, std::string* why)
{
    const fs::path dst = Resolve(path);
    if (dst.empty()) { *why = "the Documents folder could not be found"; return false; }
    if (!EnsureParent(dst, why)) { return false; }
    const fs::path tmp = dst.wstring() + L".dlvrtmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { *why = "could not open " + W2U(tmp.wstring()) + " for writing"; return false; }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) { *why = "write failed"; return false; }
    }
    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        *why = LastError();
        DeleteFileW(tmp.c_str());
        return false;
    }
    std::string back;
    if (!ReadAllBytes(dst, &back) || back != text) { *why = "read back differs from what was written"; return false; }
    return true;
}

bool RealOps::Remove(const std::string& path, std::string* why)
{
    const fs::path t = Resolve(path);
    std::error_code ec;
    if (!path.empty() && path.back() == '/') {
        fs::remove_all(t, ec);
    } else {
        fs::remove(t, ec);
    }
    const std::error_code removeEc = ec;   // kept: exists() below must not overwrite the reason (S02 item 2)
    std::error_code e2;
    if (fs::exists(t, e2)) {
        *why = "could not delete (" + (removeEc ? removeEc.message() : std::string("still present")) + ")";
        return false;
    }
    return true;
}

std::string RealOps::Md5(const std::string& path) { return Md5File(Resolve(path)); }

bool RealOps::RunVcRedist(std::string* why)
{
    std::wstring cmd = L"\"" + (staged_ / L"vc_redist.x64.exe").wstring() + L"\" /install /quiet /norestart";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        *why = LastError();
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code == 0 || code == 1638 || code == 3010) { return true; }   // ok / a newer one is installed / reboot pending
    *why = "vc_redist.x64.exe exit code " + std::to_string(code);
    return false;
}

}  // namespace dlvr::launcher
