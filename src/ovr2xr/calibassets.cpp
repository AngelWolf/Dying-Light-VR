// calibassets.cpp - the calibration screen's build D assets: the one-shot loader thread (Jay's 14 PNGs through WIC, the
// game's Boxed Semibold 30 font through calibnative_logic.h + calibassets_logic.h) and the render-thread texture creation.
// docs/superpowers/plans/2026-10-14-calibration-screen-build-d.md task 4. Contract: calibassets.h.
//
// THREADING: the loader thread writes only the buffers in this file's anonymous namespace, then InterlockedExchange(g_ready).
// The render thread reads them only after it has seen g_ready == 1 (interlocked = a full barrier), creates the textures once
// in CalibArtPump, and is from then on the only thread that touches any of this.
// puff (inside InflateRaw / InflateZlib) builds its fixed-Huffman tables lazily and is NOT thread-safe on first use: it is
// called ONLY from the loader thread, and there is exactly one loader thread per launch.
// COST: every file read, decode and inflate is on the loader thread, once per launch (the wall time is logged). The render
// thread: CalibArtPump = one bool test per calibration frame (+ one interlocked read until the textures exist); creating the
// textures is one-shot (14 + 1 CreateTexture2D, one sampler).
#include "calibassets.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "../common/log.h"
#include "calibnative_logic.h"
#include "overlay.h"
#include "settings.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {

namespace {

typedef std::vector<uint8_t> Bytes;

struct Slot {
    int w = 0, h = 0;
    Bytes px;                                   // RGBA8 straight alpha (kFloorGrid: premultiplied), w * h * 4; freed after upload
    ID3D11ShaderResourceView* srv = nullptr;    // render thread
    ID3D11Texture2D* tex = nullptr;             // kept for kFloorGrid only (CopyResource)
};
Slot g_img[calibart::kImgCount];
int g_imgLoaded = 0;

// The font: everything below is filled by the loader, read by the render thread after g_ready.
calibart::FontMetrics g_fm;
calibart::PackedGlyph g_glyph[128];
Bytes g_fontRgba[4];                            // levels 0..3, RGBA8 (RGB 255, A = coverage)
int g_fontW = 0, g_fontH = 0;
bool g_fontLoaded = false;

// The render thread's.
CalibFont g_font = {};
bool g_fontLive = false;
bool g_pumped = false;
bool g_noDeviceLogged = false;

volatile LONG g_started = 0;
volatile LONG g_ready = 0;

const char* const kPakEntry = "data/panel/boxed_semibold_30.fm";
const char* const kRpName = "boxed_semibold_30";
const int kPackW = 1024;
const int kPackPad = 4;

// Caps on sizes the game's own files supply, checked BEFORE anything is allocated: a corrupt or foreign-version pak / rpack
// must refuse by name, never ask for gigabytes (an uncaught bad_alloc on this thread would end the game). The real values
// on our install (calibnative_logic_test.cpp's oracle constants, tools/calib_native_font.py):
const uint64_t kFmMaxBytes = 1u << 20;          // the .fm entry: 3,662 packed -> 12,179 bytes; 1 MB cap
const uint64_t kSectionWantMax = 16u << 20;     // the atlas section's prefix: 274,624 + 8,388,608 = 8,663,232 bytes; 16 MB cap
const uint64_t kSectionPackedMax = 64u << 20;   // the atlas section's packed size: 9,719,741 bytes; 64 MB cap

double MsSince(const LARGE_INTEGER& t0)
{
    LARGE_INTEGER f, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t1);
    return f.QuadPart ? 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart) : 0.0;
}

// ---------------------------------------------------------------- file I/O (loader thread)
bool FileSizeW(const std::wstring& p, uint64_t* sz)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }
    *sz = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return true;
}

// Reads `n` bytes at `off`. `allowShort` = a read cut short by the end of the file is fine (the caller bounds-checks).
bool ReadAtW(const std::wstring& p, uint64_t off, size_t n, Bytes& out, bool allowShort, char* why, size_t whyN)
{
    out.clear();
    // Shared wide: the engine holds Data0.pak / the rpack open while we read; our handle stays read-only (GENERIC_READ).
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return calibnative::Fail(why, whyN, "cannot open %ls (error %lu)", p.c_str(), GetLastError());
    }
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(off);
    if (!SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) {
        const DWORD e = GetLastError();
        CloseHandle(f);
        return calibnative::Fail(why, whyN, "cannot seek %ls to %llu (error %lu)", p.c_str(), (unsigned long long)off, e);
    }
    out.assign(n, 0);
    size_t got = 0;
    while (got < n) {
        const DWORD chunk = static_cast<DWORD>((n - got) > (1u << 30) ? (1u << 30) : (n - got));
        DWORD r = 0;
        if (!ReadFile(f, out.data() + got, chunk, &r, nullptr)) {
            const DWORD e = GetLastError();
            CloseHandle(f);
            out.clear();
            return calibnative::Fail(why, whyN, "read of %ls failed at %llu (error %lu)", p.c_str(),
                                     (unsigned long long)(off + got), e);
        }
        if (r == 0) { break; }
        got += r;
    }
    CloseHandle(f);
    if (got < n) {
        out.resize(got);
        if (!allowShort) {
            return calibnative::Fail(why, whyN, "short read of %ls at %llu: %zu of %zu bytes", p.c_str(),
                                     (unsigned long long)off, got, n);
        }
    }
    return true;
}

