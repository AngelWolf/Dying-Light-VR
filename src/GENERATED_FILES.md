# Generated files

Some files in this tree are not written by hand. They are tables produced offline by Python scripts and are checked in as
they are: hand poses, finger curls, reload animations, gun and weapon shapes, mesh data, compiled shaders and test data.

**The generators are not published, and neither are their inputs.** The scripts read:

- the game's own data files (meshes, animations, scripts from the installed game's archives) - not ours to redistribute;
- hand-authored Blender scenes built on those meshes (the poses and the reload animations);
- recorded sessions from a headset (arm and hand captures, logs).

Each generated file names its generator in its first lines as `tools/<name>.py`. That path is the project's private tools
folder, kept here as the record of where each table came from. To change one of these tables, open an issue rather than
editing the numbers by hand: several are checked against each other at compile time (`static_assert`) or at run time.

| file | generator (not published) |
|---|---|
| `bootstrap/shaderbin.gen.h` | gen_shaderbin.py |
| `common/binds_table.gen.h` | binds_template.py |
| `ovr2xr/bladeline_table.inc` | bladeline_table.py |
| `ovr2xr/bow_fingers.inc` | bow_draw_fingers.py |
| `ovr2xr/bow_hand.inc` | bow_draw_hand.py |
| `ovr2xr/bow_nock_fingers.inc` | bow_nock_fingers.py |
| `ovr2xr/bow_nock_hand.inc` | bow_nock_hand.py |
| `ovr2xr/bow_stand_pose.inc` | bow_stand_pose.py |
| `ovr2xr/buggy_hold.inc` | buggy_hand_poses.py |
| `ovr2xr/buggy_poses.inc` | buggy_hand_poses.py |
| `ovr2xr/elbowreal_golden.inc` | armcap_realarm.py |
| `ovr2xr/finger_track_poses.inc` | finger_track_poses.py |
| `ovr2xr/gun_shapes_table.inc` | gun_shapes.py |
| `ovr2xr/hand_shape_table.inc` | hand_shape.py |
| `ovr2xr/melee_fingers.inc` | melee_grip_fingers.py |
| `ovr2xr/reload_break_gate.inc` | break_gate_export.py |
| `ovr2xr/reload_break_pe.inc` | break_pe_export.py |
| `ovr2xr/reload_break_sch.inc` | break_sch_export.py |
| `ovr2xr/reload_dbarrel_anim.inc` | dbarrel_anim_export.py, msh_extract.py |
| `ovr2xr/reload_dbarrel_e_anim.inc` | dbarrel_e_export.py |
| `ovr2xr/reload_dbarrel_guns.inc` | dbarrel_guns_export.py, msh_extract.py |
| `ovr2xr/reload_extra_poses.inc` | extra_poses_export.py |
| `ovr2xr/reload_fx_table.inc` | reload_fx.py |
| `ovr2xr/reload_guns_table.inc` | reload_guns.py |
| `ovr2xr/reload_hand_poses.inc` | reload_hand_poses.py |
| `ovr2xr/reload_pump_anim.inc` | pump_anim_export.py, blender_thumb_read.py |
| `ovr2xr/reload_pump_lock.inc` | pump_port_lock.py |
| `ovr2xr/reload_pump_table.inc` | reload_guns.py, anm2_tracks.py |
| `ovr2xr/reload_rv_rest.inc` | rv_rest_export.py |
| `ovr2xr/reload_xbow_table.inc` | reload_guns.py, anm2_tracks.py |
| `ovr2xr/throwable_poses.inc` | throwable_hand_poses.py |
| `ovr2xr/twohand_butt.inc` | twohand_melee_butt.py |
| `ovr2xr/twohand_forend.inc` | twohand_forend.py |
| `ovr2xr/twohand_grip.inc` | twohand_grip.py |
| `ovr2xr/twohand_melee.inc` | twohand_melee_pose.py |
| `ovr2xr/twohand_meshes.inc` | twohand_meshes.py |
| `ovr2xr/twohand_shotgun.inc` | twohand_shotgun.py |
| `tests/data/twohand_shotgun_check.inc` | twohand_shotgun.py |
| `tests/data/default_pro_sample.bin` | scrub_profile_fixture.py (a game profile with every value blanked except the controller-layout keys) |
