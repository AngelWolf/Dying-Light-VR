// See fsrinterop.h for what this is, and docs/PROPOSAL-FSR-INTEROP-2026-09-26.md
// for the design it implements. Setup = step 2a (INV-253/254); Dispatch = 2b.
#include "fsrinterop.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../common/log.h"
#include "../common/shaderbin.h"

// The FidelityFX API headers, SDK 2.3.0 (FSR_SDK/v2.3.0, each file verified
// against its GitHub blob SHA - REF-FSR-SDK). Types and constants only: the five
// entry points are resolved from the loader DLL at runtime, never imported.
// ffx_api_loader.h's ffxLoadFunctions is deliberately NOT used - its body is
// compiled only under _WINDOWS / PLATFORM_WINDOWS (ffx_api_loader.h:55), and
// without them it silently leaves every pointer uninitialised.
#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

#define LOG(...) ::dlvr::LogTo("dlvr_bootstrap.log", __VA_ARGS__)

namespace dlvr {
namespace fsr {

namespace {

// The frustum's vertical scale P[1][1] off a row-major view-projection (trace docs/traces/20261124_1612_wheel_taa_fsr_offcentre.md): row 1's
// xyz = P11 x view-up + c x view-forward, where c is the frustum's vertical off-centre (+ the jitter); row 3's xyz is the view-forward
// axis. |row 1| counted c too, so at symmetric_fov 0 FSR was told a vertical FOV ~1.5 deg too NARROW (RUN SY1's eye 0, u 0.9657 d 1.4281:
// 98.7 deg told, 2 atan((u + d) / 2) = 100.2 deg the tangent span). The part of row 1 perpendicular to row 3 is P11 alone. Symmetric
// frustum: only the jitter's ~1e-4 is removed.
inline float TrueP11(const float* vp)
{
    const double d11 = static_cast<double>(vp[4]) * vp[4] + static_cast<double>(vp[5]) * vp[5] + static_cast<double>(vp[6]) * vp[6];
    const double d13 = static_cast<double>(vp[4]) * vp[12] + static_cast<double>(vp[5]) * vp[13] + static_cast<double>(vp[6]) * vp[14];
    const double d33 = static_cast<double>(vp[12]) * vp[12] + static_cast<double>(vp[13]) * vp[13] + static_cast<double>(vp[14]) * vp[14];
    const double perp = d33 > 1e-12 ? d11 - d13 * d13 / d33 : d11;
    return perp > 0.0 ? static_cast<float>(std::sqrt(perp)) : 0.0f;
}

constexpr int kMaxEyes = 2;
// Command allocators (and timestamp slots) in flight. An allocator is only
// reset once the fence says its last list has finished. MEASURED 2026-09-26:
// 3 was enough in flat (3 stalls in 11,400 dispatches) and far too few in VR -
// at 2 dispatches a frame it let the CPU run only 1.5 frames ahead of FSR's GPU
// work, and 29,407 of 33,600 dispatches (87.5%) waited on the allocator. 8 =
// 4 frames of run-ahead in VR. Jay's go-ahead; the A/B is the stall counter.
constexpr int kRing = 8;

// ---- D3D12 side ------------------------------------------------------------
ID3D12Device*              g_dev12    = nullptr;
ID3D12CommandQueue*        g_queue    = nullptr;
ID3D12CommandAllocator*    g_alloc[kRing] = {};   // the ring 2b records into
ID3D12GraphicsCommandList* g_list     = nullptr;
ID3D12Fence*               g_fence12  = nullptr;
UINT64                     g_fenceValue = 0;

// ---- D3D11 side ------------------------------------------------------------
ID3D11Fence*               g_fence11  = nullptr;
ID3D11DeviceContext4*      g_ctx4     = nullptr;

// ---- shared textures (created in D3D12, opened in D3D11) -------------------
// One input set serves both eyes (PROPOSAL §2): eye 1's copies are issued after
// D3D11's GPU-side wait on eye 0's FSR completion.
ID3D12Resource*            g_colour12 = nullptr;
ID3D11Texture2D*           g_colour11 = nullptr;
ID3D12Resource*            g_depth12  = nullptr;
ID3D11Texture2D*           g_depth11  = nullptr;
ID3D11UnorderedAccessView* g_depthUav11 = nullptr;   // the 2b depth-plane copy pass writes it
ID3D12Resource*            g_mv12     = nullptr;
ID3D11Texture2D*           g_mv11     = nullptr;
ID3D12Resource*            g_out12[kMaxEyes]  = {};
ID3D11Texture2D*           g_out11[kMaxEyes]  = {};
ID3D11ShaderResourceView*  g_outSrv11[kMaxEyes] = {};  // what 2b substitutes into the tonemap draw

// ---- FFX -------------------------------------------------------------------
HMODULE              g_loader      = nullptr;
PfnFfxCreateContext  g_ffxCreate   = nullptr;
PfnFfxDestroyContext g_ffxDestroy  = nullptr;
PfnFfxConfigure      g_ffxConfigure = nullptr;
PfnFfxQuery          g_ffxQuery    = nullptr;
PfnFfxDispatch       g_ffxDispatch = nullptr;
ffxContext           g_ffx[kMaxEyes] = {};

// "Pointers passed in desc must remain live until ffxDestroyContext is called
// on the context" (ffx_api.h:139). The chain is linked by pointer, so it lives
// here - one per eye - and never on the stack.
struct CreateChain {
    ffxCreateContextDescUpscale        up;
    ffxCreateContextDescUpscaleVersion ver;
    ffxCreateBackendDX12Desc           be;
    ffxOverrideVersion                 ov;
};
CreateChain g_chain[kMaxEyes];

int  g_result = 0;   // Setup (the size-independent core): 0 not run, 1 ready, -1 failed
int  g_sized  = 0;   // shared textures + contexts: 0 none, 1 ready, -1 failed
bool g_upscaleMode = false;   // step 3: sized by EnsureUpscale, post-tonemap input
UINT g_inW = 0, g_inH = 0;    // render (scene) size the contexts and inputs were made at
UINT g_outW = 0, g_outH = 0;  // output size (== in for NATIVE)
int  g_eyes = 0;
ID3D11Device* g_dev11 = nullptr;   // the engine's device, not AddRef'd (it outlives us)

// The version choice, made ONCE in Setup from the logged list and reused by
// every (re)creation - so a resize cannot quietly pick a different provider.
bool     g_havePick = false;       // an override id was chosen
uint64_t g_pickId   = 0;
bool     g_pickIs4  = false;
bool     g_have3    = false;       // a 3.x entry exists (the FSR 4 fallback)
uint64_t g_id3      = 0;
char     g_pickName[32] = {};
bool     g_fsr4Fallback = false;   // FSR 4 was asked for; 3.x is what runs
char     g_provider[32] = {};      // what FFX reported creating, for the overlay

// ---- 2b: the per-frame resolve ------------------------------------------------
// The depth-plane copy: the engine's depth is R32G8X24_TYPELESS, the shared
// input R32_FLOAT, so a CopyResource is illegal and a one-line CS does it.
// Compiled at runtime with D3DCompile, the same as the MV-fix / NIS / TAA passes.
// LENS CROP (2026-11-11): `off` = the source texel the destination's origin reads (the crop's top-left; 0,0 uncropped), so FSR's
// depth starts at the crop's first row like its colour and motion vectors.
const char* const kDepthCopyCs =
    "cbuffer C : register(b0) { uint2 off; uint2 pad; };\n"
    "Texture2D<float> src : register(t0);\n"
    "RWTexture2D<float> dst : register(u0);\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) { dst[id.xy] = src.Load(int3(id.xy + off, 0)); }\n";
ID3D11ComputeShader*      g_depthCs     = nullptr;
ID3D11Buffer*             g_depthCb     = nullptr;   // kDepthCopyCs's `off` (LENS CROP)
// LENS CROP, NATIVE only: FSR writes from the origin, so a cropped NATIVE dispatch writes this D3D12 scratch texture (full size,
// the out format) and a CopyTextureRegion places it in g_out12[eye] at the crop's offset. Upscale needs none (its copy-out moves it).
ID3D12Resource*           g_scratch12[kMaxEyes] = {};
bool                      g_scratchFailed = false;
ID3D11ShaderResourceView* g_depthSrcSrv = nullptr;
ID3D11Texture2D*          g_depthSrcTex = nullptr;   // the texture that SRV views - re-made when it changes
bool                      g_depthCsFailed = false;

// Source textures whose CopyResource legality was already checked (same size,
// same format). A re-latch hands us new pointers; each is checked once.
ID3D11Texture2D* g_colourChecked = nullptr;
ID3D11Texture2D* g_mvChecked     = nullptr;

UINT64        g_allocDone[kRing] = {};   // fence value that frees each allocator
int           g_ring = 0;
HANDLE        g_waitEvent = nullptr;

// GPU time of the FSR dispatch itself: two D3D12 timestamps per ring slot,
// resolved into a persistently mapped readback buffer and read only once the
// fence says they landed - never a blocking read (INV-14's rule, D3D12 side).
ID3D12QueryHeap* g_qHeap     = nullptr;
ID3D12Resource*  g_qReadback = nullptr;
const UINT64*    g_qMapped   = nullptr;
UINT64           g_qFreq     = 0;
UINT64           g_qValue[kRing]   = {};
bool             g_qPending[kRing] = {};

LARGE_INTEGER g_qpcFreq = {};
LARGE_INTEGER g_lastDispatchQpc[kMaxEyes] = {};

// Counters. Window stats reset at every heartbeat; totals never do.
LONG64 g_dispatches = 0, g_dispatchFails = 0, g_allocStalls = 0, g_resets = 0;
LONG64 g_viewProjFallbacks = 0;
double g_gpuSum = 0, g_gpuMax = 0;  int g_gpuN = 0, g_gpuNotReady = 0;
double g_cpuSum = 0, g_cpuMax = 0;  int g_cpuN = 0;

const char* FmtName(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R11G11B10_FLOAT:    return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16_FLOAT:       return "R16G16_FLOAT";
    case DXGI_FORMAT_R32_FLOAT:          return "R32_FLOAT";
    case DXGI_FORMAT_R32G8X24_TYPELESS:  return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:  return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:     return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:  return "B8G8R8A8_TYPELESS";
    default:                             return "other";
    }
}

const char* RcName(ffxReturnCode_t rc)
{
    switch (rc) {
    case FFX_API_RETURN_OK:                              return "OK";
    case FFX_API_RETURN_ERROR:                           return "ERROR";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:          return "ERROR_UNKNOWN_DESCTYPE";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR:             return "ERROR_RUNTIME_ERROR";
    case FFX_API_RETURN_NO_PROVIDER:                     return "NO_PROVIDER";
    case FFX_API_RETURN_ERROR_MEMORY:                    return "ERROR_MEMORY";
    case FFX_API_RETURN_ERROR_PARAMETER:                 return "ERROR_PARAMETER";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
    default:                                             return "unknown code";
    }
}

// The one refusal format. Names the step, so a failed setup is never silent
// and never ambiguous about where it stopped.
// [PROBE: FSR] gate=aa_fsr.txt + dlss_flat.txt + a latched tonemap draw (reached from EnsureVisibleFeature)  site=FLAT  log-only=yes
bool Fail(const char* step, HRESULT hr)
{
    LOG("[FSR] setup *** FAILED at: %s *** (hr=0x%08X). FSR cannot run this launch; the image stays "
        "vanilla. (v2: setup CONTINUES through every step that does not depend on this one - the "
        "summary line says which.)", step, static_cast<unsigned>(hr));
    return false;
}

// FFX's own diagnostics. Capped: create-time messages are the point, and a
// per-frame message in 2b must not become a flood.
volatile LONG g_ffxMsgs = 0;
void FfxMessage(uint32_t type, const wchar_t* message)
{
    const LONG n = InterlockedIncrement(&g_ffxMsgs);
    if (n <= 40) {
        LOG("[FSR] ffx %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "warning",
            message ? message : L"(null)");
    } else if (n == 41) {
        LOG("[FSR] ffx messages capped at 40 - later ones are dropped, not printed");
    }
}

long long FileSize(const wchar_t* path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) { return -1; }
    return (static_cast<long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

// The directory xinput1_3.dll was loaded from, with a trailing backslash. The
// FFX DLLs are deployed beside it (CMake POST_BUILD), so they are loaded by FULL
// PATH from here rather than trusting the search order.
bool OwnDir(wchar_t* out, DWORD cap)
{
    static const int s_anchor = 0;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&s_anchor), &self)) {
        return false;
    }
    const DWORD n = GetModuleFileNameW(self, out, cap);
    if (n == 0 || n >= cap) { return false; }
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) { return false; }
    slash[1] = 0;
    return true;
}