// ---------------------------------------------------------------- the PNGs (loader thread)
bool DecodePng(IWICImagingFactory* fac, const char* path, calibart::Img i, Slot& s, char* why, size_t whyN)
{
    wchar_t wpath[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH) == 0) {
        return calibnative::Fail(why, whyN, "path conversion failed (error %lu)", GetLastError());
    }
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    HRESULT hr = fac->CreateDecoderFromFilename(wpath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    do {
        if (FAILED(hr) || !dec) { calibnative::Fail(why, whyN, "WIC CreateDecoderFromFilename hr 0x%08lX", (unsigned long)hr); break; }
        GUID cf = {};
        hr = dec->GetContainerFormat(&cf);
        if (FAILED(hr)) { calibnative::Fail(why, whyN, "WIC GetContainerFormat hr 0x%08lX", (unsigned long)hr); break; }
        if (!IsEqualGUID(cf, GUID_ContainerFormatPng)) { calibnative::Fail(why, whyN, "not a PNG (WIC read another container format)"); break; }
        hr = dec->GetFrame(0, &frame);
        if (FAILED(hr) || !frame) { calibnative::Fail(why, whyN, "WIC GetFrame(0) hr 0x%08lX", (unsigned long)hr); break; }
        UINT w = 0, h = 0;
        hr = frame->GetSize(&w, &h);
        if (FAILED(hr)) { calibnative::Fail(why, whyN, "WIC GetSize hr 0x%08lX", (unsigned long)hr); break; }
        const UINT maxSide = static_cast<UINT>(calibart::ImgMaxSide(i));
        if (w == 0 || h == 0 || w > maxSide || h > maxSide) {
            calibnative::Fail(why, whyN, "%u x %u is outside 1..%u pixels a side", w, h, maxSide);
            break;
        }
        hr = fac->CreateFormatConverter(&conv);
        if (FAILED(hr) || !conv) { calibnative::Fail(why, whyN, "WIC CreateFormatConverter hr 0x%08lX", (unsigned long)hr); break; }
        hr = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) {
            calibnative::Fail(why, whyN, "WIC cannot convert it to 32bpp RGBA (hr 0x%08lX)", (unsigned long)hr);
            break;
        }
        const UINT stride = w * 4;
        const UINT bytes = stride * h;
        s.px.assign(bytes, 0);
        hr = conv->CopyPixels(nullptr, stride, bytes, s.px.data());
        if (FAILED(hr)) {
            s.px.clear();
            calibnative::Fail(why, whyN, "WIC CopyPixels hr 0x%08lX", (unsigned long)hr);
            break;
        }
        s.w = static_cast<int>(w);
        s.h = static_cast<int>(h);
        ok = true;
    } while (false);
    if (conv) { conv->Release(); }
    if (frame) { frame->Release(); }
    if (dec) { dec->Release(); }
    return ok;
}

void LoadImages()
{
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* fac = nullptr;
    const HRESULT hrFac = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
    LOG("calibassets: [CALIBART] loader: CoInitializeEx(MULTITHREADED) hr 0x%08lX, WIC factory hr 0x%08lX%s",
        (unsigned long)hrCo, (unsigned long)hrFac,
        (SUCCEEDED(hrFac) && fac) ? "" : " - *** no WIC: every image is refused, the screen draws its fallbacks ***");
    for (int k = 0; k < calibart::kImgCount; ++k) {
        const calibart::Img i = static_cast<calibart::Img>(k);
        char leaf[96];
        std::snprintf(leaf, sizeof leaf, "calib\\%s", calibart::ImgFile(i));
        char path[MAX_PATH] = {};
        char why[200] = {};
        if (!DlvrFilePath(leaf, path, sizeof path)) {
            // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
            LOG("calibassets: [CALIBART] refused dlvr\\%s: the dlvr path could not be resolved", leaf);
            continue;
        }
        const DWORD attr = GetFileAttributesA(path);
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
            LOG("calibassets: [CALIBART] missing %s", path);
            continue;
        }
        if (!(SUCCEEDED(hrFac) && fac)) {
            // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
            LOG("calibassets: [CALIBART] refused %s: no WIC factory (hr 0x%08lX)", path, (unsigned long)hrFac);
            continue;
        }
        if (DecodePng(fac, path, i, g_img[k], why, sizeof why)) {
            ++g_imgLoaded;
            // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
            LOG("calibassets: [CALIBART] loaded %s: %d x %d", path, g_img[k].w, g_img[k].h);
            if (i == calibart::kFloorGrid) {
                // The floor goes to the runtime as its own quad layer (CopyResource), which OpenXR reads as PREMULTIPLIED
                // alpha; WIC gives straight alpha. Premultiplied here once, in linear light. The other slots stay straight
                // (ImGui draws them). Cost: once per launch, this thread, 2048 x 2048 = 4.2 M pixels.
                Slot& s = g_img[k];
                const size_t px = static_cast<size_t>(s.w) * static_cast<size_t>(s.h);
                size_t clear = 0, part = 0;
                for (size_t p = 0; p < px; ++p) {
                    const uint8_t a = s.px[p * 4 + 3];
                    if (a == 0) { ++clear; } else if (a != 255) { ++part; }
                }
                calibart::PremultiplySrgbRgba(s.px.data(), px);
                // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
                LOG("calibassets: [CALIBART] floor %s premultiplied in linear light: %zu pixels, %zu alpha 0, %zu partly "
                    "transparent", calibart::ImgFile(i), px, clear, part);
            }
        } else {
            // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
            LOG("calibassets: [CALIBART] refused %s: %s", path, why);
        }
    }
    if (fac) { fac->Release(); }
    if (SUCCEEDED(hrCo)) { CoUninitialize(); }
    // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
    LOG("calibassets: [CALIBART] %d of %d images", g_imgLoaded, int(calibart::kImgCount));
}

// ---------------------------------------------------------------- the font (loader thread)
// One section part's bytes: `out` holds the part at [*at, *at + part.size). A stored section is read straight from the
// file; a zlib section is read whole (its packed size) and inflated only up to the part's end.
// `rpSz` = the rpack's real size: every read is checked against it, and every size against its cap, before allocating.
bool PartBytes(const std::wstring& rp, uint64_t rpSz, const calibnative::RpSection& sec, const calibnative::RpPart& part,
               Bytes& out, size_t* at, bool* adlerChecked, char* why, size_t whyN)
{
    *adlerChecked = false;
    const uint64_t end = uint64_t(part.offset) + part.size;
    if (end > sec.unpacked) {
        return calibnative::Fail(why, whyN, "part %u + %u runs past its section's %u bytes", part.offset, part.size, sec.unpacked);
    }
    if (sec.packed == 0) {
        if (part.size > kSectionWantMax) {
            return calibnative::Fail(why, whyN, "stored part is %u bytes, past the %llu-byte cap", part.size,
                                     (unsigned long long)kSectionWantMax);
        }
        if (uint64_t(sec.offset) + end > rpSz) {
            return calibnative::Fail(why, whyN, "stored part at %llu + %u runs past the rpack's %llu bytes",
                                     (unsigned long long)(uint64_t(sec.offset) + part.offset), part.size, (unsigned long long)rpSz);
        }
        *at = 0;
        return ReadAtW(rp, uint64_t(sec.offset) + part.offset, part.size, out, false, why, whyN);
    }
    if (end > kSectionWantMax) {
        return calibnative::Fail(why, whyN, "section prefix wanted is %llu bytes, past the %llu-byte cap", (unsigned long long)end,
                                 (unsigned long long)kSectionWantMax);
    }
    if (sec.packed > kSectionPackedMax) {
        return calibnative::Fail(why, whyN, "packed section is %u bytes, past the %llu-byte cap", sec.packed,
                                 (unsigned long long)kSectionPackedMax);
    }
    if (uint64_t(sec.offset) + sec.packed > rpSz) {
        return calibnative::Fail(why, whyN, "packed section at %u + %u runs past the rpack's %llu bytes", sec.offset, sec.packed,
                                 (unsigned long long)rpSz);
    }
    Bytes raw;
    if (!ReadAtW(rp, sec.offset, sec.packed, raw, false, why, whyN)) { return false; }
    *at = part.offset;
    return calibnative::InflateZlib(raw.data(), raw.size(), static_cast<size_t>(end), out, adlerChecked, why, whyN);
}

