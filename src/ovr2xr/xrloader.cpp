#include "xrloader.h"

#include <openxr/openxr_loader_negotiation.h>

#include <cstdio>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {

PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr_ = nullptr;
char g_runtimePath[MAX_PATH * 2] = {};

#define DLVR_DEFINE(fn) PFN_##fn fn##_ = nullptr;
DLVR_XR_GLOBAL_FUNCS(DLVR_DEFINE)
DLVR_XR_INSTANCE_FUNCS(DLVR_DEFINE)
#undef DLVR_DEFINE

namespace {

HMODULE g_runtimeDll = nullptr;

// Reads HKEY_*\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime, which holds the path of
// a JSON manifest. HKCU wins over HKLM, matching the loader's own precedence.
bool ReadActiveRuntimeManifest(char* out, size_t cch)
{
    struct { HKEY root; const char* label; } roots[] = {
        { HKEY_CURRENT_USER,  "HKCU" },
        { HKEY_LOCAL_MACHINE, "HKLM" },
    };
    for (auto& r : roots) {
        HKEY k = nullptr;
        if (RegOpenKeyExA(r.root, "SOFTWARE\\Khronos\\OpenXR\\1", 0,
                          KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) {
            continue;
        }
        DWORD type = 0;
        DWORD bytes = static_cast<DWORD>(cch);
        LONG rc = RegQueryValueExA(k, "ActiveRuntime", nullptr, &type,
                                   reinterpret_cast<BYTE*>(out), &bytes);
        RegCloseKey(k);
        if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && out[0]) {
            // NOMINMAX is set, so no std::min from windows.h here.
            const DWORD cap = static_cast<DWORD>(cch - 1);
            out[bytes < cap ? bytes : cap] = '\0';
            LOG("active runtime manifest (%s): %s", r.label, out);
            return true;
        }
    }
    return false;
}

// Pulls "library_path" out of the runtime manifest. The manifest is small and
// its shape is fixed, so a targeted scan beats pulling in a JSON dependency.
// A relative library_path is resolved against the manifest's own directory.
bool ReadLibraryPathFromManifest(const char* manifestPath, char* out, size_t cch)
{
    FILE* f = nullptr;
    if (fopen_s(&f, manifestPath, "rb") != 0 || !f) {
        return false;
    }
    char json[8192] = {};
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';

    const char* key = strstr(json, "\"library_path\"");
    if (!key) {
        return false;
    }
    const char* colon = strchr(key, ':');
    if (!colon) {
        return false;
    }
    const char* open = strchr(colon, '"');
    if (!open) {
        return false;
    }
    ++open;

    char raw[MAX_PATH * 2] = {};
    size_t w = 0;
    for (const char* p = open; *p && *p != '"' && w < sizeof(raw) - 1; ++p) {
        if (*p == '\\' && p[1] == '\\') {  // JSON-escaped backslash
            raw[w++] = '\\';
            ++p;
        } else {
            raw[w++] = *p;
        }
    }
    raw[w] = '\0';
    if (!raw[0]) {
        return false;
    }

    const bool absolute = (raw[1] == ':') || (raw[0] == '\\' && raw[1] == '\\');
    if (absolute) {
        strncpy_s(out, cch, raw, _TRUNCATE);
        return true;
    }

    char dir[MAX_PATH * 2];
    strncpy_s(dir, sizeof(dir), manifestPath, _TRUNCATE);
    char* slash = strrchr(dir, '\\');
    if (slash) {
        slash[1] = '\0';
    } else {
        dir[0] = '\0';
    }
    const char* rel = raw;
    if (rel[0] == '.' && (rel[1] == '\\' || rel[1] == '/')) {
        rel += 2;
    }
    _snprintf_s(out, cch, _TRUNCATE, "%s%s", dir, rel);
    return true;
}

}  // namespace

bool LoadOpenXrRuntime(char* whyNot, size_t whyNotBytes)
{
    if (xrGetInstanceProcAddr_) {
        return true;
    }

    char manifest[MAX_PATH * 2] = {};
    if (!ReadActiveRuntimeManifest(manifest, sizeof(manifest))) {
        strncpy_s(whyNot, whyNotBytes,
                  "no OpenXR ActiveRuntime registered (is a runtime installed?)", _TRUNCATE);
        return false;
    }
    if (!ReadLibraryPathFromManifest(manifest, g_runtimePath, sizeof(g_runtimePath))) {
        _snprintf_s(whyNot, whyNotBytes, _TRUNCATE,
                    "could not read library_path from %s", manifest);
        return false;
    }
    LOG("runtime library: %s", g_runtimePath);

    g_runtimeDll = LoadLibraryA(g_runtimePath);
    if (!g_runtimeDll) {
        _snprintf_s(whyNot, whyNotBytes, _TRUNCATE,
                    "LoadLibrary(%s) failed: %lu", g_runtimePath, GetLastError());
        return false;
    }

    auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        GetProcAddress(g_runtimeDll, "xrNegotiateLoaderRuntimeInterface"));
    if (!negotiate) {
        strncpy_s(whyNot, whyNotBytes,
                  "runtime does not export xrNegotiateLoaderRuntimeInterface", _TRUNCATE);
        return false;
    }

    XrNegotiateLoaderInfo info{};
    info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    info.structSize = sizeof(info);
    info.minInterfaceVersion = 1;
    info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.maxApiVersion = XR_MAKE_VERSION(1, 1, 0xFFF);

    XrNegotiateRuntimeRequest req{};
    req.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    req.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    req.structSize = sizeof(req);

    XrResult r = negotiate(&info, &req);
    if (XR_FAILED(r) || !req.getInstanceProcAddr) {
        _snprintf_s(whyNot, whyNotBytes, _TRUNCATE,
                    "runtime negotiation failed (XrResult %d)", static_cast<int>(r));
        return false;
    }

    xrGetInstanceProcAddr_ = req.getInstanceProcAddr;
    LOG("negotiated: runtime interface v%u, api %u.%u",
        req.runtimeInterfaceVersion,
        XR_VERSION_MAJOR(req.runtimeApiVersion),
        XR_VERSION_MINOR(req.runtimeApiVersion));
    return true;
}

bool ResolveGlobalFunctions()
{
    if (!xrGetInstanceProcAddr_) {
        return false;
    }
    bool ok = true;
#define DLVR_RESOLVE(fn)                                                              \
    if (XR_FAILED(xrGetInstanceProcAddr_(XR_NULL_HANDLE, #fn,                         \
                                         reinterpret_cast<PFN_xrVoidFunction*>(&fn##_))) \
        || !fn##_) {                                                                  \
        LOG("could not resolve %s", #fn);                                             \
        ok = false;                                                                   \
    }
    DLVR_XR_GLOBAL_FUNCS(DLVR_RESOLVE)
#undef DLVR_RESOLVE
    return ok;
}

bool ResolveInstanceFunctions(XrInstance instance)
{
    if (!xrGetInstanceProcAddr_ || instance == XR_NULL_HANDLE) {
        return false;
    }
    bool ok = true;
#define DLVR_RESOLVE(fn)                                                              \
    if (XR_FAILED(xrGetInstanceProcAddr_(instance, #fn,                               \
                                         reinterpret_cast<PFN_xrVoidFunction*>(&fn##_))) \
        || !fn##_) {                                                                  \
        LOG("could not resolve %s", #fn);                                             \
        ok = false;                                                                   \
    }
    DLVR_XR_INSTANCE_FUNCS(DLVR_RESOLVE)
#undef DLVR_RESOLVE
    return ok;
}

}  // namespace dlvr