// ---- 1. the D3D12 device, on the engine's adapter ---------------------------
bool CreateDevice12(ID3D11Device* dev11)
{
    IDXGIDevice* dxgiDev = nullptr;
    HRESULT hr = dev11->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev));
    if (FAILED(hr) || !dxgiDev) { return Fail("IDXGIDevice from the engine's D3D11 device", hr); }
    IDXGIAdapter* ad11 = nullptr;
    hr = dxgiDev->GetAdapter(&ad11);
    dxgiDev->Release();
    if (FAILED(hr) || !ad11) { return Fail("IDXGIDevice::GetAdapter", hr); }
    DXGI_ADAPTER_DESC ad = {};
    hr = ad11->GetDesc(&ad);
    ad11->Release();
    if (FAILED(hr)) { return Fail("IDXGIAdapter::GetDesc", hr); }
    LOG("[FSR] engine's D3D11 adapter: \"%ls\" vendor 0x%04X LUID %08lX:%08lX",
        ad.Description, ad.VendorId, static_cast<unsigned long>(ad.AdapterLuid.HighPart),
        ad.AdapterLuid.LowPart);

    IDXGIFactory4* f4 = nullptr;
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&f4));
    if (FAILED(hr) || !f4) { return Fail("CreateDXGIFactory1(IDXGIFactory4)", hr); }
    IDXGIAdapter1* ad12 = nullptr;
    hr = f4->EnumAdapterByLuid(ad.AdapterLuid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&ad12));
    f4->Release();
    if (FAILED(hr) || !ad12) { return Fail("IDXGIFactory4::EnumAdapterByLuid (the engine's LUID)", hr); }

    // Loaded, never imported: a static d3d12 import would stop the mod loading
    // anywhere d3d12.dll is missing, for a feature most launches do not use.
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12) {
        ad12->Release();
        LOG("[FSR] setup *** FAILED at: LoadLibrary(d3d12.dll) *** (GetLastError %lu). FSR refuses "
            "this launch; the image stays vanilla.", GetLastError());
        return false;
    }
    auto create = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (!create) { ad12->Release(); return Fail("GetProcAddress(D3D12CreateDevice)", E_NOINTERFACE); }
    hr = create(ad12, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&g_dev12));
    ad12->Release();
    if (FAILED(hr) || !g_dev12) { return Fail("D3D12CreateDevice on the engine's adapter", hr); }

    // What the DRIVER reports. FSR 3.1 needs SM 6.2 and FSR 4 needs 6.6
    // (REF-FSR-SDK); the provider applies its own gate on top, so this line is
    // context for the version list, not a verdict on it. A runtime that does
    // not know a model answers E_INVALIDARG, so step down until one is known.
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {};
    bool smKnown = false;
    for (int m = 0x66; m >= 0x60 && !smKnown; --m) {
        sm.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(m);
        smKnown = SUCCEEDED(g_dev12->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)));
    }
    const int smv = smKnown ? static_cast<int>(sm.HighestShaderModel) : 0;
    LOG("[FSR] D3D12 device CREATED on the same adapter (matched by LUID). Driver-reported highest "
        "shader model: %s%d.%d (FSR 3.1 needs 6.2, FSR 4 needs 6.6)",
        smKnown ? "" : "*** QUERY FAILED *** ", smv >> 4, smv & 0xF);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = g_dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&g_queue));
    if (FAILED(hr) || !g_queue) { return Fail("CreateCommandQueue(DIRECT)", hr); }
    for (auto& a : g_alloc) {
        hr = g_dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&a));
        if (FAILED(hr) || !a) { return Fail("CreateCommandAllocator", hr); }
    }
    hr = g_dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr,
                                    __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&g_list));
    if (FAILED(hr) || !g_list) { return Fail("CreateCommandList", hr); }
    hr = g_list->Close();   // created open; 2b resets it per dispatch
    if (FAILED(hr)) { return Fail("ID3D12GraphicsCommandList::Close", hr); }

    // Timestamps for the dispatch's GPU cost. Failure here costs the COST line,
    // not the resolve, so it is reported and not fatal.
    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 2 * kRing;
    HRESULT hrQ = g_dev12->CreateQueryHeap(&qh, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&g_qHeap));
    if (SUCCEEDED(hrQ)) {
        D3D12_HEAP_PROPERTIES rb = {};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sizeof(UINT64) * 2 * kRing;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hrQ = g_dev12->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, __uuidof(ID3D12Resource),
                                               reinterpret_cast<void**>(&g_qReadback));
    }
    if (SUCCEEDED(hrQ)) {
        void* p = nullptr;
        hrQ = g_qReadback->Map(0, nullptr, &p);   // readback heaps may stay mapped
        g_qMapped = static_cast<const UINT64*>(p);
    }
    if (SUCCEEDED(hrQ)) { hrQ = g_queue->GetTimestampFrequency(&g_qFreq); }
    if (FAILED(hrQ) || !g_qMapped || !g_qFreq) {
        g_qMapped = nullptr;
        LOG("[FSR] *** GPU timestamps UNAVAILABLE (hr=0x%08X) *** - the FSR GPU-cost line will read NO "
            "DATA; the resolve itself is unaffected", static_cast<unsigned>(hrQ));
    }
    g_waitEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    QueryPerformanceFrequency(&g_qpcFreq);
    LOG("[FSR] D3D12 direct queue, %d command allocators and 1 command list created; GPU timestamps %s "
        "(%llu Hz)", kRing, g_qMapped ? "ON" : "OFF", g_qFreq);
    return true;
}

// ---- 2. the shared fence, and one round trip across both APIs ---------------
bool CreateFenceAndRoundTrip(ID3D11Device* dev11, ID3D11DeviceContext* ctx)
{
    HRESULT hr = g_dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence),
                                      reinterpret_cast<void**>(&g_fence12));
    if (FAILED(hr) || !g_fence12) { return Fail("ID3D12Device::CreateFence(SHARED)", hr); }
    HANDLE h = nullptr;
    hr = g_dev12->CreateSharedHandle(g_fence12, nullptr, GENERIC_ALL, nullptr, &h);
    if (FAILED(hr) || !h) { return Fail("CreateSharedHandle(fence)", hr); }

    ID3D11Device5* dev5 = nullptr;
    hr = dev11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&dev5));
    if (FAILED(hr) || !dev5) {
        CloseHandle(h);
        LOG("[FSR] *** ID3D11Device5 NOT AVAILABLE (hr=0x%08X) *** - needs Windows 10 1703 or "
            "later. Without it D3D11 cannot open the shared fence, so the two APIs cannot be "
            "synchronised. FSR refuses this launch; the image stays vanilla.",
            static_cast<unsigned>(hr));
        return false;
    }
    hr = dev5->OpenSharedFence(h, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&g_fence11));
    dev5->Release();
    CloseHandle(h);   // both sides hold their own reference now
    if (FAILED(hr) || !g_fence11) { return Fail("ID3D11Device5::OpenSharedFence", hr); }
    hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&g_ctx4));
    if (FAILED(hr) || !g_ctx4) { return Fail("ID3D11DeviceContext4 from the immediate context", hr); }

    // The per-frame handshake (PROPOSAL §3 steps 2, 3, 5, 6), once, with nothing
    // between the waits. D3D12 cannot wait on D3D11 work that was never
    // submitted, hence the Flush. The CPU wait is BOUNDED: a fence that never
    // arrives must report, not hang the render thread.
    LARGE_INTEGER qf = {}, t0 = {}, t1 = {};
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&t0);
    const HRESULT hrSig11 = g_ctx4->Signal(g_fence11, 1);
    ctx->Flush();
    const HRESULT hrWait12 = g_queue->Wait(g_fence12, 1);
    const HRESULT hrSig12  = g_queue->Signal(g_fence12, 2);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) { return Fail("CreateEvent for the fence round trip", HRESULT_FROM_WIN32(GetLastError())); }
    const HRESULT hrEv = g_fence12->SetEventOnCompletion(2, ev);
    const DWORD w = WaitForSingleObject(ev, 2000);
    QueryPerformanceCounter(&t1);
    CloseHandle(ev);
    const UINT64 c12 = g_fence12->GetCompletedValue();
    const UINT64 c11 = g_fence11->GetCompletedValue();
    const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
                      static_cast<double>(qf.QuadPart ? qf.QuadPart : 1);
    g_fenceValue = 2;
    if (w != WAIT_OBJECT_0 || c12 < 2 || c11 < 2) {
        LOG("[FSR] setup *** FAILED at: the fence round trip *** - wait result %lu after %.1f ms "
            "(0 = signalled, 258 = 2 s timeout); completed values D3D12 %llu / D3D11 %llu (both "
            "must read 2). hr: D3D11 Signal 0x%08X, D3D12 Wait 0x%08X, D3D12 Signal 0x%08X, "
            "SetEventOnCompletion 0x%08X. FSR refuses this launch; the image stays vanilla.",
            w, ms, c12, c11, static_cast<unsigned>(hrSig11), static_cast<unsigned>(hrWait12),
            static_cast<unsigned>(hrSig12), static_cast<unsigned>(hrEv));
        return false;
    }
    // Step 6, proven once: D3D11 waits GPU-side on the value D3D12 signalled.
    // Already reached, so it cannot stall anything.
    const HRESULT hrWait11 = g_ctx4->Wait(g_fence11, 2);
    LOG("[FSR] shared fence ROUND TRIP OK in %.3f ms: D3D11 Signal(1) -> Flush -> D3D12 Wait(1) + "
        "Signal(2) -> completed values D3D12 %llu / D3D11 %llu (both must read 2). D3D11 Wait(2) "
        "hr=0x%08X. (One-off at setup, CPU-blocking by design - NOT the per-frame cost.)",
        ms, c12, c11, static_cast<unsigned>(hrWait11));
    return SUCCEEDED(hrWait11) ? true : Fail("ID3D11DeviceContext4::Wait", hrWait11);
}

// ---- 3. one shared texture ---------------------------------------------------
// INV-253 (measured 20:49 + 20:56, and again every 2b/2c launch): a D3D12-created
// shared texture is REFUSED by D3D11 OpenSharedResource1 (0x80070057, flags 0x0 /
// ALLOW_RENDER_TARGET / ALLOW_SIMULTANEOUS_ACCESS, two formats); D3D11-created ->
// D3D12 OpenSharedHandle works for every texture used here. So that direction -
// "method D" in the old matrix - is the only one built; the matrix itself, and
// the three refused variants, were removed in step 3 once they had answered.

// CopyResource's format rule: identical, or members of the same typeless group.
// Only the groups this file copies between are listed; anything else must match.
bool SameCopyGroup(DXGI_FORMAT a, DXGI_FORMAT b)
{
    if (a == b) { return true; }
    auto rgba8 = [](DXGI_FORMAT f) {
        return f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
               f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    };
    auto bgra8 = [](DXGI_FORMAT f) {
        return f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM ||
               f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    };
    return (rgba8(a) && rgba8(b)) || (bgra8(a) && bgra8(b));
}

void ReleasePair(ID3D12Resource** r12, ID3D11Texture2D** t11)
{
    if (*t11) { (*t11)->Release(); *t11 = nullptr; }
    if (*r12) { (*r12)->Release(); *r12 = nullptr; }
}