bool LoadFont(const char** stage, char* why, size_t whyN)
{
    *stage = "game folder";
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { return calibnative::Fail(why, whyN, "GetModuleFileNameW failed (%lu)", n); }
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) { return calibnative::Fail(why, whyN, "no folder in the exe path %ls", exe); }
    slash[1] = 0;
    const std::wstring dir(exe);
    const std::wstring pak = dir + L"DW\\Data0.pak";
    const std::wstring rp = dir + L"DW\\Data\\menu_common_PC.rpack";

    // ---- the .fm from Data0.pak: tail -> EOCD -> central directory -> entry -> local header -> inflate + CRC
    *stage = "fm (Data0.pak)";
    Bytes fm;
    {
        uint64_t pakSz = 0;
        if (!FileSizeW(pak, &pakSz)) { return calibnative::Fail(why, whyN, "%ls not found", pak.c_str()); }
        const size_t tn = static_cast<size_t>(pakSz < 65557 ? pakSz : 65557);
        Bytes tail;
        if (!ReadAtW(pak, pakSz - tn, tn, tail, false, why, whyN)) { return false; }
        calibnative::ZipDir zd;
        if (!calibnative::ZipFindDir(tail.data(), tail.size(), pakSz - tn, &zd, why, whyN)) { return false; }
        Bytes cd;
        if (!ReadAtW(pak, zd.cdOff, static_cast<size_t>(zd.cdSize), cd, false, why, whyN)) { return false; }
        calibnative::ZipEntry e;
        if (!calibnative::ZipFindEntry(cd.data(), cd.size(), kPakEntry, &e, why, whyN)) { return false; }
        cd.clear();
        cd.shrink_to_fit();
        if (e.localOff >= pakSz) { return calibnative::Fail(why, whyN, "local header offset %llu past the end", (unsigned long long)e.localOff); }
        const uint64_t room = pakSz - e.localOff;
        Bytes loc;
        if (!ReadAtW(pak, e.localOff, static_cast<size_t>(room < 30 + 2 * 65535 ? room : 30 + 2 * 65535), loc, true, why, whyN)) {
            return false;
        }
        uint64_t dataOff = 0;
        if (!calibnative::ZipDataOffset(loc.data(), loc.size(), e.localOff, &dataOff, why, whyN)) { return false; }
        if (e.csize > kFmMaxBytes || e.usize > kFmMaxBytes) {
            return calibnative::Fail(why, whyN, "entry '%s' is %llu -> %llu bytes, past the %llu-byte cap", kPakEntry,
                                     (unsigned long long)e.csize, (unsigned long long)e.usize, (unsigned long long)kFmMaxBytes);
        }
        if (dataOff + e.csize > pakSz) {
            return calibnative::Fail(why, whyN, "entry data at %llu + %llu runs past the pak's %llu bytes",
                                     (unsigned long long)dataOff, (unsigned long long)e.csize, (unsigned long long)pakSz);
        }
        Bytes packed;
        if (!ReadAtW(pak, dataOff, static_cast<size_t>(e.csize), packed, false, why, whyN)) { return false; }
        if (e.method == 8) {
            if (!calibnative::InflateRaw(packed.data(), packed.size(), static_cast<size_t>(e.usize), e.crc, fm, why, whyN)) {
                return false;
            }
        } else if (e.method == 0) {
            if (e.csize != e.usize) { return calibnative::Fail(why, whyN, "stored entry: %llu != %llu bytes", (unsigned long long)e.csize, (unsigned long long)e.usize); }
            const uint32_t have = calibnative::Crc32(packed.data(), packed.size());
            if (have != e.crc) { return calibnative::Fail(why, whyN, "crc mismatch (computed %08X, entry says %08X)", have, e.crc); }
            fm.swap(packed);
        } else {
            return calibnative::Fail(why, whyN, "entry '%s' uses method %u (only 0 and 8 are read)", kPakEntry, unsigned(e.method));
        }
        // [PROBE: CALIBFONT] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBFONT] fm: %ls :: %s method %u, %llu -> %zu bytes crc OK (%08X)", pak.c_str(), kPakEntry,
            unsigned(e.method), (unsigned long long)e.csize, fm.size(), e.crc);
    }
    *stage = "fm parse";
    if (!calibart::ParseFm(reinterpret_cast<const char*>(fm.data()), fm.size(), &g_fm, why, whyN)) { return false; }
    fm.clear();
    fm.shrink_to_fit();

    // ---- the atlas from the rpack: head -> need -> directory -> file -> part 0 (texture header) + part 1 (BC3 pixels)
    *stage = "atlas (menu_common_PC.rpack)";
    int aw = 0, ah = 0;
    Bytes alpha;
    {
        uint64_t rpSz = 0;
        if (!FileSizeW(rp, &rpSz)) { return calibnative::Fail(why, whyN, "%ls not found", rp.c_str()); }
        Bytes head;
        if (!ReadAtW(rp, 0, 36, head, false, why, whyN)) { return false; }
        std::vector<calibnative::RpSection> secs;
        std::vector<calibnative::RpFile> files;
        size_t need = 0;
        // The first parse over the 36-byte header only says how big the directory is (`need`); "not RP6L" and the
        // like refuse here with need still 36.
        const bool whole = calibnative::RpParse(head.data(), head.size(), secs, files, &need, why, whyN);
        if (!whole) {
            if (need <= 36) { return false; }
            if (need > rpSz) {   // RpParse already caps it at 64 MB; this is the file's own size
                return calibnative::Fail(why, whyN, "directory is %zu bytes, the rpack only %llu", need, (unsigned long long)rpSz);
            }
            if (!ReadAtW(rp, 0, need, head, false, why, whyN)) { return false; }
            if (!calibnative::RpParse(head.data(), head.size(), secs, files, &need, why, whyN)) { return false; }
        }
        head.clear();
        head.shrink_to_fit();
        const calibnative::RpFile* ff = nullptr;
        for (const calibnative::RpFile& f : files) {
            if (f.name == kRpName) { ff = &f; break; }
        }
        if (!ff) { return calibnative::Fail(why, whyN, "'%s' not in the rpack's name table (%zu files)", kRpName, files.size()); }
        if (ff->parts.size() < 2) { return calibnative::Fail(why, whyN, "'%s' has %zu parts, 2 needed", kRpName, ff->parts.size()); }

        const calibnative::RpPart& p0 = ff->parts[0];
        const calibnative::RpPart& p1 = ff->parts[1];
        Bytes hdrBuf;
        size_t at = 0;
        bool ac0 = false;
        if (!PartBytes(rp, rpSz, secs[p0.section], p0, hdrBuf, &at, &ac0, why, whyN)) { return false; }
        if (p0.size < 16) { return calibnative::Fail(why, whyN, "texture header part is %u bytes, 16 needed", p0.size); }
        const uint8_t* hdr = hdrBuf.data() + at;
        aw = calibnative::Rd16(hdr);
        ah = calibnative::Rd16(hdr + 2);
        const uint32_t fmt = calibnative::Rd32(hdr + 12);
        if (aw != 2048 || ah != 4096 || fmt != 19) {
            return calibnative::Fail(why, whyN, "texture header reads %d x %d format %u, the BC3 atlas read is 2048 x 4096 format 19",
                                     aw, ah, fmt);
        }
        hdrBuf.clear();
        hdrBuf.shrink_to_fit();
        if (uint64_t(p1.size) < uint64_t(aw) * uint64_t(ah)) {
            return calibnative::Fail(why, whyN, "pixel part is %u bytes, BC3 %d x %d needs %d", p1.size, aw, ah, aw * ah);
        }
        if (g_fm.mapW != aw || g_fm.mapH != ah) {
            return calibnative::Fail(why, whyN, "the .fm's map %d x %d is not the atlas's %d x %d", g_fm.mapW, g_fm.mapH, aw, ah);
        }
        Bytes sec;
        bool ac1 = false;
        if (!PartBytes(rp, rpSz, secs[p1.section], p1, sec, &at, &ac1, why, whyN)) { return false; }
        // [PROBE: CALIBFONT] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBFONT] atlas: %ls :: %s %d x %d format %u, section %u %s to %zu bytes (adler %s)", rp.c_str(),
            kRpName, aw, ah, fmt, unsigned(p1.section), secs[p1.section].packed ? "inflated" : "stored, read", sec.size(),
            ac1 ? "checked OK" : (secs[p1.section].packed ? "not checked - partial" : "n/a - stored"));
        *stage = "atlas decode";
        alpha.assign(static_cast<size_t>(aw) * static_cast<size_t>(ah), 0);
        calibnative::Bc3Alpha(sec.data() + at, aw, ah, alpha.data());
        // the up-to-72 MB-capable inflate buffer goes here, before the pack
        sec.clear();
        sec.shrink_to_fit();
    }

    *stage = "pack";
    Bytes packed;
    const int H = calibart::PackGlyphs(g_fm, alpha.data(), kPackW, kPackPad, packed, g_glyph);
    alpha.clear();
    alpha.shrink_to_fit();
    if (H <= 0) { return calibnative::Fail(why, whyN, "a glyph does not fit %d pixels wide at pad %d (or no glyphs)", kPackW, kPackPad); }

    Bytes lv[4];
    lv[0].swap(packed);
    int w = kPackW, h = H;
    for (int l = 1; l < 4; ++l) {
        calibart::MipDown(lv[l - 1].data(), w, h, lv[l]);
        w /= 2;
        h /= 2;
    }
    w = kPackW;
    h = H;
    for (int l = 0; l < 4; ++l) {
        const size_t px = static_cast<size_t>(w) * static_cast<size_t>(h);
        g_fontRgba[l].assign(px * 4, 255);
        for (size_t k = 0; k < px; ++k) { g_fontRgba[l][k * 4 + 3] = lv[l][k]; }
        lv[l].clear();
        lv[l].shrink_to_fit();
        w /= 2;
        h /= 2;
    }
    g_fontW = kPackW;
    g_fontH = H;
    // [PROBE: CALIBFONT] gate=none (the calibration screen)  site=HEADSET  log-only=yes
    LOG("calibassets: [CALIBFONT] packed %d glyphs into %d x %d, 3 mips (FontHeight %d, pad %d)", g_fm.count, kPackW, H,
        g_fm.fontHeight, kPackPad);
    return true;
}

