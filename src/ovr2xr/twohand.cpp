// Two-handed grip, Phase 1 (firearms) and Phase 2 (melee, spec section 7) -docs/SPEC-TWO-HANDED-GRIP-2026-10-09.md,
// docs/superpowers/plans/2026-10-09-two-handed-grip.md. Hold LEFT GRIP with the left hand near a two-handed firearm's
// barrel: the gun points along the line between the hands, the shot follows the drawn barrel (PublishBarrelBore runs
// after ItemCtlPlace), L1 is not sent. The maths and the press machine are twohand_logic.h (unit-tested); this file
// does the side effects.
#include "twohand.h"

#include "bodygrab.h"        // BodyGrabHandActive - no two-handed hold while the right hand holds a body
#include "boneblend.h"       // ItemPlaceFromController, RigHandGripPose
#include "controllers.h"
#include "reload.h"          // ReloadPumpStroke - the pin target slides with a manual pump's stroke (pump plan Task 4)
#include "twohand_logic.h"

#include <windows.h>

#include <cstddef>   // offsetof - the [TWOHANDREC] layout asserts
#include <cstdio>    // fopen_s - [TWOHANDREC]
#include <cstring>

#include "../common/log.h"

#define LOG(...) ::dlvr::LogTo("dlvr_ovr2xr.log", __VA_ARGS__)
#include "periodic_log.h"   // LOG_PERIODIC - the GRIPPING / WRIST / HANDPOSE status lines (2026-11-08 log trim)