// D3D11 owns the texture; D3D12 opens it (INV-253).
bool CreateShared11(ID3D11Device* dev11, const char* what, DXGI_FORMAT fmt, UINT w, UINT h, bool uav,
                    ID3D12Resource** out12, ID3D11Texture2D** out11)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = dev11->CreateTexture2D(&td, nullptr, out11);
    if (FAILED(hr) || !*out11) {
        LOG("[FSR] shared %-8s %s %ux%u%s: D3D11 CreateTexture2D(SHARED_NTHANDLE|SHARED) FAILED "
            "hr=0x%08X", what, FmtName(fmt), w, h, uav ? " +UAV" : "", static_cast<unsigned>(hr));
        return false;
    }
    IDXGIResource1* dr = nullptr;
    hr = (*out11)->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&dr));
    if (FAILED(hr) || !dr) {
        LOG("[FSR] shared %-8s: IDXGIResource1 NOT AVAILABLE hr=0x%08X", what, static_cast<unsigned>(hr));
        return false;
    }
    HANDLE hnd = nullptr;
    hr = dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hnd);
    dr->Release();
    if (FAILED(hr) || !hnd) {
        LOG("[FSR] shared %-8s %s: IDXGIResource1::CreateSharedHandle FAILED hr=0x%08X", what,
            FmtName(fmt), static_cast<unsigned>(hr));
        return false;
    }
    hr = g_dev12->OpenSharedHandle(hnd, __uuidof(ID3D12Resource), reinterpret_cast<void**>(out12));
    CloseHandle(hnd);
    if (FAILED(hr) || !*out12) {
        LOG("[FSR] shared %-8s %s %ux%u: created in D3D11 but D3D12 OpenSharedHandle FAILED hr=0x%08X",
            what, FmtName(fmt), w, h, static_cast<unsigned>(hr));
        return false;
    }
    const D3D12_RESOURCE_DESC rd = (*out12)->GetDesc();
    LOG("[FSR] shared %-8s OK: D3D11 %s %ux%u%s -> D3D12 sees %s %llux%u, Flags 0x%X",
        what, FmtName(fmt), w, h, uav ? " +UAV" : "", FmtName(rd.Format), rd.Width, rd.Height,
        static_cast<unsigned>(rd.Flags));
    return true;
}

// Reports whether CopyResource(ours <- engine's) is legal. It is not tried in
// 2a; D3D11 without the debug layer drops an illegal copy SILENTLY, so the rule
// (same size, same format) is checked here from the descs, where it can speak.
void LogCopyCompat(const char* what, ID3D11Texture2D* engine, ID3D11Texture2D* ours)
{
    if (!engine || !ours) {
        LOG("[FSR] copy check %-8s: *** NO VERDICT *** - engine texture %s, shared texture %s",
            what, engine ? "present" : "NULL", ours ? "present" : "NULL");
        return;
    }
    D3D11_TEXTURE2D_DESC e = {}, o = {};
    engine->GetDesc(&e);
    ours->GetDesc(&o);
    const bool ok = e.Width == o.Width && e.Height == o.Height && SameCopyGroup(e.Format, o.Format) &&
                    e.SampleDesc.Count == o.SampleDesc.Count && e.MipLevels == o.MipLevels;
    LOG("[FSR] copy check %-8s: engine %s %ux%u mips %u msaa %u | shared %s %ux%u mips %u msaa %u -> "
        "CopyResource %s",
        what, FmtName(e.Format), e.Width, e.Height, e.MipLevels, e.SampleDesc.Count,
        FmtName(o.Format), o.Width, o.Height, o.MipLevels, o.SampleDesc.Count,
        ok ? "LEGAL" : "*** NOT LEGAL - 2b needs a conversion pass here ***");
}

bool MakeShared(ID3D11Device* dev11, const char* what, DXGI_FORMAT fmt, UINT w, UINT h,
                bool uav, ID3D12Resource** out12, ID3D11Texture2D** out11)
{
    const bool ok = CreateShared11(dev11, what, fmt, w, h, uav, out12, out11);
    if (!ok) { ReleasePair(out12, out11); }   // a half-made pair is never kept
    return ok;
}

// Releases every sized resource. Called only after the GPU has finished with
// them (WaitIdle), and only by a re-size - the launch never tears them down.
void ReleaseSharedSet()
{
    ReleasePair(&g_colour12, &g_colour11);
    if (g_depthUav11) { g_depthUav11->Release(); g_depthUav11 = nullptr; }
    ReleasePair(&g_depth12, &g_depth11);
    ReleasePair(&g_mv12, &g_mv11);
    for (int e = 0; e < kMaxEyes; ++e) {
        if (g_outSrv11[e]) { g_outSrv11[e]->Release(); g_outSrv11[e] = nullptr; }
        ReleasePair(&g_out12[e], &g_out11[e]);
        if (g_scratch12[e]) { g_scratch12[e]->Release(); g_scratch12[e] = nullptr; }   // LENS CROP: re-made at the new size
    }
}

// LENS CROP, NATIVE: eye's scratch texture, the same desc as g_out12[eye] (D3D12-only, not shared), made once per size. Left in
// UNORDERED_ACCESS between dispatches. A failure is logged once and refuses the crop for FSR native for the session.
ID3D12Resource* ScratchFor(int eye)
{
    if (g_scratch12[eye]) { return g_scratch12[eye]; }
    if (g_scratchFailed || !g_dev12 || !g_out12[eye]) { return nullptr; }
    const D3D12_RESOURCE_DESC od = g_out12[eye]->GetDesc();
    D3D12_RESOURCE_DESC rd = od;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    const HRESULT hr = g_dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                        nullptr, IID_PPV_ARGS(&g_scratch12[eye]));
    // [PROBE: LENSCROP] gate=dlss_lens_crop (FSR native, once per size)  site=HEADSET  log-only=no
    LOG("[LENSCROP] FSR native scratch out[%d] %llux%u: %s (hr=0x%08X)", eye, static_cast<unsigned long long>(rd.Width), rd.Height,
        SUCCEEDED(hr) && g_scratch12[eye] ? "CREATED" : "*** FAILED - FSR native is not cropped ***", static_cast<unsigned>(hr));
    if (FAILED(hr) || !g_scratch12[eye]) { g_scratch12[eye] = nullptr; g_scratchFailed = true; return nullptr; }
    return g_scratch12[eye];
}

// NATIVE: colour R11G11B10_FLOAT (the engine's HDR pre-tonemap format) and the
// output R16G16B16A16_FLOAT, both w x h - as measured in 2a-2c.
// UPSCALE: colour R8G8B8A8_UNORM at the render size (the engine's post-tonemap
// source is R8G8B8A8_TYPELESS - same copy group), output R8G8B8A8_UNORM at the
// output size; the caller's TYPELESS twin carries the sRGB view the engine samples.
bool CreateSharedSet(ID3D11Device* dev11, ID3D11Texture2D* colour, ID3D11Texture2D* mv,
                     ID3D11Texture2D* depth, UINT inW, UINT inH, UINT outW, UINT outH, int eyes,
                     bool upscale)
{
    const DXGI_FORMAT colourFmt = upscale ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R11G11B10_FLOAT;
    const DXGI_FORMAT outFmt    = upscale ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    // Every texture is tried even after a failure, so one run reports every outcome.
    bool all = true;
    all &= MakeShared(dev11, "colour", colourFmt, inW, inH, false, &g_colour12, &g_colour11);
    // Depth: the engine's is R32G8X24_TYPELESS (a depth-stencil), which cannot
    // be copied into R32_FLOAT - 2b writes the depth plane with a small pass, so
    // this one carries a UAV.
    all &= MakeShared(dev11, "depth", DXGI_FORMAT_R32_FLOAT, inW, inH, true, &g_depth12, &g_depth11);
    all &= MakeShared(dev11, "mv", DXGI_FORMAT_R16G16_FLOAT, inW, inH, false, &g_mv12, &g_mv11);
    for (int e = 0; e < eyes; ++e) {
        char name[16] = {};
        _snprintf_s(name, sizeof(name), _TRUNCATE, "out[%d]", e);
        all &= MakeShared(dev11, name, outFmt, outW, outH, true, &g_out12[e], &g_out11[e]);
    }

    // The two D3D11 views 2b depends on, proven now rather than discovered then.
    HRESULT hr = E_FAIL;
    if (g_depth11) {
        hr = dev11->CreateUnorderedAccessView(g_depth11, nullptr, &g_depthUav11);
        LOG("[FSR] D3D11 UAV on the shared depth: %s (hr=0x%08X)",
            SUCCEEDED(hr) && g_depthUav11 ? "OK" : "*** FAILED ***", static_cast<unsigned>(hr));
        all &= SUCCEEDED(hr) && g_depthUav11;
    } else {
        LOG("[FSR] D3D11 UAV on the shared depth: *** NOT TRIED *** - the depth texture was not created");
    }
    for (int e = 0; e < eyes; ++e) {
        if (!g_out11[e]) {
            LOG("[FSR] D3D11 SRV on shared out[%d]: *** NOT TRIED *** - the texture was not created", e);
            continue;
        }
        hr = dev11->CreateShaderResourceView(g_out11[e], nullptr, &g_outSrv11[e]);
        LOG("[FSR] D3D11 SRV on shared out[%d] (what the tonemap draw would sample): %s (hr=0x%08X)", e,
            SUCCEEDED(hr) && g_outSrv11[e] ? "OK" : "*** FAILED ***", static_cast<unsigned>(hr));
        all &= SUCCEEDED(hr) && g_outSrv11[e];
    }

    // UPSCALE sizes from the first matched draw with no engine textures in hand;
    // its sources are checked per pointer at dispatch (CheckCopy) instead.
    if (upscale) {
        if (!all) { return Fail("one or more shared textures/views (lines above)", E_FAIL); }
        return true;
    }
    LogCopyCompat("colour", colour, g_colour11);
    LogCopyCompat("mv", mv, g_mv11);
    if (depth) {
        D3D11_TEXTURE2D_DESC d = {};
        depth->GetDesc(&d);
        LOG("[FSR] copy check depth   : engine %s %ux%u -> NOT copyable into R32_FLOAT by design; 2b "
            "writes the depth plane with a pass (the MV-fix pass already reads it)",
            FmtName(d.Format), d.Width, d.Height);
    } else {
        LOG("[FSR] copy check depth   : *** NO VERDICT *** - engine depth texture NULL");
    }
    if (!all) { return Fail("one or more shared textures/views after colour (lines above)", E_FAIL); }
    return true;
}

// ---- 4. the FFX loader ------------------------------------------------------
bool LoadFfx()
{
    wchar_t dir[MAX_PATH] = {};
    if (!OwnDir(dir, MAX_PATH)) {
        LOG("[FSR] setup *** FAILED at: resolving xinput1_3.dll's own directory *** (GetLastError %lu)",
            GetLastError());
        return false;
    }
    wchar_t loaderPath[MAX_PATH] = {}, upPath[MAX_PATH] = {};
    swprintf_s(loaderPath, MAX_PATH, L"%lsamd_fidelityfx_loader_dx12.dll", dir);
    swprintf_s(upPath, MAX_PATH, L"%lsamd_fidelityfx_upscaler_dx12.dll", dir);
    const long long lsz = FileSize(loaderPath);
    const long long usz = FileSize(upPath);
    LOG("[FSR] FFX DLLs beside xinput1_3.dll (%ls): loader %lld B (SDK 2.3.0: 26376) | upscaler %lld B "
        "(SDK 2.3.0: 28761864). -1 = ABSENT.", dir, lsz, usz);

    // ALTERED_SEARCH_PATH so anything the loader pulls in resolves from ITS
    // directory first. Which upscaler actually got mapped is read back after the
    // first query, rather than assumed.
    g_loader = LoadLibraryExW(loaderPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_loader) {
        LOG("[FSR] setup *** FAILED at: LoadLibrary(%ls) *** (GetLastError %lu). FSR refuses this launch; "
            "the image stays vanilla.", loaderPath, GetLastError());
        return false;
    }
    g_ffxCreate    = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_loader, "ffxCreateContext"));
    g_ffxDestroy   = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_loader, "ffxDestroyContext"));
    g_ffxConfigure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(g_loader, "ffxConfigure"));
    g_ffxQuery     = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g_loader, "ffxQuery"));
    g_ffxDispatch  = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_loader, "ffxDispatch"));
    LOG("[FSR] FFX loader LOADED. Entry points: create %s, destroy %s, configure %s, query %s, dispatch %s",
        g_ffxCreate ? "ok" : "*** NULL ***", g_ffxDestroy ? "ok" : "*** NULL ***",
        g_ffxConfigure ? "ok" : "*** NULL ***", g_ffxQuery ? "ok" : "*** NULL ***",
        g_ffxDispatch ? "ok" : "*** NULL ***");
    if (!g_ffxCreate || !g_ffxDestroy || !g_ffxConfigure || !g_ffxQuery || !g_ffxDispatch) {
        return Fail("resolving the five FFX entry points", E_NOINTERFACE);
    }
    return true;
}