// ---------------------------------------------------------------- the floor VIDEO (decoder thread + render-thread upload)
// Jay 2026-10-15: "Sort it out ... Make sure to use floor_grid_1.mp4". dlvr\calib\floor_grid_1.mp4, when present, replaces the
// floor PNG's pixels: Media Foundation's Source Reader (H.264 -> RGB32 through its advanced video processing, every other stream
// deselected), looped, paced to the file's own timestamps, decoded on its own thread ONLY while the floor is being drawn (the
// render thread stamps Vid::wantMs on every floor draw; > 300 ms without one = paused in a 50 ms sleep). Frames reach the render
// thread through a triple buffer; it uploads the newest into a DEFAULT-usage R8G8B8A8_UNORM_SRGB texture - the format and size
// OverlayRenderCalibFloorImage copies from. The PNG stays the fallback: no file, a refused file or no frame yet = the PNG floor.
// Opaque (alpha 255): Jay's video is pure black between the lines (frames 0 / 135 / 270 checked 2026-10-15) - no mask.
// COST: decoder thread, only while the screen is up: one H.264 decode + colour conversion + one 32-bit swizzle per frame
// (2048 x 2048 at 60 fps). Render thread: one interlocked read per floor draw; per NEW frame one UpdateSubresource (w * h * 4).
// Jay 2026-10-16 ("skinning the void"): the same path three times - the floor, the four walls (one video, one texture, four
// quads) and the ceiling - each with its own decoder thread, triple buffer and texture. Only the floor has a PNG to fall back
// on; a missing / refused wall or ceiling video draws nothing there.
const int kVidMaxSide = 4096;
struct Vid {
    const char* leaf;                       // relative to the dlvr folder
    const char* name;                       // in every log line
    bool floor;                             // the floor: must match floor_grid.png's size, falls back to it
    volatile LONG state;                    // 0 not started, 1 opening, 2 playing, 3 refused / stopped
    int w, h;                               // written before state = 2 (interlocked)
    Bytes buf[3];                           // RGBA8 frames
    volatile LONG mid;                      // the shared slot's index, | 0x10 = a fresh frame is in it
    int back;                               // the decoder's own slot
    int front;                              // the render thread's own slot
    volatile ULONGLONG wantMs;
    volatile LONG64 frames, late, loops, uploads;
    ID3D11Texture2D* tex;                   // render thread
    bool texFailed;
};
enum { kVidFloor = 0, kVidWalls = 1, kVidCeiling = 2, kVidCount = 3 };
Vid g_vid[kVidCount] = {
    { "calib\\floor_grid_1.mp4", "floor", true, 0, 0, 0, {}, 1, 0, 2, 0, 0, 0, 0, 0, nullptr, false },
    { "calib\\wallgrid.mp4", "walls", false, 0, 0, 0, {}, 1, 0, 2, 0, 0, 0, 0, 0, nullptr, false },
    { "calib\\roofgrid.mp4", "ceiling", false, 0, 0, 0, {}, 1, 0, 2, 0, 0, 0, 0, 0, nullptr, false },
};
int g_pngFloorW = 0, g_pngFloorH = 0;       // the PNG floor's size (loader thread, before the decoders start)

