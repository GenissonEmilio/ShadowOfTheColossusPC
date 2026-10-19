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
| 4 | Scene-load hitches (0.5-1.2 s frames when a scene loads) | todo | the frame timing log (`SOTC_FRAME_LOG`) prints each one; find what the frame does (decode, uploads, mesh builds, file reads) and spread or cache it |
| 5 | False function splits in the function map | todo | the root cause of item 3; merging them turns nested jumps into gotos |
| 6 | Display lists the game still builds for the PS2 | todo | the VIF1 chain walk is 14% of the game thread (`Vif1Compactor::feed`) plus memcpy; skip or decompile the builders |
| 7 | `modelDispHook` cost | todo | ~5% of the game thread |
| 8 | Render thread | todo | render + present on another core; then cap at 30 by default |
| 9 | `-O2` for the hottest recompiled modules | todo | the game code is built `-Os` |
| 10 | `ctx->pc` stored before most instructions | todo | in generated code |

## Vita: picture

| # | Item | Status | Notes |
|---|------|--------|-------|
| 11 | Whites (water, haze, blur copies) | done | 01ef852: "Redraw for copies" |
| 12 | Bloom without depth textures | done | 01ef852: narrow glow, one frame late; not the Mac's GS chain yet |
| 13 | Doubled figures (Wander, the eagle) | todo | the motion trail explains Wander at very low fps, but the user still sees the eagle doubled with the trail off; check the screen blur and the game's own blur against the PS2 |
| 14 | Vita bloom as the GS chain (like the Mac) | todo | after the speed work |
| 15 | Long run past 80 min (memory) | todo | the bounded VU1 queue (9ff53c8) not confirmed in a long run |

## Mac native picture

| # | Item | Status | Notes |
|---|------|--------|-------|
| 16 | Bloom that swells and fades (GS chain) | done | 516fe94; the user confirmed |
| 17 | Parts vanishing by camera angle | done | 2a61468 |
| 18 | Water: brighter and greyer than the PS2, no splash rings | todo | 831ec32 made the water write no depth; compare layers 9 / 15 / 17 one by one against the GS |
| 19 | Gaps on animated models | todo | not seen recently; check with an F8 capture when it shows |