// The major version a provider name carries: its first digit. PRINTED BESIDE
// EVERY NAME, because the name format is [UNKNOWN] until this runs and a
// misparse must be visible in the same line rather than inferred later.
int MajorOf(const char* s)
{
    for (; s && *s; ++s) {
        if (*s >= '0' && *s <= '9') { return *s - '0'; }
    }
    return -1;
}

void DestroyContexts()
{
    for (auto& c : g_ffx) {
        if (c) { g_ffxDestroy(&c, nullptr); c = nullptr; }
    }
}

// One context per eye. Flags common to both paths: reversed-Z (measured
// 2026-09-12), INFINITE far (the view-proj's row 2 is (0,0,0,0.05) -
// ARCHIVE_CLAUDE_2026-09-14.md:1254), and auto exposure (DLSS runs it; we have
// no exposure texture). NATIVE adds HDR (pre-tonemap R11G11B10). UPSCALE adds
// NON_LINEAR_COLORSPACE: its input is the post-tonemap sRGB-encoded image
// (super-resolution-ml.md:288-300).
// DEBUG CHECKING WAS ON for 2b through step 3 and is OFF from 2026-09-26 (Jay,
// after the step-3 runs passed): across every run it reported only "cameraNear
// value is very low" - 0.05 m (flat) / ~0.1 m (VR) IS the engine's near plane -
// and nothing about the NON_LINEAR flag or colour. The SDK recommends it for
// development builds only (super-resolution-upscaler.md, "Debug Checker").
// FfxMessage stays wired, so FFX's own errors still reach the log.
constexpr uint32_t kCommonFlags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
                                  FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
                                  FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
constexpr uint32_t kNativeFlags  = kCommonFlags | FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
constexpr uint32_t kUpscaleFlags = kCommonFlags | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
// FOVEATION (Task 5): ctxs / chains default to the stock contexts; the periphery passes its own arrays (same flags, same version).
bool CreateContexts(int eyes, UINT inW, UINT inH, UINT outW, UINT outH, uint32_t flags,
                    const uint64_t* overrideId, const char* asked, ffxContext* ctxs = g_ffx, CreateChain* chains = g_chain)
{
    for (int e = 0; e < eyes; ++e) {
        CreateChain& c = chains[e];
        memset(&c, 0, sizeof(c));
        c.up.header.type          = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        c.up.header.pNext         = &c.ver.header;
        c.up.flags                = flags;
        c.up.maxRenderSize.width   = inW;
        c.up.maxRenderSize.height  = inH;
        c.up.maxUpscaleSize.width  = outW;
        c.up.maxUpscaleSize.height = outH;
        c.up.fpMessage            = &FfxMessage;
        // Mandatory (ffx_upscale.h:221-226): the API version we were BUILT
        // against, 4.1.1 - not the provider we want.
        c.ver.header.type         = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        c.ver.header.pNext        = &c.be.header;
        c.ver.version             = FFX_UPSCALER_VERSION;
        c.be.header.type          = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        c.be.device               = g_dev12;
        if (overrideId) {
            c.be.header.pNext     = &c.ov.header;
            c.ov.header.type      = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
            c.ov.versionId        = *overrideId;
        }
        const ffxReturnCode_t rc = g_ffxCreate(&ctxs[e], &c.up.header, nullptr);
        if (rc != FFX_API_RETURN_OK || !ctxs[e]) {
            LOG("[FSR] eye %d: ffxCreateContext FAILED rc=%u (%s), asked for %s", e, rc, RcName(rc), asked);
            ctxs[e] = nullptr;
            for (int k = 0; k < kMaxEyes; ++k) { if (ctxs[k]) { g_ffxDestroy(&ctxs[k], nullptr); ctxs[k] = nullptr; } }
            return false;
        }
        ffxQueryGetProviderVersion pv = {};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        const ffxReturnCode_t qrc = g_ffxQuery(&ctxs[e], &pv.header);
        LOG("[FSR] eye %d: context CREATED %ux%u -> %ux%u (%s), flags 0x%X = %s, asked for %s -> "
            "PROVIDER id 0x%016llX \"%s\" (query rc=%u %s)",
            e, inW, inH, outW, outH, (inW == outW && inH == outH) ? "NativeAA" : "UPSCALE", flags,
            flags == kNativeFlags ? "HDR|DEPTH_INVERTED|DEPTH_INFINITE|AUTO_EXPOSURE"
                                  : "NON_LINEAR_COLORSPACE|DEPTH_INVERTED|DEPTH_INFINITE|AUTO_EXPOSURE",
            asked, pv.versionId, pv.versionName ? pv.versionName : "(null)", qrc, RcName(qrc));
        if (pv.versionName) { strncpy_s(g_provider, sizeof(g_provider), pv.versionName, _TRUNCATE); }
    }
    return true;
}

// Creates the contexts with the version chosen in Setup, applying Jay's FSR 4
// rule to the listed-then-fails shape too: 3.x, loudly.
bool CreateContextsChosen(int eyes, UINT inW, UINT inH, UINT outW, UINT outH, uint32_t flags)
{
    char asked[96] = {};
    if (g_havePick) {
        _snprintf_s(asked, sizeof(asked), _TRUNCATE, "override \"%s\"", g_pickName);
    } else {
        _snprintf_s(asked, sizeof(asked), _TRUNCATE, "NO OVERRIDE (provider default)");
    }
    bool ok = CreateContexts(eyes, inW, inH, outW, outH, flags, g_havePick ? &g_pickId : nullptr, asked);
    if (!ok && g_havePick && g_pickIs4) {
        LOG("[FSR] *** FSR 4 WAS LISTED BUT ITS CONTEXT FAILED - running 3.1 *** (%s)",
            g_have3 ? "retrying with the 3.x entry" : "and no 3.x entry exists - retrying with no override");
        g_fsr4Fallback = true;
        ok = CreateContexts(eyes, inW, inH, outW, outH, flags, g_have3 ? &g_id3 : nullptr,
                            g_have3 ? "3.x override (fallback)" : "NO OVERRIDE (fallback)");
    }
    return ok;
}

// ---- 5. the version list and the choice (once, in Setup) ----------------------
bool ChooseVersion(int wantMajor)
{
    ffxQueryDescGetVersions q = {};
    q.header.type    = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device         = g_dev12;
    uint64_t count   = 0;
    q.outputCount    = &count;
    ffxReturnCode_t rc = g_ffxQuery(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK) {
        LOG("[FSR] version COUNT query FAILED rc=%u (%s)", rc, RcName(rc));
        return false;
    }
    constexpr uint64_t kCap = 16;
    uint64_t ids[kCap] = {};
    const char* names[kCap] = {};
    uint64_t n = count > kCap ? kCap : count;
    q.outputCount  = &n;
    q.versionIds   = ids;
    q.versionNames = names;
    rc = g_ffxQuery(nullptr, &q.header);
    if (rc != FFX_API_RETURN_OK) {
        LOG("[FSR] version LIST query FAILED rc=%u (%s) after a count of %llu", rc, RcName(rc), count);
        return false;
    }
    // THE ANSWER TO PROPOSAL §4's FIRST UNKNOWN: what this GPU is offered.
    // Printed whole, every launch.
    LOG("[FSR] *** VERSION LIST for this GPU: %llu reported%s ***", count,
        count > kCap ? " (only the first 16 are read)" : "");
    for (uint64_t i = 0; i < n; ++i) {
        LOG("[FSR]   [%llu] id 0x%016llX  \"%s\"  (major parsed as %d)", i, ids[i],
            names[i] ? names[i] : "(null)", MajorOf(names[i]));
    }

    // Which upscaler DLL the loader actually mapped - read back, not assumed.
    if (HMODULE up = GetModuleHandleW(L"amd_fidelityfx_upscaler_dx12.dll")) {
        wchar_t p[MAX_PATH] = {};
        GetModuleFileNameW(up, p, MAX_PATH);
        LOG("[FSR] upscaler DLL mapped from: %ls", p);
    } else {
        LOG("[FSR] upscaler DLL: *** NOT MAPPED *** after the version query (the loader may map it "
            "lazily at context creation - see the line after the contexts)");
    }

    auto find = [&](int major) -> int {
        for (uint64_t i = 0; i < n; ++i) {
            if (MajorOf(names[i]) == major) { return static_cast<int>(i); }
        }
        return -1;
    };

    // D-127: nothing below 3.1 is ever chosen, whatever the list offers (INV-254:
    // this GPU lists 2.3.4). MajorOf >= 3 is the filter.
    int pick = find(wantMajor);
    if (pick < 0 && wantMajor == 4) {
        // Jay 2026-09-26: "Fallback yes, don't force no AA".
        LOG("[FSR] *** FSR 4 NOT AVAILABLE ON THIS GPU - running 3.1 *** (FSR 4 is not in the version "
            "list above; it needs an RX 7000 / 9000)");
        g_fsr4Fallback = true;
        pick = find(3);
    }
    const int p3 = find(3);
    g_have3 = (p3 >= 0);
    if (g_have3) { g_id3 = ids[p3]; }
    g_havePick = (pick >= 0);
    if (g_havePick) {
        g_pickId  = ids[pick];
        g_pickIs4 = (MajorOf(names[pick]) == 4);
        strncpy_s(g_pickName, sizeof(g_pickName), names[pick] ? names[pick] : "(null)", _TRUNCATE);
        LOG("[FSR] version CHOSEN: [%d] \"%s\" (id 0x%016llX) - used for every context this launch",
            pick, g_pickName, g_pickId);
    } else {
        LOG("[FSR] *** no FSR %d in the version list *** - contexts will be created WITHOUT an override; "
            "the provider the loader picks is named on the context line", wantMajor);
    }
    return true;
}

// The upscaler DLL the loader actually mapped - read back after contexts exist.
void LogUpscalerMapped()
{
    if (HMODULE up = GetModuleHandleW(L"amd_fidelityfx_upscaler_dx12.dll")) {
        wchar_t p[MAX_PATH] = {};
        GetModuleFileNameW(up, p, MAX_PATH);
        LOG("[FSR] upscaler DLL mapped (after context creation) from: %ls", p);
    } else {
        LOG("[FSR] upscaler DLL: *** STILL NOT MAPPED *** after context creation - the provider named "
            "above came from somewhere else (a driver-side provider?)");
    }
}

// Waits (bounded, CPU) until the D3D12 queue has finished everything submitted
// - required before a re-size may release textures and contexts FFX still uses.
bool WaitIdle(DWORD ms)
{
    if (!g_queue || !g_fence12 || !g_waitEvent) { return false; }
    const UINT64 v = ++g_fenceValue;
    if (FAILED(g_queue->Signal(g_fence12, v))) { return false; }
    if (g_fence12->GetCompletedValue() >= v) { return true; }
    g_fence12->SetEventOnCompletion(v, g_waitEvent);
    WaitForSingleObject(g_waitEvent, ms);
    return g_fence12->GetCompletedValue() >= v;
}

