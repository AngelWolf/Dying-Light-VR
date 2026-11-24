// promptquad.cpp - see promptquad.h. Trace docs/traces/20261123_1356_inworld_prompt_quad.md. Facts (OUR engine e9fbfc2c / gamedll
// 97046bba, set-aside == deployed): INV-767 (our materials pack, ids, office_ceiling_lamp_b.mat + its cb0), INV-768 (the images, the
// locale, the tools), STATE (91) LAYER-CODE READS.
//
// WHAT IT DOES:
//   - PromptQuadOnSlot (HUD): after the game's own HudTriggerSlot::Update, for the X slot only (slot+256 == 0): the slot's visibility
//     (IUIElement::IsVisible), the text key the game just gave its action text element slot+152 (IUIText::GetText = the narrow key
//     SetText stored at [elem+24]+984, ida/tx1 + tx2), and the trigger's world box (IControlObject::GetExtentsInWorld(trigger+24),
//     INV-402). Published under a seqlock. No vfunc 179 / 180 call of ours (memory game-query-call-can-apply-gameplay).
//   - PromptQuadTick (game): once - the exports, the IGame*, IGame::LoadPack("Data/dlvr_prompts_PC.rpack"), the locale
//     (IGame::GetLocaleID). Per level - one ModelObject by the game's own recipe (+623F40): CreateObject(level+16, CRTTI
//     "ModelObject" (name-checked at +152), 0, 0) -> model = obj - 40; SetMeshName("cleansing_door_b.msh"); ClearFlag(model+24, 40);
//     SetFlag(model+24, 4); InitObject(model+40, level+16); SetWorldXform; ActivateOnModule(model+40). Its id is kept and the object
//     is only ever touched after IGSObject::FindObjectByID(level+16, id) hands back the same pointer - a freed object (level reload)
//     is never dereferenced. Per prompt - ReplaceMaterial("*" -> dlvr_prompt_<lang>_<key>[_grip|_x].mat), at least 2 s after the
//     pack loaded (its registration timing is not read); SetWorldXform each tick (a 0.36 x 0.09 m quad 0.10 m above the box,
//     facing the head about the vertical); EnableRendering on a change.
// THE QUAD (common_meshes_PC.rpack cleansing_door_b, read 2026-11-23): Y 0 -> 2.35 (origin at the BOTTOM edge), Z +0.588 (u 0.005)
//   -> -0.580 (u 0.998), normal +X; v 0 = the top. Local X -> towards the head, Y -> up, Z -> f x up: seen from the head, u runs
//   left to right.
// THREADS: the HUD slot updates hop worker threads (memory never-key-on-thread-id): one global seqlocked record, a writer lock (a
//   busy writer drops its publish). The tick runs on the game thread only.
#include "promptquad.h"

#include "camhook.h"   // CamHookGetRenderedViewMatrix - the head position (translation of the rendered view)
#include "../common/wristleaf.h"   // wristleaf::kPrompt - the routed prompt piece's index (DlvrWristPieceInfo)

#include <windows.h>
#include <intrin.h>   // _ReturnAddress - the prompt pin (trace 20261123_2000)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)

// The Epic (EGL) build: both fixed rvas are on tools/egl_allowlist.tsv (the engine IGame* global - camhook / gunsound's entry; the
// gamedll "ModelObject" CRTTI - added for this, trace 20261123_1356). The DEV tree has no buildrva.h (and no promptquad).
#if __has_include("../common/buildrva.h")
#include "../common/buildrva.h"
#define DLVR_PQ_GD_RVA(rva) ::dlvr::buildrva::Gd(rva)
#define DLVR_PQ_EN_RVA(rva) ::dlvr::buildrva::Eng(rva)
#else
#define DLVR_PQ_GD_RVA(rva) (rva)
#define DLVR_PQ_EN_RVA(rva) (rva)
#endif

namespace dlvr {

// [DEP: inworld_prompts] required-for=the in-world prompt quad (InworldPrompts) - trace 20261123_1356. Compiled ON since 2026-11-24
//      (Jay: "compiled default for world prompts is to be on", trace 20261124_1035; was OFF, Jay 2026-11-23). The bootstrap reads the
//      same line at launch with the same default (dlssflat.cpp Init: key absent = on). tree=INERT at 0: PromptQuadOnSlot returns at its first test; PromptQuadTick hides
//      a shown quad once and returns (the object stays in its level, invisible; nothing is created or loaded while off).
//      HEADSET: needs looting's SlotUpdateHook installed (loot_by_hand / the grip glyph install), the two data files deployed
//      (DW/Data/optimized_DLC29_dx11.mp, DW/Data/dlvr_prompts_PC.rpack) and the rendered view matrix (camhook).
bool InworldPrompts = true;
// [DEP: ui_prompt_brightness] required-for=the in-world prompt's brightness (InworldPromptBrightness) - the bootstrap's copy multiplies
//      the game's widget by it before the panel's material adds it x ~12 (INV-767 addendum). Tier D (Jay tunes in the headset; trace
//      20261123_1700). LIVE: read every submit. tree=INERT at inworld_prompts=0.
float InworldPromptBrightness = 2.6985f;   // Jay's disk after RUN IP12 ("I'd like those to be the defaults", trace 20261124_0100)
// [DEP: ui_prompt_size] required-for=the in-world prompt's size (InworldPromptSize): 1.0 = a 653-UI-px-wide prompt (one line) at 0.15 m;
//      the panel grows, the copy stays 1:1 (trace 20261123_1700). Tier D. LIVE: read every tick. tree=INERT at inworld_prompts=0.
float InworldPromptSize = 1.6f;   // Jay's disk after RUN IP12 (trace 20261124_0100)
// [DEP: ui_prompt_plate] required-for=the in-world prompt's dark plate (InworldPromptPlate): 1.0 = the game's own darkening behind its
//      prompt; the bootstrap writes the plate texture from the widget's alpha x this / 12 (trace 20261123_1830). Tier D. LIVE: read
//      every submit. tree=INERT at inworld_prompts=0.
float InworldPromptPlate = 0.7f;   // Jay's disk after RUN IP12 (trace 20261124_0100)

namespace {

struct NStr { const char* p; uint32_t len; uint32_t cap; };   // ttl::string_base<char> (ptr, len, cap - looting.cpp NarrowStr)

// ---- offsets (all read on OUR binaries; see the trace) ----
constexpr size_t    kSlotTypeOff = 256;        // HudTriggerSlot: slot type, 0 = the X slot (looting.cpp kSlotTypeOff)
constexpr size_t    kSlotActionTextOff = 152;  // HudTriggerSlot: the action text element (ctn9 :689-697; looting.cpp)
constexpr size_t    kCtlOff = 24;              // object -> IControlObject (INV-402; lidhand kCtlObjOff)
constexpr size_t    kGsoOff = 40;              // object -> IGSObject (the game's recipe: model + 40)
constexpr size_t    kLevelGsoOff = 16;         // ILevel* -> the IGSObject the game parents its spawns to ((char*)Level + 16, +721950)
constexpr size_t    kRttiNameOff = 152;        // CRTTI: the class name copy (CRTTI::CRTTI +40A200, ida/rt1_out.txt)
constexpr uintptr_t kModelObjRttiRva = 0x1CC50C0;   // gamedll CRTTI "ModelObject" (ctor +5FCF0; ida/sp2) - EGL row 0x1CC4010
constexpr uintptr_t kGameRva = 0xA3F3B8;            // engine IGame* global (gunsound / camhook, EGL family entry)
constexpr int       kFlagClear = 40;           // the game's recipe (+623F40): ClearFlag(40), SetFlag(4) - meaning NOT read
constexpr int       kFlagSet = 4;
const char* const   kMesh = "cleansing_door_b.msh";
// LoadPack's path = fs::full_path(<bare name>), exactly as every game caller builds it (gamedll +43E370 :360 fs::full_path(v160,
// "weapons_pc.rpack") -> LoadPack; ida/ss11_gamedll_out.txt). RUN IP1: "Data/dlvr_prompts_PC.rpack" passed straight in = FALSE
// (LoadPack's first test is fs::exists, ida/lp2_out.txt; weapons_PC.rpack itself sits in DW/Data, no DW/Data/Data).
const char* const   kPackName = "dlvr_prompts_PC.rpack";

// ---- the quad (Tier D, compiled - Jay's eye on the first run) ----
constexpr float kQuadW = 0.36f, kQuadH = 0.09f;      // metres (the wide 512 x 128 images, 4 : 1)
constexpr float kLiftM = 0.10f;                       // the quad's centre this far above the top of the object's box
constexpr float kMeshW = 1.168f, kMeshH = 2.35f;      // cleansing_door_b's own size
constexpr ULONGLONG kStaleMs = 250;                   // a slot record older than this = no prompt
constexpr ULONGLONG kPackSettleMs = 2000;             // the first material swap waits this long after a true LoadPack

// ---- the 14 keys (Jay 2026-11-23: the game's own words; icons per kind; "Use" both, by object) ----
enum Icon { kGrip = 0, kX = 1, kByObject = 2 };
struct KeyRow { const char* key; const char* stem; Icon icon; };
const KeyRow kKeys[] = {
    { "TriggerOpen", "triggeropen", kGrip },           { "TriggerClose", "triggerclose", kGrip },
    { "TriggerTake", "triggertake", kGrip },           { "TriggerSearch", "triggersearch", kGrip },
    { "TriggerTurnOn", "triggerturnon", kGrip },       { "TriggerTurnOff", "triggerturnoff", kGrip },
    { "TriggerTurnOnOff", "triggerturnonoff", kGrip }, { "TriggerLadder", "triggerladder", kGrip },
    { "TriggerLockpicking", "triggerlockpicking", kX }, { "TriggerLocked", "triggerlocked", kX },
    { "TriggerTalk", "triggertalk", kX },              { "TriggerTrade", "triggertrade", kX },
    { "TriggerBreakDoor", "triggerbreakdoor", kX },    { "TriggerUse", "triggeruse", kByObject },
};
constexpr int kNumKeys = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));
// "Use" objects whose icon is the GRIP, by RTTI class name - EMPTY until Jay assigns them from the log's USE CLASS lines.
const char* const kUseGripClasses[] = { nullptr };
// the 17 language paks (IGame::GetLocaleID codes; "Cz" has no pak -> en)
const char* const kLangs[] = { "br", "cn", "cs", "de", "el", "en", "es", "fr", "it", "jp", "ko", "nl", "pl", "ru", "th", "tr", "tw" };

