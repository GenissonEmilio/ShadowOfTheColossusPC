# Native renderer: map of SotC's drawing (from the game's symbols)

What the game calls to draw, found with Ghidra on SCUS_974.72 (the ELF
keeps its function names). The goal is a Metal renderer fed at this level
(model + matrix + material), with no VIF/VU1/GIF/GS emulation.

## Display lists

The game never draws directly: every system appends prebuilt DMA chains to
a display list split in layers (`iosDlPushLayer(n)` / `iosDlChainTailCurrent(addr)`
/ `iosDlPopLayer()`), and `transMicroCode(n)` (0x1114b50) appends the
upload of VU1 microcode n (table at 0x118cd90) when the layer's current one
differs. The layers are sent to the VIF1 once a frame.

## Models (`model*`, 0x1096840-0x1099980)

Objects, scenery and characters go through one model system. A loaded
model header (`modelInitialize`, `modelCreatePacketTransPacket`):

| offset | contents |
|--------|----------|
| +0x08 | owner set at load |
| +0x0c | non-zero: second fade factor applies |
| +0x14 | packets built at load: [0] one per draw packet (0x30 bytes), [1] one per surface (0x40) |
| +0x18 | flags (bit 0: LOD model), +0x1c LOD count |
| +0x20..+0x2c | bounds (copied to the display instance) |
| +0x30 | per LOD: clip box (8 corners tested with VCLIP) and the packets of that LOD |
| +0x40 / +0x44 | textures (0x20 each; +0x10 the texture resource) / count |
| +0x50 / +0x54 | surfaces (0x120 bytes each) / count |
| +0x60 / +0x64 | draw packets (8 words each) / count |

Surface (0x120 bytes): +0x18 flags (64-bit: alpha, two-pass, shadow,
underwater, boss effect, layer selection in `modelGetDlLayer`), +0x1c
texture mode (1 one texture, 2 two), +0x2c / +0x3c texture indices, +0x60
GS register packet (A+D: TEX0, ALPHA, TEST...), +0xc0 second-pass registers,
+0x10c alpha.

Surface flags read by `modelDisp` (bit n of the u64 at +0x18):
29 never
drawn; 17 boss effect (drawn only while `getNowBossEffect`, the word at
0x1186290, has bit 0); 24 shadow effect (bit 1 of the same word); 26 hidden
while the underwater fog (word at 0x1186328) is on.
Layer (`modelGetDlLayer`): 0x1020000 -> 8, bit 23 -> 10, 25 -> 16, 9 -> 17,
10 -> 12/13, 14 -> 1, 15 -> 18, else 2/3 (5/6 for the second owner), the
odd one when bit 8 is set or the instance fades (+0x244 > 0). Blending:
ALPHA_1 in the +0x60 packet, 0x44 normal, 0x48 additive.

Draw packet (8 words): [0] size in bytes, [1] LOD, [2] surface index, [3]
part (-1 = always), [4] address of the vertex block, [7] its DMA chain:
`CALL` surface registers (+0x60), `REF` vertex block (VIF data: the batch
the world microcode reads, see game/overrides/sotc_vu1_world.cpp), `MSCAL`
to the microcode, `RET`.

`modelDisp(instance, matrix)` (0x1097118): camera x model matrix, VCLIP of
the LOD's box (skips it off screen), then per packet of the current LOD:
layer from `modelGetDlLayer`, texture uploads (`texTransRequest[Multi]`)
when the surface changes, `iosDlChainTailCurrent(packet chain)`; surfaces
with flag bit 24 or 0x2200 add a second pass (layers 9/14/15, registers
from the per-surface packet). Callers: the object display functions
0x10F3B58 / 0x10F3DE8 (they also call `shadowDisp`). Instance setup:
`modelInitDispModelClass` (from 0x10EF9D0): lights (`setLight`), unit
matrix, specularity, the layer mask (+0x96 words).

Batch formats (STCYCL n+1: n fields per vertex; the UNPACK address is the
field): 0 GIF tag, 1 position V3-32, then V2-16 ST + V4-8 colour (unlit
scenery, ~90%); V4-16 two ST pairs + V4-8 (two textures); V3-32 normal +
V2-16 + V4-8 (lit); V4-16 normal + V2-16 + V4-8 + V4-32 skin (skinned).
Skin: w = bone count, x/y/z = bone index + weight in the fraction (one bone:
full weight).

Characters are not one model: Wander is ~24 `modelDisp` calls (body,
limbs, hair, clothes), each a model of its own. The instance chain
(instance + 0xc) is a NEXT tag past the instance's 15 qwords (lights,
colours) to a RET packet: rigid parts unpack one matrix to VU1 qword 0,
skinned parts the bone palette (up to ~75 matrices, `modelSetMatrixTransTag`),
each bone x view, in camera space. Their clip transform is the projection
alone: camera context + 0x40 (camera + 0x5a0 = that x view).

The instance's own 14 qwords (to VU1 0x3d8) start with its lights, as the
lit microcode (loop 0x1818) uses them: qwords 1-4 the light matrix L
(columns: normal x, y, z, 1), 5-8 the colour matrix C (light 0-2 colours,
ambient); d = max(0, L x (n, 1)), d.w = 1, vertex colour x= C x d (0x80 =
1, clamped to 255). Skinned normals go through the bone matrices (camera
space) with the same lights.

`modelClipSkin` only picks the LOD of skinned models.

Skinned characters: `modelClipSkin` (0x1098450); 0x1098CC0 selects 14
microcodes (characters).

