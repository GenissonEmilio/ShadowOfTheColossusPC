# VU1 fast recompiler (plan)

Why: on the Vita (Cortex-A9, 444 MHz) the VU1 thread is the bottleneck of every
3D scene (eagle cliff at real Vita speed in Vita3K: ~0.6 fps, VU1 thread ~90%
busy). The current `vu1rec_<hash>.cpp` code (VU1Interpreter::generateRecompiled)
is an unrolled interpreter: per instruction it checks the cycle budget, calls
`commitReadyPipelines()`, decodes the lower op at run time (`I.execLower(...)`),
resets the zero registers, checks branches and calls `advanceOneCycle()`.
Hand-written native loops (game/overrides/sotc_vu1_world.cpp: world 0x1140,
lit 0x1818, UV passes 0x0f80/0x1060) are exact (PS2X_VU1_NATIVE=verify:
5.3M runs, 0 mismatches) and much cheaper. The goal is to generate that kind
of code for every routine of the 14 images.

## Idea

Generate, per image, straight C++ that runs a program in program order, pair
by pair (both halves read before either writes), with the values the
interpreter's timing model would give, decided at generation time:

- The interpreter queues every write with a ready cycle (VF/VI/ACC writes,
  FMAC 4 cycles; Q from DIV/SQRT/RSQRT; P from EFU; flags) and stalls readers
  of pending registers (interlock). A read of Q, P or of the MAC/status/clip
  flags does not interlock: it sees the old value until the result is ready.
- So the generator simulates the timing model abstractly along each path:
  per instruction, its issue cycle relative to the block start, stalls from
  pending writes, and which pending Q/P/flag results are visible at each
  read. Straight-line code is exact. At loop headers, iterate until the
  pending-write state (relative ready times) is the same as on the previous
  pass; if it does not settle in a few passes, leave that routine on the
  current path.
- Flags are computed only where something reads them (liveness).
- XGKICK: submit the packet at the kick (the microcode double-buffers its
  output; the interpreter's concurrent-transfer timing is not observable to
  the program except through stalls on a second XGKICK, which the timing
  simulation covers).
- Entry/exit: register per (image hash, entry pc) through vu1native (the same
  hook the hand-written loops use), run to the end of the program ([E] bit +
  delay slot), report "ended" and the cycle count from the simulation.

## Validation

- PS2X_VU1_NATIVE=verify already runs the interpreter alongside and compares
  registers and VU data memory after every native run. Run the scripted
  routes (tools/gameplay_fps.sh, the opening) and enable only routines with
  0 mismatches.
- Then Vita3K at real Vita speed: VITA3K_CPU_MHZ=444 VITA3K_CORES=3, the
  [load] line (VU1 busy share) and VITA3K_PROFILE.

## Order of work

1. Generator skeleton: decode, CFG from each entry (branches, BAL/JR pairs
   with known return addresses), emit program-order code for blocks without
   Q/P/flag reads; fall back otherwise. Verify.
2. Timing simulation for Q (DIV/WAITQ) and clip flags (FCAND/FCOR after
   CLIP) — the patterns in the world loops.
3. MAC/status flags, P (EFU), XGKICK.
4. NEON: the per-lane loops become 4-wide vector ops (vf registers as
   float32x4_t; normalisation as compare/select).
