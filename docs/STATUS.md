# Celosia: where the port stands

*Updated 2026-10-17.* Live dashboard (treemaps per area, history):
[Celosia port status](https://claude.ai/artifact/1RnzZd7UBKjDKzogkGEFN9), built
from [`port_status.json`](port_status.json).

Celosia runs **Shadow of the Colossus** (PS2, USA, SCUS-97472) as native code
on macOS (Apple Silicon) and on the **PS Vita**. It began as a static
recompilation: the game's MIPS code turned into C++ with
[PS2Recomp](https://github.com/ran-j/PS2Recomp), running on a runtime that
emulated the PS2 hardware. Since then the hardware the game talks to has been
replaced, piece by piece, by native code. Today the game no longer needs a
GS, VU1 or VIF emulation to draw a frame. The next stage is decompiling the
game's own code where it matters.

## At a glance

| | macOS (Apple Silicon) | PS Vita |
| --- | --- | --- |
| Build | `./celosia setup` then `./celosia run` | `./celosia vita` → `.vpk` |
| Renderer | native, Metal (OpenGL kept as a check) | native, OpenGL ES (vitaGL) |
| Resolution | 512×448 × render scale (3 by default: 1536×1344) | 512×448 (scale 1) |
| Speed | 60 fps game / 60 fps display (M4 Pro) | 18–25 fps in Vita3K's intro; real hardware not measured yet |
| Audio | SPU2 reimplemented, IOP modules native | same |
| Saves | PCSX2-compatible `memcards/Mcd001.ps2` | `ux0:data/celosia/memcards/` |
| Playable | boot to gameplay (riding, exploring) | boots and plays the intro; fidelity and speed in progress |

Everything in [`port_status.json`](port_status.json) (49 features: 34 in the
renderer, 15 in the hardware) is **native**: no area still falls back to the
emulated path.

## What is native

**Graphics.** The game's model data is read straight from memory (modelDisp,
its LOD and part tables, materials and draw layers) and drawn by our own
renderer (`PS2Recomp/ps2xRuntime/src/lib/native/`):

- textures (4/8-bit CLUT and 32-bit), decoded once and cached by content;
- skinned characters, skinned on the CPU;
- lighting, two-texture surfaces;
- cloth and hair;
- shadow volumes;
- particles;
- all 2D (sprites, fades, cinema bars, subtitles, HUD, menus);
- GS vertex fog, the zFog into the sky;
- bloom, the boss-death glow;
- depth of field;
- screen blur;
- planar reflections, water refraction, heat haze.

[`native_renderer_map.md`](native_renderer_map.md) maps each effect to the
game function it replaces.

**Hardware.** EE timers, interrupts and DMA, the IOP modules (SIF, CD, pads,
memory card, sound) and the SPU2 run as host code from boot. The VU1
microcode was compiled ahead of time before the native renderer made it
unnecessary for drawing.

## Settings

**Player menu** (F1 on the Mac; Select twice on the Vita), in Portuguese or
English:
- Video: exact transparency, show FPS, screen fit (4:3, stretch, whole scale);
- Sound: volume;
- System: language, quit.

**Native renderer panel** (F10, macOS) changes the effects while the game
runs. Each strength multiplies the game's own, which it keeps changing per
scene and camera:

| Control | Default | Variable to set it at start |
| --- | --- | --- |
| Resolution × | 3 (Mac), 1 (Vita) | `SOTC_RENDER_SCALE` |
| Bloom on / strength | on / 0.46 | `SOTC_BLOOM_SCALE` (0 turns it off) |
| Bloom blur rounds / spread | 1 / 1.0 | `SOTC_BLOOM_ROUNDS`, `SOTC_BLOOM_SPREAD` |
| Vertex fog on / amount | on / 0.55 | `SOTC_NATIVE_NOVFOG=1`, `SOTC_FOG_AMOUNT` |
| Depth fog into the sky | on | `SOTC_NATIVE_ZFOG=0` |
| Glow, depth of field, shadows, particles, screen blur, reflections, water refraction | on | `SOTC_NATIVE_NOGLOW`, `_NODOF`, `_NOSHADOWS`, `_NOPTCL`, `_NOSBLUR`, `_NOREFLECT`, `_NOREFRACT` |

"Print to log" writes the current setting as these variables. On the Vita,
`ux0:data/celosia/env.txt` holds `NAME=value` lines (the Vita has no
environment).

Renderer selection: `SOTC_NATIVE=1` draws natively (the default),
`SOTC_NATIVE=2` draws natively without showing it (side-by-side checks),
`SOTC_NATIVE_GL=1` uses the OpenGL renderer on the Mac and `=check` compares
it against Metal every second.

## How the recompilation was done

1. **Boot ELF.** Function starts are recovered from symbols, JAL targets,
   code pointers and prologues (`tools/funcmap_elf.py`). Names come from
   KERNEL.XFF's imports.
2. **Modules.** The game links `KERNEL.XFF`, `MANAGER.XFF` and
   `GAMECORE.XFF` in memory with its own dynamic linker. Each is captured
   from RAM the moment the game jumps into it, then cut out at its final
   addresses (`tools/modimage.py`).
3. **Link.** `tools/build_elf.py` merges everything into one `SOTC.elf` plus
   a function map (~16 000 functions) and the list of instructions the
   guest linker rewrites.
4. **Config.** `tools/gen_config.py` writes the recompiler config:
   - stubs for the SDK calls the runtime implements;
   - since 2026-10-17, **hidden entry points**: 841 pieces of code packed
     right after another function's jump, which the game calls through
     pointers. One of them, a three-instruction leaf at 0x1357888, froze the
     game about 14 minutes in.
5. **Recompile** to C++ and build. The VU1 microcode was compiled the same
   way.

Details: [README](../README.md#how-it-works).

## Decompilation, the next stage

The recompiled C++ works but cannot be read or changed. The plan is a
*functional* decompilation: rewrite, by hand and in readable C++, the
functions that matter for speed or for the Vita. Each one goes in only when
it is proven equal to the recompiled one. A matching decomp, rebuilding the
PS2 binary, is not the goal.

The loop:
1. A guest profiler finds the hot functions (`PS2X_PROFILE`).
2. Ghidra reads them (`tools/ghidra.sh`).
3. The rewrite is registered with `SOTC_DECOMP_FUNCTION`.
4. `SOTC_DIFFTEST` runs native and recompiled code on the same state and
   compares every register and every byte of RAM.

Done so far: the SDK's software doubles (dpadd, dpmul, dpdiv…): ~93 000
checks, 0 mismatches. They turned out to truncate instead of rounding, which
an IEEE rewrite got wrong.

Next candidates:
- the display lists the game still builds for models the native renderer
  draws itself (~6%);
- collision;
- the model display path.

Guide: [`decomp.md`](decomp.md).

## Latest changes (2026-10-16 → 17)

- **Higher resolution.** The native picture is drawn at 512×448 × a render
  scale (3 on the Mac) and shown at that size, filtered. It used to go
  through the emulated display and be point-sampled down to 512×448, which
  made edges, hair and cloth look broken.
- **F10 tuning panel** for the renderer's effects, and the defaults set by
  eye against the PS2 picture.
- **Vita:**
  - native renderer on vitaGL;
  - per-frame geometry in one buffer per frame (8–16 → 18–25 fps in Vita3K);
  - uniform lookups cached;
  - fog by depth without depth textures.
- **Long-run stability:**
  - the game's freeze ~14 min in (hidden entry points, above);
  - meshes and textures nobody drew for 30 s are released (content-hashed
    ids filled the Vita's 160 MB in 12 min);
  - the VU1 worker queue is bounded (it grew by 40–90 MB in heavy scenes);
  - a per-frame log line that wrote ~100 000 lines a run is gone.

## Open issues

- **Bloom shape.** Bright openings, like the shrine's window, saturate to a
  flat white with a narrow halo. The PS2 shows a wide, soft glow with the
  landscape visible through it.
- **Character meshes.** Gaps are still reported on Wander and other animated
  models. Skinning weights and strip assembly were checked and are correct;
  under investigation.
- **Vita:**
  - bloom and glow are off (no depth textures);
  - the heat-haze screen copy shows white in one close-up of the bird;
  - speed on real hardware is not measured yet.
- **Long runs on the Vita** need a confirmation run past 80 minutes with the
  bounded VU1 queue.
- [`known_issues.md`](known_issues.md) has the older ones (the raised-sword
  arm swing, the earlier brightness study).

A note for fidelity work: the emulated picture's GPU rasterizer has no GS
fog. Compare the native picture against `PS2X_GS_GPU=0` (CPU raster, exact
GS fog), not against the default emulated view.