## Other systems that append chains or microcode

- `mesh*` (0x109a4d8-): `meshDisp`, `meshDispInstance`, `meshTransMatrix`,
  `meshTransLightMatrix`; callers 0x1138050, 0x1383048.
- `shadow*` (0x109d158-): `shadowDisp`, `shadowDraw`.
- particles `ptclman*` (0x1099e00-): material, vertex and environment packets.
- `waterRefractionBegin/End` (0x109c108).
- sprites / text: `kPrintf*`, `spriteTexFast`, `getSpriteDraw`; `bgDraw`
  (0x1085258) is a full-screen fog sprite (from `zFog`), not scenery.
- line objects (debug and effects), `dispVuColiDispWork`, `dischargeMain`.
- textures: `texTransRequest`, `texTransRequestMulti`, `texResetVRAMRequest`.

## 2D: sprite objects

Menus, the title, the map and the load/save boxes are sprite objects
(`spriteCreateObject`, 0x50 bytes: [0] texture resource (+0x20 NTO2), +4
RGBA, +8 alpha factor, +0x10..0x1c UV rectangle, +0x20..0x2c ST, +0x30/+0x34
position, +0x40/+0x44 scale, +0x48 rotation and flags: blend mode bits
40-43, ST bit 48) drawn by `spriteDrawSprite(obj, z, layer, ...)`
(0x10a32b8; floats f12-f17: position, scale, rotation, alpha factor). It
writes A+D registers with `gsReg(reg, value)` (0x1c5b00): PRIM (triangle
strip), RGBAQ, UV or ST, XYZF2 per corner (12.4, screen centred on 2048,
512 x 448), ALPHA (0x44 / 0x48 / 0x42 / 0x54 by mode), in display list layer
25 + layer. Title screen: ~9 per frame; memory card and map: ~200.
The recompiled function runs in pieces the dispatcher continues after the
call returns: hooks that wrap the call see none of its gsReg writes.

The same gsReg path: `spriteDrawSpriteUV/ST` (obj in a2) and `UV2/ST2` (obj
in a0) draw SPRITE primitives (two corners) in layer 26;
`spriteDrawSpriteTexNoSet(obj, z)`; `fadeScreen(amount)` (0x108a408, full
screen, colour at 0x1186340/44/48, layer at 0x1186350) and
`drawDemoBar(amount, layer, height)` (0x108a588, the cinema bars) blend with
ALPHA 0x64, (Cs - Cd) x FIX + Cd, FIX = amount x 128. `spriteNoTexFast`
writes its registers itself and is only called by screen effects (bloom,
shadows, reflection, fog, boss death).

Model instance opacity (modelDisp, to the microcode at packet + 0xdc):
1 - instance[0x91] (+0x244), times 1 - instance[0x92] (+0x248) when the
header's +0xc is set; 0 hides the instance (e.g. the dark wisps near Wander).

## Census (SOTC_CENSUS=1, shrine, Wander standing; per frame)

| function | calls | |
|----------|-------|---|
| modelDisp | ~50 | 35 distinct models, ~2800 draw packets in their headers |
| modelClipSolid / modelClipSkin | 124 / 20 | from `ClipLayoutObjModel`: scenery, objects and characters are all layout-object models, clipped part by part |
| meshDisp | 7-15 | |
| ptclmanEnd (particles) | ~50 | |
| spriteTexFast | 192 | screen passes: 0x109E2A0 (32 call sites, the bloom chain), 0x10A0100, `depthOfField`, `shadowDraw`, `reflectionBegin`, `waterRefractionBegin`, `screenBlur`, `bossDeathEffectEnd` |
| texTransRequest | ~310 | the game re-uploads its textures to the 4 MB GS memory every frame |
| transMicroCode | ~237 | |

So nearly all 3D goes through one model system; screen effects are
sprites over buffers; and the per-frame texture uploads disappear once
textures live in GPU memory.

## Checked against a frame (tools/model_view.py, SOTC_MODEL_DUMP)

- The VU1 matrix of a model's batches is exactly `S x camera x model`:
  camera = camera context (`iosgluGetCameraContext`) + 0x5a0, model = the
  matrix passed to `modelDisp`, S maps clip to GS pixels: x = 2048 + 256 x/w,
  y = 2048 - 224 y/w (the depth row scaled too). 22 of 50 models of the frame
  matched the matrices the VU1 used to 1e-4.
- A draw packet holds several world batches, each ending in MSCNT
  (GIF tag, positions V3-32, ST V2-16, colours V4-8; triangle strips).
- The packets drawn: the instance's LOD (bits 40-42 of the 64-bit word at
  instance + 0x250, clamped to the model's LOD count) and, per part, the
  visibility the clip wrote at LOD table + part x 0x30 + 0x2c (0, 2 drawn).
- Positions are model-local; the sky is model 0 of the frame (radius
  10000-33000); large one-packet models around Wander are transparent
  planes (fog/light). Lit surfaces have no vertex colour: the lit
  microcode computes it from normals. Skinned characters (Wander) are not
  drawn by `modelDisp`.

Still to read for drawing: textures (ST, texture resources, CLUTs), the lit
batch (normals) and its lights (instance + 0x20..0x9f), surface materials
(GS registers at surface + 0x60: alpha, test), skinned characters.

## Next

1. Read one model straight from guest memory (header -> surfaces ->
   packets -> vertex blocks) and draw it with Metal next to the emulated
   picture.
2. Then the Metal renderer at `modelDisp` level, system by system.
