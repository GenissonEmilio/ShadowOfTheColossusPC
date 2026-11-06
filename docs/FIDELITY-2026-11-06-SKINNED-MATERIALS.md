# Skinned materials with two texture passes

The Hydrus head weak-point material uses two textures on a skinned mesh.
The original vertex program draws both passes, using the same transformed
geometry and two UV pairs. The native renderer excluded the second pass
whenever the mesh was skinned. Its packed-vertex parser also replaced the
field-2 normal with the field-3 UV data, and the pose cache retained only UV1.

The correction preserves the packed normal separately, retains both UV pairs,
and emits both material passes for GPU skinning and the fast/slow CPU paths.
The second pass shares the pose and palette, with its own texture, UVs,
blend and clamp state. Rigid and single-texture materials remain covered by
the same regression fixture.

`tools/linux/test_skinned_multipass.sh` uses synthetic VIF packets and renderer
boundary wrappers, with no ROM data or GPU. The previous implementation fails
six checks: normals and pass counts in each of the three skinning paths. The
corrected implementation passes all 119 checks across eight scenarios,
including rigid and single-texture controls. This verifies submitted geometry
and draw state; it does not verify the final GPU image or a complete battle.

The check is part of the Linux CI. Gameplay and visual validation with the
corrected executable are still pending; the previous build completed the
Hydrus reminiscence and returned normally, but weak-point brightness and the
reported electrical artifacts were not marked visually validated.
