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

## Done

| Module | Functions | Checks on the route | Notes |
|---|---|---|---|
| decomp_softfloat.cpp | dpadd, dpsub, dpmul, dpdiv, dpcmp, litodp, dptoli, fptodp, dptofp, __floatdisf | ~93 000, 0 mismatches (dptoli not called on the route) | the SDK's software doubles: no rounding (truncation), denormals as zero; ~3% of the EE's busy time |
| decomp_sortlist.cpp | GetNextSortObj, ClearSortList | 19 000 + 3 000, 0 mismatches | the display's sort buckets; 4% of the Vita's game thread recompiled (throttled attract demo) |
| decomp_keyboard.cpp | kbGetStatus, usbdevFrameInit | 1 500, 0 mismatches | 256 keys copied every frame (1.2%). usbdevFrameInit tail-jumps to kbGetStatus and the recompiler makes that a direct C++ call past the function table: replaced in the caller too |

## Next candidates (profile 2026-10-17, native route)

- The PS2 display lists the game still builds for models the native
  renderer draws itself (manager_sub_01097xxx, iosDlChainTailCurrent,
  transMicroCode, and the VIF1 chain walk in iosDmaSendSChainPath1, ~6%).
- modelDisp's 25% is mostly host code (native renderer, skinning): profile
  it on the host side.
- Collision: ClipCollisionDB, GetLayoutClass, GetCollisionDBAttributeAdr.
