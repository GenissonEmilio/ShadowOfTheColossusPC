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

## Native: brighter than the emulated picture, most in the opening

Measured 2026-10-15 at the same game frame: emulated / native mean
brightness about 0.68 in the opening, 0.84 in the shrine, 0.95 on the plain.
In the shrine it looks like an offset (native mid tones about 16 higher,
equal near white); in the opening like a factor. Ruled out (each switched off
on its side, the gap stayed): vertex fog, bloom, depth of field, the game's
screenBlur and shadows, the display merge (PMODE 0x8023 blends the frame with
itself). The opening's models are mostly unlit (vertex colour x texture).
Also matching in the opening: the fog colour (FOGCOL 30 40 45, the same
in the GS and natively) and F per vertex (z 3650 -> w 245 -> F 132.6
natively, 132 in the GS stream); switching the native lighting off changes
nothing; the unlit models' instance factor (VU 991) is 1. Not yet checked:
the GS texture function and CLUT path against the native decode on one
surface, per draw.

## Native vs emulated: bloom over the sky and windows (reference doubtful)

On the new route (tools/native_run.sh) the native sky outdoors and the
shrine window blow out to white, while the emulated picture shows clouds
and the landscape through a soft glow. The game's own chain says the
native is right: the mask (Z <= 0x118c094 + 1) covers the sky also on the
PS2 (bgEnd clears Z to 1 after layer 1), the 12 shifted copies are a
normalized lerp, and the final add is colour 0x118c140 (0xaf/0xaf/0xa5
inside, 0x51/0x5e/0x5b outdoors) x alpha 0x80, which saturates a bright sky.
An earlier finding (memory: GS-exact blend gives a bright shrine window; the
software path sampled sprites half a texel off and dimmed the bloom) points
at the emulated GS here. Needs a real reference (PCSX2 software or the
console) before tuning either side.

## Native zFog (opt-in): layer 1 kept out of the main pass

bgEnd copies the main frame (layer 1 drawn into it) to the 256 x 256
background and then sets Z to 1 with a full-screen Z-only sprite: layer 1 is
in the main picture, sharp, and the zFog/bgDraw blend replaces it only by
the fog alpha. The native renderer, with SOTC_NATIVE_ZFOG=1, skips layer 1
in the main pass ("shown through the backdrop"), which leaves flat, blurred
patches where layer 1 surfaces come close to the camera (shrine floor near
the stairs). To fix before turning zFog on.