namespace dlvr {

// [DEP: rig_two_hand_grip] required-for=the two-handed grip (RigTwoHandGrip) - SPEC-TWO-HANDED-GRIP-2026-10-09.
//      tree=LOADBEARING (the master of the verified rifle grip INV-509 and melee grip INV-517). DEFAULT ON since 2026-10-09 -
//      Jay: "Ship on." (Tier A). Off: TwoHandPoll and TwoHandAdjustCtl return at their first line, L1 and both arms are
//      unchanged. HEADSET: needs both GRIP poses, the right AIM pose and the left squeeze. Needs vr_grip=1,
//      item_place_from_controller=1, rig_hand_grip_pose=1 (each REFUSED by name) and the rig (it moves the DRAWN hands, which
//      only exist while the arm solve runs).
bool RigTwoHandGrip = true;
// [DEP: rig_two_hand_gun_radius] required-for=the firearm grab zone's radius around the right AIM ray
//      (RigTwoHandGunRadius), metres. tree=INERT. A GUESS, kept small (Jay: manual reloading must not be grabbed).
float RigTwoHandGunRadius = 0.08f;
// [DEP: rig_two_hand_gun_min] [DEP: rig_two_hand_gun_max] required-for=the firearm grab zone's extent along the right
//      AIM ray (RigTwoHandGunMin / RigTwoHandGunMax), metres ahead of the right grip. tree=INERT. GUESSES.
float RigTwoHandGunMin = 0.10f;
float RigTwoHandGunMax = 0.60f;
// [DEP: rig_two_hand_slide_s] required-for=the settle time into and out of the two-handed hold (RigTwoHandSlideS),
//      seconds, 0 = instant. tree=INERT.
float RigTwoHandSlideS = 0.15f;
// [DEP: rig_two_hand_pin] required-for=pinning the DRAWN left hand to the gun's holderl while gripping (RigTwoHandPin) -
//      phase 1b, Jay 2026-10-09 ("Game's handguard point"). tree=INERT unless rig_two_hand_grip=1. 0 = phase 1's behaviour
//      (left hand at its controller, rotation locked to the gun) - the A/B switch. HEADSET.
bool RigTwoHandPin = true;
// [DEP: rig_two_hand_pin_radius] required-for=the grab zone around the gun's holderl (RigTwoHandPinRadius), metres; a gun
//      with no holderl / holder_l falls back to rig_two_hand_gun_radius/min/max. tree=INERT. A GUESS (run 1: grabs 0.054-
//      0.080, near-misses 0.088-0.148 - measured from the AIM ray, not holderl).
float RigTwoHandPinRadius = 0.10f;
// [DEP: rig_two_hand_haptic] required-for=the two-handed grip's controller pulses - near-grab hint, grab, each shot
//      (RigTwoHandHaptic), 0..1, 0 = none. tree=INERT. Jay 2026-10-09. The per-shot pulse rides BulletInitHook's firearm
//      branch, so it NEEDS fire_barrel_origin=1 (on, Tier A); a shotgun's pellets each call it (one pending pulse, but the
//      log's "shots" counts pellets).
float RigTwoHandHaptic = 0.5f;
// [DEP: rig_two_hand_palm] required-for=the pin holding the left PALM centre on holderl instead of l_handholder (RigTwoHandPalm)
//      - INV-502 (a), Jay 2026-10-09: "fingers are essentially the 'grip point' instead of the palm". tree=INERT unless
//      rig_two_hand_grip=1 and rig_two_hand_pin=1. 0 = run 3's behaviour (the knuckle-line handholder on holderl) - the A/B.
//      HEADSET. Was Tier C until Jay verified it; Tier A (INV-503).
bool RigTwoHandPalm = true;
// [DEP: rig_two_hand_wrist_overflow_deg] required-for=the wrist overflow WHILE TWO-HANDING (RigTwoHandWristOverflowDeg), deg -
//      INV-503: gripping, the right wrist asked at most 17.0 deg past limit+60 (ulnar; 98.5% under 10), and the gun follows
//      the clamped hand ("stiff"). Used as max(rig_wrist_overflow_deg, this), capped 90 (flex 85+90 = 175 < 180, D-RIG-4).
//      Not gripping: rig_wrist_overflow_deg alone, unchanged. tree=LOADBEARING while gripping (it is the clamp). HEADSET.
//      80 = 60 + the measured 17 + 3. Was Tier C until Jay verified it; Tier A (INV-505).
float RigTwoHandWristOverflowDeg = 80.0f;
// [DEP: rig_two_hand_forend] required-for=moving the pin point / grab zone from holderl to the centre of the gun's forend
//      (RigTwoHandForend) - INV-502 (c) / INV-503, per mesh from tools/twohand_forend.py (twohand_forend.inc: shotguns a, b,
//      c, f; rifles are NOT in it - their placement was verified as is). Each offset is applied only after its runtime
//      self-check (the barrel element predicted from holderl lands within 0.01 m of the real one). tree=INERT unless
//      rig_two_hand_grip=1. 0 = holderl as the game authored it - the A/B. HEADSET. Was Tier C; now Tier B (loadbearing for the verified shotgun grip, INV-658).
bool RigTwoHandForend = true;
// [DEP: rig_two_hand_rifle_pose] required-for=the left hand taking the STANDARD rotation on a two-handed RIFLE
//      (RigTwoHandRiflePose) instead of the controller's grab-latched one - INV-506, Jay 2026-10-09: run 6's last grip "is the
//      angle that the hand & wrist SHOULD be"; run 7 grab 11 captured ("11 was probably the better"). Rifles only (mesh
//      wn_rifle*); shotguns get their own capture later. Blended in by the grip's t, like the pin. tree=LOADBEARING while
//      gripping a rifle (it sets the drawn hand's rotation). Needs rig_two_hand_pin=1 (it rides the pin). HEADSET. 0 = the
//      grab-latched rotation - the A/B. Was Tier C until Jay verified it; Tier A (INV-509).
//      2026-10-30 (6): ALSO the SHOTGUNS (wn_shotgun_a / b / c / f, twohand_shotgun.inc, tools/twohand_shotgun.py) - Jay picked each
//      left hand by eye on renders ("pump_c_D_game - Most accurate, use it", "We'll go with E3", "They should be fine") and the
//      thumb ("pump_c_D_t_m10_m20_0_0"): the standard rotation + the palm offset from the forend pin, and l_finger01 bent by
//      that row's thumb (+ since the r7 contact fit, "That all looks good": fingers 1-4 curled per row until they touch the gun,
//      palm on the surface, per-gun thumb). Measured in the FOREND frame, so only applied once that item's forend self-check passed (needs
//      rig_two_hand_forend=1; refused by name otherwise). The thumb needs rig_subtree_rest=1 and rig_subtree_fingers=1 (it
//      writes through the left finger pins, TwoHandFingersApply). No new key (Jay: fix without a new key). tree=LOADBEARING
//      while gripping a shotgun. HEADSET. Not verified in the headset.
bool RigTwoHandRiflePose = true;
// [DEP: rig_two_hand_slide_len] [DEP: rig_two_hand_zone_x] required-for=the RIFLE grab area as a strip along the handguard
//      and the pin landing WHERE the hand grabbed on it (RigTwoHandSlideLen = the strip's full length along holderl's z, centred
//      on holderl, metres; RigTwoHandZoneX = its reach across the gun, metres; up/down and the ends use
//      rig_two_hand_pin_radius). Jay 2026-10-09: "have the handguard as the grab area & PIN the hand wherever they grab on the
//      handguard, with the same wrist/hand values"; "Only half the length of the handguard"; X "very slightly" wider. 0.13 =
//      half of wn_rifle_b_reddot's handguard piece (z +0.213..+0.469, tools/twohand_forend.py) - applied to EVERY rifle (the
//      other rifles' handguards were not separable). Rifles only. slide_len 0 = the single pin point. tree=LOADBEARING while
//      gripping a rifle (it moves the pin). HEADSET. Was Tier C until Jay verified it; Tier A (INV-509).
float RigTwoHandSlideLen = 0.13f;
float RigTwoHandZoneX = 0.12f;
// [DEP: rig_two_hand_pin_reach] required-for=the pinned left hand REACHING its pin target (RigTwoHandPinReach) - INV-507: the
//      target is on the gun, already in the rig's reach space, and rig_reach_scale 0.9 + rig_reach_soft pulled it toward the
//      shoulder again (palm 5-10 cm short along the barrel; Jay: the hand "sat in roughly the same spot" front and back). On:
//      SolveArmIk blends the reach to the raw distance by the pin's t; the hard cap (arm length - rig_reach_margin_mm) stays.
//      Left pinned solves only. tree=LOADBEARING while pinned. HEADSET. 0 = runs 7/8 - the A/B. Was Tier C until Jay verified it; Tier A (INV-509).
bool RigTwoHandPinReach = true;
// [DEP: rig_two_hand_zone_drawn] required-for=the grab zone testing the DRAWN left palm centre (RigTwoHandZoneDrawn) instead of
//      the raw controller grip point - INV-507: the gun is in the rig's (reach-scaled) space, the controller is not, so the
//      controller read >= 6.5 cm forward (17 of 20 grabs at the strip's front end). Needs the left snapshot (rig_left_arm=1);
//      without it the controller point is used. All two-handed guns. tree=INERT unless rig_two_hand_grip=1. HEADSET. 0 = the
//      controller point - the A/B. Was Tier C until Jay verified it; Tier A (INV-509).
bool RigTwoHandZoneDrawn = true;
// [DEP: rig_two_hand_rifle_palm_x_cm] [DEP: rig_two_hand_rifle_palm_y_cm] required-for=the rifle STANDARD palm position across
//      (x) and below (y) the handguard grip line, in holderl's frame, cm (RigTwoHandRiflePalmXCm / YCm) - INV-508: Jay's approved
//      grab 11 (run 7) was drawn with the palm +2.0 cm across and -1.2 cm below the pin target (steady median (+0.020, -0.012,
//      -0.053) m); run 9 put the palm exactly ON the target and "part of the hand is clipping again". Along the barrel stays the
//      grab spot (the strip). Rifles only, with rig_two_hand_rifle_pose=1. Jay: "Do it". tree=LOADBEARING while gripping a
//      rifle (moves the pin). HEADSET. 0/0 = run 9. Was Tier C; now Tier D (tuning).
float RigTwoHandRiflePalmXCm = 2.0f;
float RigTwoHandRiflePalmYCm = -1.2f;
// [DEP: rig_two_hand_melee] required-for=two-handed MELEE (RigTwoHandMelee) - phase 2, SPEC-TWO-HANDED-GRIP-2026-10-09 section 7:
//      hold left grip on the shaft below the right hand -> the weapon slides so its Holder (the swing pose, INV-499) comes to
//      the right hand, the left hand stacks below it with the mirrored grip (S4). The right controller aims, as today. Items =
//      WeaponVis with the desc TwoHanded flag (slot 176, INV-498), chainsaw excluded (Jay: its own case). tree=LOADBEARING while
//      gripping melee (it moves the weapon and the left hand). Needs rig_two_hand_grip=1 and rig_two_hand_pin=1 (the stack rides
//      the pin; refused by name). HEADSET. 0 = melee refused by name, as phase 1. Was Tier C until Jay verified it; Tier A (INV-517).
bool RigTwoHandMelee = true;
// [DEP: rig_two_hand_melee_radius] [DEP: rig_two_hand_melee_butt_cm] required-for=the melee grab strip (RigTwoHandMeleeRadius =
//      its radius around the shaft, metres; RigTwoHandMeleeButtCm = its lower end along the shaft from holder, cm) - Jay's
//      choice (a): from the butt up to the right hand. -16 = the deepest butt in the mesh data (sledgehammer -0.158, axe -0.104,
//      INV-499); a slider because Jay: "as long as we can adjust it if need be". tree=INERT unless rig_two_hand_grip=1 and
//      rig_two_hand_melee=1. Radius 0.10 is a GUESS (= rig_two_hand_pin_radius's default). HEADSET. Tier D.
float RigTwoHandMeleeRadius = 0.10f;
float RigTwoHandMeleeButtCm = -16.0f;
// [DEP: rig_two_hand_melee_stack_cm] required-for=the left palm centre's distance below Holder along the shaft
//      (RigTwoHandMeleeStackCm), cm - Jay's approved S4 render (wmirror:-0.09) = 9.0. tree=LOADBEARING while gripping melee (moves
//      the pin). HEADSET. Tier D.
float RigTwoHandMeleeStackCm = 9.0f;
// [DEP: rig_two_hand_melee_curl_deg] required-for=the bend added at every joint of left fingers 1-4 while gripping melee
//      (RigTwoHandMeleeCurlDeg), degrees - Jay: "Go with 30 for the time being". tree=LOADBEARING while gripping melee (the drawn
//      fingers). Needs rig_subtree_rest=1 and rig_subtree_fingers=1 (TwoHandFingersApply writes through those pins). HEADSET.
//      Tier D. 20 since INV-515 (Jay picked grip D: the fingers open enough to wrap the handle at the finger bases, 0 clipping).
float RigTwoHandMeleeCurlDeg = 20.0f;
// [DEP: rig_two_hand_melee_wrist_cm] required-for=moving the gripping melee left hand toward its WRIST so the handle sits across
//      the base of the fingers (RigTwoHandMeleeWristCm), cm - INV-515, Jay: "that's exactly how it should sit"; grip D = 3.5 with
//      curl 20 and thumb -60,0,30,30 (wrap_search2.py: near side 3.5 mm, far-side wrap 1.6 mm, 0 clipping on wn_axe_d). Live.
//      0 = run 4 / 5's placement (handle at the heel of the palm). tree=LOADBEARING while gripping melee (moves the pin). HEADSET.
//      Tier D (tuning) once Jay settles it.
float RigTwoHandMeleeWristCm = 3.5f;
// [DEP: rig_two_hand_melee_pose] required-for=the gripping melee left hand taking the S4 STANDARD rotation (RigTwoHandMeleePose) -
//      INV-510: drawn == the standard to 0.0 deg, but Jay: shaft off-centre in the fist, slight clipping, wrist bent toward the
//      shaft. 0 = CAPTURE MODE (Jay 2026-10-09 "Go with A"): the left hand stays pinned on the shaft but takes the left
//      controller's own live rotation, so HANDPOSE logs the grip Jay holds with straight wrists - the rifle route (INV-506).
//      tree=LOADBEARING while gripping melee (sets the drawn hand's rotation). HEADSET. Was Tier C (a capture A/B switch); now Tier B (loadbearing for the verified two-handed grips).
bool RigTwoHandMeleePose = true;
// [DEP: rig_two_hand_melee_roll_deg] required-for=rolling the gripping melee left hand about the shaft (RigTwoHandMeleeRollDeg),
//      degrees - INV-514, run 4: Jay "the left hand is still too far forward" (away from his body), "Try 4cm". The whole hand
//      (rotation and palm contact) turns rigidly about the shaft toward run 2's capture position (+62.9 deg away); 60 = a 3.7 cm arc
//      at the palm. 0 = run 4's placement. Live (read every update). tree=LOADBEARING while gripping melee (moves and turns the
//      hand). HEADSET. Tier D (tuning). DEFAULT 0 since INV-515: run 5 swept -90..90 and none fixed it - the fault was where the
//      handle sits IN the hand (rig_two_hand_melee_wrist_cm), which a roll cannot change. Kept (a shipped key, D-156).
float RigTwoHandMeleeRollDeg = 0.0f;
// [DEP: rig_two_hand_tune_x_cm] [DEP: rig_two_hand_tune_y_cm] [DEP: rig_two_hand_tune_z_cm] [DEP: rig_two_hand_tune_rx_deg]
// [DEP: rig_two_hand_tune_ry_deg] [DEP: rig_two_hand_tune_rz_deg] required-for=LIVE TUNING of where a gripped two-handed MELEE weapon
//      sits in the right hand (run R6, Jay on the guitar: live sliders "but add rotation for both axis and horizontal aswell") - the
//      weapon moved along the right hand-bone frame's x / y / z (cm) and turned about them (deg) at the right handholder, blended in by
//      the grip's t, applied in TwoHandMeleeRel to whichever melee weapon is gripped; the left hand rides the weapon (its pin). Read
//      every update (live from the BETA menu). The values are baked into twohand_grip.inc per weapon once Jay sets them. tree=
//      LOADBEARING while gripping melee with any value non-zero (moves the weapon). HEADSET. Tier D (debug tuning) - shipped 0.
float RigTwoHandTuneXCm = 0.0f;
float RigTwoHandTuneYCm = 0.0f;
float RigTwoHandTuneZCm = 0.0f;
float RigTwoHandTuneRxDeg = 0.0f;
float RigTwoHandTuneRyDeg = 0.0f;
float RigTwoHandTuneRzDeg = 0.0f;
// [DEP: rig_two_hand_aim_share] required-for=how much of a two-handed FIREARM's aim comes from the line between the hands
//      (RigTwoHandAimShare), 0..1 - 2026-11-08, Jay after RUN TH1 ("very restrictive", "Hard to move, gun was flipping in the
//      hands"; "make it fluent and not super restrictive"): the right wrist aims the gun as it does one-handed and the hand line
//      pulls the barrel toward itself by this share (twohand_logic.h GunTargets). 0 = the wrist alone (the left hand only holds
//      on), 1 = the hand line alone (the old aim, without its flip). Read every solve (live from the menu). tree=LOADBEARING
//      while gripping a firearm (it sets the drawn right hand's rotation, which the gun and the shot follow). HEADSET.
//      Tier D (feel tuning). 0.75 since 2026-11-08 = Jay's disk (RUN TH3: "0.75 was better than .5"; "Is the two-handed aim done at
//      0.75?" -> "Yes"). The twist-rolls-the-barrel variant was ruled out and rolled back (D-335).
//      LOCKED AT 0.75 2026-11-09 (Jay: "lock & hide the setting for the left hand follow on two handing firearms") -
//      S_FLOAT_LOCKED: the file value is never read, no menu row.
float RigTwoHandAimShare = 0.75f;
// [DEP: diag_twohand_record] required-for=the per-frame two-hand recorder [TWOHANDREC] (DiagTwoHandRecord): while a firearm
//      is gripped, every right-arm TwoHandAdjustCtl call appends its inputs (both grip poses, the blend t, the share) and its
//      output rotation to a buffer; each hold is written to dlvr\twohand_rec.bin when the grip is fully released, with the grab
//      latch, so tools/twohand_replay.py can replay the old and the new aim on Jay's real motion. tree=PROBE-ONLY (log-only:
//      it never changes a pose). HEADSET (needs the rig's arm solve and a two-handed grip). Default OFF.
bool DiagTwoHandRecord = false;

namespace {

#include "twohand_meshes.inc"   // kTwoHandedMeshes[N], kTwoHandedAmbiguous[N] - tools/twohand_meshes.py
#include "twohand_forend.inc"   // kTwoHandForend[N] - tools/twohand_forend.py
#include "twohand_melee.inc"    // kMeleeLeftStdQ, kMeleeLeftPalm0, kMeleeCurlAxis, kMeleeShaftAxis, kMeleeFingers[N]
#include "twohand_butt.inc"     // kMeleeButts[N] - tools/twohand_melee_butt.py (INV-510)
#include "twohand_grip.inc"     // kMeleeGrips[N] - tools/twohand_grip.py (INV-644, Jay-approved renders)
#include "twohand_shotgun.inc"  // kShotgunStd[N] (+ curl / thumb), kShotgunThumbRest - tools/twohand_shotgun.py (Jay-approved renders)

// The per-mesh butt row for the classified / held item (-1 = none). Classify writes g_classButt; NoteHeld publishes g_heldButt
// (both HookTail thread). FeedMelee writes g_meleeLift; TwoHandMeleeRel reads it (same thread).
int             g_classButt = -1;
int             g_heldButt = -1;
// INV-644: the per-mesh grip row (twohand_grip.inc, -1 = none) - same threads and publication as the butt row. With a row, FeedMelee
// takes this weapon's own right / left positions, handle end and (no left node) shaft; without one, the stack / lift sliders as before.
int             g_classGrip = -1;
int             g_heldGrip = -1;
float           g_meleeLift = 0.0f;
// INV-514: the melee standard rolled about this item's shaft by rig_two_hand_melee_roll_deg - FeedMelee writes (HookTail), the left
// solve and the poll's HANDPOSE line read; under g_lock.
twohand::Q4     g_meleeStdQ = { 0.6010f, 0.0512f, 0.7965f, -0.0410f };

// INV-512: the melee STANDARD left hand = Jay's captured grab 5 (run 2, INV-511: wn_axe_d, 15.6 s, last-3 spread 2.0 deg) in the
// Holder frame - Jay: "g5_t5 is the angle" (renders docs/media/twohand_s5_capture). Replaces the S4 mirrored rotation
// (kMeleeLeftStdQ, kept in twohand_melee.inc for the record), 28.4 deg from it.
const twohand::Q4 kMeleeStd = { 0.6010f, 0.0512f, 0.7965f, -0.0410f };
// INV-513: Jay "make sure the left palm is in contact with the weapon's shaft". The palm faces -z of the hand (run 3 HANDPOSE palm
// normal (-0.953 -0.131 -0.272) in the Holder frame), so the palm CENTRE goes (shaft radius + kMeleePalmDepth) out from the axis
// along +z (twohand::MeleePalmTarget). Build 13dbca81's "fist channel" offset moved the hand 5.8 cm the OTHER way and put the shaft
// on the back of the hand (Jay, run 3) - removed. Radius per mesh = the shaft's palm-side thickness (twohand_butt.inc); a mesh
// with no row uses kMeleeRadiusDefault (between axe_d 0.0151 and pickaxe_a 0.0205).
constexpr float kMeleeRadiusDefault = 0.018f;
// Phase 2 melee. FeedMelee / TwoHandMeleeRel run on the HookTail thread; the poll thread reads the volatile ones (and, for its
// 1 Hz log only, the plain floats - a torn float there is a log value, never a decision).
uintptr_t       g_meleeItem = 0;          // the item the last READY / REFUSED line was printed for
const char*     g_meleeWhy = "";          // why this item's melee grip is refused ("" = ready)
const char*     g_meleeWhyP = nullptr;    // the raw reason pointer last logged (nullptr = ready) - the change test
float           g_meleeSlide = 0.0f;      // left node - Holder along the shaft, this item, metres (runtime)
float           g_meleeOffDeg = 0.0f;     // the shaft's angle from kMeleeShaftAxis, this item
float           g_meleeU[3] = { 0.0f, 1.0f, 0.0f };   // the shaft axis in the Holder frame, this item
const char* volatile g_meleeRefuse = "";  // the poll thread's copy of g_meleeWhy
volatile LONG   g_meleeReady = 0;         // 1 = the shaft is usable and the strip was measured this update
volatile LONG   g_meleeAlongUm = 0;       // the left hand on the strip (along the shaft from Holder, micrometres)
volatile LONG   g_meleeTopUm = 0;         // the right handholder along the shaft = the strip's top (micrometres)
uintptr_t       g_mRelItem = 0;           // the item the idle-rel line was printed for
bool            g_mLatched = false;
float           g_mLatR[9] = {}, g_mLatT[3] = {}, g_mLatU[3] = { 0.0f, 1.0f, 0.0f };
float           g_mA = 0.0f;              // the slide this hold applies, metres along the shaft
long            g_mSlides = 0, g_mSlideRefused = 0;
volatile LONG64 g_mRelApplied = 0, g_meleeStdApplied = 0, g_fingerWrites = 0;
volatile LONG   g_fingerBad = 0;          // 1 = a finger row's rest did not match its authored pin - finger pose refused
bool            g_fingerChecked[32] = {}; // per generated row (ApplyDrivenBones' thread)
int             g_fingerCheckedN = 0;
float           g_fingerMaxDiff = 0.0f;

// The forend row for the classified / held item (-1 = none). Classify writes g_classForend; NoteHeld publishes g_heldForend.
int           g_classForend = -1;
int           g_heldForend = -1;
// Per item: the self-check verdict (0 = not checked yet, 1 = passed, -1 = refused), for g_hlItem.
uintptr_t     g_forendItem = 0;
int           g_forendState = 0;
float         g_forendErr = -1.0f;
// HANDPOSE probe - under g_lock. Drawn: HookTail (TwoHandFeedPlaced). Commanded: the left solve (TwoHandLeftPin).
float         g_hpDrawnQ[4] = { 0, 0, 0, 1 }, g_hpPalm[3] = {}, g_hpCmdQ[4] = { 0, 0, 0, 1 };
long long     g_hpDrawnN = 0, g_hpCmdN = 0;
float         g_hpDet = 0.0f;
// INV-506: the STANDARD left hand on a rifle, in the gun's holderl frame - HANDPOSE run 7 grab 11 (wn_rifle_b_reddot, steady
// on every line 16:49:49.9-16:50:01.9): q (+0.2015 -0.5707 +0.7529 +0.2587); distal (-0.785 +0.159 +0.599), palm normal
// (-0.008 +0.964 -0.267). Rifle holderl frames are identity in the .msh nodes (tools/twohand_forend.py), so it carries over.
const twohand::Q4 kRifleLeftStd = { 0.2015f, -0.5707f, 0.7529f, 0.2587f };
LONG          g_classRifle = 0;
volatile LONG g_heldRifle = 0;      // HookTail writes (NoteHeld); the left solve reads
volatile LONG64 g_stdApplied = 0;
// The shotgun standard (twohand_shotgun.inc): the row for the classified / held item (-1 = none; HookTail thread, published like
// g_heldForend), and the row the left solve / finger pass may USE (-1 until this item's forend self-check passed - the standard is
// measured in the forend frame). g_shotThumbBad = the authored thumb pin differs from the rest the bend was built on (session-wide).
int             g_classShot = -1;
int             g_heldShot = -1;
volatile LONG   g_shotReady = -1;
volatile LONG64 g_shotApplied = 0, g_thumbWrites = 0;
volatile LONG   g_shotThumbBad = 0;
bool            g_shotThumbChecked = false;
uintptr_t       g_shotSaidItem = 0;   // the item the shotgun READY / REFUSED line was printed for (HookTail thread)
// The handguard strip: where the left hand is along it (HookTail writes, micrometres, holderl z), and the slide latched at the
// grab (the poll thread writes, the left solve reads - under g_lock).
volatile LONG g_zoneSlideUm = 0;
// INV-507 counters: pinned left solves whose reach was un-mapped, and the summed lengthening (micrometres).
volatile LONG64 g_pinReachN = 0, g_pinReachUm = 0;
volatile LONG   g_zoneDrawnUsed = 0;   // 1 = the last zone update used the drawn palm
float         g_pinSlide = 0.0f;
long          g_forendApplied = 0, g_forendRefused = 0;

using twohand::Kind;
using twohand::State;

// Right-slot classification - HookTail thread only (ItemCtlPlace). The poll thread reads g_kind.
uintptr_t     g_classItem = 0;
LONG          g_classKind = 0;
volatile LONG g_kind = 0;
uintptr_t     g_heldItem = 1;   // never a real pointer: the first NoteHeld always publishes

// Shared with the solve thread (both SolveArmIk sides) under g_lock.
SRWLOCK            g_lock = SRWLOCK_INIT;
State              g_state = State::Free;
twohand::Blend     g_blend = { 0.0, 0.0f, 0 };
twohand::GunLatch  g_latch = {};
float              g_slideS = 0.15f;

// What the hold GRIPPED (review I-2) - published with g_state under g_lock. A path for one kind acts only when the grip is of that
// kind AND (melee slide) the same item, so a direct swap cannot run the new item through the old hold for the frames before the
// poll lets go.
Kind               g_gripKind = Kind::None;
unsigned long long g_gripItem = 0;
volatile LONG64    g_heldItemV = 0;   // g_heldItem for the poll thread (TwoHandNoteHeld writes it)
uintptr_t          g_mLatItem = 0;    // the item the melee slide latch belongs to (HookTail thread)

twohand::Machine g_machine;   // poll thread only

// Session counters (INV-10: every site that produces the event counts it).
long g_polls = 0, g_grabs = 0, g_releases = 0, g_cancels = 0, g_refused = 0, g_latchFail = 0;
volatile LONG64 g_adjR = 0, g_adjL = 0, g_heldDir = 0, g_heldUp = 0, g_failed = 0, g_noOther = 0, g_notGrip = 0;
double g_grabAt = 0.0;
// The last applied share and swing (right arm), for the 1 Hz line - under g_lock.
float g_lastShare = 0.0f, g_lastSwingDeg = 0.0f;

// [TWOHANDREC] the per-frame recorder (diag_twohand_record). Rows are appended by the right arm's TwoHandAdjustCtl under g_lock
// (the solve rotates over the job threads - the lock, not the thread, owns the buffer); the poll thread swaps the buffer out under
// the same lock once the grip is fully released and writes it outside the lock. Layout (little-endian, packed) per hold:
// RecHold, then RecHold.rows x RecRow. COST per recorded solve: one 96-byte copy under the lock the call already holds.
#pragma pack(push, 1)
struct RecHold {
    char   magic[4];                    // "THR1"
    int    grab;                        // g_grabs at the latch
    double grabAt;                      // NowS() at the latch
    float  rgp[3], rgq[4], lgp[3], lgq[4];   // the poll's right / left grip poses the latch was taken from
    float  lineLocal[3], lRel[4];       // the latch itself
    float  share;                       // rig_two_hand_aim_share when the hold was written
    int    rows, dropped;
};
struct RecRow {
    double t;                           // NowS()
    float  rp[3], rq[4];                // the right grip pose the solve was given (steadied), before the rewrite
    float  lp[3], lq[4];                // the left grip pose GetControllerGripPoseSteady returned
    float  blendT, share;               // the settle blend t and the setting read this call
    float  oq[4];                       // the rotation written back (Slerp(right, target, t))
    float  swingDeg, flags;             // the line's pull this call; flags 1 = hands together, 2 = line points back
};
#pragma pack(pop)
static_assert(sizeof(RecRow) == 96 && sizeof(RecHold) == 112, "tools/twohand_replay.py reads these exact layouts");
static_assert(offsetof(RecHold, lRel) + 4 * sizeof(float) - offsetof(RecHold, rgp) == 21 * sizeof(float),
              "the latch fills rgp .. lRel as one block of 21 floats");
constexpr int kRecRows = 16384;         // ~110 s of one hold at the right arm's ~140 solves / s; past it rows are counted, not kept
RecRow  g_recBuf[2][kRecRows];
int     g_recCur = 0, g_recN = 0, g_recDropped = 0;
volatile LONG g_recAny = 0;             // 1 = rows wait for RecFlush - the poll's lock-free test, so an idle poll takes no lock
RecHold g_recHold = {};                 // filled at the latch (poll thread, under g_lock)
long    g_recHolds = 0, g_recFail = 0;  // poll thread only
long long g_recRowsAll = 0;
char    g_recPath[MAX_PATH] = "";

bool RecPathInit()
{
    if (g_recPath[0]) { return true; }
    char p[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&RecPathInit), &self);
    const DWORD n = GetModuleFileNameA(self, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { return false; }
    char* slash = strrchr(p, '\\');
    if (!slash) { return false; }
    slash[1] = '\0';
    if (strcat_s(p, "dlvr\\") != 0) { return false; }
    CreateDirectoryA(p, nullptr);
    if (strcat_s(p, "twohand_rec.bin") != 0) { return false; }
    strcpy_s(g_recPath, p);
    return true;
}

// Poll thread, once the machine is Free: take the finished hold's rows (buffer swap under g_lock) and append them to the file.
void RecFlush()
{
    if (InterlockedCompareExchange(&g_recAny, 0, 0) == 0) { return; }
    AcquireSRWLockExclusive(&g_lock);
    const int n = g_recN, dropped = g_recDropped, buf = g_recCur;
    RecHold h = g_recHold;
    if (n > 0 || dropped > 0) {
        g_recCur ^= 1;
        g_recN = 0;
        g_recDropped = 0;
    }
    InterlockedExchange(&g_recAny, 0);
    ReleaseSRWLockExclusive(&g_lock);
    if (n <= 0 && dropped <= 0) { return; }
    std::memcpy(h.magic, "THR1", 4);
    h.rows = n;
    h.dropped = dropped;
    h.share = RigTwoHandAimShare;
    ++g_recHolds;
    g_recRowsAll += n;
    FILE* f = nullptr;
    const bool pathOk = RecPathInit();
    bool ok = pathOk && fopen_s(&f, g_recPath, "ab") == 0 && f;
    if (ok) {
        ok = fwrite(&h, sizeof(h), 1, f) == 1 &&
             (n == 0 || fwrite(g_recBuf[buf], sizeof(RecRow), static_cast<size_t>(n), f) == static_cast<size_t>(n));
        ok = (fclose(f) == 0) && ok;
    }
    if (!ok) { ++g_recFail; }
    // [PROBE: TWOHANDREC] gate=diag_twohand_record + rig_two_hand_grip  site=HEADSET  log-only=yes
    LOG("twohand: [TWOHANDREC] hold %ld (grab %d): rows %d, dropped past the buffer %d, share %.2f -> %s %s (row %u bytes, "
        "header %u) | session: holds %ld, rows %lld, write failures %ld",
        g_recHolds, h.grab, n, dropped, h.share, ok ? "WRITTEN to" : "*** WRITE FAILED *** for",
        pathOk ? g_recPath : "(the dlvr path could not be built)", static_cast<unsigned>(sizeof(RecRow)),
        static_cast<unsigned>(sizeof(RecHold)), g_recHolds, g_recRowsAll, g_recFail);
}

// Phase 1b - holderl relative to the right snapshot frame, per item (HookTail writes; the left solve reads - under g_lock).
uintptr_t       g_hlItem = 0;      // the item the latch belongs to (0 = none)
twohand::Xf     g_hlRel = {};
const char*     g_hlName = "";
bool            g_hlSaid = false;
// The grab zone around holderl - HookTail writes, the poll thread reads.
volatile LONG   g_zoneValid = 0;   // 1 = this item has holderl and the left grip point was read this update
volatile LONG   g_zoneIn = 0;
volatile LONG   g_zoneDistMm = -1;
twohand::SphereZoneState g_zoneState = {};
volatile LONG   g_gripping = 0;    // mirror of g_state == Gripped, for the hooks (interlocked, lock-free)
// Per-arm wrist clamp while gripping (INV-500 addendum: which wrist?).
volatile LONG64 g_wCalls[2] = {}, g_wPast[2] = {}, g_wStill[2] = {};
// INV-502 (b) measurement: per arm, still-clamped solves binned by how far past the overflowed limit they asked (the worst of
// the four limits; twohand::ExcessBin), and the max asked past each limit [flex, ext, ulnar, radial] in centidegrees.
volatile LONG64 g_wEx[2][twohand::kExcessBins] = {};
volatile LONG   g_wExMax[2][4] = {};
// The left palm centre minus l_handholder, in the l_hand bone frame - INV-502 (a), measured on our player_6_fpp rest pose
// (common_cod_2_PC): l_handholder (+0.0962, 0, -0.0238); 111 palm vertices (dominant l_hand / l_hand1) centroid x +0.0502
// y +0.0040, palmar surface z p5 -0.0265 (palmar = -z: the curled fingertips go there). The left bone only.
const float kPalmFromHandholder[3] = { -0.0460f, 0.0040f, -0.0027f };
volatile LONG64 g_pinned = 0, g_pinRefused = 0;
const char* volatile g_pinWhy = "";
twohand::RateGate g_hintGate;      // holster_logic.h-style gate
long g_hints = 0;                  // poll thread only
volatile LONG g_shotsL = 0;        // incremented from the shot hook's thread

double NowS()
{
    static const double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * inv;
}
twohand::Pose ToPose(const XrPosef& p)
{
    return { { p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w },
             { p.position.x, p.position.y, p.position.z } };
}
const char* KindName(Kind k) { return k == Kind::Firearm ? "FIREARM" : (k == Kind::Melee ? "MELEE" : "NONE"); }

void Publish(bool newLatch, const twohand::GunLatch& latch)
{
    AcquireSRWLockExclusive(&g_lock);
    g_state = g_machine.state();
    g_blend = g_machine.blend();
    g_gripKind = g_machine.heldKind();
    g_gripItem = g_machine.heldItem();
    InterlockedExchange(&g_gripping, g_state == State::Gripped ? 1 : 0);
    g_slideS = RigTwoHandSlideS;
    if (newLatch) { g_latch = latch; }
    ReleaseSRWLockExclusive(&g_lock);
}
void LetGo(const char* why)
{
    if (g_machine.state() == State::Free) { return; }
    g_machine.Reset();
    Publish(false, g_latch);
    ++g_cancels;
    // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
    LOG("twohand: [TWOHAND] LET GO - %s. The grip is released, L1 is sent normally again.", why);
}

// HANDPOSE probe, melee: the DRAWN left hand and its palm centre in frame hl - the rifle block in TwoHandFeedPlaced, repeated
// rather than refactored so the verified firearm path is not touched.
void NoteDrawnHand(const twohand::Xf& hl, const float leftDrawn[9], const float leftAchieved[3])
{
    twohand::Xf fr = hl;
    twohand::Ortho3(hl.R, fr.R);
    float hand[9];
    if (!twohand::Ortho3(leftDrawn, hand)) { return; }
    float palmW[3];
    for (int r = 0; r < 3; ++r) {
        palmW[r] = leftAchieved[r] + hand[3 * r] * kPalmFromHandholder[0] + hand[3 * r + 1] * kPalmFromHandholder[1] +
                   hand[3 * r + 2] * kPalmFromHandholder[2];
    }
    float q[4], p[3];
    twohand::HandInFrame(fr, hand, palmW, q, p);
    const float* m = leftDrawn;
    const float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    AcquireSRWLockExclusive(&g_lock);
    g_hpDet = det;
    for (int i = 0; i < 4; ++i) { g_hpDrawnQ[i] = q[i]; }
    for (int i = 0; i < 3; ++i) { g_hpPalm[i] = p[i]; }
    g_hpDrawnN++;
    ReleaseSRWLockExclusive(&g_lock);
}

// Phase 2 (spec 7.2 items 1, 2, 4): after ItemCtlPlace placed the melee weapon from the right hand - the shaft check, the stack
// pin target and the grab strip. The pin target is re-latched EVERY update, gripping or not: the weapon follows the right hand
// only, so nothing feeds back (the rifle freezes its latch because there the gun follows the LEFT hand too). The shaft is the
// item's own axis u in the Holder frame (twohand::MeleeShaft - the holder node is tilted in the mesh), never Holder +Y.
void FeedMelee(bool haveRight, const twohand::Xf& right, bool haveHolder, const float holder34[12], bool haveHl,
               const float hl34[12], const char* hlName, bool haveLeft, const float leftGripWorld[3], bool haveLeftDrawn,
               const float leftDrawn[9], const float leftAchieved[3])
{
    InterlockedExchange(&g_zoneValid, 0);
    InterlockedExchange(&g_meleeReady, 0);
    InterlockedExchange(&g_zoneSlideUm, 0);   // melee never slides the pin along a strip (the rifle's handguard feature)
    twohand::Xf ho{};
    bool holderOk = false;
    if (haveHolder) {
        const twohand::Xf raw = twohand::XfFrom34(holder34);
        ho = raw;
        holderOk = twohand::Ortho3(raw.R, ho.R);
    }
    twohand::ShaftCheck sc{};
    // INV-644: this weapon's grip row (twohand_grip.inc) - Jay's per-weapon placement; a weapon with no left node grabs through it
    const bool gripRow = g_heldGrip >= 0;
    const char* why = !RigTwoHandMelee ? "rig_two_hand_melee=0"
                    : !RigTwoHandPin   ? "rig_two_hand_pin=0 - the stacked left hand rides the pin"
                    : !haveRight       ? "the right grip snapshot is not ready"
                    : !haveHolder      ? "the weapon's \"Holder\" world could not be read"
                    : !holderOk        ? "the Holder basis is degenerate"
                    : (!haveHl && !gripRow) ? "the weapon has no holderl / holder_l element and no grip row - its slide distance is "
                                              "unknown (no guess)"
                                       : nullptr;
    if (!why) {
        if (haveHl) {
            const float lw[3] = { hl34[3], hl34[7], hl34[11] };
            sc = twohand::MeleeShaft(ho, lw, kMeleeShaftAxis);
            if (!sc.ok) { why = "the left node is more than 15 deg off the known shaft axis, or below holder - this mesh's shaft is unknown"; }
        } else {
            sc = twohand::MeleeShaftFromRow(kMeleeGrips[g_heldGrip].u, kMeleeShaftAxis);
            if (!sc.ok) { why = "the grip row's shaft is degenerate"; }
        }
    }
    const float stackM = RigTwoHandMeleeStackCm * 0.01f;
    const float* u = why ? kMeleeShaftAxis : sc.u;
    // [LOADBEARING: rig_two_hand_melee_butt_cm] INV-510: this weapon's own butt (twohand_butt.inc) - the grab strip's floor (the
    // slider can raise it) and the lift that keeps the stacked left palm on a short handle (both hands move up together).
    // INV-644: a grip row replaces all three - its handle end is the floor, its right position the lift, its left position the pin.
    const bool buttKnown = gripRow || g_heldButt >= 0;
    const float butt = gripRow ? kMeleeGrips[g_heldGrip].butt : (g_heldButt >= 0 ? kMeleeButts[g_heldButt].butt : 0.0f);
    const float palmAlong = kMeleeLeftPalm0[0] * u[0] + kMeleeLeftPalm0[1] * u[1] + kMeleeLeftPalm0[2] * u[2] - stackM;
    const float lift = gripRow ? kMeleeGrips[g_heldGrip].right : twohand::MeleeLift(buttKnown, butt, palmAlong);
    const float leftAlong = gripRow ? kMeleeGrips[g_heldGrip].left : palmAlong + lift;
    const float floorA = twohand::MeleeFloor(buttKnown, butt, RigTwoHandMeleeButtCm * 0.01f);
    // INV-513: the palm centre at `leftAlong` up the shaft, resting the palm on the shaft surface (MeleePalmTarget).
    // [LOADBEARING: rig_two_hand_melee_roll_deg] INV-514: the whole hand rolled about the shaft - its +z (where the palm sits) and
    // the shaft's radius in that direction (twohand_butt.inc, the handle can be oval) both taken at the rolled angle.
    float stdM[9];
    twohand::QToMat3(kMeleeStd, stdM);
    const float zStd0[3] = { stdM[2], stdM[5], stdM[8] };   // the standard hand's +z in the Holder frame
    const float roll = RigTwoHandMeleeRollDeg;
    float zStd[3];
    twohand::RotateAboutAxis(zStd0, u, roll, zStd);
    // run R4 (Jay: "Guitar is slightly clipping into my hands"): no butt row -> this grip row's own palm-side radius when it was
    // measured (the guitar neck 0.035 m), else the default as before
    const float radius = g_heldButt >= 0 ? twohand::MeleeRadiusAt(kMeleeButts[g_heldButt].radius, roll)
                       : (gripRow && kMeleeGrips[g_heldGrip].radius[0] > 0.0f) ? twohand::MeleeRadiusAt(kMeleeGrips[g_heldGrip].radius, roll)
                                                                                : kMeleeRadiusDefault;
    float pTarget[3];
    twohand::MeleePalmTarget(u, leftAlong, radius, zStd, pTarget);
    const twohand::Q4 stdRolled = twohand::QNorm(twohand::QMul(twohand::QAxisAngle(u, roll), kMeleeStd));
    // [LOADBEARING: rig_two_hand_melee_wrist_cm] INV-515: the hand slides toward its wrist so the handle sits across the finger bases
    {
        float rm[9];
        twohand::QToMat3(stdRolled, rm);
        const float xHand[3] = { rm[0], rm[3], rm[6] };
        float sh[3];
        // run R5 (Jay, the guitar: "IT NEEDS TO MOVE TOWARDS THE FINGERS"): the row's fingers grows the wrist shift - the handle moves
        // that much further toward the left hand's fingers (TwoHandMeleeRel moves the weapon toward the right hand's by the same)
        twohand::MeleeWristShift(xHand, u, RigTwoHandMeleeWristCm + (gripRow ? kMeleeGrips[g_heldGrip].fingers * 100.0f : 0.0f), sh);
        for (int i = 0; i < 3; ++i) { pTarget[i] += sh[i]; }
    }
    if (g_meleeItem != g_heldItem || why != g_meleeWhyP) {   // pointer compare of string literals: one line per change
        g_meleeItem = g_heldItem;
        g_meleeWhyP = why;
        // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
        LOG("twohand: [TWOHAND] MELEE %s for item %p - %s | left node '%s' %+.3f m up the shaft, the shaft %.1f deg off the "
            "known axis (INV-499 mesh: axe / sledgehammer 0.347-0.350, bat 0) | shaft in the Holder frame (%+.3f %+.3f %+.3f) | "
            "stack %.1f cm, roll %.0f deg, toward the wrist %.1f cm, palm target (%+.3f %+.3f %+.3f) m in the Holder frame, shaft radius on the palm side %.4f m | butt %s %+.3f m -> both hands lifted %.3f m | "
            "grip row %s: right handholder %+.3f m, left palm %+.3f m along the shaft (INV-644) | "
            "strip radius %.3f m from %+.3f m up to the right hand", why ? "REFUSED" : "READY", reinterpret_cast<void*>(g_heldItem),
            why ? why : "the left hand can grab the shaft below the right hand", haveHl ? (hlName ? hlName : "?") : "(none - grip row)",
            sc.slide, sc.offDeg,
            u[0], u[1], u[2], RigTwoHandMeleeStackCm, roll, RigTwoHandMeleeWristCm, pTarget[0], pTarget[1], pTarget[2], radius,
            gripRow ? kMeleeGrips[g_heldGrip].mesh : (g_heldButt >= 0 ? kMeleeButts[g_heldButt].mesh : "(no table row - slider, no lift)"),
            butt, lift, gripRow ? kMeleeGrips[g_heldGrip].mesh : "(none - the stack / lift sliders)", lift, leftAlong,
            RigTwoHandMeleeRadius, floorA);
    }
    g_meleeLift = why ? 0.0f : lift;
    g_meleeWhy = why ? why : "";
    g_meleeRefuse = g_meleeWhy;
    g_meleeSlide = sc.slide;
    g_meleeOffDeg = sc.offDeg;
    if (why) { return; }
    for (int i = 0; i < 3; ++i) { g_meleeU[i] = sc.u[i]; }
    // [LOADBEARING: rig_two_hand_melee_stack_cm] the stack pin target: the S4 palm point slid stack below Holder along the shaft,
    // in Holder's frame; the hand rotation is applied in TwoHandLeftPin from kMeleeStd in this same frame
    twohand::Xf hl = ho;
    twohand::ForendTarget(ho, pTarget, hl.t);
    twohand::Xf rel{};
    if (twohand::LatchRel(right, hl, &rel)) {
        AcquireSRWLockExclusive(&g_lock);
        g_hlItem = g_heldItem;
        g_hlRel = rel;
        g_hlName = "melee stack";
        g_meleeStdQ = stdRolled;
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (haveLeftDrawn && InterlockedCompareExchange(&g_gripping, 0, 0)) { NoteDrawnHand(hl, leftDrawn, leftAchieved); }
    if (!haveLeft) { return; }
    // [LOADBEARING: rig_two_hand_melee_radius] [LOADBEARING: rig_two_hand_melee_butt_cm] Jay's choice (a): the strip on the shaft
    // from the butt up to the right hand, tested with the DRAWN left palm when rig_two_hand_zone_drawn=1 (INV-507), else the
    // controller grip point.
    float zp[3];
    {
        float hand[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        const bool ok = RigTwoHandZoneDrawn && haveLeftDrawn && twohand::Ortho3(leftDrawn, hand);
        twohand::ZonePoint(ok, leftAchieved, hand, kPalmFromHandholder, leftGripWorld, zp);
        InterlockedExchange(&g_zoneDrawnUsed, ok ? 1 : 0);
    }
    float d[3], top[3];
    twohand::PointInFrame(ho, zp, d);
    twohand::PointInFrame(ho, right.t, top);
    const float topAlong = top[0] * sc.u[0] + top[1] * sc.u[1] + top[2] * sc.u[2];
    float along = 0.0f, norm = 0.0f;
    twohand::MeleeZone(d, sc.u, floorA, topAlong, RigTwoHandMeleeRadius, &along, &norm);
    g_zoneState.in = g_zoneState.in ? (norm <= 1.2f) : (norm < 1.0f);   // SphereZone's hysteresis
    InterlockedExchange(&g_zoneDistMm, static_cast<LONG>(norm * RigTwoHandMeleeRadius * 1000.0f));
    InterlockedExchange(&g_zoneIn, g_zoneState.in ? 1 : 0);
    InterlockedExchange(&g_meleeAlongUm, static_cast<LONG>(along * 1.0e6f));
    InterlockedExchange(&g_meleeTopUm, static_cast<LONG>(topAlong * 1.0e6f));
    InterlockedExchange(&g_zoneValid, 1);
    InterlockedExchange(&g_meleeReady, 1);
}

}  // namespace

void TwoHandFeedPlaced(bool haveRight, const float rightDrawn[9], const float rightAchieved[3], bool haveHl,
                       const float hl34[12], const char* hlName, bool haveLeft, const float leftGripWorld[3],
                       bool haveBarrel, const float barrelWorld[3],
                       bool haveLeftDrawn, const float leftDrawn[9], const float leftAchieved[3],
                       bool haveHolder, const float holder34[12])
{
    if (!RigTwoHandGrip) { return; }
    const LONG heldKind = InterlockedCompareExchange(&g_kind, 0, 0);
    // [LOADBEARING: rig_two_hand_melee] phase 2: a two-handed MELEE weapon takes its own frame / pin / strip; the firearm path
    // below is unchanged.
    if (heldKind == static_cast<LONG>(Kind::Melee)) {
        const twohand::Xf rightM = { { rightDrawn[0], rightDrawn[1], rightDrawn[2], rightDrawn[3], rightDrawn[4], rightDrawn[5],
                                       rightDrawn[6], rightDrawn[7], rightDrawn[8] },
                                     { rightAchieved[0], rightAchieved[1], rightAchieved[2] } };
        FeedMelee(haveRight, rightM, haveHolder, holder34, haveHl, hl34, hlName, haveLeft, leftGripWorld, haveLeftDrawn,
                  leftDrawn, leftAchieved);
        return;
    }
    InterlockedExchange(&g_meleeReady, 0);
    const bool firearm = heldKind == static_cast<LONG>(Kind::Firearm);
    if (!firearm || !haveRight || !haveHl) {
        InterlockedExchange(&g_zoneValid, 0);
        if (firearm && g_heldItem != g_hlItem && !g_hlSaid) {
            g_hlSaid = true;
            // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
            LOG("twohand: [TWOHAND] PIN UNAVAILABLE for item %p - %s. No pin; the grab zone falls back to the AIM ray "
                "(rig_two_hand_gun_radius/min/max).", reinterpret_cast<void*>(g_heldItem),
                !haveRight ? "the right grip snapshot is not ready" : "the gun has no holderl / holder_l element");
        }
        return;
    }
    const twohand::Xf right = { { rightDrawn[0], rightDrawn[1], rightDrawn[2], rightDrawn[3], rightDrawn[4], rightDrawn[5],
                                  rightDrawn[6], rightDrawn[7], rightDrawn[8] },
                                { rightAchieved[0], rightAchieved[1], rightAchieved[2] } };
    twohand::Xf hl = twohand::XfFrom34(hl34);
    // [LOADBEARING: rig_two_hand_forend] INV-502 (c): move the pin point from holderl to the forend centre (per mesh), once the
    // item's self-check passed. The check runs on the first update that has the barrel; its verdict is kept per item.
    if (g_forendItem != g_heldItem) {
        g_forendItem = g_heldItem;
        g_forendState = 0;
        g_forendErr = -1.0f;
    }
    if (RigTwoHandForend && g_heldForend >= 0) {
        const TwoHandForend& row = kTwoHandForend[g_heldForend];
        if (g_forendState == 0 && haveBarrel) {
            twohand::Xf hlO = hl;
            twohand::Ortho3(hl.R, hlO.R);
            g_forendErr = twohand::ForendCheckErr(hlO, row.barrel, barrelWorld);
            g_forendState = g_forendErr <= 0.01f ? 1 : -1;
            if (g_forendState == 1) { ++g_forendApplied; } else { ++g_forendRefused; }
            // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
            LOG("twohand: [TWOHAND] FOREND %s for '%s' (item %p) - self-check: the barrel predicted from holderl lands %.4f m "
                "from the real barrel element (limit 0.0100). Offset (%+.4f %+.4f %+.4f) m in holderl's frame. applied %ld "
                "refused %ld", g_forendState == 1 ? "APPLIED" : "REFUSED - holderl stays as authored", row.mesh,
                reinterpret_cast<void*>(g_heldItem), g_forendErr, row.off[0], row.off[1], row.off[2], g_forendApplied,
                g_forendRefused);
        }
        if (g_forendState == 1) {
            twohand::Xf hlO = hl;
            twohand::Ortho3(hl.R, hlO.R);
            float t[3];
            twohand::ForendTarget(hlO, row.off, t);
            hl.t[0] = t[0]; hl.t[1] = t[1]; hl.t[2] = t[2];
        }
    }
    // [LOADBEARING: rig_two_hand_rifle_pose] shotguns: the standard (twohand_shotgun.inc) is measured in the FOREND frame, so the
    // left solve and the thumb may use it only once this item's forend self-check passed. Published every placed update (one
    // interlocked write); the verdict is printed once per item when it is known.
    {
        const bool forendOn = RigTwoHandForend && g_heldForend >= 0;
        const bool ok = g_heldShot >= 0 && forendOn && g_forendState == 1;
        InterlockedExchange(&g_shotReady, ok ? g_heldShot : -1);
        if (g_heldShot >= 0 && g_shotSaidItem != g_heldItem && (!forendOn || g_forendState != 0)) {
            g_shotSaidItem = g_heldItem;
            // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
            LOG("twohand: [TWOHAND] SHOTGUN STANDARD %s for '%s' (item %p) - left hand q (%+.4f %+.4f %+.4f %+.4f), palm %+.4f "
                "%+.4f %+.4f m from the forend pin, thumb bend %s, finger curls %.1f %.1f %.1f %.1f deg. rig_two_hand_rifle_pose %s.",
                ok ? "READY" : (!forendOn ? "REFUSED - no forend frame (rig_two_hand_forend=0 or no forend row)"
                                          : "REFUSED - the forend self-check failed"),
                kShotgunStd[g_heldShot].mesh, reinterpret_cast<void*>(g_heldItem), kShotgunStd[g_heldShot].q[0],
                kShotgunStd[g_heldShot].q[1], kShotgunStd[g_heldShot].q[2], kShotgunStd[g_heldShot].q[3],
                kShotgunStd[g_heldShot].palm[0], kShotgunStd[g_heldShot].palm[1], kShotgunStd[g_heldShot].palm[2],
                InterlockedCompareExchange(&g_shotThumbBad, 0, 0) ? "REFUSED (authored pin mismatch)" : "ON",
                kShotgunStd[g_heldShot].curl[0], kShotgunStd[g_heldShot].curl[1], kShotgunStd[g_heldShot].curl[2],
                kShotgunStd[g_heldShot].curl[3], RigTwoHandRiflePose ? "ON" : "OFF - nothing applied");
        }
    }
    twohand::Xf rel{};
    // Re-latch every placed update while NOT gripping (tracks the shotgun pump, a re-equip); frozen while gripping so a
    // gripped frame never feeds back into its own latch.
    if (!InterlockedCompareExchange(&g_gripping, 0, 0) || g_hlItem != g_heldItem) {
        if (twohand::LatchRel(right, hl, &rel)) {
            AcquireSRWLockExclusive(&g_lock);
            const bool fresh = g_hlItem != g_heldItem;
            g_hlItem = g_heldItem;
            g_hlRel = rel;
            g_hlName = hlName ? hlName : "?";
            ReleaseSRWLockExclusive(&g_lock);
            if (fresh) {
                g_hlSaid = false;
                // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
                LOG("twohand: [TWOHAND] PIN READY for item %p - '%s' sits (%+.3f %+.3f %+.3f) m from the right handholder "
                    "(|%.3f| m). Grab zone: %.3f m around it.", reinterpret_cast<void*>(g_heldItem), g_hlName,
                    rel.t[0], rel.t[1], rel.t[2], std::sqrt(rel.t[0] * rel.t[0] + rel.t[1] * rel.t[1] + rel.t[2] * rel.t[2]),
                    RigTwoHandPinRadius);
            }
        }
    }
    // HANDPOSE probe (Jay 2026-10-09: run 6's last grip is the standard to capture): the DRAWN left hand - after the twist cap
    // and the IK - and its palm centre, in the gun's holderl frame (with the forend offset). Only while gripping.
    if (haveLeftDrawn && InterlockedCompareExchange(&g_gripping, 0, 0)) {
        twohand::Xf fr = hl;
        twohand::Ortho3(hl.R, fr.R);
        float hand[9];
        if (twohand::Ortho3(leftDrawn, hand)) {
            float palmW[3];
            for (int r = 0; r < 3; ++r) {
                palmW[r] = leftAchieved[r] + hand[3 * r] * kPalmFromHandholder[0] + hand[3 * r + 1] * kPalmFromHandholder[1] +
                           hand[3 * r + 2] * kPalmFromHandholder[2];
            }
            float q[4], p[3];
            twohand::HandInFrame(fr, hand, palmW, q, p);
            // the RAW drawn basis's determinant: -1 = a mirrored basis (INV-150's left conjugate), whose z column Ortho3 flips -
            // then "palm normal" below is sign-inverted. Printed so the capture says which.
            const float* m = leftDrawn;
            const float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                              m[2] * (m[3] * m[7] - m[4] * m[6]);
            AcquireSRWLockExclusive(&g_lock);
            g_hpDet = det;
            for (int i = 0; i < 4; ++i) { g_hpDrawnQ[i] = q[i]; }
            for (int i = 0; i < 3; ++i) { g_hpPalm[i] = p[i]; }
            g_hpDrawnN++;
            ReleaseSRWLockExclusive(&g_lock);
        }
    }
    if (!haveLeft) { InterlockedExchange(&g_zoneValid, 0); return; }
    // [LOADBEARING: rig_two_hand_zone_drawn] INV-507: test the zone with the DRAWN left palm (where the player SEES the hand),
    // not the raw controller point, which sits outside the gun's reach-scaled space.
    float zp[3];
    const bool useDrawn = RigTwoHandZoneDrawn && haveLeftDrawn;
    {
        float hand[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        const bool ok = useDrawn && twohand::Ortho3(leftDrawn, hand);
        twohand::ZonePoint(ok, leftAchieved, hand, kPalmFromHandholder, leftGripWorld, zp);
        InterlockedExchange(&g_zoneDrawnUsed, ok ? 1 : 0);
    }
    const float* leftZoneWorld = zp;
    float d = 0.0f;
    bool in = false;
    // [LOADBEARING: rig_two_hand_slide_len] rifles: the grab area is a strip along the handguard and `along` is where on it the
    // hand is - the poll thread latches it at the grab and the pin slides there. Others / length 0: the ball around the point.
    if (InterlockedCompareExchange(&g_heldRifle, 0, 0) && RigTwoHandSlideLen > 0.0f) {
        float fr[9];
        if (!twohand::Ortho3(hl.R, fr)) { InterlockedExchange(&g_zoneValid, 0); return; }
        const float w[3] = { leftZoneWorld[0] - hl.t[0], leftZoneWorld[1] - hl.t[1], leftZoneWorld[2] - hl.t[2] };
        float dl[3];
        for (int c = 0; c < 3; ++c) { dl[c] = fr[c] * w[0] + fr[3 + c] * w[1] + fr[6 + c] * w[2]; }   // fr^T * w
        float along = 0.0f, norm = 0.0f;
        twohand::SlideZone(dl, 0.5f * RigTwoHandSlideLen, RigTwoHandZoneX, RigTwoHandPinRadius, &along, &norm);
        g_zoneState.in = g_zoneState.in ? (norm <= 1.2f) : (norm < 1.0f);   // SphereZone's hysteresis
        in = g_zoneState.in;
        const float ex = dl[0], ey = dl[1], ez = dl[2] - along;
        d = std::sqrt(ex * ex + ey * ey + ez * ez);
        InterlockedExchange(&g_zoneSlideUm, static_cast<LONG>(along * 1.0e6f));
    } else {
        const float c[3] = { hl.t[0], hl.t[1], hl.t[2] };
        in = twohand::SphereZone(&g_zoneState, leftZoneWorld, c, RigTwoHandPinRadius, &d);
        InterlockedExchange(&g_zoneSlideUm, 0);
    }
    InterlockedExchange(&g_zoneDistMm, static_cast<LONG>(d * 1000.0f));
    InterlockedExchange(&g_zoneIn, in ? 1 : 0);
    InterlockedExchange(&g_zoneValid, 1);
}

bool TwoHandLeftPin(const float rightDrawn[9], const float rightAchieved[3], const float* hh12, const float grip[9],
                    const float post[9], float cWorld[9], float wristPos[3], float* t)
{
    if (!RigTwoHandGrip || !RigTwoHandPin) { return false; }
    AcquireSRWLockShared(&g_lock);
    const bool active = g_state != State::Free && g_hlItem != 0 && g_hlItem == g_heldItem;
    const Kind gripKind = g_gripKind;
    const twohand::Q4 meleeStd = g_meleeStdQ;   // INV-514: the standard rolled about this item's shaft
    // the pin point slid along the handguard to where this hold grabbed (0 off a rifle), and on a rifle with the standard pose
    // offset across / below to the approved palm position (INV-508)
    twohand::Xf rel = twohand::SlideRel(g_hlRel, g_pinSlide);
    // [LOADBEARING: rig_two_hand_rifle_palm_x_cm] [LOADBEARING: rig_two_hand_rifle_palm_y_cm] INV-508
    if (RigTwoHandRiflePose && InterlockedCompareExchange(&g_heldRifle, 0, 0)) {
        const float o[3] = { RigTwoHandRiflePalmXCm * 0.01f, RigTwoHandRiflePalmYCm * 0.01f, 0.0f };
        rel = twohand::OffsetRel(rel, o);
    }
    // [LOADBEARING: rig_two_hand_rifle_pose] shotguns: the pin target moves to Jay's approved palm position (twohand_shotgun.inc,
    // in the forend frame rel is latched in). Only a FIREARM grip (review I-2).
    const LONG shot = (RigTwoHandRiflePose && gripKind == Kind::Firearm) ? InterlockedCompareExchange(&g_shotReady, 0, 0) : -1;
    if (shot >= 0 && shot < static_cast<LONG>(kShotgunStdN)) {
        rel = twohand::OffsetRel(rel, kShotgunStd[shot].palm);
    }
    const twohand::Blend blend = g_blend;
    const float slide = g_slideS;
    ReleaseSRWLockShared(&g_lock);
    if (!active) { return false; }
    // [LOADBEARING: manual reload pump] pump plan Task 4 (ruling P5): while the two-hand grip holds a MANUAL pump (wn_shotgun_b / f) the
    // pin target slides back with the pump as reload.cpp draws it - the pump's BACK axis in holderl's frame (rel's frame) x the drawn
    // stroke - so the held hand rides the pump. Any other gun / manual_reload off: ReloadPumpStroke is false, rel unchanged.
    // COST: one shared SRW acquire + a 16-byte copy per pin call; + one OffsetRel while racking.
    {
        float pumpM = 0.0f, pumpAx[3] = {};
        if (ReloadPumpStroke(&pumpM, pumpAx) && pumpM != 0.0f) {
            const float o[3] = { pumpAx[0] * pumpM, pumpAx[1] * pumpM, pumpAx[2] * pumpM };
            rel = twohand::OffsetRel(rel, o);
        }
    }
    const twohand::Xf right = { { rightDrawn[0], rightDrawn[1], rightDrawn[2], rightDrawn[3], rightDrawn[4], rightDrawn[5],
                                  rightDrawn[6], rightDrawn[7], rightDrawn[8] },
                                { rightAchieved[0], rightAchieved[1], rightAchieved[2] } };
    twohand::PinOut p{};
    const char* why = "";
    const float tNow = twohand::BlendAt(blend, NowS(), slide);
    // [LOADBEARING: rig_two_hand_rifle_pose] INV-506: on a RIFLE the left hand takes the STANDARD rotation in the gun frame (Jay's
    // pick), blended in from the grab-latched one by the grip's t. This is a MEASURED rotation in holderl's frame, not holderl's
    // own axes (D-205 rules those out). The pin below then places the wrist for THIS rotation.
    // [LOADBEARING: rig_two_hand_rifle_pose] shotguns: the same, with that mesh's approved rotation (twohand_shotgun.inc).
    const bool rifleStd = RigTwoHandRiflePose && InterlockedCompareExchange(&g_heldRifle, 0, 0);
    const bool shotStd = shot >= 0 && shot < static_cast<LONG>(kShotgunStdN);
    if (rifleStd || shotStd) {
        const twohand::Q4 stdQ = shotStd ? twohand::Q4{ kShotgunStd[shot].q[0], kShotgunStd[shot].q[1], kShotgunStd[shot].q[2],
                                                        kShotgunStd[shot].q[3] }
                                         : kRifleLeftStd;
        twohand::Xf rf = right;
        twohand::Ortho3(right.R, rf.R);
        const twohand::Xf fr0 = twohand::XfMul(rf, rel);
        float fr[9], cStd[9];
        if (twohand::Ortho3(fr0.R, fr) && twohand::StandardCWorld(fr, stdQ, grip, post, cStd)) {
            float blended[9];
            twohand::SlerpM3(cWorld, cStd, tNow, blended);
            for (int i = 0; i < 9; ++i) { cWorld[i] = blended[i]; }
            InterlockedIncrement64(shotStd ? &g_shotApplied : &g_stdApplied);
        }
    }
    // [LOADBEARING: rig_two_hand_melee] spec 7.2 item 4: on two-handed MELEE the left hand takes the S4 mirrored grip's rotation
    // in the weapon's Holder frame (generated, twohand_melee.inc), blended in by t. A COMPUTED rotation in the Holder frame, not
    // an element's own axes taken as a hand orientation (D-205); its one risk - the mirror plane - is named in spec 7.1.
    // review I-2: only a MELEE grip of a weapon still held as melee (a rifle grip swapped straight to an axe does not get this)
    // [LOADBEARING: rig_two_hand_melee_pose] 0 = capture mode: the pinned hand keeps the left controller's own rotation.
    if (RigTwoHandMelee && RigTwoHandMeleePose && gripKind == Kind::Melee &&
        InterlockedCompareExchange(&g_kind, 0, 0) == static_cast<LONG>(Kind::Melee)) {
        twohand::Xf rf = right;
        twohand::Ortho3(right.R, rf.R);
        const twohand::Xf fr0 = twohand::XfMul(rf, rel);
        float fr[9], cStd[9];
        if (twohand::Ortho3(fr0.R, fr) && twohand::StandardCWorld(fr, meleeStd, grip, post, cStd)) {
            float blended[9];
            twohand::SlerpM3(cWorld, cStd, tNow, blended);
            for (int i = 0; i < 9; ++i) { cWorld[i] = blended[i]; }
            InterlockedIncrement64(&g_meleeStdApplied);
        }
    }
    // POSITION (INV-501): cWorld - the grab-latched left rotation, or on a rifle the standard above - is passed in and not
    // changed by the pin; only the wrist target is returned.
    // [LOADBEARING: rig_two_hand_palm] which point of the hand the pin puts on holderl - palm centre or handholder (INV-502)
    static const float kNoPalm[3] = { 0.0f, 0.0f, 0.0f };
    const float* palm = RigTwoHandPalm ? kPalmFromHandholder : kNoPalm;
    if (!twohand::PinLeftHand(right, rel, hh12, palm, grip, post, cWorld, &p, &why)) {
        g_pinWhy = why;
        InterlockedIncrement64(&g_pinRefused);
        return false;
    }
    *t = tNow;
    for (int i = 0; i < 3; ++i) { wristPos[i] = p.wristPos[i]; }
    InterlockedIncrement64(&g_pinned);
    // HANDPOSE probe: the COMMANDED hand (cWorld * grip * post, what the solve is asked for) in the same holderl frame, so the
    // log shows how far the drawn hand (twist cap, IK) lands from it. Pure arithmetic, no engine call.
    {
        twohand::Xf rf = right;
        twohand::Ortho3(right.R, rf.R);
        const twohand::Xf fr0 = twohand::XfMul(rf, rel);
        twohand::Xf fr = fr0;
        twohand::Ortho3(fr0.R, fr.R);
        float cg[9], cmd[9], postO[9], hand[9];
        twohand::Ortho3(post, postO);
        twohand::M3Mul(cWorld, grip, cg);
        twohand::M3Mul(cg, postO, cmd);
        if (twohand::Ortho3(cmd, hand)) {
            float q[4], pp[3];
            twohand::HandInFrame(fr, hand, fr.t, q, pp);
            AcquireSRWLockExclusive(&g_lock);
            for (int i = 0; i < 4; ++i) { g_hpCmdQ[i] = q[i]; }
            g_hpCmdN++;
            ReleaseSRWLockExclusive(&g_lock);
        }
    }
    return true;
}

void TwoHandNoteWrist(bool isLeft, bool pastLimit, bool stillClamped, const float excess4[4])
{
    if (!RigTwoHandGrip || !InterlockedCompareExchange(&g_gripping, 0, 0)) { return; }
    const int s = isLeft ? 1 : 0;
    InterlockedIncrement64(&g_wCalls[s]);
    if (pastLimit) { InterlockedIncrement64(&g_wPast[s]); }
    if (stillClamped) { InterlockedIncrement64(&g_wStill[s]); }
    if (!excess4) { return; }
    float worst = 0.0f;
    for (int k = 0; k < 4; ++k) {
        if (excess4[k] > worst) { worst = excess4[k]; }
        const LONG cd = static_cast<LONG>(excess4[k] * 100.0f);
        LONG prev = g_wExMax[s][k];
        while (cd > prev) {
            const LONG got = InterlockedCompareExchange(&g_wExMax[s][k], cd, prev);
            if (got == prev) { break; }
            prev = got;
        }
    }
    const int bin = twohand::ExcessBin(worst);
    if (bin >= 0) { InterlockedIncrement64(&g_wEx[s][bin]); }
}

float TwoHandPinnedReach(float mapped, float raw, float t)
{
    if (!RigTwoHandGrip || !RigTwoHandPinReach) { return mapped; }
    const float out = twohand::PinnedReach(mapped, raw, t);
    InterlockedIncrement64(&g_pinReachN);
    InterlockedExchangeAdd64(&g_pinReachUm, static_cast<LONG64>((out - mapped) * 1.0e6f));
    return out;
}

float TwoHandWristOverflow()
{
    const bool gripping = RigTwoHandGrip && InterlockedCompareExchange(&g_gripping, 0, 0) != 0;
    return twohand::WristOverflowDeg(RigWristOverflowDeg, RigTwoHandWristOverflowDeg, gripping);
}

void TwoHandNoteShot()
{
    if (!RigTwoHandGrip || !(RigTwoHandHaptic > 0.0f) || !InterlockedCompareExchange(&g_gripping, 0, 0)) { return; }
    // No pulse here since 2026-10-25 (game rumble -> Touch haptics, spec 2026-10-25-game-rumble-to-touch-haptics-design; Jay: "Stop
    // ours"): the game's own per-weapon Shot_* rumble now reaches both controllers on every shot. The count stays for the log.
    InterlockedIncrement(&g_shotsL);
}

void TwoHandClassifyItem(uintptr_t item, const char* cls, const char* meshRaw, bool descOk, bool descTwoHanded, bool typeOk,
                         int itemType)
{
    char norm[96] = {};
    const bool haveMesh = twohand::NormMesh(meshRaw, norm, sizeof(norm));
    const twohand::Class table = twohand::Classify(cls, haveMesh ? norm : nullptr, kTwoHandedMeshes, kTwoHandedMeshesN,
                                                   kTwoHandedAmbiguous, kTwoHandedAmbiguousN);
    const twohand::Class c = twohand::ClassifyWithType(cls, haveMesh ? norm : nullptr, descOk, descTwoHanded, typeOk, itemType,
                                                       kTwoHandedMeshes, kTwoHandedMeshesN, kTwoHandedAmbiguous,
                                                       kTwoHandedAmbiguousN);
    g_classItem = item;
    g_classKind = static_cast<LONG>(c.kind);
    g_classRifle = (haveMesh && c.kind == Kind::Firearm && std::strncmp(norm, "wn_rifle", 8) == 0) ? 1 : 0;
    g_classForend = -1;
    for (unsigned i = 0; haveMesh && i < kTwoHandForendN; ++i) {
        if (std::strcmp(norm, kTwoHandForend[i].mesh) == 0) { g_classForend = static_cast<int>(i); break; }
    }
    g_classShot = -1;
    for (unsigned i = 0; haveMesh && c.kind == Kind::Firearm && i < kShotgunStdN; ++i) {
        if (std::strcmp(norm, kShotgunStd[i].mesh) == 0) { g_classShot = static_cast<int>(i); break; }
    }
    g_classButt = -1;
    for (unsigned i = 0; haveMesh && c.kind == Kind::Melee && i < kMeleeButtsN; ++i) {
        if (std::strcmp(norm, kMeleeButts[i].mesh) == 0) { g_classButt = static_cast<int>(i); break; }
    }
    g_classGrip = -1;
    for (unsigned i = 0; haveMesh && c.kind == Kind::Melee && i < kMeleeGripsN; ++i) {
        if (std::strcmp(norm, kMeleeGrips[i].mesh) == 0) { g_classGrip = static_cast<int>(i); break; }
    }
    if (!RigTwoHandGrip) { return; }
    // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
    // S1's open part (INV-498): the desc flag logged beside the mesh table's answer, per item.
    // INV-644: the ItemType beside the flag, per item - 15..17 = the game's two-handed set; -1 with UNREADABLE = the read faulted.
    LOG("twohand: [TWOHAND] right item %p class '%s' mesh '%s' -> '%s' -> %s (%s) | desc TwoHanded flag (slot 176): %s | "
        "ItemType (slot 56): %d %s | the mesh table alone says %s (%s). Table: %u two-handed meshes, %u ambiguous.",
        reinterpret_cast<void*>(item), cls ? cls : "?", meshRaw && meshRaw[0] ? meshRaw : "(unreadable)", haveMesh ? norm : "(none)",
        KindName(c.kind), c.why, descOk ? (descTwoHanded ? "SET" : "clear") : "UNREADABLE", typeOk ? itemType : -1,
        typeOk ? (twohand::IsTwoHandedItemType(itemType) ? "(two-handed)" : "(not two-handed)") : "UNREADABLE",
        KindName(table.kind), table.why, kTwoHandedMeshesN, kTwoHandedAmbiguousN);
}

// [LOADBEARING: rig_two_hand_grip] a reused right model address (same pointer, another InventoryItem - a tester, 2026-11-08). The machine
// and every latch here compare POINTERS, so the change is invisible to them: ItemCtlPlace calls TwoHandNoteItemReplaced (HookTail)
// and TwoHandPoll lets go on its own thread when this count moves.
static volatile LONG g_itemReplaced = 0;

void TwoHandNoteItemReplaced()
{
    // HookTail-thread latches back to "none" (0 = none for each); g_hlItem is shared with the solve thread under g_lock.
    AcquireSRWLockExclusive(&g_lock);
    g_hlItem = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_hlSaid = false;
    g_forendItem = 0;
    g_shotSaidItem = 0;
    g_meleeItem = 0;
    g_mRelItem = 0;
    g_mLatItem = 0;
    g_mLatched = false;
    InterlockedIncrement(&g_itemReplaced);
}

void TwoHandNoteHeld(uintptr_t item)
{
    if (item == g_heldItem) { return; }
    g_heldItem = item;
    InterlockedExchange64(&g_heldItemV, static_cast<LONG64>(item));
    const LONG k = (item && item == g_classItem) ? g_classKind : 0;
    InterlockedExchange(&g_kind, k);
    g_heldForend = (item && item == g_classItem) ? g_classForend : -1;   // HookTail thread, like g_heldItem
    g_heldShot = (item && item == g_classItem) ? g_classShot : -1;
    InterlockedExchange(&g_shotReady, -1);   // TwoHandFeedPlaced re-publishes it once this item's forend check has passed
    g_heldButt = (item && item == g_classItem) ? g_classButt : -1;
    g_heldGrip = (item && item == g_classItem) ? g_classGrip : -1;
    InterlockedExchange(&g_heldRifle, (item && item == g_classItem) ? g_classRifle : 0);
}

bool TwoHandPoll(bool vrGripOn, bool holsterBusyL, bool reloadOwnsL)
{
    if (!RigTwoHandGrip) {
        LetGo("rig_two_hand_grip was switched OFF");
        return false;
    }
    const char* refuse =
          !vrGripOn                ? "vr_grip=0 - the left grip button is not read"
        : !ItemPlaceFromController ? "item_place_from_controller=0 - the gun is not placed from the drawn hand, so it "
                                     "could not follow the two-handed hold"
        : !RigHandGripPose         ? "rig_hand_grip_pose=0 - the arms follow the AIM pose; this maths is written for "
                                     "GRIP poses"
                                   : nullptr;
    static const char* s_saidRefuse = nullptr;
    if (refuse) {
        if (s_saidRefuse != refuse) {
            s_saidRefuse = refuse;
            // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
            LOG("twohand: [TWOHAND] REFUSED - %s. rig_two_hand_grip=1 but NOTHING will grab.", refuse);
        }
        LetGo(refuse);
        return false;
    }
    s_saidRefuse = nullptr;
    static bool s_armed = false;
    if (!s_armed) {
        s_armed = true;
        // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
        LOG("twohand: [TWOHAND] ARMED - firearms (phase 1) and melee (phase 2). Firearm AIM-ray zone: %.3f m around the right AIM ray, %.3f to "
            "%.3f m ahead of the right grip. Settle %.3f s. Min hand span %.3f m. Table: %u two-handed meshes, %u "
            "ambiguous. Bow and crossbow are never grabbed. Phase 1b: pin %s, holderl "
            "zone radius %.3f m (the AIM ray above is the fallback for a gun without holderl). Haptic %.2f. Palm pin %s "
            "(palm - handholder %+.4f %+.4f %+.4f m, hand frame). Wrist overflow while two-handing %.0f deg (normal %.0f). "
            "Forend table %s, %u rows. Rifle standard left pose %s (INV-506), shotgun standard rows %u + finger curls / thumb (same switch). "
            "Phase 2 MELEE %s: strip radius %.3f m from %+.3f m "
            "up to the right hand, stack %.1f cm, curl %.0f deg, finger rows %u, chainsaw excluded.",
            RigTwoHandGunRadius, RigTwoHandGunMin, RigTwoHandGunMax, RigTwoHandSlideS, twohand::kMinSpan,
            kTwoHandedMeshesN, kTwoHandedAmbiguousN, RigTwoHandPin ? "ON" : "OFF", RigTwoHandPinRadius,
            RigTwoHandHaptic, RigTwoHandPalm ? "ON" : "OFF", kPalmFromHandholder[0], kPalmFromHandholder[1],
            kPalmFromHandholder[2], twohand::WristOverflowDeg(RigWristOverflowDeg, RigTwoHandWristOverflowDeg, true),
            twohand::WristOverflowDeg(RigWristOverflowDeg, RigTwoHandWristOverflowDeg, false),
            RigTwoHandForend ? "ON" : "OFF", kTwoHandForendN, RigTwoHandRiflePose ? "ON" : "OFF", kShotgunStdN,
            RigTwoHandMelee ? "ON" : "OFF", RigTwoHandMeleeRadius, RigTwoHandMeleeButtCm * 0.01f, RigTwoHandMeleeStackCm,
            RigTwoHandMeleeCurlDeg, kMeleeFingersN);
        // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
        LOG("twohand: [TWOHAND] FIREARM AIM - the right wrist aims, the line between the hands pulls the barrel by the share "
            "(rig_two_hand_aim_share %.2f, live; 0 = the wrist alone, 1 = the hand line alone). The roll is the right wrist's own "
            "(no up vector). The line's share fades in from %.3f to %.3f m between the hands.",
            RigTwoHandAimShare, twohand::kMinSpan, twohand::kFullSpan);
        const bool recPath = RecPathInit();
        // [PROBE: TWOHANDREC] gate=diag_twohand_record + rig_two_hand_grip  site=HEADSET  log-only=yes
        LOG("twohand: [TWOHANDREC] recorder %s - %s", DiagTwoHandRecord ? "ON" : "OFF (diag_twohand_record=0)",
            !DiagTwoHandRecord ? "nothing is recorded"
                               : (recPath ? g_recPath : "*** the dlvr path could not be built - every hold will FAIL to write ***"));
    }
    ++g_polls;

    twohand::Input in{};
    in.squeezeActive = GetControllerSqueeze(kLeftHand, in.squeeze);
    in.holsterBusy = holsterBusyL;
    in.kind = static_cast<Kind>(InterlockedCompareExchange(&g_kind, 0, 0));
    // Body grab (plan 2026-10-23-body-grab Task 4): the right hand holds a body - its weapon is hidden, so there is nothing to hold
    // two-handed: fed as no weapon, a held grip lets go (the machine's own cancel) and a new press is refused. COST: one volatile load.
    if (BodyGrabHandActive(1)) { in.kind = Kind::None; }
    in.now = NowS();
    in.slideS = RigTwoHandSlideS;
    in.meleeOn = RigTwoHandMelee;
    in.item = static_cast<unsigned long long>(InterlockedCompareExchange64(&g_heldItemV, 0, 0));
    // A reused right model address (TwoHandNoteItemReplaced): the machine would see the same pointer and keep holding the previous
    // item - let go here, on the poll thread that owns the machine. COST: one volatile load.
    {
        static LONG s_replacedSeen = 0;
        const LONG rep = InterlockedCompareExchange(&g_itemReplaced, 0, 0);
        if (rep != s_replacedSeen) {
            s_replacedSeen = rep;
            LetGo("the right item was REPLACED at the same model address (a swap reused it)");
        }
    }
    XrPosef rg{}, lg{}, ra{};
    const bool rOk = GetControllerGripPoseSteady(kRightHand, rg);
    const bool lOk = GetControllerGripPoseSteady(kLeftHand, lg);
    const bool aOk = GetControllerPoseSteady(kRightHand, ra);
    in.posesOk = rOk && lOk && aOk;
    twohand::Zone z{ false, 0.0f, 0.0f };
    const bool pinZone = InterlockedCompareExchange(&g_zoneValid, 0, 0) != 0;
    const bool melee = in.kind == Kind::Melee;
    if (melee) {
        // phase 2: only the shaft strip (FeedMelee). No AIM-ray fallback - a melee weapon whose strip is not ready is not grabbable.
        if (pinZone && InterlockedCompareExchange(&g_meleeReady, 0, 0)) {
            z.in = InterlockedCompareExchange(&g_zoneIn, 0, 0) != 0;
            z.off = static_cast<float>(InterlockedCompareExchange(&g_zoneDistMm, 0, 0)) / 1000.0f;
            z.along = static_cast<float>(InterlockedCompareExchange(&g_meleeAlongUm, 0, 0)) * 1.0e-6f;
        }
    } else if (pinZone) {
        // phase 1b: the zone around the gun's holderl, measured in game space after ItemCtlPlace (TwoHandFeedPlaced)
        z.in = InterlockedCompareExchange(&g_zoneIn, 0, 0) != 0;
        z.off = static_cast<float>(InterlockedCompareExchange(&g_zoneDistMm, 0, 0)) / 1000.0f;
        // holderl zone: `along` = where on the rifle's handguard strip the hand is (m along holderl z, 0 = holderl; always 0
        // for the ball zone)
        z.along = static_cast<float>(InterlockedCompareExchange(&g_zoneSlideUm, 0, 0)) * 1.0e-6f;
    } else if (in.posesOk) {
        const twohand::V3 fwd = twohand::QRotate(ToPose(ra).q, twohand::V3{ 0.0f, 0.0f, -1.0f });
        z = twohand::GunZone(ToPose(rg).p, fwd, ToPose(lg).p, RigTwoHandGunMin, RigTwoHandGunMax, RigTwoHandGunRadius);
    }
    in.inZone = z.in;
    // Near-grab hint (Jay): a faint pulse when the left hand ENTERS the zone around a two-handed gun, not while gripping.
    static bool s_wasIn = false;
    if (z.in && !s_wasIn && (in.kind == Kind::Firearm || (melee && RigTwoHandMelee)) && g_machine.state() == State::Free &&
        RigTwoHandHaptic > 0.0f &&
        g_hintGate.Allow(GetTickCount64(), 500)) {
        PulseController(kLeftHand, RigTwoHandHaptic * 0.35f, 15);
        ++g_hints;
    }
    s_wasIn = z.in;

    // [TWOHANDREC]: a hold the machine has fully let go of is written before a new grab can start the next one. Lock-free when
    // nothing waits (RecFlush's first test).
    if (g_machine.state() == State::Free) { RecFlush(); }
    const twohand::Step st = g_machine.Update(in);
    twohand::GunLatch latch{};
    bool newLatch = false;
    if (st.grab && st.resumed) {
        // Re-grab during the ease-back: KEEP the latch the hold already has (final review 2026-10-09) - a new latch would
        // target the controllers' own orientations and the drawn hands would jump by t x the old two-hand offset.
        g_grabAt = in.now;
        ++g_grabs;
        if (RigTwoHandHaptic > 0.0f) { PulseController(kLeftHand, RigTwoHandHaptic, 40); }
    } else if (st.grab && melee) {
        // melee: no gun latch (the right controller aims, TwoHandAdjustCtl leaves it alone); the pin never slides along a strip
        g_grabAt = in.now;
        ++g_grabs;
        AcquireSRWLockExclusive(&g_lock);
        g_pinSlide = 0.0f;
        ReleaseSRWLockExclusive(&g_lock);
        if (RigTwoHandHaptic > 0.0f) { PulseController(kLeftHand, RigTwoHandHaptic, 40); }
    } else if (st.grab) {
        if (!twohand::LatchGun(ToPose(rg), ToPose(lg), &latch)) {
            g_machine.Reset();
            Publish(false, latch);
            ++g_latchFail;
            // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
            LOG("twohand: [TWOHAND] GRAB REFUSED at the latch - hands under %.3f m apart. along %.3f off %.3f. That press "
                "stays L1.", twohand::kMinSpan, z.along, z.off);
            return false;
        }
        newLatch = true;
        g_grabAt = in.now;
        ++g_grabs;
        // the handguard strip: where on it the hand grabbed (0 off a rifle / length 0) - the pin slides there for this hold
        const float slide = pinZone ? static_cast<float>(InterlockedCompareExchange(&g_zoneSlideUm, 0, 0)) * 1.0e-6f : 0.0f;
        const twohand::Pose rp = ToPose(rg), lp = ToPose(lg);
        AcquireSRWLockExclusive(&g_lock);
        g_pinSlide = slide;
        // [TWOHANDREC] the hold header: the latch and the poses it was taken from (written by RecFlush with the rows)
        g_recHold = RecHold{};
        g_recHold.grab = static_cast<int>(g_grabs);
        g_recHold.grabAt = in.now;
        const float hv[] = { rp.p.x, rp.p.y, rp.p.z, rp.q.x, rp.q.y, rp.q.z, rp.q.w, lp.p.x, lp.p.y, lp.p.z, lp.q.x, lp.q.y,
                             lp.q.z, lp.q.w, latch.lineLocal.x, latch.lineLocal.y, latch.lineLocal.z, latch.lRel.x,
                             latch.lRel.y, latch.lRel.z, latch.lRel.w };
        std::memcpy(g_recHold.rgp, hv, sizeof(hv));   // rgp .. lRel are 21 consecutive packed floats
        ReleaseSRWLockExclusive(&g_lock);
        if (RigTwoHandHaptic > 0.0f) { PulseController(kLeftHand, RigTwoHandHaptic, 40); }
    }
    Publish(newLatch, latch);

    if (st.grab || st.refusedPress) {
        if (st.refusedPress) { ++g_refused; }
        // Rifle plan Task 5 fix round 1: a press the manual reload owns (ovr2xr.cpp folds it into holsterBusyL) is refused by the
        // machine's one busy input - the line names the RELOAD as the owner, and it is NOT sent as L1 (ReloadPadPoll withholds it).
        // reloadOwnsL is set only while the reload owns the press; a holster press keeps its own text. COST: per press only.
        const bool byReload = st.refusedPress && reloadOwnsL;
        // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
        LOG("twohand: [TWOHAND] PRESS %s - %s | right item %s | left hand along %.3f m off %.3f m (%s zone; holderl r "
            "%.3f | AIM ray %.3f..%.3f r %.3f) | poses r%d l%d aim%d | busy (holster or reload) %d, reload owns %d. grabs %ld refused %ld | "
            "rifle strip %s len %.3f x %.3f | zone point %s | melee: %s, strip along %+.3f top %+.3f m",
            st.grab ? "GRAB" : byReload ? "NOT A GRAB (the manual reload's press - not sent as L1)" : "NOT A GRAB (sent as L1)",
            byReload ? "the manual reload took this press (at the mag / handle, or mid-reload)" : st.why, KindName(in.kind),
            z.along, z.off,
            melee ? "MELEE SHAFT" : (pinZone ? "HOLDERL" : "AIM-RAY"), RigTwoHandPinRadius, RigTwoHandGunMin, RigTwoHandGunMax, RigTwoHandGunRadius,
            rOk ? 1 : 0, lOk ? 1 : 0, aOk ? 1 : 0, holsterBusyL ? 1 : 0, reloadOwnsL ? 1 : 0, g_grabs, g_refused,
            (InterlockedCompareExchange(&g_heldRifle, 0, 0) && RigTwoHandSlideLen > 0.0f) ? "ON" : "OFF",
            RigTwoHandSlideLen, RigTwoHandZoneX,
            InterlockedCompareExchange(&g_zoneDrawnUsed, 0, 0) ? "DRAWN palm" : "controller",
            !melee ? "-" : (InterlockedCompareExchange(&g_meleeReady, 0, 0) ? "READY" : g_meleeRefuse),
            z.along, static_cast<float>(InterlockedCompareExchange(&g_meleeTopUm, 0, 0)) * 1.0e-6f);
    }
    if (st.release || st.cancelled) {
        if (st.release) { ++g_releases; } else { ++g_cancels; }
        // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
        LOG("twohand: [TWOHAND] %s - %s after %.2f s held. releases %ld cancels %ld | recorder %s, rows so far this hold %d "
            "(written once the grip has eased out)",
            st.release ? "RELEASE" : "CANCELLED", st.why, in.now - g_grabAt, g_releases, g_cancels,
            DiagTwoHandRecord ? "ON" : "OFF (diag_twohand_record=0)", g_recN);
    }
    static double s_lastStatus = 0.0;
    if (g_machine.state() == State::Gripped && in.posesOk && in.now - s_lastStatus >= 1.0) {
        s_lastStatus = in.now;
        twohand::GunLatch probe{};
        AcquireSRWLockShared(&g_lock);
        probe = g_latch;
        ReleaseSRWLockShared(&g_lock);
        const twohand::GunOut o = twohand::GunTargets(ToPose(rg), ToPose(lg), &probe, RigTwoHandAimShare);
        AcquireSRWLockShared(&g_lock);
        const float appliedShare = g_lastShare, appliedSwing = g_lastSwingDeg;
        ReleaseSRWLockShared(&g_lock);
        const twohand::V3 span = twohand::Sub(ToPose(lg).p, ToPose(rg).p);
        // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
        if (melee) {
            LOG_PERIODIC("twohand: [TWOHAND] MELEE GRIPPING t %.2f | slide: updates applied %lld, this hold moves %+.3f m (left node - "
                "holder %+.3f m; slides %ld refused %ld; both hands lifted %.3f m) | left pose %s, roll %.0f deg, wrist %.1f cm, std applied %lld | fingers %s, "
                "curl %.0f deg, writes %lld | strip: "
                "left hand %+.3f m, right hand %+.3f m up the shaft | PIN pinned %lld refused %lld (%s) | wrist R calls %lld "
                "past %lld still %lld | L calls %lld past %lld still %lld | haptics hints %ld",
                st.t, static_cast<long long>(g_mRelApplied), g_mA, g_meleeSlide, g_mSlides, g_mSlideRefused, g_meleeLift,
                RigTwoHandMeleePose ? "STANDARD (captured g5)" : "CAPTURE - the left controller's own rotation",
                RigTwoHandMeleeRollDeg, RigTwoHandMeleeWristCm,
                static_cast<long long>(g_meleeStdApplied),
                InterlockedCompareExchange(&g_fingerBad, 0, 0) ? "REFUSED (rest mismatch - see the FINGERS line)" : "ON",
                RigTwoHandMeleeCurlDeg, static_cast<long long>(g_fingerWrites),
                static_cast<float>(InterlockedCompareExchange(&g_meleeAlongUm, 0, 0)) * 1.0e-6f,
                static_cast<float>(InterlockedCompareExchange(&g_meleeTopUm, 0, 0)) * 1.0e-6f, g_pinned, g_pinRefused,
                g_pinWhy[0] ? g_pinWhy : "-", g_wCalls[0], g_wPast[0], g_wStill[0], g_wCalls[1], g_wPast[1], g_wStill[1],
                g_hints);
        } else {
        LOG_PERIODIC("twohand: [TWOHAND] GRIPPING t %.2f | hand span %.3f m | gun turned %.1f deg from the right controller, "
            "left hand %.1f deg from the left controller | AIM: share setting %.2f, applied %.2f (last right solve), the hand "
            "line pulled the gun %.1f deg off the wrist | solves adjusted R %lld L %lld | hands together (wrist aims alone) "
            "%lld, line pointing back (last pull held) %lld, failed %lld | other pose missing %lld | not grip pose %lld | "
            "PIN %s ('%s') pinned %lld refused %lld (%s) | "
            "wrist while gripping R calls %lld past %lld still %lld | L calls %lld past %lld still %lld | haptics hints %ld "
            "shots %ld",
            st.t, twohand::Len(span), twohand::QAngleDeg(o.r, ToPose(rg).q), twohand::QAngleDeg(o.l, ToPose(lg).q),
            RigTwoHandAimShare, appliedShare, appliedSwing,
            g_adjR, g_adjL, g_heldDir, g_heldUp, g_failed, g_noOther, g_notGrip,
            RigTwoHandPin ? "ON" : "OFF", g_hlName, g_pinned, g_pinRefused, g_pinWhy[0] ? g_pinWhy : "-",
            g_wCalls[0], g_wPast[0], g_wStill[0], g_wCalls[1], g_wPast[1], g_wStill[1], g_hints,
            static_cast<long>(g_shotsL));
        }
        // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
        // INV-502 (b): how far past the OVERFLOWED limit the still-clamped solves asked - sizes a two-hand-only overflow.
        LOG_PERIODIC("twohand: [TWOHAND] WRIST PAST OVERFLOW (deg asked beyond limit+%.0f, still-clamped solves while gripping) | R "
            "bins <10 %lld <20 %lld <30 %lld <45 %lld <60 %lld <90 %lld 90+ %lld max flex %.1f ext %.1f ulnar %.1f radial "
            "%.1f | L bins <10 %lld <20 %lld <30 %lld <45 %lld <60 %lld <90 %lld 90+ %lld max flex %.1f ext %.1f ulnar %.1f "
            "radial %.1f | palm pin %s | forend applied %ld refused %ld (last check %.4f m)",
            twohand::WristOverflowDeg(RigWristOverflowDeg, RigTwoHandWristOverflowDeg, true), g_wEx[0][0], g_wEx[0][1], g_wEx[0][2], g_wEx[0][3], g_wEx[0][4], g_wEx[0][5], g_wEx[0][6],
            g_wExMax[0][0] / 100.0, g_wExMax[0][1] / 100.0, g_wExMax[0][2] / 100.0, g_wExMax[0][3] / 100.0,
            g_wEx[1][0], g_wEx[1][1], g_wEx[1][2], g_wEx[1][3], g_wEx[1][4], g_wEx[1][5], g_wEx[1][6],
            g_wExMax[1][0] / 100.0, g_wExMax[1][1] / 100.0, g_wExMax[1][2] / 100.0, g_wExMax[1][3] / 100.0,
            RigTwoHandPalm ? "ON" : "OFF", g_forendApplied, g_forendRefused, g_forendErr);
        float dq[4], cq[4], palm[3];
        long long dn = 0, cn = 0;
        AcquireSRWLockShared(&g_lock);
        for (int i = 0; i < 4; ++i) { dq[i] = g_hpDrawnQ[i]; cq[i] = g_hpCmdQ[i]; }
        for (int i = 0; i < 3; ++i) { palm[i] = g_hpPalm[i]; }
        dn = g_hpDrawnN;
        cn = g_hpCmdN;
        const float det = g_hpDet;
        const float pinSlide = g_pinSlide;
        const twohand::Q4 meleeStdNow = g_meleeStdQ;
        ReleaseSRWLockShared(&g_lock);
        float dm[9];
        twohand::QToMat3(twohand::Q4{ dq[0], dq[1], dq[2], dq[3] }, dm);
        const float dotq = std::fabs(dq[0] * cq[0] + dq[1] * cq[1] + dq[2] * cq[2] + dq[3] * cq[3]);
        const float cmdOffDeg = 2.0f * std::acos(dotq > 1.0f ? 1.0f : dotq) * 57.29578f;
        const LONG shotNow = InterlockedCompareExchange(&g_shotReady, 0, 0);
        const twohand::Q4 shotQ = (shotNow >= 0 && shotNow < static_cast<LONG>(kShotgunStdN))
            ? twohand::Q4{ kShotgunStd[shotNow].q[0], kShotgunStd[shotNow].q[1], kShotgunStd[shotNow].q[2], kShotgunStd[shotNow].q[3] }
            : kRifleLeftStd;
        // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
        // Jay 2026-10-09: capture the left hand/wrist angle he picks as the STANDARD. holderl frame = the gun's (rifles: the
        // gun's own axes, z along the barrel; shotguns: holderl's authored tilt). Grab number = the PRESS GRAB count.
        LOG_PERIODIC("twohand: [TWOHAND] HANDPOSE grab %ld '%s' | DRAWN left hand in the holderl frame q (%+.4f %+.4f %+.4f %+.4f) | "
            "distal axis (%+.3f %+.3f %+.3f) palm normal (%+.3f %+.3f %+.3f) | palm centre (%+.4f %+.4f %+.4f) m from the pin "
            "point | COMMANDED q (%+.4f %+.4f %+.4f %+.4f), drawn is %.1f deg off it | raw drawn det %+.3f | samples drawn "
            "%lld commanded %lld | rifle standard pose %s (rifle %ld) applied %lld | shotgun standard row %ld applied %lld, finger "
            "+ thumb pin writes %lld | drawn is %.1f deg off the standard (melee: the captured g5; shotgun: its row) | "
            "pin slid %+.3f m along the handguard (palm centre above is from holderl, so its z includes this) | pin reach "
            "%s: un-mapped solves %lld, mean lengthening %.1f mm",
            g_grabs, g_hlName, dq[0], dq[1], dq[2], dq[3], dm[0], dm[3], dm[6], -dm[2], -dm[5], -dm[8], palm[0], palm[1],
            palm[2], cq[0], cq[1], cq[2], cq[3], cmdOffDeg, det, dn, cn, RigTwoHandRiflePose ? "ON" : "OFF",
            static_cast<long>(InterlockedCompareExchange(&g_heldRifle, 0, 0)), static_cast<long long>(g_stdApplied),
            static_cast<long>(shotNow), static_cast<long long>(g_shotApplied), static_cast<long long>(g_thumbWrites),
            twohand::QAngleDeg(twohand::Q4{ dq[0], dq[1], dq[2], dq[3] }, melee ? meleeStdNow : (shotNow >= 0 ? shotQ : kRifleLeftStd)),
            pinSlide,
            RigTwoHandPinReach ? "ON" : "OFF", static_cast<long long>(g_pinReachN),
            g_pinReachN > 0 ? static_cast<double>(g_pinReachUm) / static_cast<double>(g_pinReachN) / 1000.0 : 0.0);
    }
    return st.suppressL1;
}

void TwoHandAdjustCtl(bool isLeft, bool gripPoseUsed, XrPosef& ctl)
{
    if (!RigTwoHandGrip) { return; }
    // [LOADBEARING: rig_two_hand_melee] melee: the right controller aims as today (spec 7.2 item 6) and the left rotation comes
    // from the pin (TwoHandLeftPin) - neither controller orientation is rewritten. One interlocked read.
    if (InterlockedCompareExchange(&g_kind, 0, 0) == static_cast<LONG>(Kind::Melee)) { return; }
    AcquireSRWLockExclusive(&g_lock);
    // review I-2: only a FIREARM grip steers (a melee grip swapped straight to a rifle has no gun latch of its own)
    if (g_state == State::Free || g_gripKind != Kind::Firearm) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (!gripPoseUsed) {
        ReleaseSRWLockExclusive(&g_lock);
        InterlockedIncrement64(&g_notGrip);
        return;
    }
    XrPosef other{};
    if (!GetControllerGripPoseSteady(isLeft ? kRightHand : kLeftHand, other)) {
        ReleaseSRWLockExclusive(&g_lock);
        InterlockedIncrement64(&g_noOther);
        return;
    }
    const twohand::Pose me = ToPose(ctl);
    const twohand::Pose ot = ToPose(other);
    // [LOADBEARING: rig_two_hand_aim_share] the share of the aim the hand line takes (GunTargets) - read every call, so the
    // menu slider is live. Each arm's call advances the latch's last swing from the same poses, so both agree.
    const float share = RigTwoHandAimShare;
    const twohand::GunOut o = twohand::GunTargets(isLeft ? ot : me, isLeft ? me : ot, &g_latch, share);
    const double now = NowS();
    const float t = twohand::BlendAt(g_blend, now, g_slideS);
    const twohand::Q4 q = twohand::Slerp(me.q, isLeft ? o.l : o.r, t);
    if (!isLeft) {
        g_lastShare = o.share;
        g_lastSwingDeg = o.swingDeg;
        if (DiagTwoHandRecord) {
            if (g_recN < kRecRows) {
                RecRow& w = g_recBuf[g_recCur][g_recN++];
                w.t = now;
                w.rp[0] = me.p.x; w.rp[1] = me.p.y; w.rp[2] = me.p.z;
                w.rq[0] = me.q.x; w.rq[1] = me.q.y; w.rq[2] = me.q.z; w.rq[3] = me.q.w;
                w.lp[0] = ot.p.x; w.lp[1] = ot.p.y; w.lp[2] = ot.p.z;
                w.lq[0] = ot.q.x; w.lq[1] = ot.q.y; w.lq[2] = ot.q.z; w.lq[3] = ot.q.w;
                w.blendT = t;
                w.share = share;
                w.oq[0] = q.x; w.oq[1] = q.y; w.oq[2] = q.z; w.oq[3] = q.w;
                w.swingDeg = o.swingDeg;
                w.flags = (o.heldDir ? 1.0f : 0.0f) + (o.heldUp ? 2.0f : 0.0f);
            } else {
                ++g_recDropped;
            }
            InterlockedExchange(&g_recAny, 1);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (o.heldDir) { InterlockedIncrement64(&g_heldDir); }
    if (o.heldUp) { InterlockedIncrement64(&g_heldUp); }
    ctl.orientation = { q.x, q.y, q.z, q.w };
    InterlockedIncrement64(isLeft ? &g_adjL : &g_adjR);
}

bool TwoHandWantsHolder()
{
    return RigTwoHandGrip && RigTwoHandMelee && InterlockedCompareExchange(&g_kind, 0, 0) == static_cast<LONG>(Kind::Melee);
}

bool TwoHandMeleeRel(uintptr_t item, const float hhT[3], float relR[9], float relT[3])
{
    if (!RigTwoHandGrip || !RigTwoHandMelee || item != g_heldItem ||
        InterlockedCompareExchange(&g_kind, 0, 0) != static_cast<LONG>(Kind::Melee)) {
        g_mLatched = false;
        return false;
    }
    // the shaft for this item (FeedMelee, last update); the known axis until FeedMelee has read this item
    const bool shaftKnown = g_meleeItem == item && g_meleeWhy[0] == 0;
    const float* u = shaftKnown ? g_meleeU : kMeleeShaftAxis;
    float a = 0.0f;
    const bool aOk = twohand::MeleeHandAlong(relR, relT, hhT, u, &a);
    if (item != g_mRelItem) {
        g_mRelItem = item;
        // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
        // INV-499's open mechanism: does the engine's grip transform carry the idle offset? Once per melee item, unconditionally.
        LOG("twohand: [TWOHAND] MELEE IDLE REL item %p | the right handholder sits %+.3f m up the shaft from Holder (%s; shaft %s) "
            "| rel T (%+.3f %+.3f %+.3f) m | INV-499: ~+0.35 on the axe / sledgehammer = the idle offset IS in rel; ~0 = it is not",
            reinterpret_cast<void*>(item), a, aOk ? "read" : "UNREADABLE - degenerate rel basis",
            shaftKnown ? "this item's" : "the known axis - the item's own not read yet", relT[0], relT[1], relT[2]);
    }
    AcquireSRWLockShared(&g_lock);
    const State st = g_state;
    const twohand::Blend b = g_blend;
    const float s = g_slideS;
    const bool mine = g_gripKind == Kind::Melee && g_gripItem == static_cast<unsigned long long>(item);
    ReleaseSRWLockShared(&g_lock);
    // review I-1 / I-2: only a melee grip of THIS item slides it; a latch made on another item is dropped
    if (item != g_mLatItem) { g_mLatched = false; }
    const float t = (st == State::Free || !mine) ? 0.0f : twohand::BlendAt(b, NowS(), s);
    if (t <= 0.0f) {
        g_mLatched = false;
        return false;
    }
    if (!g_mLatched) {
        g_mLatched = true;
        g_mLatItem = item;
        for (int i = 0; i < 9; ++i) { g_mLatR[i] = relR[i]; }
        for (int i = 0; i < 3; ++i) { g_mLatT[i] = relT[i]; g_mLatU[i] = u[i]; }
        const float slide = shaftKnown ? g_meleeSlide : 0.0f;
        // INV-644: a weapon with a grip row slides from where the game holds it (bats / swords ~+0.10 m) - see MeleeSlideRangeTop
        const bool inRange = aOk && twohand::MeleeSlideInRange(a, twohand::MeleeSlideRangeTop(g_heldGrip >= 0, slide));
        // INV-510: on a short handle both hands sit g_meleeLift higher - the weapon slides that much less
        const float lift = shaftKnown ? g_meleeLift : 0.0f;
        g_mA = inRange ? a - lift : 0.0f;
        if (inRange) { ++g_mSlides; } else { ++g_mSlideRefused; }
        // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
        LOG("twohand: [TWOHAND] MELEE SLIDE %s | latched: the right handholder %+.3f m up the shaft from Holder; this weapon's left "
            "node - holder %+.3f m | the weapon moves %+.3f m along its shaft | slides %ld refused %ld",
            inRange ? "LATCHED - Holder comes to the right hand"
                    : "REFUSED - the latched hand is outside -0.05 .. slide+0.05 (with a grip row the top is at least 0.20) - a grab "
                      "mid-swing?, this hold does not slide",
            a, slide, g_mA, g_mSlides, g_mSlideRefused);
    }
    float oR[9], oT[3];
    twohand::MeleeRel(relR, relT, g_mLatR, g_mLatT, g_mLatU, g_mA, t, oR, oT);
    // run R5 (Jay, the guitar: "IT NEEDS TO MOVE TOWARDS THE FINGERS"): the row's fingers moves the weapon toward the right hand's
    // fingers - along wrist -> right handholder (hhT, the same hand-bone frame as relT), blended by t like the slide
    if (g_heldGrip >= 0 && kMeleeGrips[g_heldGrip].fingers != 0.0f) {
        twohand::TowardFingers(hhT, kMeleeGrips[g_heldGrip].fingers * t, oT);
    }
    // [LOADBEARING: rig_two_hand_tune_x_cm] run R6 live tuning (Jay: sliders "but add rotation for both axis and horizontal aswell"):
    // the gripped weapon moved / turned in the right hand-bone frame about the handholder, blended by t. Off (all 0) = one compare.
    // run R7 (Jay dialled the guitar: "Not perfect, but it'll do" - bake + sliders to 0): this weapon's baked hold from its grip row
    // (twohand_grip.inc tune), the same transform as the sliders; the sliders below then tune on top of it
    if (g_heldGrip >= 0) {
        const float* bk = kMeleeGrips[g_heldGrip].tune;
        if (bk[0] != 0.0f || bk[1] != 0.0f || bk[2] != 0.0f || bk[3] != 0.0f || bk[4] != 0.0f || bk[5] != 0.0f) {
            twohand::GripTune(hhT, bk, bk + 3, t, oR, oT);
        }
    }
    {
        const float off[3] = { RigTwoHandTuneXCm * 0.01f, RigTwoHandTuneYCm * 0.01f, RigTwoHandTuneZCm * 0.01f };
        const float deg[3] = { RigTwoHandTuneRxDeg, RigTwoHandTuneRyDeg, RigTwoHandTuneRzDeg };
        const bool any = off[0] != 0.0f || off[1] != 0.0f || off[2] != 0.0f || deg[0] != 0.0f || deg[1] != 0.0f || deg[2] != 0.0f;
        if (any) { twohand::GripTune(hhT, off, deg, t, oR, oT); }
        static float s_said[7] = { 0, 0, 0, 0, 0, 0, -1 };
        const float now[7] = { RigTwoHandTuneXCm, RigTwoHandTuneYCm, RigTwoHandTuneZCm, deg[0], deg[1], deg[2], 0 };
        bool changed = false;
        for (int i = 0; i < 6; ++i) { changed = changed || now[i] != s_said[i]; }
        if (changed || s_said[6] < 0.0f) {
            for (int i = 0; i < 6; ++i) { s_said[i] = now[i]; }
            s_said[6] = 0.0f;
            // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
            LOG("twohand: [TWOHAND] TUNE item %p '%s' | right hand-bone frame: move x %+.1f y %+.1f z %+.1f cm, turn x %+.1f y %+.1f "
                "z %+.1f deg about the handholder (%+.3f %+.3f %+.3f) | grip t %.2f | rig_two_hand_tune_* (bake per weapon when set)",
                reinterpret_cast<void*>(item), g_heldGrip >= 0 ? kMeleeGrips[g_heldGrip].mesh : "(no grip row)", now[0], now[1], now[2],
                now[3], now[4], now[5], hhT[0], hhT[1], hhT[2], t);
        }
    }
    for (int i = 0; i < 9; ++i) { relR[i] = oR[i]; }
    for (int i = 0; i < 3; ++i) { relT[i] = oT[i]; }
    InterlockedIncrement64(&g_mRelApplied);
    return true;
}

bool TwoHandLeftFingerT(float* t)
{
    if (!RigTwoHandGrip) { return false; }
    const LONG kind = InterlockedCompareExchange(&g_kind, 0, 0);
    // [LOADBEARING: rig_two_hand_rifle_pose] a gripped SHOTGUN with its standard ready: that row's thumb + finger curls
    // (TwoHandLeftFingerLocal writes l_finger01 and fingers 1-4). Melee: the S4 grip on all 16, as before. COST: two more
    // interlocked reads on a non-melee pass.
    const bool shotThumb = kind == static_cast<LONG>(Kind::Firearm) && RigTwoHandRiflePose &&
                           InterlockedCompareExchange(&g_shotReady, 0, 0) >= 0 &&
                           !InterlockedCompareExchange(&g_shotThumbBad, 0, 0);
    if (!shotThumb) {
        if (!RigTwoHandMelee) { return false; }
        if (kind != static_cast<LONG>(Kind::Melee)) { return false; }
        if (InterlockedCompareExchange(&g_fingerBad, 0, 0)) { return false; }
    }
    AcquireSRWLockShared(&g_lock);
    const State st = g_state;
    const twohand::Blend b = g_blend;
    const float s = g_slideS;
    const Kind gk = g_gripKind;
    ReleaseSRWLockShared(&g_lock);
    if (st == State::Free || gk != (shotThumb ? Kind::Firearm : Kind::Melee)) { return false; }   // review I-2: that kind's grip only
    const float u = twohand::BlendAt(b, NowS(), s);
    if (!(u > 0.0f)) { return false; }
    *t = u;
    return true;
}

int TwoHandFingerRow(const char* pinName)
{
    for (unsigned i = 0; pinName && i < kMeleeFingersN; ++i) {
        if (std::strcmp(pinName, kMeleeFingers[i].name) == 0) { return static_cast<int>(i); }
    }
    return -1;
}

// [LOADBEARING: rig_two_hand_rifle_pose] fix round 1 (pump plan 2026-11-04 Task 2, ledger ruling R9): the palm bone l_hand1 (parent
// of fingers 3 / 4, a subtree pin but not a finger row) for a gripped SHOTGUN whose row has hasPalm1 (Jay's own pump grip, wn_shotgun_b
// / f): its local ROTATION = row.palm1, blended from the authored pin by t (the translation stays the authored one, FingerBlend).
// False (not written - the authored l_hand1 stays) for melee, a shotgun row without palm1, or no ready row. The caller
// (boneblend.cpp TwoHandFingersApply) gates on TwoHandLeftFingerT first, so this runs only while a grip holds.
// COST: two interlocked reads + a 12-float blend, once per pass for the one l_hand1 pin.
bool TwoHandLeftPalm1Local(const float authored12[12], float t, float out12[12])
{
    if (InterlockedCompareExchange(&g_kind, 0, 0) != static_cast<LONG>(Kind::Firearm)) { return false; }
    const LONG shot = InterlockedCompareExchange(&g_shotReady, 0, 0);
    if (shot < 0 || shot >= static_cast<LONG>(kShotgunStdN)) { return false; }
    const ShotgunStd& sg = kShotgunStd[shot];
    if (!sg.hasPalm1) { return false; }
    twohand::FingerBlend(authored12, sg.palm1, t, out12);
    InterlockedIncrement64(&g_thumbWrites);
    return true;
}

bool TwoHandLeftFingerLocal(int row, const float authored12[12], float t, float out12[12])
{
    if (row < 0 || row >= static_cast<int>(kMeleeFingersN) || row >= 32) { return false; }
    const MeleeFinger& f = kMeleeFingers[row];
    // [LOADBEARING: rig_two_hand_rifle_pose] shotgun grip (TwoHandLeftFingerT let it through on a FIREARM): that gun's row
    // (twohand_shotgun.inc, Jay's r7 contact fit) - the thumb's base pin = authored * row.thumb, fingers 1-4 = the authored pin
    // curled row.curl[k] at every joint about kMeleeCurlAxis (twohand::CurlFinger, as the melee grip); blended by t. The thumb's
    // outer joints (01extra / 02 / 03) keep the authored pose (false = not written).
    if (InterlockedCompareExchange(&g_kind, 0, 0) == static_cast<LONG>(Kind::Firearm)) {
        const LONG shot = InterlockedCompareExchange(&g_shotReady, 0, 0);
        if (shot < 0 || shot >= static_cast<LONG>(kShotgunStdN)) { return false; }
        const ShotgunStd& sg = kShotgunStd[shot];
        float a9[9];
        for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) { a9[3 * r + c] = authored12[4 * r + c]; } }
        // fingers 1-4: names l_finger<k><j>, k 1..4 (the generated rows' own names - twohand_melee.inc)
        const char k = f.name[8];
        // [LOADBEARING: rig_two_hand_rifle_pose] pump plan 2026-11-04 Task 2 (ledger ruling R2): a row with hasFingers16 (Jay's own
        // pump grip, wn_shotgun_b / f - a free pose that is not curl + thumb, tools/twohand_shotgun.py printed FINGERS16) writes ALL 16
        // pins from fingers16[row] (the posed locals, kMeleeFingers' row order), thumb outer joints included, after the same per-pin
        // rest check the curled fingers get. COST: one bool test per pin; the 9-float copy replaces the curl's Rodrigues.
        if (sg.hasFingers16 || (std::strncmp(f.name, "l_finger", 8) == 0 && k >= '1' && k <= '4')) {
            if (!g_fingerChecked[row]) {
                // the curl is applied to the authored pin; it was fitted on the skeleton's rest - they must be the same hand
                g_fingerChecked[row] = true;
                const float d = twohand::MaxAbsDiff(a9, f.rest, 9);
                if (d > g_fingerMaxDiff) { g_fingerMaxDiff = d; }
                if (d > 0.02f) {
                    InterlockedExchange(&g_fingerBad, 1);
                    // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
                    LOG("twohand: [TWOHAND] SHOTGUN FINGERS REFUSED - '%s': the authored pin differs from the rest the curl was fitted "
                        "on by %.4f (limit 0.0200). That finger keeps the authored pose.", f.name, d);
                }
            }
            if (InterlockedCompareExchange(&g_fingerBad, 0, 0)) { return false; }
            float g[9];
            if (sg.hasFingers16) {
                for (int i = 0; i < 9; ++i) { g[i] = sg.fingers16[row][i]; }
            } else {
                twohand::CurlFinger(a9, kMeleeCurlAxis, sg.curl[k - '1'], true, g);
            }
            twohand::FingerBlend(authored12, g, t, out12);
            InterlockedIncrement64(&g_thumbWrites);
            return true;
        }
        if (std::strcmp(f.name, "l_finger01") != 0) { return false; }
        if (!g_shotThumbChecked) {
            // the bend was built on this rest (tools/twohand_shotgun.py); a different latched pin = a different hand - refuse
            g_shotThumbChecked = true;
            const float d = twohand::MaxAbsDiff(a9, kShotgunThumbRest, 9);
            if (d > 0.02f) { InterlockedExchange(&g_shotThumbBad, 1); }
            // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
            LOG("twohand: [TWOHAND] SHOTGUN THUMB %s - the authored l_finger01 pin differs from the rest the bend was built on by "
                "%.4f (limit 0.0200).%s", d > 0.02f ? "REFUSED" : "CHECKED", d,
                d > 0.02f ? " The thumb keeps the authored pose for the session; the hand angle is unaffected." : "");
            if (d > 0.02f) { return false; }
        }
        float g[9];
        twohand::M3Mul(a9, sg.thumb, g);
        twohand::FingerBlend(authored12, g, t, out12);
        InterlockedIncrement64(&g_thumbWrites);
        return true;
    }
    if (!g_fingerChecked[row]) {
        // the generated REST local must be the authored pin this skeleton latched, or the recipe is for a different hand
        g_fingerChecked[row] = true;
        float a9[9];
        for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) { a9[3 * r + c] = authored12[4 * r + c]; } }
        const float d = twohand::MaxAbsDiff(a9, f.rest, 9);
        if (d > g_fingerMaxDiff) { g_fingerMaxDiff = d; }
        ++g_fingerCheckedN;
        if (d > 0.02f) {
            InterlockedExchange(&g_fingerBad, 1);
            // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
            LOG("twohand: [TWOHAND] FINGERS REFUSED - '%s': the generated rest rotation differs from the authored pin by %.4f "
                "(limit 0.0200). The melee finger pose is OFF for the session; the hand keeps the authored open pose.", f.name, d);
            return false;
        }
        if (g_fingerCheckedN == static_cast<int>(kMeleeFingersN)) {
            // [PROBE: TWOHAND] gate=rig_two_hand_grip  site=HEADSET  log-only=yes
            LOG("twohand: [TWOHAND] FINGERS CHECKED - all %u rows: generated rest == authored pin within %.4f (limit 0.0200). Curl "
                "%.0f deg about (%+.3f %+.3f %+.3f).", kMeleeFingersN, g_fingerMaxDiff, RigTwoHandMeleeCurlDeg, kMeleeCurlAxis[0],
                kMeleeCurlAxis[1], kMeleeCurlAxis[2]);
        }
    }
    float g[9];
    twohand::CurlFinger(f.grip0, kMeleeCurlAxis, RigTwoHandMeleeCurlDeg, f.curls, g);
    twohand::FingerBlend(authored12, g, t, out12);
    InterlockedIncrement64(&g_fingerWrites);
    return true;
}

bool TwoHandGrippingMelee()
{
    return RigTwoHandGrip && InterlockedCompareExchange(&g_gripping, 0, 0) != 0 &&
           InterlockedCompareExchange(&g_kind, 0, 0) == static_cast<LONG>(Kind::Melee);
}

bool TwoHandGripping()
{
    return RigTwoHandGrip && InterlockedCompareExchange(&g_gripping, 0, 0) != 0;
}

}  // namespace dlvr