const char* VidFallback(const Vid& v) { return v.floor ? "the PNG floor is used" : "nothing is drawn there"; }

double VidNowMs()
{
    static double s_perMs = 0.0;
    if (s_perMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_perMs = f.QuadPart ? double(f.QuadPart) / 1000.0 : 1.0;
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / s_perMs;
}

// One decoded RGB32 (B G R X in memory) sample -> RGBA8, opaque. Row y of the picture is scan0 + pitch * y (pitch < 0 =
// bottom-up), from IMF2DBuffer::Lock2D, else the media type's default stride.
bool VidCopy(IMFSample* smp, int w, int h, LONG defStride, uint8_t* dst)
{
    IMFMediaBuffer* mb = nullptr;
    if (FAILED(smp->ConvertToContiguousBuffer(&mb)) || !mb) { return false; }
    IMF2DBuffer* b2 = nullptr;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    bool l2 = false, l1 = false, ok = false;
    if (SUCCEEDED(mb->QueryInterface(IID_PPV_ARGS(&b2))) && b2 && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
        l2 = true;
    } else {
        BYTE* p = nullptr;
        DWORD maxLen = 0, cur = 0;
        if (SUCCEEDED(mb->Lock(&p, &maxLen, &cur))) {
            l1 = true;
            pitch = defStride;
            const size_t need = size_t(pitch < 0 ? -pitch : pitch) * size_t(h);
            if (cur >= need) { scan0 = pitch < 0 ? p + size_t(-pitch) * size_t(h - 1) : p; }
        }
    }
    const LONG apitch = pitch < 0 ? -pitch : pitch;
    if (scan0 && apitch >= w * 4) {
        for (int y = 0; y < h; ++y) {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(scan0 + ptrdiff_t(pitch) * y);
            uint32_t* d = reinterpret_cast<uint32_t*>(dst + size_t(y) * size_t(w) * 4);
            for (int x = 0; x < w; ++x) {
                const uint32_t v = s[x];
                d[x] = 0xFF000000u | ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu);
            }
        }
        ok = true;
    }
    if (l2) { b2->Unlock2D(); }
    if (l1) { mb->Unlock(); }
    if (b2) { b2->Release(); }
    mb->Release();
    return ok;
}

