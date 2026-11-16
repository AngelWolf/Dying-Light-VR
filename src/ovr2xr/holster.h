// Right-shoulder holster - docs/SPEC-SHOULDER-HOLSTER-2026-09-29.md. Settings live in holster.cpp
// (each with its [DEP:] marker); the kSettings rows are in ovr2xr.cpp next to vr_grip.
#pragma once

namespace dlvr {

extern bool  HolsterEnable;    // holster_enable     - master. DEFAULT ON.
extern float HolsterCenterX;   // holster_center_x   - zone centre, head-yaw frame, metres (+X right)
extern float HolsterCenterY;   // holster_center_y   - (+Y up)
extern float HolsterCenterZ;   // holster_center_z   - (+Z behind)
extern float HolsterRadius;    // holster_radius     - enter radius, metres; exit = x1.2
extern float HolsterHaptic;    // holster_haptic     - pulse amplitude 0..1; 0 = no pulses
extern int   HolsterTapMs;     // holster_tap_ms     - D-pad-right tap length for the weapon switch, ms

// Once per scePad poll, from dlvr_FilterScePad, BEFORE the vr_grip block. Returns true when the right
// grip's R1 must NOT be OR'd this poll (a holster press is active). outDpadRight = true while the
// weapon-switch tap is being held (OR D-pad right into the pad buffer). holster_enable=0 -> both false at
// the first line, so the grip block behaves exactly as before. vrGripOn = vr_grip; with it off the holster
// refuses by name (once) and returns false.
// Right hip (Jay 2026-10-04): a second zone on the same right grip. outDpadLeft = true while its item-swap tap
// is held (OR D-pad left). The function runs while EITHER holster_enable or holster_rhip_enable is on.
extern bool  HolsterRHipEnable;    // holster_rhip_enable
extern float HolsterRHipCenterX;   // holster_rhip_center_x  - head-yaw frame, metres (+X right)
extern float HolsterRHipCenterY;   // holster_rhip_center_y
extern float HolsterRHipCenterZ;   // holster_rhip_center_z
extern float HolsterRHipRadius;    // holster_rhip_radius
extern int   HolsterRHipTapMs;     // holster_rhip_tap_ms    - D-pad left tap length
// Hip yield (spec 2026-10-26-roomscale-physical-crouch-design §2.4): a right-hip press with a body / severed limb under the hand goes
// to the body (LootYieldAt) - the hip does not fire and R1 is not withheld. While the limb reading is due (<= 150 ms) R1 is withheld
// and nothing fires. outOwnsGrip = a holster press is active (pass it to LootPoll); the return value stays "do not OR R1".
// gameMenu = an in-game menu is up (a wheel open or confirming is aborted, named in its [WHEEL] line).
bool HolsterPoll(bool vrGripOn, bool& outDpadRight, bool& outDpadLeft, bool& outOwnsGrip, bool gameMenu = false);

// Left hand: hip (heal, D-pad down - was D-pad left until 2026-10-04) + left temple (torch, D-pad up). Fires ON GRIP
// (the press edge), not on release. Each zone has its own enable; both DEFAULT ON.
extern bool  HolsterHipEnable;    // holster_hip_enable
extern float HolsterHipCenterX;   // holster_hip_center_x   - head-yaw frame, metres (+X right, so left is -)
extern float HolsterHipCenterY;   // holster_hip_center_y
extern float HolsterHipCenterZ;   // holster_hip_center_z
extern float HolsterHipRadius;    // holster_hip_radius
extern int   HolsterHipTapMs;     // holster_hip_tap_ms     - D-pad down tap length
extern bool  HolsterHeadEnable;   // holster_head_enable
extern float HolsterHeadCenterX;  // holster_head_center_x
extern float HolsterHeadCenterY;  // holster_head_center_y
extern float HolsterHeadCenterZ;  // holster_head_center_z
extern float HolsterHeadRadius;   // holster_head_radius
extern int   HolsterHeadTapMs;    // holster_head_tap_ms    - D-pad up tap length, < 100 (FAST_EQUIP_ITEM)

// Once per scePad poll, BEFORE the vr_grip block. Returns true when the left grip's L1 must NOT be OR'd this
// poll (a left holster press is active). outDpadDown / outDpadUp = true while that tap is held. Both zones
// off -> false at the first line, so the L1 path behaves exactly as before. The left HIP yields to a body as the right hip does
// (the temple never does); outOwnsGrip as HolsterPoll's.
bool HolsterLeftPoll(bool vrGripOn, bool& outDpadDown, bool& outDpadUp, bool& outOwnsGrip);

// Spec 2026-11-01-manual-reload-design goal 2: HEAL moved to the LEFT CHEST (D-pad down there); the LEFT HIP is the ammo pouch - it
// fires no tap, and is live only while manual_reload is on (ManualReload, reload.cpp). Body-locked like the hips.
extern bool  HolsterLChestEnable;    // holster_lchest_enable
extern float HolsterLChestCenterX;   // holster_lchest_center_x - body frame, metres (+X right, so left is -)
extern float HolsterLChestCenterY;   // holster_lchest_center_y
extern float HolsterLChestCenterZ;   // holster_lchest_center_z (+Z behind, so the front is -)
extern float HolsterLChestRadius;    // holster_lchest_radius
// True while the LEFT grip is held inside the left-hip zone (the pouch press). Any thread. COST: one volatile read.
bool HolsterLeftPouchHeld();

// The crossbow's left-shoulder TAKE zone (plan 2026-11-03-manual-reload-crossbow Task 4): the right-shoulder zone mirrored in x for
// the LEFT hand (holster_center_x negated, the same y / z / radius, the shoulder's frame and enter / leave hysteresis - no new key).
// reload.cpp sets it live each tick (manual reload on AND a crossbow in the right hand); HolsterLeftPoll evaluates the zone only while
// it is live, and while live with the left hand in the zone the left-chest heal, the left-hip pouch and the left-temple torch do not
// fire on that press (holster::LeftChestFires / LeftTempleFires; temple: Ruling T4-a). Not live -> HolsterLeftShoulderIn() is false. Any thread. COST: one volatile write / read.
void HolsterSetXbowTakeLive(bool on);
bool HolsterLeftShoulderIn();
// Why the LEFT zones (and with them the crossbow take) are off this session, as HolsterLeftPoll last found it: "vr_grip=0", or the
// overlapping left pair it refused ("the chest and hip" ...); nullptr = not refused (or the poll has not run). For reload.cpp's
// crossbow ARMED / TAKE OFF lines (crossbow plan Task 5, carry ruling T4 M4). Any thread. COST: one volatile read.
const char* HolsterLeftTakeRefusal();

// holster_hip_from_calib (Jay 2026-10-15): both hip zones at the BELT, placed from the arm calibration's down-pose resting hand
// (dlvr\arm_fit.txt r_/l_rest_hand_*_m) - up holster_hip_belt_up_cm (10..15) and forward holster_hip_belt_fwd_cm (0..4).
// Off, or no usable rest hand for that side = the fixed holster_rhip_center_* / holster_hip_center_* exactly as before.
extern bool  HolsterHipFromCalib;   // holster_hip_from_calib
extern float HolsterHipBeltUpCm;    // holster_hip_belt_up_cm
extern float HolsterHipBeltFwdCm;   // holster_hip_belt_fwd_cm
// The hip zone centre IN USE for side 0 = right hip (item swap), 1 = left hip (heal, and the throwables' ready zone), head-yaw
// frame +X right +Y up +Z behind, metres. Logs its source once and on every change. COST: one shared lock + a few compares.
void HolsterHipCentre(int side, float out[3]);

// ===== BODY-LOCK (docs/superpowers/specs/2026-10-20-holster-body-lock-design.md) =====
// The arm solve (right side) publishes the `pelvis` bone in TRACKING space (the inverse of its own hand mapping) - since 2026-10-20
// run 1 only for the 5 s line's pelvis - neck field and the new-model reset (the hips anchor on the neck point);
// lock-free, defined in boneblend.cpp. Tracking = false: nothing published yet. ageMs = since the publish.
void HolsterPelvisPublish(const float trk[3], unsigned long long model);
bool HolsterPelvisTracking(float out[3], unsigned long long* modelOut, unsigned long long* ageMsOut);
// The game's own recentre (ovr_RecenterTrackingOrigin) turned appSpace: the facing and every latch start again.
void HolsterBodyReset();

// The zones' frame for OTHER modules (Jay 2026-10-20: the bow's quiver, the throwables' ready zone and the swing's holster check
// "should" follow the body too). Any thread. out = the hand's offset in that zone's frame (+X right +Y up +Z behind, metres) -
// body-locked when the zone is latched and the holster polls refreshed the body frame within 250 ms, else today's head-relative
// LocalOffset. Returns true when body-locked. The caller passes its own head pose (the neck point is computed from it).
// COST: one shared SRW lock, one NeckPoint, a few dozen flops.
// kHolsterLChest (heal, spec 2026-11-01-manual-reload-design goal 2) is a BODY zone (3 here); holster_place_logic.h numbers it 4.
enum HolsterZone { kHolsterShoulder = 0, kHolsterRHip = 1, kHolsterLHip = 2, kHolsterLChest = 3 };
enum HolsterCaller { kHolsterCallerBow = 0, kHolsterCallerThrowable = 1, kHolsterCallerSwing = 2 };
bool HolsterZoneLocal(int zone, int caller, const float headPos[3], const float headQ[4], const float grip[3], float out[3]);

// ===== PLACING A HOLSTER BY HAND (docs/superpowers/plans/2026-10-30-holster-place-by-hand.md Task 3) =====
// Placing (spec goal 2). Render thread. Begin is ignored while one is running.
void HolsterPlaceBegin(int zone);               // 0 shoulder, 1 right hip, 2 left hip, 3 head
bool HolsterPlacing();                           // a placement is waiting for the trigger
// Once per overlay frame while HolsterPlacing(); menuEdge = the menu button's press edge this frame.
// Returns the outcome when the placement ENDS this frame, else -1.
// kPlaceNotRunning: the zone is latched to the body but its body frame is not updating (the holster polls are not running).
enum HolsterPlaceResult { kPlaceStored = 0, kPlaceCancelled = 1, kPlaceUntracked = 2, kPlaceOutOfRange = 3, kPlaceOverlaps = 4,
                          kPlaceNotRunning = 5 };
int  HolsterPlaceTick(bool menuEdge);
void HolsterPlaceDefault(int zone);              // compiled default centre + radius (a hip then follows the calibration again)
// The zone's effective centre / radius / enable, its partner's, and the largest radius the slider may take.
bool  HolsterZoneParams(int zone, float centre[3], float* radius, bool* enabled);
float HolsterRadiusMax(int zone);
bool  HolsterSameHandOverlap(int hand);          // 0 right, 1 left - for the page's warning (spec goal 12)
// The zone's centre in the grip poses' space (Task 1 G1) for drawing; false = no head pose.
bool HolsterZoneWorld(int zone, float out[3]);
// Notes (Task 3): HolsterPlacing() is safe from any thread (one volatile read). HolsterPlaceTick(true) is also the way to end a
// placement when the panel closes for another reason. HolsterZoneParams / HolsterZoneWorld / HolsterRadiusMax /
// HolsterSameHandOverlap: COST up to two HolsterHipCentre calls (each one shared lock + ArmFitRestHand) and, for World, one
// GetHeadPose + one shared SRW lock - fine once per overlay frame, not per zone per pixel.

// ===== SELECTION WHEELS (docs/superpowers/specs/2026-10-27-vr-selection-wheels-design.md) =====
// Hold a holster grip HolsterWheelHoldMs to open the game's own wheel; the OTHER hand's position about the body centre picks the
// slot; the release confirms; the centre steers back to the current slot (D11). Rides holster_enable / holster_rhip_enable (D10).
extern int   HolsterWheelHoldMs;    // holster_wheel_hold_ms  - the hold that opens a wheel (Jay: 750)
extern float HolsterWheelCentreM;   // holster_wheel_centre_m - the centre (cancel) radius about the body centre, metres
// True while a wheel owns the RIGHT stick: x / y = full deflection toward the slot to steer to (+y up). dlvr_FilterScePad writes the
// stick bytes from it and skips the turn. Any thread (pad poll). COST: two volatile reads.
bool HolsterWheelStick(float& x, float& y);
// The player's current slot (0..3 = up / right / down / left) on wheel 0 weapons, 1 throwables, 2 arrows, 3 bolts; -1 = not read
// within 500 ms. Published by item_hide_by_state's hook (boneblend.cpp). Any thread.
int HolsterWheelCurrentSlot(int which);
// The arrow wheel from the bow's quiver (2026-11-11, Jay: "Yes."): true while the quiver drive's wheel is open or confirming - the
// quiver hold may then leave the shoulder zone (BowNockTick's BoltHoldStep). Any thread. COST: one volatile read.
bool HolsterArrowWheelOpen();
// The arrow wheel (plan Task 5, Jay: "C for now, but make it still selectable with the left hand"): true while the layer must keep X
// (Square, 0x8000) down - from the player's own X press with the bow out until kWheelConfirmMs after its release. Any thread.
bool HolsterWheelHoldsX();
// Task 6 (D13, Jay: "hide the regular HUD UI whilst the wheels are open, returning once closed"): true from a wheel's OPEN to the
// end of its confirm steer. uipanel's RenderHud then shows only the wheel, further out. Any thread. COST: one volatile read.
bool HolsterWheelShowing();
// The crossbow bolt wheel (plan 2026-11-04-crossbow-bolt-wheel, Jay: R2 "Hold off"): true while its show action is held or it is open -
// dlvr_FilterScePad zeroes R2 (byte 0x09) then. Pad thread. COST: one volatile read.
bool HolsterBoltWheelHoldsR2();
// Task 6 v3 (D13 + Jay "A", INV-642): GAME THREAD (the player-update tail, boneblend.cpp). From a wheel's OPEN to 400 ms past its
// close, every other registered HUD component gets bit 31 of its hide mask (the game's own per-group mask) and SetHudEnabled(false);
// afterwards the bit is cleared and SetHudEnabled(mask == 0) - so a game group that also hides it keeps it hidden. Refuses by name
// (byte check, registry layout check, group count) and then does nothing. Idle COST: one volatile read + a compare.
void HolsterHudHideTick();
// HIPCAP (trace 20261116_1400_hipcap_probe): diag_hip_capture - log-only, DEFAULT OFF. HolsterHipCapture is called from LocateViews
// (ovr2xr.cpp) once per locate with the frame's display time, whether the head has both valid bits, the head pose and the appSpace
// base (position + yaw); it writes one line per display time to dlvr_hipcap.log. Any LocateViews thread. COST off: one bool load.
extern bool DiagHipCapture;
void HolsterHipCapture(long long t, bool headValid, const float hp[3], const float hq[4], const float base[3], float baseYawDeg);

}  // namespace dlvr
