# Decompiling the game, function by function

The game runs as recompiled code: every MIPS function of the boot ELF and
the XFF modules turned into C++ mechanically (recomp/game/output, ~16 000
functions, 2.5 MB of MIPS). That code works but cannot be read or changed.
Decompiling here means rewriting a function (or a whole system) by hand in
readable C++ and putting it in place of the recompiled one, proven equal.
Not a matching decomp: nothing has to rebuild a PS2 binary.

## The loop

1. **Find what matters.** `PS2X_PROFILE=<file>` samples the running EE
   context's pc 1000 times a second (`PS2X_PROFILE_START=<s>` skips the
   boot); `tools/guest_profile.py <file> --module` names the functions and
   their callers. Run it on the route: `PS2X_PROFILE=... tools/native_run.sh <dir>`.
2. **Read it.** `tools/ghidra.sh import` builds the Ghidra project once
   (build/ghidra, from recomp/game/SOTC.elf with the recomp names applied);
   then `tools/ghidra.sh DecompileTo.java $PWD/build/ghidra/out <name|0xaddr> ...`,
   `Disasm.java 0xaddr`, `Callers.java name`, `RefsTo.java 0xaddr`,
   `Containing.java 0xaddr`. Decompiler output is a guide, not the truth:
   check the disassembly where it matters (it splits functions at stray
   branches, see FUN_00125448 inside dpmul).
3. **Write it.** In `game/overrides/decomp_<system>.cpp`, a function with the
   recompiled signature, registered with `SOTC_DECOMP_FUNCTION(address, name, fn)`
   (`SOTC_DECOMP_FLOAT_FUNCTION` when it returns in f0). Arguments in a0-a3
   (64-bit GPRs), f12/f13; result in v0 or f0; end with `returnTo(ctx)`.
4. **Prove it.** `SOTC_DIFFTEST=name,name` (or `all`) runs each call twice on
   the same state, the native one and the recompiled one, and compares v0,
   f0, the callee-saved registers and every byte of RAM and scratchpad
   (the dead stack below sp excepted). `SOTC_DIFFTEST_FIRST` / `_EVERY` set
   how many calls are checked (each check copies 64 MB: the game stutters).
   A function goes in with 0 mismatches over the route.
5. **Measure it** with the profiler again (Mac, then the Vita).

`SOTC_DECOMP=0` puts every recompiled function back; `SOTC_DECOMP_SKIP=name`
one of them.

A function the recompiled code reaches by a tail jump (`j` at the end of
its caller) is called directly as C++, not through the function table, so
its registration is never seen there: replace the caller as well (see
decomp_keyboard.cpp). Check with SOTC_DIFFTEST that the checks count up.
Difftest runs are slow enough that the scripted route's inputs (timed in
VBlanks) land elsewhere: compare pictures from a run without it.

What a native function has to keep, learned the hard way:

- **Relocated code in the XFF modules.** The modules' references to the
  boot ELF (and to each other) are patched into the code when the game
  links them: `lui` / `lw` pairs and `jal`s whose values differ from the
  disassembly (the recompiled code reads them at run time: `_dynInsn`,
  `jumpTarget = READ32(site)`). Read them the same way:
  `sotc_decomp::relocated(rdram, luiSite, loSite)` and
  `sotc_decomp::jalTarget(rdram, site)`. charaLinks first compared against
  the disassembly's 0x1b7b60 and got another pointer.
- **Guest calls** from a native function: `sotc_decomp::callGuest(rdram,
  ctx, runtime, target, ra, sp)` with the original's ra (the jal site + 8)
  and frame sp; set the argument registers the callee reads; whatever the
  callee reads from the caller's frame has to be written there first.