// Sizes (or re-sizes) the shared textures and contexts. The version was chosen
// in Setup; a re-size waits for the GPU, destroys, and rebuilds with the same one.
bool EnsureSized(ID3D11Texture2D* colour, ID3D11Texture2D* mv, ID3D11Texture2D* depth, UINT inW,
                 UINT inH, UINT outW, UINT outH, int eyes, bool upscale)
{
    if (g_sized > 0 && inW == g_inW && inH == g_inH && outW == g_outW && outH == g_outH &&
        eyes == g_eyes && upscale == g_upscaleMode) {
        return true;
    }
    if (g_sized != 0) {
        LOG("[FSR] *** RE-SIZE *** %ux%u->%ux%u (%d eye%s) -> %ux%u->%ux%u (%d eye%s): waiting for the GPU, "
            "then destroying and rebuilding the textures and contexts (history resets).",
            g_inW, g_inH, g_outW, g_outH, g_eyes, g_eyes == 1 ? "" : "s", inW, inH, outW, outH, eyes,
            eyes == 1 ? "" : "s");
        if (!WaitIdle(1000)) {
            LOG("[FSR] *** the D3D12 queue did not go idle in 1000 ms - NOT releasing in-use resources; FSR "
                "stops for this launch ***");
            g_sized = -1;
            return false;
        }
        DestroyContexts();
        ReleaseSharedSet();
        g_colourChecked = g_mvChecked = nullptr;
    }
    g_sized = -1;   // only the end sets 1
    g_inW = inW; g_inH = inH; g_outW = outW; g_outH = outH; g_eyes = eyes; g_upscaleMode = upscale;
    if (!CreateSharedSet(g_dev11, colour, mv, depth, inW, inH, outW, outH, eyes, upscale)) { return false; }
    if (!CreateContextsChosen(eyes, inW, inH, outW, outH, upscale ? kUpscaleFlags : kNativeFlags)) {
        return Fail("creating the FFX upscale context(s)", E_FAIL);
    }
    LogUpscalerMapped();
    for (int e = 0; e < kMaxEyes; ++e) { g_lastDispatchQpc[e].QuadPart = 0; }
    g_sized = 1;
    return true;
}

// ---- 2b helpers ---------------------------------------------------------------
// CopyResource is legal only between same-size textures of the same FORMAT
// GROUP, and D3D11 drops an illegal one SILENTLY. Each new source pointer (a
// re-latch makes new ones) is checked once, from its desc, and a mismatch
// refuses the dispatch loudly rather than resolving yesterday's frame.
bool CheckCopy(ID3D11Texture2D* src, ID3D11Texture2D* dst, const char* what, ID3D11Texture2D** checked)
{
    if (src == *checked) { return true; }
    D3D11_TEXTURE2D_DESC s = {}, d = {};
    src->GetDesc(&s);
    dst->GetDesc(&d);
    const bool ok = s.Width == d.Width && s.Height == d.Height && SameCopyGroup(s.Format, d.Format) &&
                    s.SampleDesc.Count == d.SampleDesc.Count && s.MipLevels == d.MipLevels;
    LOG("[FSR] copy source %-6s %p: %s %ux%u mips %u -> shared %s %ux%u: %s", what,
        static_cast<void*>(src), FmtName(s.Format), s.Width, s.Height, s.MipLevels, FmtName(d.Format),
        d.Width, d.Height, ok ? "LEGAL" : "*** NOT LEGAL - dispatch REFUSED ***");
    if (ok) { *checked = src; }
    return ok;
}

// The depth-plane copy pass: shader once, SRV per engine depth texture.
bool EnsureDepthCopy(ID3D11Texture2D* depth)
{
    if (g_depthCsFailed || !g_dev11) { return false; }
    if (!g_depthCs) {
        ID3DBlob* code = nullptr;
        ID3DBlob* err  = nullptr;
        const HRESULT hr = ::dlvr::shaderbin::Compile(kDepthCopyCs, strlen(kDepthCopyCs), "fsr_depthcopy", nullptr, nullptr,
                                      "main", "cs_5_0", 0, 0, &code, &err);
        if (FAILED(hr) || !code) {
            LOG("[FSR] depth-copy CS compile FAILED 0x%08X: %s", static_cast<unsigned>(hr),
                err ? static_cast<const char*>(err->GetBufferPointer()) : "(no message)");
            if (err) { err->Release(); }
            if (code) { code->Release(); }
            g_depthCsFailed = true;
            return false;
        }
        if (err) { err->Release(); }
        const HRESULT hr2 = g_dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                         nullptr, &g_depthCs);
        const size_t bytes = code->GetBufferSize();
        code->Release();
        if (FAILED(hr2) || !g_depthCs) {
            LOG("[FSR] depth-copy CreateComputeShader FAILED 0x%08X", static_cast<unsigned>(hr2));
            g_depthCsFailed = true;
            return false;
        }
        LOG("[FSR] depth-copy CS compiled (cs_5_0, %zu bytes)", bytes);
    }
    if (!g_depthCb) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const HRESULT hr = g_dev11->CreateBuffer(&bd, nullptr, &g_depthCb);
        if (FAILED(hr) || !g_depthCb) {
            LOG("[FSR] depth-copy constant buffer FAILED 0x%08X", static_cast<unsigned>(hr));
            g_depthCsFailed = true;
            return false;
        }
    }
    if (depth != g_depthSrcTex || !g_depthSrcSrv) {
        if (g_depthSrcSrv) { g_depthSrcSrv->Release(); g_depthSrcSrv = nullptr; }
        g_depthSrcTex = nullptr;
        // Same view the MV-fix pass uses: the readable plane of R32G8X24.
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        HRESULT hr = g_dev11->CreateShaderResourceView(depth, &sv, &g_depthSrcSrv);
        if (FAILED(hr)) { hr = g_dev11->CreateShaderResourceView(depth, nullptr, &g_depthSrcSrv); }
        LOG("[FSR] depth SRV for engine depth %p: %s (hr=0x%08X)", static_cast<void*>(depth),
            SUCCEEDED(hr) && g_depthSrcSrv ? "OK" : "*** FAILED ***", static_cast<unsigned>(hr));
        if (FAILED(hr) || !g_depthSrcSrv) { return false; }
        g_depthSrcTex = depth;
    }
    return true;
}

