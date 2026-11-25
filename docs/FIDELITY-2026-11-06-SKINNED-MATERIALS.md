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

The check is part of the Linux CI. The complete workflow passed for
`ce1851b` (run `42160594216`).

The actual Hydrus material was also audited against its selected VU entry,
rather than another routine in the same microprogram. Packets 200/201 select
MSCAL `0x12`: micro5 entry `0x0090` reaches `0x05B8`, then the first pass
at `0x10E0` and second at `0x1008`. The latter uses the other two ST components
and preserves the first pass's transformed geometry and lit RGBA. The
environment-mapping routine at `0x0B18` belongs to a different entry and is
not the contract for these packets.

The corrected executable completed a normal Hydrus reminiscence: all three
electrical parts accepted damage, the head accepted charged attacks to its
capacity of 170, and the game returned to the ruins with the original
eight-cleared campaign restored. No progress flags, health or guest memory
were edited. The charge indicator was visible. F5/F8 restored the head
checkpoint's position, pose, charge, weak-point fade and active palettes.

This is gameplay and state-restoration evidence, not complete visual approval.
The main sigil was not unambiguously visible in the examined camera angles;
its brightness remains pending. Electrical artifacts, the reported Agro
deformation, Pelagia and the remaining campaign also remain under investigation.

The published Linux v0.3.3 is the exact package from that CI run, kit
`7fe91f38ffee26ff8a553bdd`, archive SHA-256
`be27ac0387fe08fe529337ed113d16559dfd6b00e4556b01e01c6dc9e59bb293`.
Its eleven linker inputs match those used by the completed replay. The final
package selected the same executable bytes and loaded the preserved head and
return checkpoints with the normal runtime/disc checks enabled. The launcher
and canonical local installation were updated; the personal card, settings
and controls were preserved.