DWORD WINAPI VideoThread(void* param)
{
    Vid& v = *static_cast<Vid*>(param);
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = MFStartup(MF_VERSION);
    const bool mfUp = SUCCEEDED(hr);
    IMFAttributes* attr = nullptr;
    IMFSourceReader* rd = nullptr;
    IMFMediaType* want = nullptr;
    IMFMediaType* got = nullptr;
    char path[MAX_PATH] = {};
    char why[220] = {};
    UINT32 w = 0, h = 0, fpsN = 0, fpsD = 0;
    LONG defStride = 0;
    bool ok = false;
    const DWORD vs = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    try {
        do {
            if (!mfUp) { calibnative::Fail(why, sizeof why, "MFStartup hr 0x%08lX", (unsigned long)hr); break; }
            if (!DlvrFilePath(v.leaf, path, sizeof path)) { calibnative::Fail(why, sizeof why, "the dlvr path could not be resolved"); break; }
            wchar_t wpath[MAX_PATH];
            if (MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH) == 0) { calibnative::Fail(why, sizeof why, "path conversion failed"); break; }
            hr = MFCreateAttributes(&attr, 1);
            if (FAILED(hr) || !attr) { calibnative::Fail(why, sizeof why, "MFCreateAttributes hr 0x%08lX", (unsigned long)hr); break; }
            attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            hr = MFCreateSourceReaderFromURL(wpath, attr, &rd);
            if (FAILED(hr) || !rd) { calibnative::Fail(why, sizeof why, "MFCreateSourceReaderFromURL hr 0x%08lX", (unsigned long)hr); break; }
            rd->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            hr = rd->SetStreamSelection(vs, TRUE);
            if (FAILED(hr)) { calibnative::Fail(why, sizeof why, "no video stream (hr 0x%08lX)", (unsigned long)hr); break; }
            hr = MFCreateMediaType(&want);
            if (FAILED(hr) || !want) { calibnative::Fail(why, sizeof why, "MFCreateMediaType hr 0x%08lX", (unsigned long)hr); break; }
            want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            hr = rd->SetCurrentMediaType(vs, nullptr, want);
            if (FAILED(hr)) { calibnative::Fail(why, sizeof why, "cannot decode it to RGB32 (SetCurrentMediaType hr 0x%08lX)", (unsigned long)hr); break; }
            hr = rd->GetCurrentMediaType(vs, &got);
            if (FAILED(hr) || !got) { calibnative::Fail(why, sizeof why, "GetCurrentMediaType hr 0x%08lX", (unsigned long)hr); break; }
            MFGetAttributeSize(got, MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(got, MF_MT_FRAME_RATE, &fpsN, &fpsD);
            UINT32 st = 0;
            defStride = SUCCEEDED(got->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? static_cast<LONG>(st) : static_cast<LONG>(w * 4);
            if (w == 0 || h == 0 || int(w) > kVidMaxSide || int(h) > kVidMaxSide) {
                calibnative::Fail(why, sizeof why, "%u x %u is outside 1..%d pixels a side", w, h, kVidMaxSide);
                break;
            }
            if (v.floor && g_pngFloorW > 0 && (int(w) != g_pngFloorW || int(h) != g_pngFloorH)) {
                calibnative::Fail(why, sizeof why, "%u x %u is not floor_grid.png's %d x %d (the floor swapchain takes one size)", w, h,
                                  g_pngFloorW, g_pngFloorH);
                break;
            }
            for (int k = 0; k < 3; ++k) { v.buf[k].assign(size_t(w) * size_t(h) * 4, 0); }
            ok = true;
        } while (false);
    } catch (...) {
        ok = false;
        std::snprintf(why, sizeof why, "out of memory or an exception while opening it");
    }
    if (!ok) {
        // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBVIDEO] %s: REFUSED %s: %s - %s", v.name, path[0] ? path : v.leaf, why, VidFallback(v));
        InterlockedExchange(&v.state, 3);
    } else {
        v.w = int(w);
        v.h = int(h);
        InterlockedExchange(&v.state, 2);
        // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBVIDEO] %s: opened %s: %u x %u, %u/%u fps, RGB32 default stride %ld - decodes only while it is "
            "drawn, loops, audio not used", v.name, path, w, h, fpsN, fpsD, static_cast<long>(defStride));
        bool based = false, firstSaid = false, loopSaid = false;
        int typeChanges = 0;   // may repeat after every loop seek - logged 3 times, then on a stop only
        double baseMs = 0.0;
        for (;;) {
            const ULONGLONG wantMs = v.wantMs;
            if (wantMs == 0 || GetTickCount64() - wantMs > 300) {
                Sleep(50);
                based = false;   // resume re-bases the clock (no burst of catch-up frames)
                continue;
            }
            DWORD si = 0, fl = 0;
            LONGLONG ts = 0;
            IMFSample* smp = nullptr;
            hr = rd->ReadSample(vs, 0, &si, &fl, &ts, &smp);
            if (FAILED(hr) || (fl & MF_SOURCE_READERF_ERROR)) {
                if (smp) { smp->Release(); }
                // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
                LOG("calibassets: [CALIBVIDEO] %s: STOPPED: ReadSample hr 0x%08lX flags 0x%lX after %lld frames - it keeps its "
                    "last frame", v.name, (unsigned long)hr, (unsigned long)fl, static_cast<long long>(v.frames));
                break;
            }
            if (fl & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                // Run 1 (18:08:23): this flag comes with the FIRST sample - the decoder settling its real output type - and was
                // treated as fatal. Re-read the type: same size = take its stride and carry on (the sample is used below);
                // a different size = stop by name (the texture and the floor swapchain have one size).
                IMFMediaType* nt = nullptr;
                UINT32 nw = 0, nh = 0, nst = 0;
                const HRESULT hrT = rd->GetCurrentMediaType(vs, &nt);
                if (SUCCEEDED(hrT) && nt) {
                    MFGetAttributeSize(nt, MF_MT_FRAME_SIZE, &nw, &nh);
                    defStride = SUCCEEDED(nt->GetUINT32(MF_MT_DEFAULT_STRIDE, &nst)) ? static_cast<LONG>(nst)
                                                                                      : static_cast<LONG>(nw * 4);
                    nt->Release();
                }
                const bool same = SUCCEEDED(hrT) && int(nw) == v.w && int(nh) == v.h;
                if (++typeChanges <= 3 || !same)
                // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
                LOG("calibassets: [CALIBVIDEO] %s: output type changed (after %lld frames): %u x %u, default stride %ld (hr 0x%08lX) - %s",
                    v.name, static_cast<long long>(v.frames), nw, nh, static_cast<long>(defStride), (unsigned long)hrT,
                    same ? "same size, carrying on" : "*** a different size (or unreadable) - STOPPED, it keeps its last frame ***");
                if (!same) {
                    if (smp) { smp->Release(); }
                    break;
                }
            }
            if (fl & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (smp) { smp->Release(); }
                PROPVARIANT pv;
                PropVariantInit(&pv);
                pv.vt = VT_I8;
                pv.hVal.QuadPart = 0;
                hr = rd->SetCurrentPosition(GUID_NULL, pv);
                PropVariantClear(&pv);
                const LONG64 loops = InterlockedIncrement64(&v.loops);
                based = false;
                if (!loopSaid) {
                    loopSaid = true;
                    // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
                    LOG("calibassets: [CALIBVIDEO] %s: loop #%lld: %lld frames so far, %lld late by > 50 ms, %lld uploaded; seek to 0 "
                        "hr 0x%08lX", v.name, static_cast<long long>(loops), static_cast<long long>(v.frames),
                        static_cast<long long>(v.late), static_cast<long long>(v.uploads), (unsigned long)hr);
                }
                if (FAILED(hr)) {
                    // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
                    LOG("calibassets: [CALIBVIDEO] %s: STOPPED: cannot seek back to the start (hr 0x%08lX) - it keeps its last frame",
                        v.name, (unsigned long)hr);
                    break;
                }
                continue;
            }
            if (!smp) { continue; }
            const double tsMs = double(ts) / 10000.0;
            double now = VidNowMs();
            if (!based) { baseMs = now - tsMs; based = true; }
            const double due = baseMs + tsMs;
            if (due > now + 1.0) {
                Sleep(static_cast<DWORD>(due - now));
            } else if (now - due > 50.0) {
                InterlockedIncrement64(&v.late);
                if (now - due > 250.0) { based = false; }   // far behind: re-base rather than stay behind for good
            }
            uint8_t* dst = v.buf[v.back].data();
            if (VidCopy(smp, v.w, v.h, defStride, dst)) {
                if (!firstSaid) {
                    firstSaid = true;
                    const int sx = v.w > 65 ? 65 : 0, sy = v.h > 65 ? 65 : 0;
                    const uint8_t* p = dst + (size_t(sy) * size_t(v.w) + size_t(sx)) * 4;
                    size_t black = 0, n = 0;
                    for (int y = 0; y < v.h; y += 16) {
                        for (int x = 0; x < v.w; x += 16) {
                            const uint8_t* q = dst + (size_t(y) * size_t(v.w) + size_t(x)) * 4;
                            ++n;
                            if (q[0] == 0 && q[1] == 0 && q[2] == 0) { ++black; }
                        }
                    }
                    // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
                    LOG("calibassets: [CALIBVIDEO] %s: first frame decoded: pixel (%d,%d) = %u %u %u%s, pure black on %zu of %zu "
                        "sampled pixels", v.name, sx, sy, p[0], p[1], p[2],
                        v.floor ? " (Jay's frame 0 reads 0 0 0 there - not 0 = the video range / colour conversion is wrong)" : "",
                        black, n);
                }
                const LONG old = InterlockedExchange(&v.mid, v.back | 0x10);
                v.back = old & 3;
                InterlockedIncrement64(&v.frames);
            }
            smp->Release();
        }
        InterlockedExchange(&v.state, 3);
    }
    if (got) { got->Release(); }
    if (want) { want->Release(); }
    if (rd) { rd->Release(); }
    if (attr) { attr->Release(); }
    if (mfUp) { MFShutdown(); }
    if (SUCCEEDED(hrCo)) { CoUninitialize(); }
    return 0;
}