// Harvests every timestamp pair whose fence value has landed. Never waits.
void HarvestTimestamps()
{
    if (!g_qMapped || !g_qFreq) { return; }
    const UINT64 done = g_fence12->GetCompletedValue();
    for (int s = 0; s < kRing; ++s) {
        if (!g_qPending[s] || done < g_qValue[s]) { continue; }
        g_qPending[s] = false;
        const UINT64 a = g_qMapped[2 * s], b = g_qMapped[2 * s + 1];
        if (b <= a) { continue; }
        const double ms = 1000.0 * static_cast<double>(b - a) / static_cast<double>(g_qFreq);
        g_gpuSum += ms;
        if (ms > g_gpuMax) { g_gpuMax = ms; }
        ++g_gpuN;
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    return b;
}

// ---- FIXED FOVEATED FSR (2026-11-11, plan docs/superpowers/plans/2026-11-11-foveated-dlss.md Task 5) -------------------------------
// The periphery's own contexts (one per eye: each eye keeps its own history) and ONE shared set at the working size (the two eyes'
// dispatches are sequential, and each result is copied out before the next dispatch records).
ffxContext                 g_ffxP[kMaxEyes] = {};
CreateChain                g_chainP[kMaxEyes];
ID3D12Resource*            g_pColour12 = nullptr;
ID3D11Texture2D*           g_pColour11 = nullptr;
ID3D12Resource*            g_pDepth12  = nullptr;
ID3D11Texture2D*           g_pDepth11  = nullptr;
ID3D12Resource*            g_pMv12     = nullptr;
ID3D11Texture2D*           g_pMv11     = nullptr;
ID3D12Resource*            g_pOut12    = nullptr;
ID3D11Texture2D*           g_pOut11    = nullptr;
UINT                       g_pW = 0, g_pH = 0;
DXGI_FORMAT                g_pColourFmt = DXGI_FORMAT_UNKNOWN;
bool                       g_pUpscale = false;
int                        g_pSized = 0;           // 0 none, 1 ready, -1 the last attempt failed (not retried for the same config)
LARGE_INTEGER              g_lastQpcC[kMaxEyes] = {}, g_lastQpcP[kMaxEyes] = {};
LONG64                     g_fovDispatches = 0, g_fovFails = 0;

void ReleasePeriphery()
{
    for (auto& c : g_ffxP) { if (c) { g_ffxDestroy(&c, nullptr); c = nullptr; } }
    ReleasePair(&g_pColour12, &g_pColour11);
    ReleasePair(&g_pDepth12, &g_pDepth11);
    ReleasePair(&g_pMv12, &g_pMv11);
    ReleasePair(&g_pOut12, &g_pOut11);
    g_pSized = 0;
}

// The periphery set at w x h for the current mode (native = HDR, upscale = non-linear sRGB - the stock contexts' flags) and the
// downsampled colour's format family. Re-made only when one of those changes (the foveation module's 300 ms debounce bounds it).
bool EnsurePeriphery(UINT w, UINT h, DXGI_FORMAT dsColourFmt)
{
    const DXGI_FORMAT cf = (dsColourFmt == DXGI_FORMAT_R8G8B8A8_TYPELESS || dsColourFmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
                            dsColourFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM
                         : (dsColourFmt == DXGI_FORMAT_B8G8R8A8_TYPELESS || dsColourFmt == DXGI_FORMAT_B8G8R8A8_UNORM ||
                            dsColourFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? DXGI_FORMAT_B8G8R8A8_UNORM
                         : dsColourFmt;
    const bool same = w == g_pW && h == g_pH && cf == g_pColourFmt && g_upscaleMode == g_pUpscale;
    if (same && g_pSized != 0) { return g_pSized > 0; }
    if (g_pSized != 0) {
        if (!WaitIdle(1000)) {
            LOG("[FSR] [FOVDLSS] periphery re-size: the D3D12 queue did not go idle in 1000 ms - NOT releasing in-use resources");
            return false;
        }
        ReleasePeriphery();
    }
    g_pW = w; g_pH = h; g_pColourFmt = cf; g_pUpscale = g_upscaleMode;
    g_pSized = -1;
    const DXGI_FORMAT of = g_upscaleMode ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;   // as CreateSharedSet
    bool all = MakeShared(g_dev11, "fov p-colour", cf, w, h, false, &g_pColour12, &g_pColour11);
    all &= MakeShared(g_dev11, "fov p-depth", DXGI_FORMAT_R32_FLOAT, w, h, false, &g_pDepth12, &g_pDepth11);
    all &= MakeShared(g_dev11, "fov p-mv", DXGI_FORMAT_R16G16_FLOAT, w, h, false, &g_pMv12, &g_pMv11);
    all &= MakeShared(g_dev11, "fov p-out", of, w, h, true, &g_pOut12, &g_pOut11);
    if (!all) { ReleasePeriphery(); g_pSized = -1; g_pW = w; g_pH = h; return false; }
    // The stock contexts' version choice, including an FSR 4 -> 3.x fallback already taken.
    const uint64_t* id = g_fsr4Fallback ? (g_have3 ? &g_id3 : nullptr) : (g_havePick ? &g_pickId : nullptr);
    if (!CreateContexts(g_eyes, w, h, w, h, g_upscaleMode ? kUpscaleFlags : kNativeFlags, id, "the stock contexts' version "
                        "(foveation periphery)", g_ffxP, g_chainP)) {
        ReleasePeriphery(); g_pSized = -1; g_pW = w; g_pH = h;
        return false;
    }
    for (auto& q : g_lastQpcP) { q.QuadPart = 0; }
    g_pSized = 1;
    // [PROBE: FOVDLSS] gate=fov_dlss (FSR periphery, each re-size)  site=HEADSET  log-only=no
    LOG("[FSR] [FOVDLSS] periphery contexts + shared set READY: %d x %ux%u NativeAA (%s), colour %s, output %s", g_eyes, w, h,
        g_upscaleMode ? "upscale mode's flags" : "native mode's flags", FmtName(cf), FmtName(of));
    return true;
}

// One foveated FSR dispatch, the stock Dispatch's sequence (D3D11 copies -> signal + flush -> D3D12 wait -> record -> submit ->
// D3D11 wait) for a sub-rectangle / the periphery. Results are copied out of `o11` into `dst` at (0, 0) after the D3D11 wait.
struct OneSpec {
    ffxContext* fx;
    ID3D12Resource *c12, *d12, *m12, *o12;
    ID3D11Texture2D *c11, *d11, *m11, *o11;
    ID3D11Texture2D *colour, *mv, *depth;
    bool engineDepth;              // true: the engine's R32G8X24 depth through the depth-copy CS (off = box corner); false: R32 copy
    D3D11_BOX box;                 // the source region of colour / vectors / depth, copied to the shared inputs' origin
    UINT renderW, renderH, upW, upH;
    float mvScaleX, mvScaleY, jx, jy, fovScale;
    const float* viewProj;
    bool reset;
    ID3D11Texture2D* dst;
    LARGE_INTEGER* lastQpc;
    const char* what;
};

bool RunOne(ID3D11DeviceContext* ctx, int eye, const OneSpec& s)
{
    const LONG64 n = ++g_fovDispatches;
    auto failOut = [&](const char* why, unsigned code) -> bool {
        ++g_fovFails;
        if (g_fovFails <= 5 || (g_fovFails % 600) == 0) {
            // [PROBE: FOVDLSS] gate=fov_dlss (FSR, a failed foveated dispatch)  site=HEADSET  log-only=no
            LOG("[FSR] [FOVDLSS] %s dispatch #%lld eye %d *** FAILED *** at: %s (code 0x%08X). Fails so far %lld.", s.what, n, eye, why,
                code, g_fovFails);
        }
        return false;
    };
    if (!s.fx || !*s.fx || !s.colour || !s.mv || !s.depth || !s.dst) { return failOut("a null context / input / output", 0); }
    // ---- 1. D3D11: the inputs into the shared textures (each source checked once for copy legality by format group + region)
    ctx->CopySubresourceRegion(s.c11, 0, 0, 0, 0, s.colour, 0, &s.box);
    ctx->CopySubresourceRegion(s.m11, 0, 0, 0, 0, s.mv, 0, &s.box);
    if (s.engineDepth) {
        ID3D11ShaderResourceView* oldSrv = nullptr;
        ID3D11UnorderedAccessView* oldUav = nullptr;
        ID3D11Buffer* oldCb = nullptr;
        ctx->CSGetShaderResources(0, 1, &oldSrv);
        ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
        ctx->CSGetConstantBuffers(0, 1, &oldCb);
        const UINT off[4] = { s.box.left, s.box.top, 0, 0 };
        ctx->UpdateSubresource(g_depthCb, 0, nullptr, off, 0, 0);
        ctx->CSSetShader(g_depthCs, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &g_depthCb);
        ctx->CSSetShaderResources(0, 1, &g_depthSrcSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_depthUav11, nullptr);
        ctx->Dispatch((s.renderW + 7) / 8, (s.renderH + 7) / 8, 1);
        ctx->CSSetShaderResources(0, 1, &oldSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &oldCb);
        if (oldSrv) { oldSrv->Release(); }
        if (oldUav) { oldUav->Release(); }
        if (oldCb) { oldCb->Release(); }
    } else {
        ctx->CopySubresourceRegion(s.d11, 0, 0, 0, 0, s.depth, 0, &s.box);
    }
    // ---- 2. D3D11 signals; Flush; 3. D3D12 waits GPU-side
    const UINT64 vIn = ++g_fenceValue;
    HRESULT hr = g_ctx4->Signal(g_fence11, vIn);
    if (FAILED(hr)) { return failOut("ID3D11DeviceContext4::Signal", static_cast<unsigned>(hr)); }
    ctx->Flush();
    hr = g_queue->Wait(g_fence12, vIn);
    if (FAILED(hr)) { return failOut("ID3D12CommandQueue::Wait", static_cast<unsigned>(hr)); }
    // ---- 4. record (the stock ring; a slot still in flight is waited on, bounded, and counted as an allocator stall)
    const int slot = g_ring;
    g_ring = (g_ring + 1) % kRing;
    if (g_fence12->GetCompletedValue() < g_allocDone[slot]) {
        ++g_allocStalls;
        g_fence12->SetEventOnCompletion(g_allocDone[slot], g_waitEvent);
        WaitForSingleObject(g_waitEvent, 100);
        if (g_fence12->GetCompletedValue() < g_allocDone[slot]) { return failOut("command allocator still in flight after 100 ms", 0); }
    }
    if (g_qPending[slot]) { ++g_gpuNotReady; g_qPending[slot] = false; }   // its stock timestamp pair is abandoned, counted
    hr = g_alloc[slot]->Reset();
    if (FAILED(hr)) { return failOut("ID3D12CommandAllocator::Reset", static_cast<unsigned>(hr)); }
    hr = g_list->Reset(g_alloc[slot], nullptr);
    if (FAILED(hr)) { return failOut("ID3D12GraphicsCommandList::Reset", static_cast<unsigned>(hr)); }
    D3D12_RESOURCE_BARRIER in[4] = {
        Transition(s.c12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(s.d12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(s.m12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(s.o12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    g_list->ResourceBarrier(4, in);
    // Camera terms: the stock Dispatch's rule (near = row 2 .w, vFOV = 2 atan(1 / |row 1 xyz|)), its own last-good pair.
    static float s_near = 0.05f, s_fov = 1.0f;
    if (s.viewProj) {
        const float nearZ = s.viewProj[11];
        const float p11 = TrueP11(s.viewProj);
        const float fov = p11 > 1e-3f ? 2.0f * std::atan(1.0f / p11) : 0.0f;
        if (nearZ > 0.001f && nearZ < 1.0f && fov > 0.2f && fov < 3.0f) { s_near = nearZ; s_fov = fov; }
    }
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    float dtMs = 16.6f;
    if (s.lastQpc->QuadPart && g_qpcFreq.QuadPart) {
        dtMs = static_cast<float>(1000.0 * static_cast<double>(now.QuadPart - s.lastQpc->QuadPart) /
                                  static_cast<double>(g_qpcFreq.QuadPart));
        if (dtMs < 1.0f) { dtMs = 1.0f; }
        if (dtMs > 200.0f) { dtMs = 200.0f; }
    }
    *s.lastQpc = now;
    ffxDispatchDescUpscale d = {};
    d.header.type             = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList             = g_list;
    d.color                   = ffxApiGetResourceDX12(s.c12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth                   = ffxApiGetResourceDX12(s.d12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors           = ffxApiGetResourceDX12(s.m12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output                  = ffxApiGetResourceDX12(s.o12, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset.x          = s.jx;
    d.jitterOffset.y          = s.jy;
    d.motionVectorScale.x     = s.mvScaleX;
    d.motionVectorScale.y     = s.mvScaleY;
    d.renderSize.width        = s.renderW;
    d.renderSize.height       = s.renderH;
    d.upscaleSize.width       = s.upW;
    d.upscaleSize.height      = s.upH;
    d.enableSharpening        = false;
    d.sharpness               = 0.0f;
    d.frameTimeDelta          = dtMs;
    d.preExposure             = 1.0f;
    d.reset                   = s.reset;
    d.cameraNear              = s_near;
    d.cameraFar               = 1.0e8f;
    // The rows FSR is given: the frame's vertical FOV scaled in tangent by their share (the lens crop's rule; FSR's use not read).
    d.cameraFovAngleVertical  = 2.0f * std::atan(std::tan(0.5f * s_fov) * s.fovScale);
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags                   = g_upscaleMode ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB : 0;
    const ffxReturnCode_t rc = g_ffxDispatch(s.fx, &d.header);
    D3D12_RESOURCE_BARRIER outB[4];
    for (int i = 0; i < 4; ++i) {
        outB[i] = in[i];
        outB[i].Transition.StateBefore = in[i].Transition.StateAfter;
        outB[i].Transition.StateAfter  = in[i].Transition.StateBefore;
    }
    g_list->ResourceBarrier(4, outB);
    hr = g_list->Close();
    if (FAILED(hr)) { return failOut("ID3D12GraphicsCommandList::Close", static_cast<unsigned>(hr)); }
    // ---- 5. submit, signal; 6. D3D11 waits GPU-side, then copies the result out
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    const UINT64 vOut = ++g_fenceValue;
    g_queue->Signal(g_fence12, vOut);
    g_allocDone[slot] = vOut;
    hr = g_ctx4->Wait(g_fence11, vOut);
    if (FAILED(hr)) { return failOut("ID3D11DeviceContext4::Wait", static_cast<unsigned>(hr)); }
    if (rc != FFX_API_RETURN_OK) { return failOut(RcName(rc), rc); }
    const D3D11_BOX ob = { 0, 0, 0, s.upW, s.upH, 1 };
    ctx->CopySubresourceRegion(s.dst, 0, 0, 0, 0, s.o11, 0, &ob);
    if (n <= 2 || (n % 1200) == 0) {
        // [PROBE: FOVDLSS] gate=fov_dlss (FSR, first two then every 1200)  site=HEADSET  log-only=no
        LOG("[FSR] [FOVDLSS] %s dispatch #%lld eye %d rc OK | render %ux%u (source box %u,%u) -> %ux%u | MV scale (%.0f, %.0f) | jitter "
            "(%.4f, %.4f) | vFOV %.2f deg (x%.3f) | reset %d | fails %lld | allocator stalls %lld (all FSR)", s.what, n, eye, s.renderW,
            s.renderH, s.box.left, s.box.top, s.upW, s.upH, s.mvScaleX, s.mvScaleY, s.jx, s.jy,
            57.29578f * d.cameraFovAngleVertical, s.fovScale, s.reset ? 1 : 0, g_fovFails, g_allocStalls);
    }
    return true;
}

// The copy-out's legality, once per destination: same format group, and the destination at least the region's size.
bool CheckOut(ID3D11Texture2D* src, ID3D11Texture2D* dst, UINT w, UINT h, const char* what)
{
    static ID3D11Texture2D* s_ok[8] = {};
    for (auto* p : s_ok) { if (p == dst) { return true; } }
    D3D11_TEXTURE2D_DESC a = {}, b = {};
    src->GetDesc(&a);
    dst->GetDesc(&b);
    const bool ok = SameCopyGroup(a.Format, b.Format) && b.Width >= w && b.Height >= h && a.Width >= w && a.Height >= h;
    // [PROBE: FOVDLSS] gate=fov_dlss (FSR, once per destination)  site=HEADSET  log-only=no
    LOG("[FSR] [FOVDLSS] %s copy-out: shared %s %ux%u -> private %s %ux%u, region %ux%u: %s", what, FmtName(a.Format), a.Width,
        a.Height, FmtName(b.Format), b.Width, b.Height, w, h, ok ? "LEGAL" : "*** NOT LEGAL - foveated dispatch REFUSED ***");
    if (ok) {
        static int s_next = 0;
        s_ok[s_next] = dst;
        s_next = (s_next + 1) % 8;
    }
    return ok;
}

}  // namespace

ID3D11Texture2D* OutputTex(int eye)
{
    return (g_sized > 0 && eye >= 0 && eye < g_eyes) ? g_out11[eye] : nullptr;
}

ID3D11ShaderResourceView* OutputSrv(int eye)
{
    return (g_sized > 0 && eye >= 0 && eye < g_eyes) ? g_outSrv11[eye] : nullptr;
}

const char* ProviderName(int* fsr4Fallback)
{
    if (fsr4Fallback) { *fsr4Fallback = g_fsr4Fallback ? 1 : 0; }
    return g_provider;
}

bool EnsureUpscale(UINT inW, UINT inH, UINT outW, UINT outH, int eyes)
{
    if (g_result <= 0 || !inW || !inH || !outW || !outH || eyes < 1 || eyes > kMaxEyes) { return false; }
    if (g_sized < 0 && inW == g_inW && inH == g_inH && outW == g_outW && outH == g_outH) {
        return false;   // this exact size already failed - do not retry it every frame
    }
    const bool had = (g_sized > 0);
    const bool ok = EnsureSized(nullptr, nullptr, nullptr, inW, inH, outW, outH, eyes, true);
    if (ok && !had) {
        LOG("[FSR] *** FSR UPSCALING SIZED *** %ux%u -> %ux%u (%.3fx linear), %d context%s, post-tonemap "
            "sRGB input (NON_LINEAR), substituted at the engine's own upscale copy - the HUD is drawn after, "
            "at full resolution.", inW, inH, outW, outH, outW / static_cast<double>(inW), eyes,
            eyes == 1 ? "" : "s");
    }
    return ok;
}

bool Dispatch(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour, ID3D11Texture2D* mv,
              ID3D11Texture2D* depth, float jitterX, float jitterY, const float* viewProj, bool reset,
              ID3D11Texture2D* copyOutTo, const ::dlvr::lenscrop::Rect* inRc, const ::dlvr::lenscrop::Rect* outRc)
{
    if (g_sized <= 0 || !ctx || eye < 0 || eye >= g_eyes || !g_ffx[eye]) { return false; }
    // LENS CROP: a crop must fit the sized images, and NATIVE needs its scratch texture; otherwise the whole image (said once).
    bool crop = inRc && outRc && inRc->w == g_inW && outRc->w == g_outW && inRc->h > 0 && outRc->h > 0 &&
                inRc->y + inRc->h <= g_inH && outRc->y + outRc->h <= g_outH;
    ID3D12Resource* scratch = (crop && !g_upscaleMode) ? ScratchFor(eye) : nullptr;
    if (crop && !g_upscaleMode && !scratch) { crop = false; }
    if (inRc && !crop) {
        static LONG s_whole = 0;
        if (InterlockedIncrement(&s_whole) <= 3) {
            // [PROBE: LENSCROP] gate=dlss_lens_crop (FSR, a refused crop)  site=HEADSET  log-only=no
            LOG("[LENSCROP] FSR eye %d: crop asked for but NOT applied (rows outside the %ux%u / %ux%u images, or no NATIVE scratch) - "
                "the whole image is reconstructed", eye, g_inW, g_inH, g_outW, g_outH);
        }
    }
    const UINT inY = crop ? inRc->y : 0, inH = crop ? inRc->h : g_inH;
    const UINT outY = crop ? outRc->y : 0, outH = crop ? outRc->h : g_outH;
    if (g_upscaleMode != (copyOutTo != nullptr)) { return false; }   // a caller/mode mismatch never dispatches
    LARGE_INTEGER c0 = {}, c1 = {};
    QueryPerformanceCounter(&c0);
    const LONG64 n = ++g_dispatches;   // render thread only
    auto failOut = [&](const char* why, unsigned code) -> bool {
        ++g_dispatchFails;
        if (g_dispatchFails <= 5 || (g_dispatchFails % 600) == 0) {
            LOG("[FSR] dispatch #%lld eye %d *** FAILED *** at: %s (code 0x%08X). Fails so far %lld.", n, eye,
                why, code, g_dispatchFails);
        }
        return false;
    };
    if (!colour || !mv || !depth) { return failOut("a null engine input", 0); }
    if (!CheckCopy(colour, g_colour11, "colour", &g_colourChecked)) { return failOut("colour copy legality", 0); }
    if (!CheckCopy(mv, g_mv11, "mv", &g_mvChecked)) { return failOut("mv copy legality", 0); }
    if (!EnsureDepthCopy(depth)) { return failOut("the depth-copy pass", 0); }

    // ---- 1. D3D11: this frame's inputs into the shared textures -------------
    // LENS CROP: only the crop's rows, placed at the origin (FSR reads from (0,0)); uncropped = the whole texture as before.
    if (crop) {
        const D3D11_BOX box = { 0, inY, 0, g_inW, inY + inH, 1 };
        ctx->CopySubresourceRegion(g_colour11, 0, 0, 0, 0, colour, 0, &box);
        ctx->CopySubresourceRegion(g_mv11, 0, 0, 0, 0, mv, 0, &box);
    } else {
        ctx->CopyResource(g_colour11, colour);
        ctx->CopyResource(g_mv11, mv);
    }
    // CS slot 0 SRV/UAV/CB are saved and put back: StateBackup restores the CS
    // SHADER but not its views, and the engine's must not be left nulled.
    ID3D11ShaderResourceView*  oldSrv = nullptr;
    ID3D11UnorderedAccessView* oldUav = nullptr;
    ID3D11Buffer*              oldCb  = nullptr;
    ctx->CSGetShaderResources(0, 1, &oldSrv);
    ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
    ctx->CSGetConstantBuffers(0, 1, &oldCb);
    const UINT off[4] = { 0, inY, 0, 0 };
    ctx->UpdateSubresource(g_depthCb, 0, nullptr, off, 0, 0);
    ctx->CSSetShader(g_depthCs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_depthCb);
    ctx->CSSetShaderResources(0, 1, &g_depthSrcSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_depthUav11, nullptr);
    ctx->Dispatch((g_inW + 7) / 8, (inH + 7) / 8, 1);
    ctx->CSSetShaderResources(0, 1, &oldSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &oldCb);
    if (oldSrv) { oldSrv->Release(); }
    if (oldUav) { oldUav->Release(); }
    if (oldCb)  { oldCb->Release(); }

    // ---- 2. D3D11 signals; Flush, because D3D12 cannot wait on unsubmitted work
    const UINT64 vIn = ++g_fenceValue;
    HRESULT hr = g_ctx4->Signal(g_fence11, vIn);
    if (FAILED(hr)) { return failOut("ID3D11DeviceContext4::Signal", static_cast<unsigned>(hr)); }
    ctx->Flush();

    // ---- 3. D3D12 waits GPU-side - the CPU does not block here ---------------
    hr = g_queue->Wait(g_fence12, vIn);
    if (FAILED(hr)) { return failOut("ID3D12CommandQueue::Wait", static_cast<unsigned>(hr)); }

    // ---- 4. record: allocator free? (bounded CPU wait, counted) --------------
    const int s = g_ring;
    g_ring = (g_ring + 1) % kRing;
    if (g_fence12->GetCompletedValue() < g_allocDone[s]) {
        ++g_allocStalls;
        g_fence12->SetEventOnCompletion(g_allocDone[s], g_waitEvent);
        WaitForSingleObject(g_waitEvent, 100);
        if (g_fence12->GetCompletedValue() < g_allocDone[s]) {
            return failOut("command allocator still in flight after 100 ms", 0);
        }
    }
    HarvestTimestamps();
    if (g_qPending[s]) { ++g_gpuNotReady; g_qPending[s] = false; }   // slot reused before its result landed
    hr = g_alloc[s]->Reset();
    if (FAILED(hr)) { return failOut("ID3D12CommandAllocator::Reset", static_cast<unsigned>(hr)); }
    hr = g_list->Reset(g_alloc[s], nullptr);
    if (FAILED(hr)) { return failOut("ID3D12GraphicsCommandList::Reset", static_cast<unsigned>(hr)); }
    if (g_qMapped) { g_list->EndQuery(g_qHeap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s); }

    // Shared resources arrive in COMMON and must be back in COMMON for D3D11.
    D3D12_RESOURCE_BARRIER in[4] = {
        Transition(g_colour12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(g_depth12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(g_mv12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        // LENS CROP, NATIVE: FSR writes the scratch (kept in UNORDERED_ACCESS); g_out12 only receives the copy.
        Transition(g_out12[eye], D3D12_RESOURCE_STATE_COMMON,
                   scratch ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    g_list->ResourceBarrier(4, in);

    // Camera terms from THIS eye's latched view-projection (row-major, VS CB
    // slot 2): row 2 = (0,0,0,near) (reversed-Z infinite, ARCHIVE:1254), and
    // |row 1 xyz| = P[1][1] (view rows are orthonormal), so vFOV = 2 atan(1/P11).
    // Out-of-band values fall back to the last good pair and are counted.
    static float s_near = 0.05f, s_fov = 1.0f;
    static bool  s_haveCam = false;
    bool camOk = false;
    if (viewProj) {
        const float nearZ = viewProj[11];
        const float p11 = TrueP11(viewProj);
        const float fov = p11 > 1e-3f ? 2.0f * std::atan(1.0f / p11) : 0.0f;
        if (nearZ > 0.001f && nearZ < 1.0f && fov > 0.2f && fov < 3.0f) {
            s_near = nearZ; s_fov = fov; s_haveCam = true; camOk = true;
        }
    }
    if (!camOk) { ++g_viewProjFallbacks; }

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    float dtMs = 16.6f;
    if (g_lastDispatchQpc[eye].QuadPart && g_qpcFreq.QuadPart) {
        dtMs = static_cast<float>(1000.0 * static_cast<double>(now.QuadPart - g_lastDispatchQpc[eye].QuadPart) /
                                  static_cast<double>(g_qpcFreq.QuadPart));
        if (dtMs < 1.0f) { dtMs = 1.0f; }
        if (dtMs > 200.0f) { dtMs = 200.0f; }
    }
    g_lastDispatchQpc[eye] = now;
    if (reset) { ++g_resets; }

    // Far: unused by the infinite-far depth math as far as the flag implies, but
    // the debug checker warns when it is "very low" (SDK doc :343) - so large.
    constexpr float kFar = 1.0e8f;
    ffxDispatchDescUpscale d = {};
    d.header.type             = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList             = g_list;
    d.color                   = ffxApiGetResourceDX12(g_colour12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth                   = ffxApiGetResourceDX12(g_depth12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors           = ffxApiGetResourceDX12(g_mv12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output                  = ffxApiGetResourceDX12(scratch ? scratch : g_out12[eye], FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset.x          = jitterX;
    d.jitterOffset.y          = jitterY;
    // Our MVs are UV, previous = current - MV; FSR wants pixels pointing to the
    // previous position - the same (-W, -H) the DLSS path passes.
    // MVs are at the RENDER size (the scene), so the scale is the render size in
    // both modes - the same InMVScale the DLSS upscale path passes.
    d.motionVectorScale.x     = -static_cast<float>(g_inW);
    d.motionVectorScale.y     = -static_cast<float>(g_inH);
    d.renderSize.width        = g_inW;
    d.renderSize.height       = inH;    // LENS CROP: the crop's rows (g_inH uncropped)
    d.upscaleSize.width       = g_outW;
    d.upscaleSize.height      = outH;   // "optional, assumed maxUpscaleSize otherwise" (ffx_upscale.h:101): a crop may be smaller
    d.enableSharpening        = false;   // NIS already sharpens - one sharpener, not two
    d.sharpness               = 0.0f;
    d.frameTimeDelta          = dtMs;
    d.preExposure             = 1.0f;
    d.reset                   = reset;
    d.cameraNear              = s_near;
    d.cameraFar               = kFar;
    // LENS CROP: the FOV of the rows FSR is given. ffx_upscale.h:109 says only "the camera angle field of view in the vertical
    // direction"; the crop is symmetric to a row (Task 1's BK12 band 0.0581..0.9419), so the crop's FOV is the frame's scaled in
    // tangent by inH / g_inH. Claude's call - FSR's use of it was not read.
    d.cameraFovAngleVertical  = crop ? 2.0f * std::atan(std::tan(0.5f * s_fov) * static_cast<float>(inH) / static_cast<float>(g_inH))
                                     : s_fov;
    d.viewSpaceToMetersFactor = 1.0f;    // engine units are metres
    // The upscale input is perceptual sRGB (super-resolution-ml.md:299).
    d.flags                   = g_upscaleMode ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB : 0;
    const ffxReturnCode_t rc = g_ffxDispatch(&g_ffx[eye], &d.header);

    // LENS CROP, NATIVE: the scratch's rows (written from the origin) into g_out12[eye] at the crop's offset.
    if (scratch) {
        D3D12_RESOURCE_BARRIER toSrc = Transition(scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g_list->ResourceBarrier(1, &toSrc);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = g_out12[eye]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
        src.pResource = scratch;      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        const D3D12_BOX box = { 0, 0, 0, g_outW, outH, 1 };
        g_list->CopyTextureRegion(&dst, 0, outY, 0, &src, &box);
        D3D12_RESOURCE_BARRIER back = Transition(scratch, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_list->ResourceBarrier(1, &back);
    }

    D3D12_RESOURCE_BARRIER outB[4];
    for (int i = 0; i < 4; ++i) {
        outB[i] = in[i];
        outB[i].Transition.StateBefore = in[i].Transition.StateAfter;
        outB[i].Transition.StateAfter  = in[i].Transition.StateBefore;
    }
    g_list->ResourceBarrier(4, outB);
    if (g_qMapped) {
        g_list->EndQuery(g_qHeap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s + 1);
        g_list->ResolveQueryData(g_qHeap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s, 2, g_qReadback,
                                 sizeof(UINT64) * 2 * s);
    }
    hr = g_list->Close();
    if (FAILED(hr)) { return failOut("ID3D12GraphicsCommandList::Close", static_cast<unsigned>(hr)); }

    // ---- 5. submit, signal; 6. D3D11 waits GPU-side before the tonemap reads --
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    const UINT64 vOut = ++g_fenceValue;
    g_queue->Signal(g_fence12, vOut);
    g_allocDone[s] = vOut;
    if (g_qMapped) { g_qValue[s] = vOut; g_qPending[s] = true; }
    hr = g_ctx4->Wait(g_fence11, vOut);

    QueryPerformanceCounter(&c1);
    const double cpuMs = 1000.0 * static_cast<double>(c1.QuadPart - c0.QuadPart) /
                         static_cast<double>(g_qpcFreq.QuadPart ? g_qpcFreq.QuadPart : 1);
    g_cpuSum += cpuMs;
    if (cpuMs > g_cpuMax) { g_cpuMax = cpuMs; }
    ++g_cpuN;

    if (FAILED(hr)) { return failOut("ID3D11DeviceContext4::Wait", static_cast<unsigned>(hr)); }
    if (rc != FFX_API_RETURN_OK) { return failOut(RcName(rc), rc); }

    // UPSCALE: the result into the caller's TYPELESS twin, whose _UNORM_SRGB view
    // the engine samples and whose _UNORM view NIS reads - the view pair the DLSS
    // upscale path measured colour-correct (INV-13). Queued after the D3D11 Wait
    // above, so it reads FSR's finished output. Legality checked once per target.
    if (copyOutTo) {
        static ID3D11Texture2D* s_dstChecked[kMaxEyes] = {};
        if (s_dstChecked[eye] != copyOutTo) {
            D3D11_TEXTURE2D_DESC s = {}, t = {};
            g_out11[eye]->GetDesc(&s);
            copyOutTo->GetDesc(&t);
            const bool ok = s.Width == t.Width && s.Height == t.Height && SameCopyGroup(s.Format, t.Format);
            LOG("[FSR] output copy eye %d: shared %s %ux%u -> target %s %ux%u: %s", eye, FmtName(s.Format),
                s.Width, s.Height, FmtName(t.Format), t.Width, t.Height,
                ok ? "LEGAL" : "*** NOT LEGAL - substitution REFUSED ***");
            if (!ok) { return failOut("the output copy's legality", 0); }
            s_dstChecked[eye] = copyOutTo;
        }
        if (crop) {
            // LENS CROP, UPSCALE: FSR wrote rows 0..outH of the shared output; they belong at the crop's offset. The rows outside are
            // filled by the caller (dlssflat lenscrop::Fill).
            const D3D11_BOX box = { 0, 0, 0, g_outW, outH, 1 };
            ctx->CopySubresourceRegion(copyOutTo, 0, 0, outY, 0, g_out11[eye], 0, &box);
        } else {
            ctx->CopyResource(copyOutTo, g_out11[eye]);
        }
    }

    // Heartbeat: the first dispatch, then every 600. Every quantity the 2b
    // passing values name is on this one line.
    if (n == 1 || (n % 600) == 0) {
        char gpu[96] = {};
        if (g_gpuN > 0) {
            _snprintf_s(gpu, sizeof(gpu), _TRUNCATE, "avg %.3f max %.3f ms over %d (not ready %d)",
                        g_gpuSum / g_gpuN, g_gpuMax, g_gpuN, g_gpuNotReady);
        } else {
            _snprintf_s(gpu, sizeof(gpu), _TRUNCATE, "NO DATA yet (samples 0, not ready %d)", g_gpuNotReady);
        }
        LOG("[FSR] %s%s%sdispatch #%lld eye %d rc OK | %s %ux%u -> %ux%u | fails %lld | resets %lld | "
            "allocator stalls %lld | fence "
            "%llu | GPU FSR dispatch %s | CPU copies+sync+record avg %.3f max %.3f ms over %d | jitter px "
            "(%.4f, %.4f) | MV scale (%.0f, %.0f) | near %.4f m, vFOV %.2f deg from %s (fallbacks %lld) | "
            "far %.0e DEPTH_INFINITE | dt %.2f ms",
            n == 1 ? "*** FSR " : "", n == 1 ? g_provider : "", n == 1 ? " LIVE *** first " : "",
            n, eye, g_upscaleMode ? "UPSCALE" : "NATIVE", g_inW, g_inH, g_outW, g_outH,
            g_dispatchFails, g_resets, g_allocStalls,
            vOut, gpu, g_cpuSum / g_cpuN, g_cpuMax, g_cpuN, jitterX, jitterY, d.motionVectorScale.x,
            d.motionVectorScale.y, s_near, s_fov * 57.29578f,
            camOk ? "this eye's view-proj" : (s_haveCam ? "*** LAST GOOD view-proj ***"
                                                        : "*** DEFAULTS - no view-proj yet ***"),
            g_viewProjFallbacks, static_cast<double>(kFar), dtMs);
        g_gpuSum = g_gpuMax = 0; g_gpuN = 0; g_gpuNotReady = 0;
        g_cpuSum = g_cpuMax = 0; g_cpuN = 0;
    }
    return true;
}

bool DispatchCentre(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour, ID3D11Texture2D* mv, ID3D11Texture2D* depth,
                    float jitterX, float jitterY, const float* viewProj, bool reset, const ::dlvr::lenscrop::Rect& inRc,
                    UINT outW, UINT outH, ID3D11Texture2D* dst)
{
    if (g_sized <= 0 || !ctx || eye < 0 || eye >= g_eyes || !g_ffx[eye] || !colour || !mv || !depth || !dst) { return false; }
    // The context was created at g_in -> g_out (its MAX sizes): a sub-rectangle must fit inside both.
    if (inRc.w == 0 || inRc.h == 0 || inRc.x + inRc.w > g_inW || inRc.y + inRc.h > g_inH || outW == 0 || outH == 0 ||
        outW > g_outW || outH > g_outH) {
        static LONG s_bad = 0;
        if (InterlockedIncrement(&s_bad) <= 3) {
            // [PROBE: FOVDLSS] gate=fov_dlss (FSR centre, a refused rectangle)  site=HEADSET  log-only=no
            LOG("[FSR] [FOVDLSS] centre REFUSED: render %u,%u %ux%u -> %ux%u outside the context's %ux%u -> %ux%u", inRc.x, inRc.y,
                inRc.w, inRc.h, outW, outH, g_inW, g_inH, g_outW, g_outH);
        }
        return false;
    }
    if (!CheckCopy(colour, g_colour11, "colour", &g_colourChecked)) { return false; }
    if (!CheckCopy(mv, g_mv11, "mv", &g_mvChecked)) { return false; }
    if (!EnsureDepthCopy(depth)) { return false; }
    if (!CheckOut(g_out11[eye], dst, outW, outH, "centre")) { return false; }
    OneSpec s = {};
    s.fx = &g_ffx[eye];
    s.c12 = g_colour12; s.d12 = g_depth12; s.m12 = g_mv12; s.o12 = g_out12[eye];
    s.c11 = g_colour11; s.d11 = g_depth11; s.m11 = g_mv11; s.o11 = g_out11[eye];
    s.colour = colour; s.mv = mv; s.depth = depth; s.engineDepth = true;
    s.box = D3D11_BOX{ inRc.x, inRc.y, 0, inRc.x + inRc.w, inRc.y + inRc.h, 1 };
    s.renderW = inRc.w; s.renderH = inRc.h; s.upW = outW; s.upH = outH;
    // The vectors are UV over the WHOLE render image: the stock scale (render size) keeps them in render pixels.
    s.mvScaleX = -static_cast<float>(g_inW); s.mvScaleY = -static_cast<float>(g_inH);
    s.jx = jitterX; s.jy = jitterY;
    s.fovScale = static_cast<float>(inRc.h) / static_cast<float>(g_inH);
    s.viewProj = viewProj; s.reset = reset; s.dst = dst; s.lastQpc = &g_lastQpcC[eye]; s.what = "centre";
    return RunOne(ctx, eye, s);
}

bool DispatchPeriphery(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour, ID3D11Texture2D* mv, ID3D11Texture2D* depth,
                       UINT w, UINT h, float jitterX, float jitterY, const float* viewProj, bool reset, ID3D11Texture2D* dst)
{
    if (g_sized <= 0 || !ctx || eye < 0 || eye >= g_eyes || !colour || !mv || !depth || !dst || !w || !h) { return false; }
    D3D11_TEXTURE2D_DESC cd = {};
    colour->GetDesc(&cd);
    if (!EnsurePeriphery(w, h, cd.Format) || !g_ffxP[eye]) { return false; }
    if (!CheckOut(g_pOut11, dst, w, h, "periphery")) { return false; }
    OneSpec s = {};
    s.fx = &g_ffxP[eye];
    s.c12 = g_pColour12; s.d12 = g_pDepth12; s.m12 = g_pMv12; s.o12 = g_pOut12;
    s.c11 = g_pColour11; s.d11 = g_pDepth11; s.m11 = g_pMv11; s.o11 = g_pOut11;
    s.colour = colour; s.mv = mv; s.depth = depth; s.engineDepth = false;
    s.box = D3D11_BOX{ 0, 0, 0, w, h, 1 };
    s.renderW = w; s.renderH = h; s.upW = w; s.upH = h;
    s.mvScaleX = -static_cast<float>(w); s.mvScaleY = -static_cast<float>(h);   // the downsampled vectors keep their UV values
    s.jx = jitterX; s.jy = jitterY;
    s.fovScale = 1.0f;
    s.viewProj = viewProj; s.reset = reset; s.dst = dst; s.lastQpc = &g_lastQpcP[eye]; s.what = "periphery";
    return RunOne(ctx, eye, s);
}

bool Setup(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* colour,
           ID3D11Texture2D* mv, ID3D11Texture2D* depth, UINT w, UINT h, int eyes, int wantMajor,
           bool nativeContexts)
{
    if (g_result != 0) {
        if (g_result > 0 && nativeContexts) {
            // NATIVE re-entry (ResolutionChanged tore the outputs down): a size
            // change is refused as before - NATIVE resizing is not built.
            if (g_sized > 0 && (w != g_inW || h != g_inH || eyes != g_eyes)) {
                LOG("[FSR] *** SIZE CHANGED %ux%u (%d eye%s) -> %ux%u (%d eye%s) *** - NATIVE resizing is not "
                    "built; FSR stops for this launch (vanilla image).",
                    g_inW, g_inH, g_eyes, g_eyes == 1 ? "" : "s", w, h, eyes, eyes == 1 ? "" : "s");
                g_result = -1;
            }
            return g_result > 0 && g_sized > 0;
        }
        return g_result > 0;
    }
    g_result = -1;   // every early return below is a failure; only the end sets 1

    if (!dev || !ctx || (nativeContexts && (!w || !h || eyes < 1 || eyes > kMaxEyes))) {
        LOG("[FSR] setup *** REFUSED *** - bad arguments: device %p context %p size %ux%u eyes %d",
            static_cast<void*>(dev), static_cast<void*>(ctx), w, h, eyes);
        return false;
    }
    g_dev11 = dev;
    LOG("[FSR] SETUP START - FSR %d asked for; %s", wantMajor,
        nativeContexts ? "NATIVE: textures + contexts at the render size now"
                       : "UPSCALE mode: core only now, textures + contexts at the first upscale draw");
    LARGE_INTEGER qf = {}, t0 = {}, t1 = {};
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&t0);

    // ONLY THE DEVICE IS FATAL - every later step needs it. v1 stopped at the
    // first failure and so never reached the version list, which depends on
    // nothing but the device (20:49 run). Each remaining part runs and reports.
    if (!CreateDevice12(dev)) { return false; }
    const bool fenceOk = CreateFenceAndRoundTrip(dev, ctx);
    const bool ffxOk   = LoadFfx() && ChooseVersion(wantMajor);
    bool sizedOk = true;
    if (nativeContexts && ffxOk) {
        sizedOk = EnsureSized(colour, mv, depth, w, h, w, h, eyes, false);
    }

    QueryPerformanceCounter(&t1);
    const bool all = fenceOk && ffxOk && sizedOk;
    g_result = (fenceOk && ffxOk) ? 1 : -1;
    LOG("[FSR] *** SETUP %s *** in %.1f ms: D3D12 device OK | fence round trip %s | FFX loader + version "
        "choice %s | %s.",
        all ? "COMPLETE" : "INCOMPLETE",
        1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
            static_cast<double>(qf.QuadPart ? qf.QuadPart : 1),
        fenceOk ? "OK" : "*** FAILED ***", ffxOk ? "OK" : "*** FAILED ***",
        !nativeContexts ? "textures + contexts deferred to the first upscale draw"
        : sizedOk       ? "NATIVE textures + views + contexts OK"
                        : "*** NATIVE textures/contexts FAILED ***");
    return all;
}

}  // namespace fsr
}  // namespace dlvr
