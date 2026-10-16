# Native port plan

Goal: nothing of the PS2 hardware is emulated at run time. The game code
stays the recompiled EE code (already native ARM/x86); every hardware block
it talks to is replaced by a native implementation on the Mac and on the
Vita (GXM does the graphics work). Same behaviour and picture on both
platforms, 30 fps on the Vita at real speed (Vita3K 444 MHz, 3 cores).

## Order: Mac first, then the Vita

Every step is developed and gated on the Mac (fast builds, a route that runs
at full speed). Each phase is then brought to the Vita in one go and checked
there (Vita3K at 444 MHz / 3 cores, then a real Vita). To make that cheap,
everything is written for the Vita from the start: each shader also in CG,
only GL that vitaGL has, Vita memory and a Cortex-A9 CPU budget in mind.

## Measurement gate (every step)

A step is one hardware path moved to native (for example "lit loop 0x0710
as a GPU batch"). It is committed only after this gate, and its numbers go
in `docs/native_port_log.md` (one row per step, before and after):

1. **Same route every run.** `tools/measure.sh <label>`: title -> Load Game
   (the save in `memcards/Mcd001.ps2`) -> the wake-up cutscene skipped ->
   Wander standing at the altar -> a walk out of the shrine. The menus are
   driven by game frame (`PS2X_INPUT_CLOCK=frames`); the rest hangs off the
   start of the cutscene (a word in guest RAM) and is timed in VBlanks, the
   game's own clock. With `SOTC_FRAME_INTERVAL=4` every frame lasts four
   VBlanks, so the game runs the same in every run, walk included. The Vita
   reads the same switches from `ux0:data/celosia/env.txt`.
2. **Speed.** Busy ms per game frame of each emulation thread (game / vu1 /
   gif / gs) standing and walking (capped run: comparable to ~2%), and the
   fps of an uncapped run (~10% noise), printed in one line.
3. **The new path did the work.** Counters per frame (like `direct`)
   show its share of the work, and its fallback count.
4. **Picture.** The route's pictures (SNAP entries of the input script,
   saved by the GS backend) are compared with the previous step's by
   `tools/framediff.py`; differences above the threshold are looked at.
5. **Rule.** No fps drop, no picture difference that is not explained.
   Otherwise the step is fixed or reverted, never committed "for later".
   The emulated path stays behind a switch until the step is proven, then
   is removed.

At the end of each phase: the same route on the Vita (Vita3K, no other
Vita3K running; then a real Vita).

## Phases

0. **Measurement tooling.** `tools/measure.sh`, `tools/framediff.py`,
   frame-clocked input and snapshots, env.txt on the Vita; baseline row in
   the log.
1. **Real cost of the game logic on the Vita.** Finish the idle-thread wait
   (no core spun by SotC's idle loop), then measure the game thread alone.
   This sets the budget: if the logic alone is over ~33 ms a frame, its
   hot spots (VU0 maths -> NEON, etc.) move ahead of phase 5.
2. **VU1 -> GPU.** Each microcode program becomes a VIF->GPU batch with its
   maths in the vertex shader: world (done), lit loop 0x0710, the second-UV
   pass, characters (059698f3), e471dbf3, b504a980, the rest. Exit: VU1
   instructions ~0 on the route; no VU1 thread.
3. **GS -> native.** Textures decoded once into GXM/GL textures from the
   game's uploads (no emulated GS memory, swizzle or CLUT per frame); draw
   state set natively; the GIF decoder goes away. Exit: no GS/GIF thread.
4. **Native renderer.** Hook the game's draw calls for models (found with
   Ghidra) and draw them from their own data, meshes kept in GPU buffers;
   no DMA chains walked per frame. Exit: no VIF/DMA work per frame.
   Mac, 2026-10-16: with SOTC_NATIVE=1 (GS::skipRaster) the chains are only
   walked: `Vif1Compactor` keeps the VIF1 codes without their data and
   `GifEventScanner` reads GIF data in place for SIGNAL/FINISH/LABEL, the
   only GS registers the game waits on. Left: the walk itself and the
   VIFcode pass over the compacted stream (~0.5 % of the game thread).
5. **Game-logic hot spots.** VU0 macro maths and the top profiled EE
   functions written natively (NEON on the Vita).
6. **Audio.** SPU2/IOP replaced by a native mixer fed by the game's sound
   requests. Done on the Mac (2026-10-16): native SG2 driver and mixer
   (docs/native_audio_plan.md); with SOTC_NATIVE=1 the emulated IOP runs no
   code once GAMECORE is entered (`IopSubsystem::enterNativeMode`): RPC
   0x11223344 native, DBCMAN (0x80001300) by its host service, the
   DualShock's vibration (0x8000131e/f) and the USB serial console
   completed without effect; pad and memory card were host-side already.
   The IRXs still load and start on the emulated IOP during the boot: the
   boot's timing places the XFF modules (without it GAMECORE lands 0x40
   bytes off, the main thread's letter post being allocated before it;
   `SOTC_LOG_MALLOC=1` shows the order), and the recompiled code needs the
   captured layout.
7. **System.** Kernel threads/semaphores/timers on native threads and
   clocks; CDVD reads as direct file reads.

Each phase ends with a full route on both platforms and a real Vita run.
