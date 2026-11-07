# Conditional depth writes for translucent model materials — candidate

A complete native OpenGL worker-frame capture of Hydrus contained 1,542
draws. At one inspected head pixel, the glow shader produced a nonzero colour,
but failed its depth test. Earlier fur fragments had updated the shared scene
depth with blend alpha values of approximately 0.222, 0.097 and 0.070.

The two occluding GPU vertex streams match the original model packets 256 and
292 exactly in the sampled positions. Their surfaces 52 and 53 use textures 7
and 8. The original texture resolver builds TEST `0x5140b`: alpha GEQUAL with
reference 64, AFAIL FB_ONLY, destination-alpha testing disabled, and depth
GEQUAL. Failing the alpha comparison must retain framebuffer writes while
suppressing depth writes. The native backend previously used a small discard
threshold and an unconditional depth mask for those model draws.

The candidate carries this limited TEST contract from each texture's NTO2
configuration into the draw state, independently for the two material passes.
Invalid or unsupported configurations retain the previous path. The special
boss-mask and reflection/refraction substitutions do not inherit a placeholder
texture's alpha test. Existing depth masks remain authoritative.

OpenGL 3.3 cannot conditionally disable a depth write within the fragment
shader. For a mixed-alpha triangle, the backend first draws colour without Z,
then draws only Z for fragments passing the original alpha comparison. It
finishes both operations before the next triangle. Applying both operations
to an entire overlapping mesh can change colour and occlusion order, so that
shortcut is deliberately excluded. Consecutive triangles proven entirely
passing or entirely failing by conservative texture/vertex alpha bounds share
a normal draw. Bounds include every uploaded mip and the draw's opacity.
The alpha reference is compared in GS byte units, before blend-alpha
saturation; references above 128 are therefore meaningful.

The original glow composition uses texture alpha and normal blending. Neither
the glow strength nor its depth test is replaced to hide this defect.

`test_alpha_depth.sh` exercises real OpenGL pixels with synthetic inputs. Its
negative control disables only this conditional-depth path. The frontend
fixture separately covers texture lookup, references, invalid configurations,
single/dual materials and opacity.

The protected local run passed **219 frontend checks and 84 GPU checks** with
no failures. The GPU cases include static meshes, indexed frame-arena meshes
with nonzero offsets, and actual GPU skinning; overlapping primitives in both
orders; alpha 63/64/65, reference 193 before blend saturation, very low alpha,
masked depth writes, mip changes and opacity. The compiled negative control
completed the same 84 GPU checks and failed 18, including the fur/scar/glow
colour and depth probes in all three mesh paths. This demonstrates the old
failure rather than treating a compiler or driver error as a regression test.
Game performance and the final appearance still require observation before
this candidate becomes the principal release.

This is a demonstrated local rendering defect, not a completed validation of
Hydrus, Basaran, the electrical effects or the remaining campaign. The current
installed principal release remains v0.3.3 until the candidate passes the
required checks and is explicitly published.
