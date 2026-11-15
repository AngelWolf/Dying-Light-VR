// See launcher_net.h.
#include "launcher_net.h"

#include <windows.h>
#include <winhttp.h>

#include <fstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace dlvr::launcher {

namespace {

std::wstring Widen(const std::string& s)
{
    if (s.empty()) { return {}; }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool Get(const std::string& url, std::string* body, std::string* why)
{
    const std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        *why = "not an https URL: " + url;
        return false;
    }
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"DyingLightVR-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    if (r) {
        const wchar_t* hdr = L"Accept: application/octet-stream, application/json\r\n";
        if (WinHttpSendRequest(r, hdr, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                WINHTTP_NO_HEADER_INDEX);
            if (status != 200) {
                *why = "HTTP status " + std::to_string(status) + " for " + url;
            } else {
                body->clear();
                std::vector<char> buf(1 << 16);
                DWORD got = 0;
                ok = true;
                while (true) {
                    if (!WinHttpReadData(r, buf.data(), static_cast<DWORD>(buf.size()), &got)) { ok = false; *why = "the download broke off"; break; }
                    if (got == 0) { break; }
                    body->append(buf.data(), got);
                }
            }
        } else {
            *why = "could not reach " + url + " (WinHTTP error " + std::to_string(GetLastError()) + ")";
        }
    } else if (why->empty()) {
        *why = "could not open a connection for " + url;
    }
    if (r) { WinHttpCloseHandle(r); }
    if (c) { WinHttpCloseHandle(c); }
    if (s) { WinHttpCloseHandle(s); }
    return ok;
}

}  // namespace

bool HttpGetText(const std::string& url, std::string* body, std::string* why) { return Get(url, body, why); }

bool HttpGetToFile(const std::string& url, const std::filesystem::path& out, std::string* why)
{
    std::string body;
    if (!Get(url, &body, why)) { return false; }
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) { *why = "could not write " + out.u8string(); return false; }
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(f);
}

}  // namespace dlvr::launcher