// Loader thread, after the PNGs: start one video's decoder if its file is there. Says so either way (first sample
// unconditional).
void StartVideo(Vid& v)
{
    char path[MAX_PATH] = {};
    const bool havePath = DlvrFilePath(v.leaf, path, sizeof path);
    const DWORD a = havePath ? GetFileAttributesA(path) : INVALID_FILE_ATTRIBUTES;
    if (!havePath || a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
        InterlockedExchange(&v.state, 3);
        // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBVIDEO] %s: no video (%s %s) - %s", v.name, havePath ? path : v.leaf,
            havePath ? "missing" : "path not resolved", VidFallback(v));
        return;
    }
    InterlockedExchange(&v.state, 1);
    DWORD tid = 0;
    HANDLE h = CreateThread(nullptr, 0, VideoThread, &v, 0, &tid);
    if (!h) {
        InterlockedExchange(&v.state, 3);
        // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBVIDEO] %s: REFUSED: CreateThread failed (error %lu) - %s", v.name, GetLastError(), VidFallback(v));
        return;
    }
    // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
    LOG("calibassets: [CALIBVIDEO] %s: decoder thread %lu started for %s (PNG floor %d x %d)", v.name, tid, path, g_pngFloorW,
        g_pngFloorH);
    CloseHandle(h);
}

void StartVideos()
{
    const Slot& png = g_img[calibart::kFloorGrid];
    g_pngFloorW = png.px.empty() ? 0 : png.w;
    g_pngFloorH = png.px.empty() ? 0 : png.h;
    for (int k = 0; k < kVidCount; ++k) {
        try {
            StartVideo(g_vid[k]);
        } catch (...) {
            InterlockedExchange(&g_vid[k].state, 3);
            // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
            LOG("calibassets: [CALIBVIDEO] %s: REFUSED: an exception while starting the decoder - %s", g_vid[k].name,
                VidFallback(g_vid[k]));
        }
    }
}

// Render thread, once per draw of that surface: take the newest frame, if there is one, into the video's texture.
void VidUpload(Vid& v)
{
    if (v.state != 2 || v.texFailed) { return; }
    if (!(v.mid & 0x10)) { return; }
    const LONG old = InterlockedExchange(&v.mid, v.front);
    v.front = old & 3;
    ID3D11Device* dev = OverlayDevice();
    if (!dev) { return; }
    const Bytes& f = v.buf[v.front];
    if (!v.tex) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(v.w);
        td.Height = static_cast<UINT>(v.h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;   // OverlayRenderCalibFloorImage refuses any other
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { f.data(), static_cast<UINT>(v.w * 4), 0 };
        const HRESULT hr = dev->CreateTexture2D(&td, &sd, &v.tex);
        if (FAILED(hr) || !v.tex) {
            v.tex = nullptr;
            v.texFailed = true;
            // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
            LOG("calibassets: [CALIBVIDEO] %s: REFUSED: the video texture (%d x %d) FAILED hr 0x%08lX - %s", v.name, v.w, v.h,
                (unsigned long)hr, VidFallback(v));
            return;
        }
        // [PROBE: CALIBVIDEO] gate=none (a dlvr\calib void video present)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBVIDEO] %s: now the VIDEO (%d x %d texture, first frame uploaded)", v.name, v.w, v.h);
    } else {
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        if (!ctx) { return; }
        ctx->UpdateSubresource(v.tex, 0, nullptr, f.data(), static_cast<UINT>(v.w * 4), 0);
        ctx->Release();
    }
    InterlockedIncrement64(&v.uploads);
}

DWORD WINAPI LoaderThread(void*)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    // Nothing thrown here may leave this thread (std::terminate would end the game) or skip the ready flag below.
    // An image that threw mid-decode has an empty buffer and is not uploaded; the slots before it are complete.
    try {
        LoadImages();
    } catch (const std::bad_alloc&) {
        // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBART] REFUSED: out of memory (bad_alloc) while loading the images - %d loaded before it",
            g_imgLoaded);
    } catch (...) {
        // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBART] REFUSED: an exception while loading the images - %d loaded before it", g_imgLoaded);
    }
    const double msImages = MsSince(t0);
    // The void videos (floor, walls, ceiling - g_vid): a thread each; reads the PNG floor's size first, before g_ready
    // hands the PNG buffers to the render thread. Each start catches its own exception.
    StartVideos();
    QueryPerformanceCounter(&t1);
    const char* stage = "?";
    char why[256] = {};
    try {
        g_fontLoaded = LoadFont(&stage, why, sizeof why);
    } catch (const std::bad_alloc&) {
        g_fontLoaded = false;
        std::snprintf(why, sizeof why, "out of memory (bad_alloc)");
    } catch (...) {
        g_fontLoaded = false;
        std::snprintf(why, sizeof why, "an exception was thrown");
    }
    if (!g_fontLoaded) {
        g_fontW = g_fontH = 0;
        for (int l = 0; l < 4; ++l) { g_fontRgba[l].clear(); g_fontRgba[l].shrink_to_fit(); }
        // [PROBE: CALIBFONT] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBFONT] REFUSED: %s: %s - ImGui's default font", stage, why);
    }
    const double msFont = MsSince(t1);
    // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
    LOG("calibassets: [CALIBART] loader done in %.1f ms (images %.1f ms, font %.1f ms) - %d of %d images, font %s",
        MsSince(t0), msImages, msFont, g_imgLoaded, int(calibart::kImgCount), g_fontLoaded ? "loaded" : "REFUSED");
    InterlockedExchange(&g_ready, 1);
    return 0;
}

}  // namespace

void CalibArtStart()
{
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) { return; }
    DWORD tid = 0;
    HANDLE h = CreateThread(nullptr, 0, LoaderThread, nullptr, 0, &tid);
    if (!h) {
        const DWORD e = GetLastError();
        // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBART] REFUSED: CreateThread failed (error %lu) - no images, ImGui's default font", e);
        InterlockedExchange(&g_ready, 1);
        return;
    }
    // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
    LOG("calibassets: [CALIBART] loader thread %lu started (once per launch)", tid);
    CloseHandle(h);
}

