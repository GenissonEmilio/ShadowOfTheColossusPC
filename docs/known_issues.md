# Known issues

## Raised sword: the arm sweeps back and forth (emulated and native)

Seen 2026-10-15 in the shrine and on the plain: with the sword raised and the
stick untouched, Wander's arm swings widely within a second (10 screenshots
in one second all differ). Same in `./celosia run` and with SOTC_NATIVE=1, so
it is in the recompiled game code or the runtime, not in a renderer.

Ruled out so far:
- Stick drift: a radial dead zone (PS2X_STICK_DEADZONE, f683eae) did not
  change it.
- GAMEUTIL_get_yaw_pitch_by_direction (0x10816c0): checked against atan2 on
  15,000 calls, no mismatch (SOTC_MATH_CHECK=1).
- The aim interpolation's time step: doit_aim_angle_interp_cntrl (0x10848b8)
  gets dt ~ 0.033 (1/30 s), rate 4, max 1.57 rad: a stable step
  (step = -diff x rate x dt, 0.13 of the gap per frame).

Not yet checked:
- the aiming IK (nico_IK_targetAiming 0x13758d8, GetTargetLookAtQuaternionOnBase
  0x1134bd0: quaternions with VOPMULA/VOPMSUB, VMR32, VRSQRT in VU0 macro mode);
- the sight controller (doit_nico_sight_on 0x13a9508) and what it aims at
  (the light beam seeks the colossus: its target may jump);
- the VU0 macro translations in ps2xRecomp (vu_translation_helpers.cpp)
  against an interpreter on the same inputs.

Note: a hook around a recompiled function that calls others sees its result
too early (the function runs in pieces the dispatcher continues), so the
SOTC_MATH_CHECK results for sinf/cosf/atanf/acos* are not evidence.

## Native: cutscene walls brighter than on the PS2

In the shrine cutscenes (bossDeathEffect active: word 0x1186290 != 0) the
emulated picture has darker walls and Wander as a blurred ghost. The vertex
fog is not the cause: the native F matches base + scale / w pixel for pixel
and the emulated GIF stream carries the same F. bossDeathEffectEnd blends a
reduced, blurred copy over the frame twice (alpha 0x11862c4, then 0x11862c0
= 160: overshoot) tinted by 0x11862b0..b8; on the PS2 that blurred buffer
must be darker than the picture (its alpha, or the copy's colour). The
native composite (SOTC_NATIVE_GLOW=1) does not darken yet.
