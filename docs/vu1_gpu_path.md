# VU1 work on the GPU (plan)

Measured at real Vita speed in Vita3K (444 MHz, 3 cores; eagle cliff scene,
2026-10-12): about 450 VU1 programs a second, ~2 ms of CPU each, VU1 thread
~88% busy, ~0.6 fps. With the transform loops native (and the software float
normalisation gone) the transform maths is no longer the cost: the layers
around it are. Each program's data crosses VIF unpack (word by word), the VU1
interpreter's per-program setup and pipeline state, XGKICK (qword by qword),
the GIF decoder (register by register) and the GS frontend (a full draw state
per triangle). Local optimisations give 5-20% each; 30 fps needs ~50x.

The fix is to let the GPU do what VU1 did, for the routines whose maths is
known, and skip every layer in between:

    EE DMA chain --VIF1--> VU1 data memory (input batch, matrices)
          program f40e22b1@05b0 (world): per vertex
              position xyz (float), texcoord (12.4 int), colour (int)
              -> M (vf1-vf4) * p, divide by w, ST * q, fog = vf17.w + vf17.x / w
              clip against C (vf5-vf8) -> ADC / clipper
          XGKICK -> GIF -> GS

becomes

    VU1 program start (native hook, same place as the native loops):
        batch = { GIF tag (input header), vertex count, raw vertices,
                  M, fog constants }
        -> one "VU batch" command in GIF order (after the A+D register
           packets that set TEX0/ALPHA/TEST for it)
    GS backend: one draw call, vertex shader = the loop's maths, GPU
    clipping instead of ADC + clipper.

## Stages (each validated before the next)

1. Mac, behind PS2X_VU1_DIRECT=1: the world program still runs on the CPU
   (so dependent programs, e.g. the second-UV pass @0698 that reads the
   world pass's output, see the same VU memory), but its XGKICK packet is
   replaced by the VU batch drawn with the new vertex shader. Compare
   frames against the current path (the two must match up to rasterisation
   rounding).
2. The second-UV pass (@0698/@07f8) as a batch re-drawn with the other UV
   set; then the world program stops writing its output (no CPU transform).
3. Same for the lit loop (@0710 family: + normal, light matrix vf9-vf16),
   then the character image (059698f3) after reverse engineering its loops.
4. Vita: the CG version of the vertex shader; measure in Vita3K.

Open questions to settle in stage 1: the GIF tag's PRIM/REGS as the program
leaves it (it copies the header qword through), strip restarts (ADC from the
data vs from clipping), and where the frontend's per-primitive state comes
from when a batch bypasses GS::vertexKick.

## Stage 1 result (Mac, 2026-10-12)

PS2X_VU1_DIRECT=1 (game/overrides/sotc_vu1_world.cpp, entries 0x0538,
0x05b0, 0x0628): the input batch is captured at TOP before the loop; the
loop still runs; at 0x13b8 the native code stores vi6/vi8/vi14 to 1017 and
resumes at 0x13f8 (after the XGKICK), skipping the clipper; the batch goes
to GS::submitVuBatch in GIF order (PRIM from the tag, draw state from the
frontend) and the HW backend draws it with kVuVertexShader.

Answers to the open questions:
- The tag is the input header copied through: PRE=1, PRIM = triangle strip,
  IIP=1, FST=0. On the gameplay route no batch fell back (16M+ batches).
- Strip restarts: none in the data. The input position's w is not read; the
  loop's ADC comes only from clipping, so a GPU strip with GPU clipping is
  the same picture.
- The loop writes back into its input (float ST/colour, the clip flag in the
  position's w) for the dependent passes; the capture happens before it.
- ~6000 world batches a frame: one draw each fell to 22 fps on the Mac.
  Consecutive batches with the same draw state are now merged (triangle
  lists, a batch index per vertex into uM[32]/uBatch[32]): back to 30 fps,
  frames the same as the current path.