// ---- engine exports (by name) ----
using IsVisibleFn = bool(__fastcall*)(const void* uiElem);
using GetTextFn = const NStr*(__fastcall*)(void* uiText);
using GetExtentsFn = const float*(__fastcall*)(void* co);
using GetLevelFn = void*(__fastcall*)(const void* gso);
using CreateObjectFn = void*(__fastcall*)(void* parentGso, const void* crtti, bool b, const NStr* name);
using SetMeshNameFn = void(__fastcall*)(void* model, const NStr* name);
using FlagFn = void(__fastcall*)(void* co, int flag);
using InitObjectFn = void(__fastcall*)(void* gso, void* parentGso);
using SetXformFn = void(__fastcall*)(void* co, const float* m);
using GsoFn = void(__fastcall*)(void* gso);
using ReplaceMaterialFn = bool(__fastcall*)(void* model, const NStr* oldName, const NStr* newName);
using EnableRenderingFn = void(__fastcall*)(void* model, bool on);
using GetIdFn = int(__fastcall*)(const void* gso);
using FindByIdFn = void*(__fastcall*)(const void* gso, int id);
using LoadPackFn = bool(__fastcall*)(void* game, const NStr* path, bool a3, bool a4, void** handle);
using GetLocaleFn = NStr*(__fastcall*)(void* game, NStr* out);
using FullPathFn = NStr*(__fastcall*)(NStr* out, const char* name);   // filesystem ?full_path@fs@@YA?BV?$string_base@D@ttl@@PEBD@Z +143D0

IsVisibleFn       g_isVisible = nullptr;
GetTextFn         g_getText = nullptr;
GetExtentsFn      g_getExtents = nullptr;
GetLevelFn        g_getLevel = nullptr;
CreateObjectFn    g_create = nullptr;
SetMeshNameFn     g_setMesh = nullptr;
FlagFn            g_clearFlag = nullptr;
FlagFn            g_setFlag = nullptr;
InitObjectFn      g_init = nullptr;
SetXformFn        g_setXform = nullptr;
GsoFn             g_activate = nullptr;
ReplaceMaterialFn g_replace = nullptr;
EnableRenderingFn g_enable = nullptr;
GetIdFn           g_getId = nullptr;
FindByIdFn        g_findById = nullptr;
LoadPackFn        g_loadPack = nullptr;
GetLocaleFn       g_getLocale = nullptr;
FullPathFn        g_fullPath = nullptr;
volatile LONG     g_slotReady = 0;   // 0 not tried, 1 ready, -1 refused (the HUD-side exports)

// ---- published by PromptQuadOnSlot (HUD) -> PromptQuadTick (game) ----
volatile LONG     g_wLock = 0;       // writer lock: a busy writer drops its publish
volatile LONG     g_wSeq = 0;        // odd = writing
void*             g_wTrig = nullptr;
int               g_wKey = -1;       // index into kKeys, -1 = a key not in the table
int               g_wVis = 0;
float             g_wTop[3] = {};    // top centre of the trigger's world box
float             g_wHalf[2] = {};   // its XZ half sizes (trace 20261123_2200)
float             g_wBottom = 0.0f;  // its min y (trace 20261123_2330)
ULONGLONG         g_wMs = 0;
char              g_wKeyText[48] = {};   // the raw key (for the not-in-table log)
// read on the HUD thread INSIDE the game's slot update (the trigger is held by HudTrigger there) - the tick never reads the trigger
void*             g_wLevel = nullptr;    // IGSObject::GetLevel(trigger + 40)
uintptr_t         g_wVt = 0;             // the trigger's primary vtable (the "Use" class dedupe)
char              g_wClass[64] = {};     // its RTTI class name ("Use" icon by class)

// ---- game-thread state ----
volatile LONG     g_tickReady = 0;   // 0 not tried, 1 ready, -1 refused
const void*       g_crtti = nullptr;
void*             g_game = nullptr;
bool              g_packOk = false;
bool              g_packTried = false;
ULONGLONG         g_packMs = 0;
char              g_lang[4] = "en";
void*             g_level = nullptr;     // the level our object lives in
void*             g_obj = nullptr;       // the IGSObject (CreateObject's return); model = obj - 40
int               g_objId = 0;
bool              g_objFailed = false;   // a creation / id check failed on this level - not retried until the level changes
bool              g_shown = false;
char              g_curMat[96] = {};     // the material we last swapped in ("" = the stock one)
// ROUTE B (trace 20261123_1535): ONE material, whose texture (dlvr_prompt_live, pack fmt 2, 768 x 192, the marker) the bootstrap's
// CreateTexture2D hook makes writable and fills with the game's own prompt widget each eye-0 HUD pass.
const char* const kLiveMat = "dlvr_prompt_live.mat";
// THE PLATE (trace 20261123_1830): a SECOND object of the same mesh, 3 mm behind the text quad, on dlvr_prompt_plate.mat (our template:
// blend ZERO / INV_SRC_COLOR = the scene x (1 - texel x 12)); its texture (dlvr_prompt_plate, marker DLVRPROMPTPLATE1) the bootstrap
// fills with the widget's alpha x ui_prompt_plate / 12. A plate failure never hides the prompt: the text shows without it.
const char* const kPlateMat = "dlvr_prompt_plate.mat";
constexpr float   kPlateBackM = 0.003f;
constexpr float   kPullM = 0.15f;   // trace 20261123_2300: the panel pulled this far from the box centre toward the head (max)
void*             g_plateObj = nullptr;
int               g_plateId = 0;
bool              g_plateFailed = false;  // created / swap refused on this level - not retried until the level changes
bool              g_plateSwapped = false;
bool              g_plateShown = false;   // enabled (shown) now; Hide clears it
volatile LONG     g_nPlate = 0;
volatile LONG     g_routeWant = 0;      // 1 = the quad is showing: wristprobe sets the prompt's routeMask bit (any thread reads)
using LiveReadyFn = int (*)();
volatile LONG     g_liveReady = 0;       // the bootstrap's DlvrPromptLiveReady() == 1, read by the tick (game thread)
LiveReadyFn       g_liveReadyFn = nullptr;
bool              g_saidLive = false;
volatile LONG     g_nShow = 0, g_nSwap = 0, g_nSwapFault = 0, g_nCreate = 0, g_nNotInTable = 0, g_nSlot = 0;
// "Use" classes already logged (pointer to the vtable is enough to dedupe)
uintptr_t         g_useVt[32] = {};
int               g_useVtN = 0;

