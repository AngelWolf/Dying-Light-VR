// coopnet.cpp - the co-op arm-pose channel's EOS transport ([COOPNET]). See coopnet.h; plan Task 3; trace
// docs/traces/20261113_1900_coopnet_transport.md; option layouts = spec section 5b (OUR engine's own calls, EOS SDK 1.15.5).
//
// How it stays out of the game's way (spec 5a item 5, read in OUR engine e9fbfc2c): every game EOS receive passes RequestedChannel =
// its own channel byte (+72AAD0 / +6BE410) and every game connection notify is registered with its OWN socket id (+72A680 /
// +6BD810). We use our own socket name "DLVRPose" on a channel no game send ever used, so our packets and requests never reach the
// game's code. The game's two EOS calls we hook are only OBSERVED (local user, peers, channels) and passed through unchanged.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../common/iathook.h"
#include "../common/log.h"
#include "coopnet.h"
#include "periodic_log.h"   // LOG_PERIODIC - the 5 s line, behind diag_periodic_logs

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

namespace dlvr {

// [DEP: coop_pose] required-for=the co-op arm-pose channel (CoopPose): our own EOS P2P packets carrying this player's head /
//      controller / finger inputs to every other player running the mod, and (plan Task 6) their bodies posed from theirs. Spec
//      2026-11-13-coop-arm-pose-channel-design. OFF = no IAT hook installed, nothing sent or received. Tier C, unverified in co-op.
//      BETA compiled default ON (Jay 2026-11-13: "Build a beta package with it on aswell" - a co-op TEST package; trace
//      docs/traces/20261113_2130_coop_pose_beta_default_on.md). DEV stays OFF. tree=EXPERIMENT.
bool CoopPose = true;

namespace {

constexpr const char* kEosDll     = "EOSSDK-Win64-Shipping.dll";
constexpr const char* kEngineDll  = "engine_x64_rwdi.dll";
constexpr const char* kOurSocket  = "DLVRPose";
constexpr const char* kGameSocket = "DyingLightMultiplayer";   // engine +699C90 (cp2) - a CEosSocket name (NOT the co-op traffic, INV-734)
constexpr const char* kQosSocket  = "DyingLightQoS";           // engine +68C680 (cp2) - QoS probes: never a peer source
constexpr int         kMaxSockNames = 8;                       // distinct game socket names logged once each
constexpr int         kMaxPeers   = 3;                          // 4-player co-op
constexpr int         kMaxRecvPerTick = 16;

// ---- EOS SDK 1.15.5 option structs, as OUR engine fills them (spec 5b) ----
struct SocketId {   // EOS_P2P_SocketId v1
    int32_t ApiVersion;
    char    SocketName[33];
};
struct SendOpts {   // EOS_P2P_SendPacketOptions v3 (engine +72A990)
    int32_t         ApiVersion;
    void*           LocalUserId;
    void*           RemoteUserId;
    const SocketId* Socket;
    uint8_t         Channel;
    uint32_t        DataLengthBytes;
    const void*     Data;
    int32_t         bAllowDelayedDelivery;
    int32_t         Reliability;          // 0 = unreliable unordered
    int32_t         bDisableAutoAcceptConnection;
};
static_assert(offsetof(SendOpts, LocalUserId) == 8 && offsetof(SendOpts, Socket) == 24 && offsetof(SendOpts, Channel) == 32 &&
              offsetof(SendOpts, DataLengthBytes) == 36 && offsetof(SendOpts, Data) == 40 &&
              offsetof(SendOpts, bAllowDelayedDelivery) == 48 && offsetof(SendOpts, bDisableAutoAcceptConnection) == 56,
              "EOS_P2P_SendPacketOptions v3 layout (spec 5b)");
struct RecvOpts {   // EOS_P2P_ReceivePacketOptions v2 (engine +72AAD0)
    int32_t        ApiVersion;
    void*          LocalUserId;
    uint32_t       MaxDataSizeBytes;
    const uint8_t* RequestedChannel;
};
static_assert(offsetof(RecvOpts, MaxDataSizeBytes) == 16 && offsetof(RecvOpts, RequestedChannel) == 24, "ReceivePacketOptions v2");
struct NotifyOpts {   // EOS_P2P_AddNotifyPeerConnectionRequestOptions v1 (engine +72A680)
    int32_t         ApiVersion;
    void*           LocalUserId;
    const SocketId* Socket;
};
struct AcceptOpts {   // EOS_P2P_AcceptConnectionOptions v1 (engine +72AC60 fills it from the request info)
    int32_t         ApiVersion;
    void*           LocalUserId;
    void*           RemoteUserId;
    const SocketId* Socket;
};
struct RequestInfo {   // EOS_P2P_OnIncomingConnectionRequestInfo (engine reads +8 / +16 / +24)
    void*           ClientData;
    void*           LocalUserId;
    void*           RemoteUserId;
    const SocketId* Socket;
};

using TickFn      = void (*)(void*);
using GetP2PFn    = void* (*)(void*);
using SendFn      = int (*)(void*, const SendOpts*);
using RecvFn      = int (*)(void*, const RecvOpts*, void**, SocketId*, uint8_t*, void*, uint32_t*);
using ReqCb       = void (*)(const RequestInfo*);
using AddNotifyFn = uint64_t (*)(void*, const NotifyOpts*, void*, ReqCb);
using AcceptFn    = int (*)(void*, const AcceptOpts*);

TickFn      g_trueTick = nullptr;     // the engine's IAT entries we replaced
SendFn      g_trueSend = nullptr;
GetP2PFn    g_getP2P = nullptr;       // the real exports, for OUR calls
SendFn      g_send = nullptr;
RecvFn      g_recv = nullptr;
AddNotifyFn g_addNotify = nullptr;
AcceptFn    g_accept = nullptr;

volatile LONG g_installed = 0, g_saidRefused = 0;
void* volatile g_localUser = nullptr;
void* volatile g_p2p = nullptr;
uint64_t       g_notifyId = 0;
SocketId       g_ourSock = {};

struct Peer {
    void*              id = nullptr;
    coop::JitterBuffer jb;
    SRWLOCK            lock = SRWLOCK_INIT;
    LONG64             sent = 0, recv = 0, rejected = 0;
    bool               saidMismatch = false;
    bool               saidBadValues = false;   // B5: Unpack refused this peer's values (named once)
};
Peer          g_peers[kMaxPeers];
volatile LONG g_peerN = 0;
SRWLOCK       g_peerLock = SRWLOCK_INIT;

uint64_t      g_gameCh[4] = {};       // every channel seen on a game send (bitset)
volatile LONG g_ourCh = -1;

SRWLOCK           g_qLock = SRWLOCK_INIT;
coop::PoseSample  g_q = {};
bool              g_qHave = false;
uint32_t          g_qSentSeq = 0;

LONG64     g_ticks = 0, g_ticksWin = 0;
ULONGLONG  g_winStartMs = 0;
volatile LONG g_saidFirstTick = 0;

double NowMs()
{
    static LARGE_INTEGER f = {};
    if (f.QuadPart == 0) { QueryPerformanceFrequency(&f); }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

bool SockIs(const SocketId* s, const char* name)
{
    __try { return s && strncmp(s->SocketName, name, 32) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A peer's slot (added when new, up to kMaxPeers). -1 = full / null. `via` = the socket it was learned from (for the log).
int PeerSlot(void* id, const char* via)
{
    if (!id) { return -1; }
    AcquireSRWLockExclusive(&g_peerLock);
    int found = -1;
    for (int i = 0; i < g_peerN; ++i) { if (g_peers[i].id == id) { found = i; break; } }
    if (found < 0 && g_peerN < kMaxPeers) {
        found = g_peerN;
        g_peers[found].id = id;
        g_peers[found].jb.SetParams(500.0f, 150.0f, 0.0f);   // spec 4.3 defaults (settings arrive with the receiver, plan Task 6)
        InterlockedIncrement(&g_peerN);
        // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
        LOG("coopnet: [COOPNET] PEER #%d %p (learned from socket '%.32s')", found, id, via ? via : "?");
    }
    ReleaseSRWLockExclusive(&g_peerLock);
    return found;
}

void ChooseChannel()
{
    for (int ch = 255; ch >= 1; --ch) {
        if ((g_gameCh[ch >> 6] >> (ch & 63) & 1ull) == 0) {
            InterlockedExchange(&g_ourCh, ch);
            // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
            LOG("coopnet: [COOPNET] OUR CHANNEL %d (the highest no game send has used)", ch);
            return;
        }
    }
}

// Each distinct socket name the game sends on, logged once (INV-734: the co-op traffic is CSessionSocketEos, named at join from the
// session's string - so the name is not known in advance). Game network thread only.
char g_sockNames[kMaxSockNames][33] = {};
int  g_sockNameN = 0;
void NoteSocketName(const char* name, int ch, bool peerSource)
{
    for (int i = 0; i < g_sockNameN; ++i) { if (strncmp(g_sockNames[i], name, 32) == 0) { return; } }
    if (g_sockNameN >= kMaxSockNames) { return; }
    strncpy_s(g_sockNames[g_sockNameN], sizeof(g_sockNames[0]), name, 32);
    ++g_sockNameN;
    // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
    LOG("coopnet: [COOPNET] GAME SOCKET #%d '%.32s' first send on channel %d - %s", g_sockNameN, name, ch,
        peerSource ? "a peer source (its remote users are co-op players)" : "NOT a peer source (QoS / ours)");
}

int SendHook(void* p2p, const SendOpts* o)
{
    __try {
        if (o) {
            if (o->LocalUserId) { g_localUser = o->LocalUserId; }
            const int ch = o->Channel;
            g_gameCh[ch >> 6] |= 1ull << (ch & 63);
            if (ch == g_ourCh) {
                // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
                LOG("coopnet: [COOPNET] the game sent on OUR channel %d - moving to another", ch);
                InterlockedExchange(&g_ourCh, -1);
            }
            // After RUN CO1 (trace 20261113_2035): ANY game socket but QoS (and ours) carries co-op players - the session socket's
            // name is the session's own string (INV-734), so no fixed name can be matched.
            if (o->Socket) {
                char nm[33] = {};
                strncpy_s(nm, sizeof(nm), o->Socket->SocketName, 32);
                const bool src = !SockIs(o->Socket, kQosSocket) && !SockIs(o->Socket, kOurSocket);
                NoteSocketName(nm, ch, src);
                if (src) { PeerSlot(o->RemoteUserId, nm); }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return g_trueSend(p2p, o);
}

void OnRequest(const RequestInfo* info)
{
    __try {
        if (!info || !SockIs(info->Socket, kOurSocket) || !g_p2p) { return; }
        AcceptOpts a{ 1, info->LocalUserId, info->RemoteUserId, info->Socket };
        const int r = g_accept(g_p2p, &a);
        // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
        LOG("coopnet: [COOPNET] ACCEPT %p on '%s' -> result %d", info->RemoteUserId, kOurSocket, r);
        PeerSlot(info->RemoteUserId, kOurSocket);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Refuse(const char* why)
{
    if (InterlockedExchange(&g_saidRefused, 1) == 0) {
        // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
        LOG("coopnet: [COOPNET] NOT SENDING YET - %s (logged once; the 5 s line keeps counting)", why);
    }
}

void OurTick(void* platform)
{
    ++g_ticks;
    ++g_ticksWin;
    if (InterlockedExchange(&g_saidFirstTick, 1) == 0) {
        // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
        LOG("coopnet: [COOPNET] FIRST EOS TICK (logged unconditionally) - thread %lu, platform %p, our build %08X",
            GetCurrentThreadId(), platform, CoopNetBuildId());
    }
    const ULONGLONG nowTick = GetTickCount64();
    if (g_winStartMs == 0) { g_winStartMs = nowTick; }
    if (nowTick - g_winStartMs >= 5000) {
        LOG_PERIODIC("coopnet: [COOPNET] 5 s - EOS ticks %.1f/s, peers %ld, our channel %ld, local user %p; per peer sent / recv / rejected: "
                     "%lld/%lld/%lld %lld/%lld/%lld %lld/%lld/%lld",
                     g_ticksWin * 1000.0 / static_cast<double>(nowTick - g_winStartMs), g_peerN, g_ourCh, g_localUser,
                     g_peers[0].sent, g_peers[0].recv, g_peers[0].rejected, g_peers[1].sent, g_peers[1].recv, g_peers[1].rejected,
                     g_peers[2].sent, g_peers[2].recv, g_peers[2].rejected);
        g_winStartMs = nowTick;
        g_ticksWin = 0;
    }
    void* p2p = g_getP2P(platform);
    if (!p2p) { Refuse("EOS_Platform_GetP2PInterface returned null"); return; }
    g_p2p = p2p;
    void* const local = g_localUser;
    if (!local) { Refuse("no local EOS user seen yet (no game EOS send so far - not in a co-op session)"); return; }
    if (g_ourCh < 0) { ChooseChannel(); }
    const LONG ch = g_ourCh;
    if (ch < 0) { Refuse("every channel 1..255 is used by the game"); return; }
    if (!g_notifyId) {
        NotifyOpts n{ 1, local, &g_ourSock };
        g_notifyId = g_addNotify(p2p, &n, nullptr, &OnRequest);
        // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
        LOG("coopnet: [COOPNET] connection-request notify on '%s' -> id %llu (0 = refused by EOS)", kOurSocket,
            static_cast<unsigned long long>(g_notifyId));
    }
    // send the newest local sample once
    coop::PoseSample s{};
    bool have = false;
    AcquireSRWLockExclusive(&g_qLock);
    if (g_qHave && g_q.seq != g_qSentSeq) { s = g_q; have = true; g_qSentSeq = g_q.seq; }
    ReleaseSRWLockExclusive(&g_qLock);
    if (have) {
        uint8_t buf[coop::kMaxPacket];
        const size_t n = coop::Pack(s, buf, sizeof(buf));
        const LONG peers = g_peerN;
        for (int i = 0; i < peers && n; ++i) {
            SendOpts o{ 3, local, g_peers[i].id, &g_ourSock, static_cast<uint8_t>(ch), static_cast<uint32_t>(n), buf, 1, 0, 0 };
            if (g_send(p2p, &o) == 0) { InterlockedIncrement64(&g_peers[i].sent); }
        }
    }
    // drain ours
    const uint8_t want = static_cast<uint8_t>(ch);
    for (int k = 0; k < kMaxRecvPerTick; ++k) {
        RecvOpts r{ 2, local, static_cast<uint32_t>(coop::kMaxPacket), &want };
        void* from = nullptr;
        SocketId sock = {};
        uint8_t gotCh = 0;
        uint8_t buf[coop::kMaxPacket];
        uint32_t got = 0;
        if (g_recv(p2p, &r, &from, &sock, &gotCh, buf, &got) != 0) { break; }
        if (!SockIs(&sock, kOurSocket)) { continue; }
        const int slot = PeerSlot(from, kOurSocket);
        if (slot < 0) { continue; }
        Peer& p = g_peers[slot];
        coop::PoseSample in{};
        const char* why = nullptr;
        const bool unpacked = coop::Unpack(buf, got, &in, &why);
        // B5: a packet of OUR protocol whose values fail the checks - named apart from the build mismatch, once per peer
        if (!unpacked && got >= 1 && buf[0] == coop::kCoopProto && got >= coop::detail::kPacketSize) {
            InterlockedIncrement64(&p.rejected);
            if (!p.saidBadValues) {
                p.saidBadValues = true;
                // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
                LOG("coopnet: [COOPNET] PEER #%d %p REJECTED - %s (the sample is dropped, nothing reaches the partner's arms; once - "
                    "the running count is 'rejected' on the 5 s line)", slot, from, why ? why : "?");
            }
            continue;
        }
        if (!unpacked || !p.jb.Accept(buf[0], in.build, CoopNetBuildId())) {
            InterlockedIncrement64(&p.rejected);
            if (!p.saidMismatch) {
                p.saidMismatch = true;
                // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
                LOG("coopnet: [COOPNET] PEER #%d %p REJECTED - protocol %u build %08X, ours %u / %08X (another mod build = no data; once)",
                    slot, from, got ? static_cast<unsigned>(buf[0]) : 0u, in.build, static_cast<unsigned>(coop::kCoopProto),
                    CoopNetBuildId());
            }
            continue;
        }
        AcquireSRWLockExclusive(&p.lock);
        p.jb.Push(in, NowMs());
        ReleaseSRWLockExclusive(&p.lock);
        InterlockedIncrement64(&p.recv);
    }
}

void TickHook(void* platform)
{
    g_trueTick(platform);   // the game's tick first, unchanged
    if (!CoopPose) { return; }
    __try { OurTick(platform); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Refuse("a fault inside our EOS tick work (caught)"); }
}

}  // namespace

uint32_t CoopNetBuildId()
{
    static uint32_t id = 0;
    if (id) { return id; }
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&CoopNetBuildId), &self) && self) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
        id = nt->FileHeader.TimeDateStamp;
    }
    return id;
}

// [LOADBEARING: coop_pose transport] the only path our pose packets travel (spec 4.2). COST: OFF = one bool test per rig tick.
bool CoopNetStart()
{
    if (!CoopPose) { return false; }
    if (g_installed) { return true; }
    HMODULE eng = GetModuleHandleA(kEngineDll);
    HMODULE eos = GetModuleHandleA(kEosDll);
    if (!eng || !eos) { return false; }   // not loaded yet - tried again next tick
    InterlockedExchange(&g_installed, 1);
    g_getP2P    = reinterpret_cast<GetP2PFn>(GetProcAddress(eos, "EOS_Platform_GetP2PInterface"));
    g_send      = reinterpret_cast<SendFn>(GetProcAddress(eos, "EOS_P2P_SendPacket"));
    g_recv      = reinterpret_cast<RecvFn>(GetProcAddress(eos, "EOS_P2P_ReceivePacket"));
    g_addNotify = reinterpret_cast<AddNotifyFn>(GetProcAddress(eos, "EOS_P2P_AddNotifyPeerConnectionRequest"));
    g_accept    = reinterpret_cast<AcceptFn>(GetProcAddress(eos, "EOS_P2P_AcceptConnection"));
    if (!g_getP2P || !g_send || !g_recv || !g_addNotify || !g_accept) {
        // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
        LOG("coopnet: [COOPNET] REFUSED - an EOS export is missing (GetP2PInterface %p SendPacket %p ReceivePacket %p AddNotify %p "
            "Accept %p); nothing hooked", g_getP2P, g_send, g_recv, g_addNotify, g_accept);
        return false;
    }
    g_ourSock.ApiVersion = 1;
    strncpy_s(g_ourSock.SocketName, sizeof(g_ourSock.SocketName), kOurSocket, _TRUNCATE);
    // THE LAUNCH CRASH FIX (2026-11-15, Jay "Do both together"; trace 20261115_1400_b5_coop_unpack_and_tick_hook; crash
    // 20261115_105924974 = EXECUTE at 0 from TickHook): PatchIat writes the slot and only THEN returns the original, so an engine tick
    // on another thread in between called a null g_trueTick. The originals are now stored FIRST, from the EOS DLL's own exports (what
    // the import slots hold), and the slots are patched after; PatchIat's return still wins if it differs. COST: one GetProcAddress,
    // once per launch.
    const TickFn tickExport = reinterpret_cast<TickFn>(GetProcAddress(eos, "EOS_Platform_Tick"));
    if (!tickExport) {
        // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
        LOG("coopnet: [COOPNET] REFUSED - %s does not export EOS_Platform_Tick; nothing hooked", kEosDll);
        return false;
    }
    g_trueSend = g_send;
    g_trueTick = tickExport;
    MemoryBarrier();   // both originals are visible before either slot points at our hook
    void* prevSend = PatchIat(eng, kEosDll, "EOS_P2P_SendPacket", reinterpret_cast<void*>(&SendHook));
    if (!prevSend) {
        LOG("coopnet: [COOPNET] REFUSED - the engine's IAT has no EOS_P2P_SendPacket import from %s; nothing hooked", kEosDll);
        return false;
    }
    if (prevSend != reinterpret_cast<void*>(g_trueSend)) { g_trueSend = reinterpret_cast<SendFn>(prevSend); }   // changed in between
    void* prevTick = PatchIat(eng, kEosDll, "EOS_Platform_Tick", reinterpret_cast<void*>(&TickHook));
    if (prevTick && prevTick != reinterpret_cast<void*>(g_trueTick)) { g_trueTick = reinterpret_cast<TickFn>(prevTick); }
    if (!prevTick) {
        PatchIat(eng, kEosDll, "EOS_P2P_SendPacket", reinterpret_cast<void*>(g_trueSend));   // undo the first
        LOG("coopnet: [COOPNET] REFUSED - the engine's IAT has no EOS_Platform_Tick import from %s; the send hook was undone", kEosDll);
        return false;
    }
    // [PROBE: COOPNET] gate=coop_pose  site=FLAT+HEADSET  log-only=yes
    LOG("coopnet: [COOPNET] INSTALLED - engine IAT EOS_Platform_Tick + EOS_P2P_SendPacket hooked (observe + pass through); our socket "
        "'%s', build %08X, protocol %u", kOurSocket, CoopNetBuildId(), static_cast<unsigned>(coop::kCoopProto));
    return true;
}

void CoopNetQueueSend(const coop::PoseSample& s)
{
    AcquireSRWLockExclusive(&g_qLock);
    g_q = s;
    g_qHave = true;
    ReleaseSRWLockExclusive(&g_qLock);
}

int CoopNetPeers(uint64_t* puidOut, int cap)
{
    const int n = g_peerN;
    for (int i = 0; i < n && i < cap && puidOut; ++i) { puidOut[i] = reinterpret_cast<uint64_t>(g_peers[i].id); }
    return n;
}

double CoopNetNowMs() { return NowMs(); }

bool CoopNetLatest(int peer, double nowMs, coop::PoseSample* out, float* weight)
{
    if (peer < 0 || peer >= g_peerN) { return false; }
    Peer& p = g_peers[peer];
    AcquireSRWLockExclusive(&p.lock);
    const bool ok = p.jb.Sample(nowMs, out, weight);
    ReleaseSRWLockExclusive(&p.lock);
    return ok;
}

}  // namespace dlvr