void CalibArtPump()
{
    if (g_pumped) { return; }
    if (InterlockedCompareExchange(&g_ready, 1, 1) != 1) { return; }   // one interlocked read per frame until done
    ID3D11Device* dev = OverlayDevice();
    if (!dev) {
        if (!g_noDeviceLogged) {
            g_noDeviceLogged = true;
            // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
            LOG("calibassets: [CALIBART] loader done but the overlay has no device yet - textures wait");
        }
        return;
    }
    g_pumped = true;

    int created = 0;
    for (int k = 0; k < calibart::kImgCount; ++k) {
        Slot& s = g_img[k];
        if (s.px.empty()) { continue; }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(s.w);
        td.Height = static_cast<UINT>(s.h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        // Drawn by ImGui into the quad RTVs: plain UNORM when those are UNORM (no decode here, no encode there - the PNG as
        // authored), _SRGB when they are _SRGB (the fallback; decode + encode). kFloorGrid is copy-only and must stay _SRGB
        // (RenderCalibImageInto refuses any other source). Trace 20261114_1230_imgui_quad_rtv_gamma.
        td.Format = (k == calibart::kFloorGrid || OverlayQuadRtvIsSrgb()) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { s.px.data(), static_cast<UINT>(s.w * 4), 0 };
        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = dev->CreateTexture2D(&td, &sd, &tex);
        if (SUCCEEDED(hr) && tex) {
            hr = dev->CreateShaderResourceView(tex, nullptr, &s.srv);
            if (FAILED(hr)) { s.srv = nullptr; }
        }
        if (s.srv) {
            ++created;
            if (k == calibart::kFloorGrid) { s.tex = tex; tex = nullptr; }   // kept for CopyResource
        } else {
            // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
            LOG("calibassets: [CALIBART] texture for %s (%d x %d) FAILED: hr 0x%08lX - that element falls back",
                calibart::ImgFile(static_cast<calibart::Img>(k)), s.w, s.h, (unsigned long)hr);
            s.w = s.h = 0;
        }
        if (tex) { tex->Release(); }
        s.px.clear();
        s.px.shrink_to_fit();
    }

    HRESULT hrFont = S_OK, hrSrv = S_OK, hrSamp = S_OK;
    ID3D11ShaderResourceView* fontSrv = nullptr;
    ID3D11SamplerState* samp = nullptr;
    if (g_fontLoaded) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(g_fontW);
        td.Height = static_cast<UINT>(g_fontH);
        td.MipLevels = 4;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd[4] = {};
        for (int l = 0; l < 4; ++l) {
            sd[l].pSysMem = g_fontRgba[l].data();
            sd[l].SysMemPitch = static_cast<UINT>((g_fontW >> l) * 4);
        }
        ID3D11Texture2D* tex = nullptr;
        hrFont = dev->CreateTexture2D(&td, sd, &tex);
        if (SUCCEEDED(hrFont) && tex) {
            hrSrv = dev->CreateShaderResourceView(tex, nullptr, &fontSrv);
            if (FAILED(hrSrv)) { fontSrv = nullptr; }
        }
        if (tex) { tex->Release(); }
        D3D11_SAMPLER_DESC smp = {};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.MaxAnisotropy = 1;
        smp.ComparisonFunc = D3D11_COMPARISON_NEVER;
        smp.MinLOD = 0.0f;
        smp.MaxLOD = 3.0f;
        if (fontSrv) {
            hrSamp = dev->CreateSamplerState(&smp, &samp);
            if (FAILED(hrSamp)) { samp = nullptr; }
        }
        if (fontSrv && samp) {
            g_font.tex = reinterpret_cast<unsigned long long>(fontSrv);
            g_font.fontHeight = static_cast<float>(g_fm.fontHeight);
            g_font.texW = static_cast<float>(g_fontW);
            g_font.texH = static_cast<float>(g_fontH);
            g_font.m = &g_fm;
            g_font.g = g_glyph;
            g_font.sampler = samp;
            g_fontLive = true;
        } else {
            if (fontSrv) { fontSrv->Release(); }
            if (samp) { samp->Release(); }
        }
        for (int l = 0; l < 4; ++l) { g_fontRgba[l].clear(); g_fontRgba[l].shrink_to_fit(); }
    }
    if (g_fontLive) {
        // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBART] textures created: %d, font texture %d x %d x 4 mips", created, g_fontW, g_fontH);
    } else {
        // [PROBE: CALIBART] gate=none (the calibration screen)  site=HEADSET  log-only=yes
        LOG("calibassets: [CALIBART] textures created: %d, font texture NONE (%s; texture hr 0x%08lX, srv hr 0x%08lX, "
            "sampler hr 0x%08lX) - ImGui's default font", created, g_fontLoaded ? "creation failed" : "the font was refused",
            (unsigned long)hrFont, (unsigned long)hrSrv, (unsigned long)hrSamp);
    }
}

unsigned long long CalibArtTex(calibart::Img i, int* w, int* h)
{
    if (w) { *w = 0; }
    if (h) { *h = 0; }
    if (!g_pumped || (int)i < 0 || (int)i >= calibart::kImgCount) { return 0; }
    const Slot& s = g_img[(int)i];
    if (!s.srv) { return 0; }
    if (w) { *w = s.w; }
    if (h) { *h = s.h; }
    return reinterpret_cast<unsigned long long>(s.srv);
}

ID3D11Texture2D* CalibArtFloorTexture()
{
    if (!g_pumped) { return nullptr; }
    // The floor video: this call IS "the floor is being drawn" - the decoder runs only while these stamps keep coming.
    Vid& v = g_vid[kVidFloor];
    v.wantMs = GetTickCount64();
    VidUpload(v);
    if (v.tex) { return v.tex; }
    return g_img[calibart::kFloorGrid].tex;
}

ID3D11Texture2D* CalibArtVoidTexture(bool ceiling)
{
    if (!g_pumped) { return nullptr; }
    // Same contract as the floor: this call IS "it is being drawn" - its decoder runs only while these stamps keep coming.
    Vid& v = g_vid[ceiling ? kVidCeiling : kVidWalls];
    v.wantMs = GetTickCount64();
    VidUpload(v);
    return v.tex;
}

const CalibFont* CalibArtFont()
{
    return g_fontLive ? &g_font : nullptr;
}

}  // namespace dlvr
