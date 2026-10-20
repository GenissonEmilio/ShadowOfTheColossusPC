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
| 6 | Display lists the game still builds for the PS2 | todo | the VIF1 chain walk is 14% of the game thread (`Vif1Compactor::feed`) plus memcpy; skip or decompile the builders |
| 7 | `modelDispHook` cost | todo | ~5% of the game thread |
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
