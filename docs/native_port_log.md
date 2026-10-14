# Native port log

One row per step of `native_port_plan.md`, measured with the gate
(`tools/measure.sh`). Mac: busy ms per frame per thread (game / vu1 / gif /
gs) with Wander standing at the altar and on the walk, from runs with
SOTC_FRAME_INTERVAL=4 (the same game run every time; about 2% run to run),
plus the uncapped fps (about 10% run to run). Vita: Vita3K 444 MHz 3 cores,
game fps and thread loads (game / vu1 / gif / gs %).

| Date | Step | Mac fps | Vita fps | Vita loads | Counters | Picture |
|------|------|---------|----------|------------|----------|---------|
| 2026-10-12 | before the plan (world VIF->GPU, uncommitted) | 30 on the route | ~0.7 (eagle scene, not the scripted route) | 72 / 94 / ? / ? | direct ~45% of MSCNT | Mac matches |
| 2026-10-14 | baseline after phase 0 (direct world batches, near clip, TEXFLUSH, exact blend; PS2X_VU1_DIRECT=1) | ms/frame standing 12.8 / 14.7 / 4.2 / 47.5, walk 13.2 / 7.3 / 2.7 / 45.2; uncapped standing 22-25 fps | not measured yet | | direct/frame ~3550, mscnt/frame ~7050 | two capped runs: standing and walk pictures within 0.14% |
| 2026-10-14 | GS: blends that truncate by biasing the source (no target copy), copies only for other blends of targets up to 256x256 | ms/frame standing 13.1 / 14.2 / 4.0 / 29.3 (gs was 47.5), walk 13.0 / 7.3 / 2.7 / 26.7; uncapped standing 31.4 fps, walk 30.5 (exact copies everywhere: 28.4 / 28.7) | not measured yet | | draws/frame ~720 (was ~3700) | standing pictures within 0.13% of the baseline |