// ---- SEH-guarded game access (no C++ objects in these frames) ----
bool ReadPtr(uintptr_t a, uintptr_t* out)
{
    __try { *out = *reinterpret_cast<const uintptr_t*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeVisible(void* slot, bool* out)
{
    __try { *out = g_isVisible(slot); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// copies the narrow key (without '&') into out; false when unreadable
bool SafeKey(void* elem, char* out, size_t n)
{
    __try {
        const NStr* s = g_getText(elem);
        if (!s || !s->p) { out[0] = 0; return true; }
        size_t j = 0;
        for (uint32_t i = 0; i < s->len && j + 1 < n; ++i) {
            const char c = s->p[i];
            if (c == 0) { break; }
            if (c != '&') { out[j++] = c; }
        }
        out[j] = 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeTop(void* trig, float out[3], float half[2], float* bottom)
{
    __try {
        const float* e = g_getExtents(static_cast<char*>(trig) + kCtlOff);   // [min xyz, max xyz] (looting_logic BoxDist)
        if (!e) { return false; }
        out[0] = 0.5f * (e[0] + e[3]);
        out[1] = e[4];
        out[2] = 0.5f * (e[2] + e[5]);
        half[0] = 0.5f * (e[3] - e[0]);   // trace 20261123_2200: the XZ half sizes (the near-face placement)
        half[1] = 0.5f * (e[5] - e[2]);
        *bottom = e[1];                   // trace 20261123_2330: the box's min y (the eye-height clamp)
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]) && std::isfinite(half[0]) &&
               std::isfinite(half[1]) && half[0] >= 0.0f && half[1] >= 0.0f && std::isfinite(*bottom) && *bottom <= out[1];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeLevelOf(void* trig, void** out)
{
    __try { *out = g_getLevel(static_cast<char*>(trig) + kGsoOff); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// the MSVC RTTI class name of an object's primary vtable (".?AVName@@" -> "Name"); false when not readable
bool SafeClassName(void* obj, char* out, size_t n, uintptr_t* vtOut)
{
    __try {
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        const uintptr_t col = *reinterpret_cast<const uintptr_t*>(vt - 8);
        if (*reinterpret_cast<const uint32_t*>(col) != 1) { return false; }   // x64 COL signature
        const uint32_t td = *reinterpret_cast<const uint32_t*>(col + 12);
        const uint32_t self = *reinterpret_cast<const uint32_t*>(col + 20);
        const char* nm = reinterpret_cast<const char*>(col - self + td + 16);
        size_t j = 0;
        const char* p = (nm[0] == '.' && nm[1] == '?' && nm[2] == 'A') ? nm + 4 : nm;
        while (p[j] && p[j] != '@' && j + 1 < n) { out[j] = p[j]; ++j; }
        out[j] = 0;
        *vtOut = vt;
        return j > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeNameIs(const void* crtti, const char* want)
{
    __try {
        const char* nm = *reinterpret_cast<const char* const*>(static_cast<const char*>(crtti) + kRttiNameOff);
        return nm && std::strcmp(nm, want) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeLoadPack(void* game, const NStr* path, bool* ok)
{
    __try { *ok = g_loadPack(game, path, false, false, nullptr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// fs::full_path(name) into out (a new ttl string, malloc'd by the filesystem DLL; called once, left)
bool SafeFullPath(const char* name, NStr* out)
{
    __try { g_fullPath(out, name); return out->p != nullptr && out->len > 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// IGame::GetLocaleID returns a new ttl string (malloc'd by the engine; the 3 bytes are left - called once)
bool SafeLocale(void* game, char out[4])
{
    __try {
        NStr s{ nullptr, 0, 0 };
        g_getLocale(game, &s);
        if (!s.p || s.len < 2) { return false; }
        out[0] = static_cast<char>(s.p[0] | 0x20);
        out[1] = static_cast<char>(s.p[1] | 0x20);
        out[2] = 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// the game's own recipe (+623F40); returns the IGSObject (model = obj - 40) or nullptr
bool SafeCreate(void* level, void** objOut, int* idOut, bool* idOk)
{
    __try {
        void* parent = static_cast<char*>(level) + kLevelGsoOff;
        void* obj = g_create(parent, g_crtti, false, nullptr);
        *objOut = obj;
        if (!obj) { return true; }
        char* model = static_cast<char*>(obj) - kGsoOff;
        NStr mesh{ kMesh, static_cast<uint32_t>(std::strlen(kMesh)), static_cast<uint32_t>(std::strlen(kMesh)) };
        g_setMesh(model, &mesh);
        g_clearFlag(model + kCtlOff, kFlagClear);
        g_setFlag(model + kCtlOff, kFlagSet);
        g_init(model + kGsoOff, parent);
        const float park[12] = { 1, 0, 0, 0, 0, 1, 0, -10000.0f, 0, 0, 1, 0 };   // far below the world until the first placement
        g_setXform(model + kCtlOff, park);
        g_activate(model + kGsoOff);
        g_enable(model, false);
        *idOut = g_getId(obj);
        *idOk = g_findById(parent, *idOut) == obj;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// true when the level still hands back our object by its id (never dereferences the object itself)
bool SafeAlive(void* level, void* obj, int id, bool* alive)
{
    __try { *alive = g_findById(static_cast<char*>(level) + kLevelGsoOff, id) == obj; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeReplace(void* obj, const char* mat, bool* ok)
{
    __try {
        NStr all{ "*", 1, 1 };
        NStr nw{ mat, static_cast<uint32_t>(std::strlen(mat)), static_cast<uint32_t>(std::strlen(mat)) };
        *ok = g_replace(static_cast<char*>(obj) - kGsoOff, &all, &nw);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeEnable(void* obj, bool on)
{
    __try { g_enable(static_cast<char*>(obj) - kGsoOff, on); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafePlace(void* obj, const float m[12])
{
    __try { g_setXform(static_cast<char*>(obj) - kGsoOff + kCtlOff, m); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

template <typename T> bool Exp(HMODULE m, const char* name, T* out)
{
    *out = reinterpret_cast<T>(GetProcAddress(m, name));
    return *out != nullptr;
}

// HUD-side exports (resolved on the HUD thread, once)
bool SlotReady()
{
    if (g_slotReady != 0) { return g_slotReady == 1; }
    const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    if (!en) { return false; }
    const char* miss = nullptr;
    if (!Exp(en, "?IsVisible@IUIElement@@QEBA_NXZ", &g_isVisible)) { miss = "IUIElement::IsVisible"; }
    else if (!Exp(en, "?GetText@IUIText@@UEAAAEBV?$string_base@D@ttl@@XZ", &g_getText)) { miss = "IUIText::GetText"; }
    else if (!Exp(en, "?GetExtentsInWorld@IControlObject@@QEBAAEBVextents@@XZ", &g_getExtents)) { miss = "IControlObject::GetExtentsInWorld"; }
    else if (!Exp(en, "?GetLevel@IGSObject@@QEBAPEAVILevel@@XZ", &g_getLevel)) { miss = "IGSObject::GetLevel"; }
    InterlockedExchange(&g_slotReady, miss ? -1 : 1);
    // [PROBE: PROMPTQ] gate=inworld_prompts  site=FLAT  log-only=yes
    LOG("promptquad: [PROMPTQ] SLOT SIDE %s (logged unconditionally) - %s%s | thread %lu", miss ? "*** REFUSED ***" : "READY",
        miss ? "missing engine export " : "IsVisible / GetText / GetExtentsInWorld / GetLevel resolved", miss ? miss : "",
        GetCurrentThreadId());
    return miss == nullptr;
}

// ---- THE PROMPT PIN (trace 20261123_2000). HudTrigger's +5B9EA0 projects the trigger's world point and SetRelativePos-es the prompt
// group EVERY FRAME (ida/pin3_gd_out.txt) - the prompt chases its object with the head and the router's leaves lag it 2-3 frames
// (RUN IP6). Its SetRelativePos is the engine import below, called from 296 gamedll sites; exactly ONE (call +5BA11C) returns to
// +5BA122 (scratch/20261123_prompt_alpha/setrelpos_site.py). While our quad shows (g_routeWant), that one call is skipped - the group
// keeps its last place; every other call, and that one whenever the quad is hidden, goes to the engine unchanged.
// [LOADBEARING: inworld_prompts] the prompt pin - without it the routed prompt moves 120-150 UI px / frame and falls to the flat HUD.
constexpr uintptr_t kPinRetRva = 0x5BA122;   // Steam; EGL through tools/egl_allowlist.tsv (0x5BA092)
const char* const kSetRelPosImport = "?SetRelativePos@IUIElement@@QEAAXAEBVvec3@@@Z";
using SetRelPosFn = void (*)(void* el, const void* pos);
SetRelPosFn   g_origSetRelPos = nullptr;
uintptr_t     g_pinRet = 0;          // gamedll + kPinRetRva (as loaded); 0 = not installed
volatile LONG g_pinSkipped = 0, g_pinPassedSite = 0, g_pinCalls = 0;
bool          g_pinTried = false;

void PinSetRelPos(void* el, const void* pos)
{
    InterlockedIncrement(&g_pinCalls);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_pinRet) {
        // the prompt group stays where it is - while the quad shows, and (trace 20261124_0230, the prompt always routed) whenever the
        // feature is live (the bootstrap's live texture exists), so it never moves on its first frames either
        if (g_routeWant || PromptQuadLive()) { InterlockedIncrement(&g_pinSkipped); return; }
        InterlockedIncrement(&g_pinPassedSite);
    }
    g_origSetRelPos(el, pos);
}

// The call before the return site must be FF 15 <rel32> through the gamedll IAT slot we patch - else nothing is patched.
bool PinSiteOk(uintptr_t ret, uintptr_t iatSlot)
{
    __try {
        const unsigned char* c = reinterpret_cast<const unsigned char*>(ret - 6);
        if (c[0] != 0xFF || c[1] != 0x15) { return false; }
        int32_t disp = 0;
        std::memcpy(&disp, c + 2, 4);
        return ret + static_cast<intptr_t>(disp) == iatSlot;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The gamedll IAT slot of an import (by name, from fromDll) - read-only walk, nullptr if absent.
void** IatSlotOf(HMODULE mod, const char* fromDll, const char* name)
{
    auto base = reinterpret_cast<uint8_t*>(mod);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) { return nullptr; }
    for (auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), fromDll) != 0 || !desc->OriginalFirstThunk) { continue; }
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) { continue; }
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), name) == 0) { return reinterpret_cast<void**>(&slots->u1.Function); }
        }
    }
    return nullptr;
}

void InstallPin(HMODULE gd)
{
    if (g_pinTried) { return; }
    g_pinTried = true;
    const uintptr_t ret = reinterpret_cast<uintptr_t>(gd) + DLVR_PQ_GD_RVA(kPinRetRva);
    void** slot = IatSlotOf(gd, "engine_x64_rwdi.dll", kSetRelPosImport);
    const char* why = nullptr;
    if (!slot) { why = "the gamedll does not import SetRelativePos by that name"; }
    else if (!PinSiteOk(ret, reinterpret_cast<uintptr_t>(slot))) { why = "the 6 bytes before the return site are not FF 15 through that IAT slot (a different build)"; }
    else {
        g_origSetRelPos = reinterpret_cast<SetRelPosFn>(*slot);
        g_pinRet = ret;   // set BEFORE the slot points at us: the hook never runs with it unset
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) { why = "VirtualProtect refused the IAT page"; g_pinRet = 0; }
        else {
            InterlockedExchangePointer(slot, reinterpret_cast<void*>(&PinSetRelPos));
            VirtualProtect(slot, sizeof(void*), old, &old);
        }
    }
    // [PROBE: PROMPTQ] gate=inworld_prompts  site=FLAT  log-only=no
    LOG("promptquad: [PROMPTQ] PIN %s (logged unconditionally) - gamedll IAT %s slot %p (engine %p), return site gamedll +%llX %s",
        why ? "*** NOT INSTALLED - the prompt keeps chasing its object (flat flicker on head turns) ***" : "READY", kSetRelPosImport,
        static_cast<void*>(slot), reinterpret_cast<void*>(g_origSetRelPos), static_cast<unsigned long long>(DLVR_PQ_GD_RVA(kPinRetRva)),
        why ? why : "(FF 15 through that slot checked)");
}

// Game-side: exports, the CRTTI (name-checked), the IGame*, the pack, the locale. Retried until the IGame* exists.
bool TickReady()
{
    if (g_tickReady != 0) { return g_tickReady == 1; }
    const HMODULE en = GetModuleHandleA("engine_x64_rwdi.dll");
    const HMODULE gd = GetModuleHandleA("gamedll_x64_rwdi.dll");
    const HMODULE fsd = GetModuleHandleA("filesystem_x64_rwdi.dll");
    if (!en || !gd || !fsd) { return false; }
    const char* miss = nullptr;
    if (!Exp(fsd, "?full_path@fs@@YA?BV?$string_base@D@ttl@@PEBD@Z", &g_fullPath)) { miss = "fs::full_path (filesystem)"; }
    else if (!Exp(en, "?GetLevel@IGSObject@@QEBAPEAVILevel@@XZ", &g_getLevel)) { miss = "IGSObject::GetLevel"; }
    else if (!Exp(en, "?CreateObject@IGSObject@@QEAAPEAV1@AEBVCRTTI@@_NPEBV?$string_base@D@ttl@@@Z", &g_create)) { miss = "IGSObject::CreateObject"; }
    else if (!Exp(en, "?SetMeshName@IModelObject@@QEAAXAEBV?$string_base@D@ttl@@@Z", &g_setMesh)) { miss = "IModelObject::SetMeshName"; }
    else if (!Exp(en, "?ClearFlag@IControlObject@@QEAAXW4TYPE@COFlags@@@Z", &g_clearFlag)) { miss = "IControlObject::ClearFlag"; }
    else if (!Exp(en, "?SetFlag@IControlObject@@QEAAXW4TYPE@COFlags@@@Z", &g_setFlag)) { miss = "IControlObject::SetFlag"; }
    else if (!Exp(en, "?InitObject@IGSObject@@QEAAXPEAV1@@Z", &g_init)) { miss = "IGSObject::InitObject"; }
    else if (!Exp(en, "?SetWorldXform@IControlObject@@QEAAXAEBVmtx34@@@Z", &g_setXform)) { miss = "IControlObject::SetWorldXform"; }
    else if (!Exp(en, "?ActivateOnModule@IGSObject@@QEAAXXZ", &g_activate)) { miss = "IGSObject::ActivateOnModule"; }
    else if (!Exp(en, "?ReplaceMaterial@IModelObject@@QEAA_NAEBV?$string_base@D@ttl@@0@Z", &g_replace)) { miss = "IModelObject::ReplaceMaterial"; }
    else if (!Exp(en, "?EnableRendering@IModelObject@@QEAAX_N@Z", &g_enable)) { miss = "IModelObject::EnableRendering"; }
    else if (!Exp(en, "?GetID@IGSObject@@QEBAHXZ", &g_getId)) { miss = "IGSObject::GetID"; }
    else if (!Exp(en, "?FindObjectByID@IGSObject@@UEBAPEAV1@H@Z", &g_findById)) { miss = "IGSObject::FindObjectByID"; }
    else if (!Exp(en, "?LoadPack@IGame@@QEAA_NAEBV?$string_base@D@ttl@@_N1PEAPEAUSRPackHandle@1@@Z", &g_loadPack)) { miss = "IGame::LoadPack"; }
    else if (!Exp(en, "?GetLocaleID@IGame@@QEAA?AV?$string_base@D@ttl@@XZ", &g_getLocale)) { miss = "IGame::GetLocaleID"; }
    if (miss) {
        InterlockedExchange(&g_tickReady, -1);
        // [PROBE: PROMPTQ] gate=inworld_prompts  site=FLAT  log-only=yes
        LOG("promptquad: [PROMPTQ] *** REFUSED *** (logged unconditionally) - missing engine export %s: no in-world prompt this session", miss);
        return false;
    }
    uintptr_t game = 0;
    if (!ReadPtr(reinterpret_cast<uintptr_t>(en) + DLVR_PQ_EN_RVA(kGameRva), &game) || !game) { return false; }   // not created yet: retried
    g_game = reinterpret_cast<void*>(game);
    g_crtti = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(gd) + DLVR_PQ_GD_RVA(kModelObjRttiRva));
    if (!SafeNameIs(g_crtti, "ModelObject")) {
        InterlockedExchange(&g_tickReady, -1);
        // [PROBE: PROMPTQ] gate=inworld_prompts  site=FLAT  log-only=yes
        LOG("promptquad: [PROMPTQ] *** REFUSED *** (logged unconditionally) - the class at gamedll +%llX is not named \"ModelObject\" "
            "at +%zu (a different build / the EGL row refused): no object will be created",
            static_cast<unsigned long long>(DLVR_PQ_GD_RVA(kModelObjRttiRva)), kRttiNameOff);
        return false;
    }
    char lang[4] = {};
    const bool haveLocale = SafeLocale(g_game, lang);
    bool known = false;
    if (haveLocale) {
        for (const char* l : kLangs) { if (std::strcmp(l, lang) == 0) { known = true; } }
    }
    if (known) { std::memcpy(g_lang, lang, 3); }
    NStr path{ nullptr, 0, 0 };
    const bool havePath = SafeFullPath(kPackName, &path);
    char shown[260] = {};
    if (havePath) { std::snprintf(shown, sizeof(shown), "%.*s", static_cast<int>(path.len < 250 ? path.len : 250), path.p); }
    bool ok = false;
    const bool called = havePath && SafeLoadPack(g_game, &path, &ok);
    g_packTried = true;
    g_packOk = called && ok;
    g_packMs = GetTickCount64();
    InstallPin(gd);   // trace 20261123_2000 - before READY: the pin is in place by the first prompt
    InterlockedExchange(&g_tickReady, 1);
    // [PROBE: PROMPTQ] gate=inworld_prompts  site=FLAT  log-only=yes
    LOG("promptquad: [PROMPTQ] READY (logged unconditionally) - 14 engine exports + fs::full_path resolved; IGame %p; class \"ModelObject\" "
        "at gamedll +%llX (name checked); fs::full_path(\"%s\") = \"%s\"; LoadPack %s; locale \"%s\" -> images \"%s\"%s | thread %lu", g_game,
        static_cast<unsigned long long>(DLVR_PQ_GD_RVA(kModelObjRttiRva)), kPackName, havePath ? shown : "*** FAULTED / EMPTY ***",
        !called ? "*** NOT CALLED / FAULTED ***" : ok ? "true" : "*** FALSE - no prompt image will be swapped in ***",
        haveLocale ? lang : "(unreadable)", g_lang, known ? "" : " (not one of the 17 language paks -> en)", GetCurrentThreadId());
    return true;
}

// FLICKER COUNTERS (trace 20261123_1700, log-only): one prompt EPISODE = show -> hide; its end says how long it showed and why it hid.
ULONGLONG g_epStartMs = 0;
long      g_epTicks = 0;
char      g_epKey[48] = {};
LONG      g_epLogged = 0;

void Hide(const char* why = "the feature is off")
{
    if (g_shown) {
        if (++g_epLogged <= 60) {
            // [PROBE: PROMPTQ] gate=inworld_prompts  site=HEADSET  log-only=yes
            LOG("promptquad: [PROMPTQ] EPISODE %ld (first 60) - \"%s\" shown %llu ms, %ld ticks, then HIDDEN: %s", g_epLogged, g_epKey,
                static_cast<unsigned long long>(GetTickCount64() - g_epStartMs), g_epTicks, why);
        }
        if (g_obj) { SafeEnable(g_obj, false); }
        if (g_plateObj) { SafeEnable(g_plateObj, false); }   // the plate goes with the text (trace 20261123_1830)
    }
    g_shown = false;
    g_plateShown = false;
    g_routeWant = 0;   // the game's widget goes back to the flat HUD at the next publish (trace 20261123_1535)
}

// THE PLATE (trace 20261123_1830): create + swap the plate object once per level; true = ready to place. Every failure is logged
// and leaves the text quad alone. Called on the game thread from the tick, after the text quad's own create / swap succeeded.
bool EnsurePlate(void* level, ULONGLONG now)
{
    if (g_plateFailed) { return false; }
    if (g_plateObj) {
        bool alive = false;
        if (!SafeAlive(level, g_plateObj, g_plateId, &alive) || !alive) { g_plateObj = nullptr; g_plateSwapped = false; }
    }
    if (!g_plateObj) {
        void* obj = nullptr;
        int id = 0;
        bool idOk = false;
        const bool called = SafeCreate(level, &obj, &id, &idOk);
        const LONG n = InterlockedIncrement(&g_nPlate);
        if (!called || !obj || !idOk) { g_plateFailed = true; }
        else { g_plateObj = obj; g_plateId = id; }
        // [PROBE: PROMPTQ] gate=inworld_prompts  site=HEADSET  log-only=yes
        LOG("promptquad: [PROMPTQ] %s (logged unconditionally, #%ld) - level %p: CreateObject(ModelObject) %s -> object %p, id %d %s",
            g_plateObj ? "PLATE CREATED" : "*** PLATE NOT CREATED - the text shows without its plate on this level ***", n, level,
            called ? "returned" : "FAULTED", obj, id, idOk ? "(FindObjectByID hands it back)" : "*** FindObjectByID does NOT hand it back ***");
        if (!g_plateObj) { return false; }
    }
    if (!g_plateSwapped) {
        if (now - g_packMs < kPackSettleMs) { return false; }
        bool ok = false;
        const bool called = SafeReplace(g_plateObj, kPlateMat, &ok);
        // [PROBE: PROMPTQ] gate=inworld_prompts  site=HEADSET  log-only=yes
        LOG("promptquad: [PROMPTQ] PLATE SWAP %s (logged unconditionally) - ReplaceMaterial(\"*\" -> \"%s\") on object %p %s",
            (called && ok) ? "OK" : "*** FAILED - no plate on this level ***", kPlateMat, g_plateObj,
            !called ? "FAULTED" : ok ? "true" : "FALSE");
        if (!called || !ok) { g_plateFailed = true; return false; }
        g_plateSwapped = true;
    }
    return true;
}

// SIZE (trace 20261123_1700): the panel's metres from the prompt piece's box. The bootstrap copies the box 1:1 (or at an integer step when
// it exceeds 768 x 192 - the same rule as dlssflat.cpp PromptCopy) centred into the live texture; so the whole texture covers
// (768 x step x uiW / boxW) x (192 x step x uiH / boxH) UI px, and one UI px = kMetresPerUiPx x ui_prompt_size metres.
// No box this tick -> the last good one (first: RUN IP4's single-line prompt, 186 x 41 layer px = 653 x 142 UI px).
constexpr float kLiveTexW = 768.0f, kLiveTexH = 192.0f;
constexpr float kMetresPerUiPx = 0.15f / 653.0f;   // size 1.0: a one-line prompt (653 UI px wide) = 0.15 m
using PieceInfoFn = int (*)(int piece, int* box4, float* ui2);
PieceInfoFn g_pieceInfo = nullptr;
bool        g_pieceInfoLooked = false;
float       g_lastBoxW = 186.0f, g_lastBoxH = 41.0f, g_lastUiW = 653.0f, g_lastUiH = 142.0f;
void PanelSize(float* w, float* h)
{
    if (!g_pieceInfoLooked) {
        if (const HMODULE bs = GetModuleHandleA("xinput1_3.dll")) {
            g_pieceInfoLooked = true;
            g_pieceInfo = reinterpret_cast<PieceInfoFn>(GetProcAddress(bs, "DlvrWristPieceInfo"));
        }
    }
    int box[4] = {};
    float ui[2] = {};
    if (g_pieceInfo && g_pieceInfo(wristleaf::kPrompt, box, ui) == 0 && box[2] > 0 && box[3] > 0 && ui[0] > 0.0f && ui[1] > 0.0f &&
        std::isfinite(ui[0]) && std::isfinite(ui[1])) {
        // SWAP COUNTER (trace 20261123_2200, log-only): the routed widget's box changed size since the last tick that read it
        static LONG s_changes = 0;
        if ((static_cast<float>(box[2]) != g_lastBoxW || static_cast<float>(box[3]) != g_lastBoxH) && ++s_changes <= 20) {
            // [PROBE: PROMPTQ] gate=inworld_prompts  site=HEADSET  log-only=yes
            LOG("promptquad: [PROMPTQ] BOX SIZE CHANGE %ld (first 20) - %.0f x %.0f -> %d x %d layer px (%.0f x %.0f UI px): the panel's size "
                "follows from this tick", s_changes, g_lastBoxW, g_lastBoxH, box[2], box[3], static_cast<double>(ui[0]),
                static_cast<double>(ui[1]));
        }
        g_lastBoxW = static_cast<float>(box[2]); g_lastBoxH = static_cast<float>(box[3]);
        g_lastUiW = ui[0]; g_lastUiH = ui[1];
    }
    int step = static_cast<int>(std::ceil(g_lastBoxW / kLiveTexW));
    const int sh = static_cast<int>(std::ceil(g_lastBoxH / kLiveTexH));
    if (sh > step) { step = sh; }
    if (step < 1) { step = 1; }
    float s = InworldPromptSize;
    if (!(s >= 0.25f)) { s = 0.25f; }
    if (s > 4.0f) { s = 4.0f; }
    const float mpp = kMetresPerUiPx * s;
    *w = kLiveTexW * static_cast<float>(step) * (g_lastUiW / g_lastBoxW) * mpp;
    *h = kLiveTexH * static_cast<float>(step) * (g_lastUiH / g_lastBoxH) * mpp;
}

// ---- RUN IP2 follow-up (Jay: "Sure"): every outcome says something. HUD side: each distinct (class vtable, key) seen VISIBLE on the
// X slot, once, with what was read. Tick side: each hide reason (first 3 each), each distinct material swapped in and each distinct
// (key, class) shown (first 40 each). ----
struct SeenRow { uintptr_t vt; char key[48]; };
SeenRow       g_seen[40] = {};
int           g_seenN = 0;
volatile LONG g_seenLock = 0;
void NoteSlotSeen(void* slot, void* trig, const char* key, bool haveKey, int idx, const char* cls, uintptr_t vt, bool haveTop,
                  const float top[3], bool haveLevel, void* level)
{
    if (InterlockedCompareExchange(&g_seenLock, 1, 0) != 0) { return; }   // another HUD thread is noting: skip this sample
    bool seen = false;
    for (int i = 0; i < g_seenN; ++i) { if (g_seen[i].vt == vt && std::strcmp(g_seen[i].key, key) == 0) { seen = true; break; } }
    const bool room = !seen && g_seenN < 40;
    if (room) { g_seen[g_seenN].vt = vt; std::snprintf(g_seen[g_seenN].key, sizeof(g_seen[g_seenN].key), "%s", key); ++g_seenN; }
    const int n = g_seenN;
    InterlockedExchange(&g_seenLock, 0);
    if (!room) { return; }
    // [PROBE: PROMPTQ] gate=inworld_prompts  site=HEADSET  log-only=yes
    LOG("promptquad: [PROMPTQ] SLOT SEEN #%d (first 40 distinct class + key) - X slot %p VISIBLE, trigger %p class \"%s\" (vtable %p): "
        "key \"%s\" %s (row %d); box top %s %.2f %.2f %.2f; level %s %p -> %s | thread %lu", n, slot, trig, cls[0] ? cls : "(unreadable)",
        reinterpret_cast<void*>(vt), haveKey ? key : "(unreadable)", idx >= 0 ? "in the table" : "NOT in the table", idx,
        haveTop ? "read" : "*** NOT READ ***", top[0], top[1], top[2], haveLevel ? "read" : "*** NOT READ ***", level,
        !haveTop || !haveLevel ? "published HIDDEN (box / level missing)" : idx < 0 ? "published, the tick hides it (key not in the table)"
                                                                           : "published SHOWN", GetCurrentThreadId());
}
enum HideWhy { kWhyNoHead = 0, kWhyNoFacing, kWhyPlaceFault, kWhyNoLevel, kWhyCount };
const char* const kWhyName[kWhyCount] = { "no rendered view matrix (head)", "head straight above / below the box", "SetWorldXform faulted",
                                          "no level published" };
LONG g_why[kWhyCount] = {};
void HideFor(int why, const char* key)
{
    if (++g_why[why] <= 3) {
        // [PROBE: PROMPTQ] gate=inworld_prompts  site=HEADSET  log-only=yes
        LOG("promptquad: [PROMPTQ] HIDDEN (first 3 per reason) - \"%s\": %s (#%ld)", key, kWhyName[why], g_why[why]);
    }
    Hide(kWhyName[why]);
}
char g_swapSeen[40][96] = {};
int  g_swapSeenN = 0;
char g_showSeen[40][112] = {};
int  g_showSeenN = 0;
bool FirstOf(char (*rows)[96], int* n, const char* s)
{
    for (int i = 0; i < *n; ++i) { if (std::strcmp(rows[i], s) == 0) { return false; } }
    if (*n >= 40) { return false; }
    std::snprintf(rows[(*n)++], 96, "%s", s);
    return true;
}
bool FirstShow(const char* s)
{
    for (int i = 0; i < g_showSeenN; ++i) { if (std::strcmp(g_showSeen[i], s) == 0) { return false; } }
    if (g_showSeenN >= 40) { return false; }
    std::snprintf(g_showSeen[g_showSeenN++], sizeof(g_showSeen[0]), "%s", s);
    return true;
}

}  // namespace

void PromptQuadOnSlot(void* slot, void* trigger)
{
    if (!InworldPrompts || !slot || !trigger) { return; }
    uintptr_t raw = 0;
    if (!ReadPtr(reinterpret_cast<uintptr_t>(slot) + kSlotTypeOff, &raw) || static_cast<uint32_t>(raw) != 0) { return; }   // X slot only
    if (!SlotReady()) { return; }
    bool vis = false;
    char key[48] = {};
    float top[3] = {};
    uintptr_t elem = 0;
    if (!SafeVisible(slot, &vis)) { return; }
    const bool haveKey = ReadPtr(reinterpret_cast<uintptr_t>(slot) + kSlotActionTextOff, &elem) && elem &&
                         SafeKey(reinterpret_cast<void*>(elem), key, sizeof(key));
    float half[2] = {};
    float bottom = 0.0f;
    const bool haveTop = vis && SafeTop(trigger, top, half, &bottom);
    int idx = -1;
    if (haveKey) {
        for (int i = 0; i < kNumKeys; ++i) { if (_stricmp(kKeys[i].key, key) == 0) { idx = i; break; } }
    }
    // the level and the class - read HERE, while the game's own update holds the trigger (the tick never reads it). RUN IP2: an
    // unknown key used to zero `visible` here (haveLevel needed idx >= 0), so the tick's not-in-table line could never print - the
    // level no longer depends on the key; the tick decides on the key and says so.
    void* level = nullptr;
    const bool haveLevel = vis && SafeLevelOf(trigger, &level) && level;
    char cls[64] = {};
    uintptr_t vt = 0;
    if (vis && !SafeClassName(trigger, cls, sizeof(cls), &vt)) { cls[0] = 0; vt = 0; }
    if (vis) { NoteSlotSeen(slot, trigger, key, haveKey, idx, cls, vt, haveTop, top, haveLevel, level); }
    if (InterlockedCompareExchange(&g_wLock, 1, 0) != 0) { return; }   // another HUD thread is publishing: drop this one
    InterlockedIncrement(&g_wSeq);   // odd: writing
    g_wTrig = trigger;
    g_wKey = idx;
    g_wVis = (vis && haveTop && haveLevel) ? 1 : 0;
    if (haveTop) { g_wTop[0] = top[0]; g_wTop[1] = top[1]; g_wTop[2] = top[2]; g_wHalf[0] = half[0]; g_wHalf[1] = half[1]; g_wBottom = bottom; }
    g_wMs = GetTickCount64();
    std::memcpy(g_wKeyText, key, sizeof(g_wKeyText));
    g_wLevel = level;
    g_wVt = vt;
    std::memcpy(g_wClass, cls, sizeof(g_wClass));
    InterlockedIncrement(&g_wSeq);   // even: settled
    InterlockedExchange(&g_wLock, 0);
    if (InterlockedIncrement(&g_nSlot) == 1) {
        // [PROBE: PROMPTQ] gate=inworld_prompts  site=HEADSET  log-only=yes
        LOG("promptquad: [PROMPTQ] FIRST SLOT (logged unconditionally) - X slot %p of trigger %p: visible %d, key \"%s\" %s (row %d), box top "
            "%.2f %.2f %.2f %s | thread %lu", slot, trigger, vis ? 1 : 0, haveKey ? key : "(unreadable)",
            idx >= 0 ? "in the table" : "NOT in the table", idx, top[0], top[1], top[2], haveTop ? "" : "(no box)", GetCurrentThreadId());
    }
}

void PromptQuadTick()
{
    if (!InworldPrompts) { Hide(); return; }
    if (!TickReady()) { return; }
    {   // THE PIN's 5 s line (trace 20261123_2000, log-only)
        static ULONGLONG s_pinMs = 0;
        const ULONGLONG t = GetTickCount64();
        if (g_pinRet && t - s_pinMs >= 5000) {
            s_pinMs = t;
            const LONG sk = InterlockedExchange(&g_pinSkipped, 0), ps = InterlockedExchange(&g_pinPassedSite, 0),
                       all = InterlockedExchange(&g_pinCalls, 0);
            // [PROBE: PROMPTQ] gate=inworld_prompts  site=HEADSET  log-only=yes
            LOG("promptquad: [PROMPTQ] PIN 5 s - the prompt's placement SKIPPED (quad shown) %ld, let through (quad hidden) %ld | gamedll "
                "SetRelativePos calls through the hook %ld", sk, ps, all);
        }
    }
    // the newest slot record
    void* trig = nullptr;
    int key = -1, vis = 0;
    float top[3] = {};
    ULONGLONG ms = 0;
    char keyText[48] = {};
    void* level = nullptr;
    uintptr_t vt = 0;
    char cls[64] = {};
    const LONG s1 = g_wSeq;
    if (s1 & 1) { return; }   // being written: next tick
    trig = g_wTrig; key = g_wKey; vis = g_wVis; top[0] = g_wTop[0]; top[1] = g_wTop[1]; top[2] = g_wTop[2]; ms = g_wMs;
    float half[2] = { g_wHalf[0], g_wHalf[1] };
    const float bottom = g_wBottom;
    std::memcpy(keyText, g_wKeyText, sizeof(keyText));
    level = g_wLevel; vt = g_wVt;
    std::memcpy(cls, g_wClass, sizeof(cls));
    if (g_wSeq != s1) { return; }   // torn: next tick
    cls[sizeof(cls) - 1] = 0;
    const ULONGLONG now = GetTickCount64();
    if (!trig) { Hide("no slot record"); return; }
    if (now - ms > kStaleMs) { Hide("the slot record went stale (> 250 ms without a HUD slot update)"); return; }
    if (!vis) { Hide("the game's X slot not visible (or its box / level not read)"); return; }
    (void)key;   // ROUTE B (trace 20261123_1535): the key no longer picks an image - the game's own widget is copied (SLOT SEEN logs it)
    if (!g_packOk) { Hide("the pack did not load"); return; }
    // the trigger's level (read on the HUD thread); our object lives there or is (re)created
    if (!level) { HideFor(kWhyNoLevel, keyText); return; }
    if (level != g_level) {   // a new level: forget the old object WITHOUT touching it (it went with its level)
        g_level = level; g_obj = nullptr; g_objId = 0; g_objFailed = false; g_shown = false; g_curMat[0] = 0;
        g_plateObj = nullptr; g_plateId = 0; g_plateFailed = false; g_plateSwapped = false;   // the plate went with its level too
    }
    if (g_obj) {
        bool alive = false;
        if (!SafeAlive(level, g_obj, g_objId, &alive) || !alive) { g_obj = nullptr; g_shown = false; g_curMat[0] = 0; }
    }
    if (!g_obj) {
        if (g_objFailed) { return; }
        void* obj = nullptr;
        int id = 0;
        bool idOk = false;
        const bool called = SafeCreate(level, &obj, &id, &idOk);
        const LONG n = InterlockedIncrement(&g_nCreate);
        if (!called || !obj || !idOk) { g_objFailed = true; }
        else { g_obj = obj; g_objId = id; }
        // [PROBE: PROMPTQ] gate=inworld_prompts  site=HEADSET  log-only=yes
        LOG("promptquad: [PROMPTQ] %s (logged unconditionally, #%ld) - level %p: CreateObject(ModelObject) %s -> object %p, mesh \"%s\", id %d %s",
            (called && obj && idOk) ? "QUAD CREATED" : "*** QUAD NOT CREATED - no in-world prompt on this level ***", n, level,
            called ? "returned" : "FAULTED", obj, kMesh, id, idOk ? "(FindObjectByID hands it back)" : "*** FindObjectByID does NOT hand it back ***");
        if (!g_obj) { return; }
    }
    // ROUTE B (trace 20261123_1535): ONE material - its texture is the bootstrap's live copy of the game's own prompt widget
    (void)vt;
    if (!g_liveReadyFn) {
        const HMODULE bs = GetModuleHandleA("xinput1_3.dll");
        g_liveReadyFn = bs ? reinterpret_cast<LiveReadyFn>(GetProcAddress(bs, "DlvrPromptLiveReady")) : nullptr;
    }
    const bool liveNow = g_liveReadyFn && g_liveReadyFn() == 1;
    InterlockedExchange(&g_liveReady, liveNow ? 1 : 0);
    if (!g_saidLive) {
        g_saidLive = true;
        // [PROBE: PROMPTQ] gate=inworld_prompts  site=FLAT  log-only=yes
        LOG("promptquad: [PROMPTQ] LIVE TEXTURE %s (logged unconditionally at the first prompt) - the bootstrap's DlvrPromptLiveReady %s%s",
            liveNow ? "WRITABLE" : "*** NOT WRITABLE - the game's prompt stays on the flat HUD, the quad shows black ***",
            g_liveReadyFn ? "returned " : "NOT FOUND in xinput1_3.dll", g_liveReadyFn ? (liveNow ? "1" : "0") : "");
    }
    const char* const mat = kLiveMat;
    if (std::strcmp(mat, g_curMat) != 0) {
        if (now - g_packMs < kPackSettleMs) { Hide("waiting 2 s after the pack loaded"); return; }   // registration timing not read
        bool ok = false;
        const bool called = SafeReplace(g_obj, mat, &ok);
        const LONG n = (called && ok) ? InterlockedIncrement(&g_nSwap) : InterlockedIncrement(&g_nSwapFault);
        if (n == 1 || FirstOf(g_swapSeen, &g_swapSeenN, mat)) {
            // [PROBE: PROMPTQ] gate=inworld_prompts  site=HEADSET  log-only=yes
            LOG("promptquad: [PROMPTQ] SWAP %s (first of each material, first 40) - ReplaceMaterial(\"*\" -> \"%s\") on object %p %s, %llu ms "
                "after the pack (swaps ok %ld, faulted %ld)", (called && ok) ? "OK" : "*** FAULTED ***", mat, g_obj,
                !called ? "FAULTED" : ok ? "true" : "FALSE", static_cast<unsigned long long>(now - g_packMs), g_nSwap, g_nSwapFault);
        }
        if (!called || !ok) { Hide("ReplaceMaterial failed"); return; }
        std::snprintf(g_curMat, sizeof(g_curMat), "%s", mat);
    }
    // place it: facing the head about the vertical, 0.36 x 0.09 m, kLiftM above the box (see the header for the mesh axes)
    float vm[12] = {};
    unsigned age = 0;
    if (!CamHookGetRenderedViewMatrix(vm, &age)) { HideFor(kWhyNoHead, keyText); return; }
    // PLACEMENT (trace 20261123_2300, Jay: "Lets test B"): the box's top centre, pulled toward the head along the head direction by
    // min(kPullM, half the distance) - never sideways (RUN IP9: the near-face clamp of trace 20261123_2200 picked box corners, up to
    // 0.82 m off the object).
    float fx = vm[3] - top[0], fz = vm[11] - top[2];
    const float len = std::sqrt(fx * fx + fz * fz);
    if (!(len > 0.01f)) { HideFor(kWhyNoFacing, keyText); return; }
    fx /= len; fz /= len;
    const float pull = kPullM < 0.5f * len ? kPullM : 0.5f * len;
    const float ax = top[0] + fx * pull, az = top[2] + fz * pull;
    // EYE HEIGHT (trace 20261123_2330, Jay: "Either A/B" -> A): the panel's centre at the head's height, clamped between the box's
    // bottom and its top + kLiftM - small objects keep it above them, a tall one (the payphone) gets it at eye height
    const float topY = top[1] + kLiftM;
    // NPCs (trace 20261124_0000, Jay: "set the height for them specifically to what it was prior"): HumanAI / ActorDI - the only NPC
    // classes seen this session (HumanAI also = searchable bodies) - keep the panel above the box, as before trace 20261123_2330
    const bool npc = std::strcmp(cls, "HumanAI") == 0 || std::strcmp(cls, "ActorDI") == 0;
    const float cy = npc ? topY : (vm[7] < bottom ? bottom : (vm[7] > topY ? topY : vm[7]));
    // SIZE (trace 20261123_1700): the copy is 1:1 into the 768 x 192 live texture, centred; the PANEL is sized so one UI pixel of the game's
    // widget is kMetresPerUiPx x ui_prompt_size metres on every headset (layer px differ per headset, UI px do not). The rest of the
    // texture is black = adds nothing on this additive material. The box: the bootstrap's DlvrWristPieceInfo(5) (read-only).
    float panelW = 0.0f, panelH = 0.0f;
    PanelSize(&panelW, &panelH);
    const float sy = panelH / kMeshH, sz = panelW / kMeshW;
    const float m[12] = {
        fx, 0.0f, -fz * sz, ax,
        0.0f, sy, 0.0f, cy - 0.5f * panelH,   // the panel's centre (= the widget's centre) at cy (trace 20261123_2330)
        fz, 0.0f, fx * sz, az,
    };
    if (!SafePlace(g_obj, m)) { HideFor(kWhyPlaceFault, keyText); return; }
    // THE PLATE (trace 20261123_1830): the same matrix, kPlateBackM further from the head (the facing points at the head)
    if (EnsurePlate(level, now)) {
        float pm[12];
        std::memcpy(pm, m, sizeof(pm));
        pm[3] -= fx * kPlateBackM;
        pm[11] -= fz * kPlateBackM;
        if (!SafePlace(g_plateObj, pm)) {
            // [PROBE: PROMPTQ] gate=inworld_prompts  site=HEADSET  log-only=yes
            LOG("promptquad: [PROMPTQ] *** PLATE PLACE FAULTED - no plate on this level (the text stays) *** object %p", g_plateObj);
            g_plateFailed = true;
            SafeEnable(g_plateObj, false);
            g_plateShown = false;
        } else if (!g_plateShown) {
            SafeEnable(g_plateObj, true);
            g_plateShown = true;
        }
    }
    ++g_epTicks;
    if (!g_shown) {
        SafeEnable(g_obj, true);
        g_shown = true;
        g_epStartMs = now; g_epTicks = 1;
        std::snprintf(g_epKey, sizeof(g_epKey), "%s", keyText);
        g_routeWant = 1;   // wristprobe's next publish asks the bootstrap to route the game's widget (it leaves the flat HUD)
        char showKey[112];
        std::snprintf(showKey, sizeof(showKey), "%s|%s", keyText, cls);
        const LONG n = InterlockedIncrement(&g_nShow);
        if (n == 1 || FirstShow(showKey)) {
            // [PROBE: PROMPTQ] gate=inworld_prompts  site=HEADSET  log-only=yes
            LOG("promptquad: [PROMPTQ] SHOW (first of each key + class, first 40; shows so far %ld) - \"%s\" on a \"%s\" (trigger %p) at "
                "%.2f %.2f %.2f (centre height %s: head %.2f, box bottom %.2f .. top + %.2f = %.2f), facing %.2f %.2f, panel %.2f x "
                "%.2f m, material \"%s\" | box XZ %.2f x %.2f m, panel pulled %.2f m from the box centre toward the head (cap %.2f)", n,
                keyText, cls[0] ? cls : "(class unreadable)", trig, ax, cy, az, npc ? "NPC = above the box" : "eye height, clamped",
                vm[7], bottom, kLiftM, topY, fx, fz, panelW, panelH, g_curMat, 2.0f * half[0], 2.0f * half[1], pull, kPullM);
        }
    }
}

bool PromptQuadOnThisLaunch()
{
    static const bool on = InworldPrompts;   // the first call (wristprobe::Start, after the settings load) latches the launch value
    return on;
}

bool PromptQuadRouteWanted()
{
    return g_routeWant != 0 && g_liveReady != 0;
}

// trace 20261124_0230 (the prompt always routed): the feature is live NOW - on at launch, still on in the menu, the bootstrap's live
// texture exists. wristprobe publishes it as routeMask bit 6; the bootstrap's always-routing and the pin require it.
bool PromptQuadLive()
{
    return PromptQuadOnThisLaunch() && InworldPrompts && g_liveReady != 0;
}

}  // namespace dlvr
