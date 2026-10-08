# VU1 routines of Shadow of the Colossus (USA)

The game renders everything 3D through 14 VU1 microcode images (`build/vu1/<hash>.bin`,
16 KB each) with several entry points. A *routine* is image + entry pc. This map is the
plan for replacing VU1 + GS emulation with native GPU rendering (Vita: GXM).

How it was measured: `PS2X_VU1_PROFILE=1` (per-routine calls, VU instructions and what
the routine sends to the GS) over the scripted route (opening, title, bridge, shrine
cutscene, gameplay); `PS2X_VU1_SKIP=hash@pc,...` drops a routine's output to see what
disappears. Profile run: 2026-10-08.

## What each routine draws (skip test)

| Routine | Draws |
| --- | --- |
| `f40e22b1ef541d18@05b0` (+`@0710`?) | World geometry: cliffs, buildings, ground (52% of VU1 work) |
| `059698f3510c1559@0598` | Character parts (Wander's hair/head) |
| `8178e9d1551375fc@0548`, `1fab41fc4a48ff39@1a88` | Wander's cape (cloth) |
| `f40e22b1@0080/@0648/@0090`, `1fab41fc@00b0` | Material setup only: A+D register kicks (TEX0 etc.), no geometry |
| `f40e22b1@0020/@0030/@0040`, `059698f3@0020/@0960`, `e471dbf3@0000` | No GS output: setup (matrices/constants) |
| others | Not visible in the tested scenes (likely fields, colossi, effects) |

## `f40e22b1@05b0` (world) in detail

Entry 0x05b0 calls the transform loop at 0x1140 and kicks the result.
Per vertex, 3 qwords: position xyz (float), texture coordinate (12.4 fixed, `ITOF12`),
colour RGBA (int, `ITOF0`). The loop multiplies the position by a 4x4 matrix in
vf1-vf4 (to screen), divides by w, scales ST by q, computes per-vertex fog (from vf17/vf29,
clamped 0-255), and CLIP-tests against a second matrix in vf5-vf8: triangles fully outside
get the ADC bit, triangles crossing the guard band go to a clipper. Output: textured,
alpha-blended, fogged, Gouraud triangle strips/fans (GIF regs RGBAQ, ST, XYZF2).
Native equivalent: one vertex shader (matrix, perspective UV, fog) + GPU clipping.

## Profile (VU1 instruction share over the route)

| Routine | VU1 instr share | Calls | Vertices | Prims (strip/fan) | Tex | Blend | Fog | TEX0 PSM | GIF regs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `f40e22b1@05b0` | 52.1% | 16,880,715 | 148,728,887 | 16880715/4308144 | 21188859 | 21188859 | 20219699 | - | 1 2 4 |
| `b504a980@01e0` | 8.9% | 627,267 | 13,848,490 | 1384849/0 | 0 | 1384849 | 0 | - | 1 4 |
| `059698f3@0920` | 6.5% | 1,259,660 | 10,458,839 | 1259660/23816 | 1283476 | 1283476 | 1283476 | 20:1259660 | 1 2 4 14 |
| `f40e22b1@0710` | 5.9% | 2,762,103 | 15,221,258 | 2762103/204008 | 2966111 | 2966111 | 2966111 | - | 1 2 4 |
| `059698f3@0598` | 5.3% | 1,188,840 | 8,799,909 | 1188840/6072 | 1194912 | 1194912 | 1194912 | - | 1 2 4 |
| `f40e22b1@0628` | 4.1% | 1,309,131 | 11,101,763 | 1309131/419974 | 1729105 | 1729105 | 1729105 | - | 1 2 4 |
| `059698f3@0970` | 3.5% | 1,259,660 | 10,458,839 | 1259660/23816 | 1283476 | 1283476 | 23816 | 20:1259660 | 1 2 4 14 |
| `f40e22b1@0698` | 2.8% | 1,309,131 | 11,101,763 | 1309131/419974 | 1729105 | 1729105 | 1729105 | 19:753507 20:555624 | 1 2 4 14 |
| `e471dbf3@0088` | 2.6% | 89,246 | 498,149 | 3323/230687 | 234010 | 234010 | 234010 | - | 1 2 4 |
| `059698f3@09b8` | 2.2% | 444,031 | 4,067,914 | 444031/5676 | 449707 | 449707 | 449707 | 20:444031 | 1 2 4 14 |
| `e471dbf3@0b98` | 1.1% | 4,649 | 299,915 | 0/142737 | 142737 | 142737 | 142737 | - | 1 2 4 |
| `059698f3@0a08` | 0.8% | 444,031 | 4,067,914 | 444031/5676 | 449707 | 449707 | 5676 | 20:444031 | 1 2 4 14 |
| `98a396b7@0538` | 0.6% | 40,871 | 401,769 | 40871/33192 | 74063 | 74063 | 74063 | - | 1 2 4 |
| `8178e9d1@0548` | 0.5% | 155,099 | 1,170,537 | 155099/65486 | 220585 | 220585 | 204668 | - | 1 2 4 |
| `059698f3@07f0` | 0.3% | 67,494 | 632,753 | 67494/674 | 68168 | 68168 | 68168 | 20:67494 | 1 2 4 14 |
| `1fab41fc@1a88` | 0.3% | 68,357 | 764,292 | 68357/3316 | 71673 | 71673 | 71673 | - | 1 2 4 |
| `4156b804@0088` | 0.3% | 25,276 | 72,583 | 0/29264 | 29264 | 29264 | 29264 | - | 1 2 4 |
| `c54bd6b6@0538` | 0.3% | 40,871 | 401,769 | 40871/33192 | 74063 | 74063 | 74063 | - | 1 2 4 |
| `b504a980@0000` | 0.3% | 8,640 | 592,390 | 59239/0 | 0 | 59239 | 0 | - | 1 4 |
| `f40e22b1@0080` | 0.2% | 2,391,380 | 0 | 0/0 | 0 | 0 | 0 | 0:1978 19:280799 20:2108603 | 14 |
| `1ae815b9@0550` | 0.2% | 34,675 | 288,800 | 34675/0 | 34675 | 34675 | 34675 | - | 1 2 4 |
| `f40e22b1@0020` | 0.2% | 2,724,603 | 0 | 0/0 | 0 | 0 | 0 | - | - |
| `f40e22b1@0030` | 0.1% | 235,045 | 0 | 0/0 | 0 | 0 | 0 | - | - |
| `1fab41fc@19b0` | 0.1% | 26,369 | 285,805 | 26369/1286 | 27655 | 27655 | 27655 | - | 1 2 4 |
| `f40e22b1@0648` | 0.1% | 1,309,131 | 0 | 0/0 | 0 | 0 | 0 | 20:1309131 | 14 |
| `d00719d8@01d8` | 0.1% | 9,736 | 159,870 | 15987/0 | 0 | 15987 | 0 | - | 1 4 |
| `059698f3@0840` | 0.1% | 67,494 | 632,753 | 67494/674 | 68168 | 68168 | 674 | 19:61066 20:6428 | 1 2 4 14 |
| `f40e22b1@0aa0` | 0.0% | 24,020 | 163,352 | 24020/8 | 24028 | 24028 | 24028 | - | 1 2 4 |
| `1fab41fc@00b0` | 0.0% | 26,369 | 0 | 0/0 | 0 | 0 | 0 | 20:26369 | 14 |
| `f40e22b1@0090` | 0.0% | 247,689 | 0 | 0/0 | 0 | 0 | 0 | 19:120463 20:127226 | 14 |
| `f40e22b1@0af8` | 0.0% | 24,020 | 163,352 | 24020/8 | 24028 | 24028 | 8 | 20:24020 | 1 2 4 14 |
| `059698f3@0960` | 0.0% | 1,259,660 | 0 | 0/0 | 0 | 0 | 0 | - | - |
| `1fab41fc@0030` | 0.0% | 26,369 | 0 | 0/0 | 0 | 0 | 0 | - | - |
| `d00719d8@0000` | 0.0% | 1,217 | 17,880 | 1788/0 | 0 | 1788 | 0 | - | 1 4 |
| `8178e9d1@0658` | 0.0% | 5,080 | 42,545 | 5080/0 | 5080 | 5080 | 5080 | - | 1 2 4 |
| `f40e22b1@0040` | 0.0% | 235,045 | 0 | 0/0 | 0 | 0 | 0 | - | - |
| `c17dcb79@05b0` | 0.0% | 4,369 | 38,443 | 4369/810 | 5179 | 5179 | 5007 | - | 1 2 4 |
| `e471dbf3@0000` | 0.0% | 88,625 | 0 | 0/0 | 0 | 0 | 0 | - | - |
| `059698f3@0020` | 0.0% | 159,723 | 0 | 0/0 | 0 | 0 | 0 | - | - |
| `98a396b7@0030` | 0.0% | 15,265 | 0 | 0/0 | 0 | 0 | 0 | - | - |
| `c54bd6b6@0030` | 0.0% | 14,056 | 0 | 0/0 | 0 | 0 | 0 | - | - |
| `059698f3@0080` | 0.0% | 98,312 | 0 | 0/0 | 0 | 0 | 0 | 20:98312 | 14 |
| `c17dcb79@0710` | 0.0% | 3,436 | 17,618 | 3436/276 | 3712 | 3712 | 3712 | - | 1 2 4 |
| `059698f3@09f8` | 0.0% | 444,031 | 0 | 0/0 | 0 | 0 | 0 | - | - |
| `059698f3@0030` | 0.0% | 39,153 | 0 | 0/0 | 0 | 0 | 0 | - | - |