- **Registers compared:** v0 (also for void functions), f0, s0-s7, fp, gp,
  sp, ra. Keep v0 / f0 as the original leaves them (the last value it
  wrote, often a callee's). VU0 registers, fcr31 and the other FPU
  registers are not compared: the native code writes back what the
  original leaves there anyway where it is cheap (vf / ACC / Q / R,
  status).
- **Same operations, same order:** FPU_* and PS2_V* macros (EE rounding,
  clamping, operand alignment), the original's constants by their bits
  (0.001 is 0x3a83126e, not 0x3a83126f).
- **Rare cases go back to the recompiled code** before anything is written
  (`sotc_decomp::recompiled(address)`): a dying particle, a collision test,
  a negative count. A function can also do its hot part natively and then
  enter the recompiled one at one of its labels with the registers and
  frame it expects there (decomp_cloth.cpp, at 0x1138200).
- **Unity build:** every decomp_*.cpp gets a namespace of its own.
- The difftest is re-entrant: a checked function reached while a check
  runs (recursion) runs recompiled, unchecked.

## Done

| Module | Functions | Checks on the route | Notes |
|---|---|---|---|
| decomp_softfloat.cpp | dpadd, dpsub, dpmul, dpdiv, dpcmp, litodp, dptoli, fptodp, dptofp, __floatdisf | ~93 000, 0 mismatches (dptoli not called on the route) | the SDK's software doubles: no rounding (truncation), denormals as zero; ~3% of the EE's busy time |
| decomp_sortlist.cpp | GetNextSortObj, ClearSortList | 19 000 + 3 000, 0 mismatches | the display's sort buckets; 4% of the Vita's game thread recompiled (throttled attract demo) |
| decomp_keyboard.cpp | kbGetStatus, usbdevFrameInit | 1 500, 0 mismatches | 256 keys copied every frame (1.2%). usbdevFrameInit tail-jumps to kbGetStatus and the recompiler makes that a direct C++ call past the function table: replaced in the caller too |
| decomp_time.cpp | mktime (newlib, with validate_structure) | 900, 0 mismatches | our console clock reads zeros, so the year is -1900 and mktime's year loop ran ~1 900 times a frame (0.8%); days counted in closed form, checked against the loop for every year in +-10000 |
| decomp_softfloat.cpp | litodp, __floatdisf (faster) | 160 000, 0 mismatches | litodp normalised bit by bit (up to 60 steps): one clz; __floatdisf within +-2^24 is the plain conversion (every step exact) |
| decomp_particles.cpp | manager_sub_0113CA58 (particle step) with 0113D280, 01141918, 01141968, 01141B70 | 37 000 (attract demo + route), 0 mismatches | ~9% of the Vita's game thread. Dying particles, the collision test (0113D5F0) and shape 0 go to the recompiled code; iosGetRandom / iosGetReflectVector / the damage test (0113C8C8) stay guest calls. In the attract demo 99.8% of the calls are native; the damage test runs for almost every particle (guest) |
| decomp_chara.cpp | manager_sub_010CE558 (charaLinks), manager_sub_010CDD10 (findRecord) | 2 000 + 1 200, 0 mismatches | the character's handle-list searches (3.4% + 0.9%) |
| decomp_layout.cpp | UpdateLayoutClassGeometry | 1 800, 0 mismatches | bone matrices: inverses, scaled, products (3.6%); iosScaleMatrixV / iosGetScaleMatrixV / iosGetScaleInverseVector stay guest calls |
| decomp_cloth.cpp | manager_sub_01137F78 (cloth mesh) | 8 500, 0 mismatches | the grid normals and the mesh vertices natively (2.1%), then the recompiled function from 0x1138200 (display list layer, camera matrix, meshDisp / meshFlip) |
| decomp_clothstep.cpp | manager_sub_011370E8 (cloth step) | 8 500, 0 mismatches | gravity, wind, VRNEXT noise, the pinned row ends (1.3%) |
| decomp_skeleton.cpp | manager_sub_010B8590 (skeleton pose, recursive) | 37 000, 0 mismatches | the quaternion -> matrix VU0 block natively (2.8%), the matrix / quaternion stack helpers as guest calls |
| decomp_math.cpp | iosGetSinCosf | 30 000, 0 mismatches | VU0 polynomial (1.2%) |

Together (throttled Vita3K attract demo, frames 1100-1460, the same VPK
with SOTC_DECOMP_SKIP / PS2X_FPU_SETCSR=1 for the old code): 45.7 -> 38.0
ms a frame, game thread 42.1 -> 34.5 ms.

## Next candidates (profile 2026-10-20, throttled attract demo, after the above)

- manager_sub_010A9EA8, texture scroll + CLUT animation (3.3%; the native
  renderer reads its output: rewrite, don't skip).
- ExecCharaGeometry (1.6%), manager_sub_0113E940 (1.5%), 010AC6C8 (1.5%,
  a 15-way curve evaluation), ClipCollisionDirectCallBack (1.4%),
  0108D730 (1.3%).
- isysWakeupSubThread (1.8%): the sub-thread switch saves and restores
  every 128-bit register.

## Older candidates (profile 2026-10-17, native route)

- The PS2 display lists the game still builds for models the native
  renderer draws itself (manager_sub_01097xxx, iosDlChainTailCurrent,
  transMicroCode, and the VIF1 chain walk in iosDmaSendSChainPath1, ~6%).
- modelDisp's 25% is mostly host code (native renderer, skinning): profile
  it on the host side.
- Collision: ClipCollisionDB, GetLayoutClass, GetCollisionDBAttributeAdr.
