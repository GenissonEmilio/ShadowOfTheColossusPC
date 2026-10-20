# Worklist

What is being fixed or improved, in order. Updated as each item moves.
Status: **todo**, **doing**, **done**, **parked** (waits on something else).
Speeds are from Vita3K unless the row says otherwise; real hardware is not
measured yet. "Throttled" is Vita3K at 444 MHz / 3 cores (`MHZ=444 CORES=3`).

## Vita: speed (goal: a steady 30 fps)

| # | Item | Status | Notes |
|---|------|--------|-------|
| 1 | Renderer CPU cost: draw sort, GL state cache, uniforms | done | 01ef852: render 38.8 -> 12.7 ms, throttled title |
| 2 | Scheduler and VIF/GIF micro-costs | done | b9d2f37 |
| 3 | EE dispatch without scheduler round trips | done | 5a3999c: throttled attract demo 157 -> 112 ms, altar ~410 -> ~250 ms |
| 4 | Scene-load hitches (0.5-1.2 s frames when a scene loads) | done (nothing of ours to cut) | Measured in Vita3K unthrottled, Load Game -> shrine: one ~0.75 s frame (2641 / 2716 / 3218 in three runs) and a run of 0.25-0.35 s ones before the first 3D frame. A per-frame guest sampler on those frames: 97-99% the main EE thread (9) running the game's own scene setup (player / camera / geometry init, flat profile, MVHIST_Init top at 1%); the loader thread 11 and idle (waits on our IOP / CD) 1-3%; hooks 0, render 0.1 ms, 2-3 draws. One logic step per frame (iosGPFrameInit once) worth 2-5 guest VBlanks: the PS2 itself takes 33-83 ms there (behind a fade); the Mac 47-51 ms. Ours only in the first 3D frame: 237 ms = game 183 + hooks 42 (30 textures decoded 6 ms, 137 meshes built ~33 ms, uploads 2.4 ms) + render 12. What makes it shorter is the general speed of the recompiled code (items 5, 9, 10) |
| 5 | False function splits in the function map | done (Vita not measured yet) | tools/build_elf.py + funcmap_elf.merge_split_starts: starts inside a sized module symbol or crossed by a branch are merged back (7 251 of 16 274; ~9 000 functions), kept callable as entry points (gen_config.py). Recompiler: a JAL into the function itself is a call, not a goto; `lui rX; addiu rBase, rX` jump tables; an unresolved JALR no longer makes every instruction an entry. Vita3K (earlier run): jumpGuest ~2 100 a second |
| 6 | Display lists the game still builds for the PS2 | done (builders not decompiled) | With the native renderer drawing (GS::skipRaster) the VIF1 chain is no longer gathered: its VIFcodes are walked where they lie (`Vif1Walker`, ps2_vif1_compact.h) on the EE thread, with no copy and no VU1 worker job. VIF1 registers change as before (STCYCL/BASE/OFFSET/MSCAL DBF/ROW...), UNPACK/MPG data is stepped over (UNPACK size inline), DIRECT packets still go to the GIF event scanner (they carry the frame's FINISH), DMA completion (CHCR.STR, D_STAT, interrupt) as before; normal-mode VIF1 transfers likewise. No MSKPATH3, MARK or VIFcode interrupt bits in the chains; the native renderer's ROW scrolling reads the game's NTO2 data, not VIF1. `PS2X_VIF1_FULL=1`: the old compacted copy through the VU1 worker. Throttled attract demo (frames 1000-1300): 104-106 -> 92.5 ms (game 80 -> 68, hooks 14, render 10); unthrottled 35-38 -> 30 ms. What is left: `Vif1Walker::feed` 9.4% (the chains have ~100 000 VIFcodes a frame in gameplay; was feed + append + memset + unpackDataBytes + processVIF1Data ~19%). The 5% `sceClibMemcpy` is one record of Vita3K's profiler (all calls in one bucket, labelled with whichever caller came first): it did not change, and is not this path. Mac route to the plain: pictures match the old path as closely as two runs of the old path match each other, game clock 1.000x |
| 7 | `modelDispHook` cost | done (not committed) | Throttled attract demo (frames 1000-1400): 92.5 -> 81.5 ms a frame (game 68 -> 68.5, hooks 14.2 -> 3.2, render 9.8 -> 9.8); unthrottled 30 -> 25 ms (game 21.5 -> 20.7, hooks 5.4 -> 1.1, render 3.0 -> 3.3). Where it went: posing skinned packets on the CPU (~11 000 vertices a frame, ~25 000 triangle-list vertices, 900 KB) was ~10 of the 14 ms. (1) GPU skinning on GL / Vita (`gpuSkinning`, F10 Debug "GPU skinning", `SOTC_GPU_SKIN=0/1`): a skinned packet's bind pose is uploaded once as `SkinVertex` (3 palette qwords + weights) with an index list; each draw sends its instance's palette range (<= 192 qwords, `framePalette`) to a second program (the main vertex program with the posing prepended, GLSL and Cg); packets with a lit vertex without normal (face normals) or a wider palette stay on the CPU. Metal poses on the CPU. (2) CPU posing (Metal, fallback): each strip vertex posed once, written in place into the frame arena (`frameMesh` / `frameMeshIndices` / `commitFrameMesh`, no vector copies) as vertices + 32-bit indices; unlit packets skip normals; bit-identical to the old triangle lists on the Mac (142 000 meshes compared). (3) a texture's and a packet's content hash taken once a frame (memo), the bone palette clears only what it last wrote (was 16 KB a skinned instance), particles and cloth written in place (cloth grid vertices read once), no getenv per draw. `dbg_fast_hooks` / `SOTC_NATIVE_SLOWHOOKS=1` turns (2)-(3) back to the old code. Tried and dropped: NEON posing (slower in Vita3K: 5.7 vs 4.8 ms, and 4 ulp off the VFP); worth a retry on hardware if the CPU path matters. Render on the Vita: unchanged (skinned draws switch program; the arena upload is 900 -> 170 KB) |
| 8 | Render thread | todo | render + present on another core; then cap at 30 by default |
| 9 | `-O2` for the hottest recompiled modules | doing | all game code `-O2` on the Vita (`PS2X_VITA_GAME_OPT`, -Os to go back): text 88 MB, VPK 28.6 -> 39.5 MB. Not measured yet; revert or limit to the hot files if slower throttled |
| 10 | `ctx->pc` stored before most instructions | done (Vita not measured yet) | stored only before code that reads it (runtime calls other than memory, throws), at backward-branch checkpoints, calls and the function end; delay-slot state only when the slot can raise. `PS2X_RECOMP_ALL_PC=1` at recompile time stores it everywhere, as before. Mac: route to the plain, game clock 1.000x |

## Vita: picture

| # | Item | Status | Notes |
|---|------|--------|-------|
| 11 | Whites (water, haze, blur copies) | done | 01ef852: "Redraw for copies" |
| 12 | Bloom without depth textures | done | 01ef852: narrow glow, one frame late; not the Mac's GS chain yet |
| 13 | Doubled figures (Wander, the eagle) | done | Ours, desktop GL and Vita only: (1) draws that sample a picture this renderer drew at their own PS2 UV (the heat haze behind the eagle: 0x10f37c8's 256 x 256 copy; feedbackBlur's last frame) read it upside down (GL pictures are y-up), so the haze strips showed another part of the screen and the trail laid a flipped frame; (2) the haze strips (layer 23, TEST 0x5000d: ZTE GEQUAL) were drawn without depth over the eagle, showing its copy shifted on top of it. Fix: v turned over for textured 4/5 (GLSL + Cg); the haze carries its GS Z as view depth (zA + zB / w) and is depth tested, no write (DrawState::screenDepthTest, Metal and GL). Vita3K: the sky close-up with Rastro off shows one eagle (was a ghost copy in blocks); with Rastro on the trail matches the GS's. The eagle is drawn at layer 6, before the copy, on the PS2 too |
| 14 | Vita bloom as the GS chain (like the Mac) | todo | after the speed work |
| 15 | Long run past 80 min (memory) | todo | the bounded VU1 queue (9ff53c8) not confirmed in a long run |

## Mac native picture

| # | Item | Status | Notes |
|---|------|--------|-------|
| 16 | Bloom that swells and fades (GS chain) | done | 516fe94; the user confirmed |
| 17 | Parts vanishing by camera angle | done | 2a61468 |
| 18 | Water: brighter and greyer than the PS2, no splash rings | todo | 831ec32 made the water write no depth; compare layers 9 / 15 / 17 one by one against the GS |
| 19 | Gaps on animated models | todo | not seen recently; check with an F8 capture when it shows |
