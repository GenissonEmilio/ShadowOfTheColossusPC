# Particle material fidelity

This change is reserved for the **v0.2.8 candidate**. It is excluded from the
`5c84a62/cb15` package selected for the planned v0.2.7 release. Neither that
release nor a newly packaged v0.2.8 is confirmed published by this document.

The native particle hook now preserves two material properties written by
`ptclmanBuildMatPacket` (`0x01099e00`): texture wrapping and depth writes.
This change applies only to particle draws. It does not change the game’s
particle simulation, input, geometry expansion, blend selection, or depth test.

## Evidence and scope

The original packet builder writes `TEST_1 = 0x51011` at packet `+0x20`,
with the A+D register ID `0x47` at `+0x28`. The low bits decode as alpha test
enabled, `ATST = NEVER`, and `AFAIL = FB_ONLY`; depth testing remains enabled
with `ZTST = GEQUAL`. Thus its particles contribute colour without writing Z.
The native renderer previously wrote Z for normal and subtractive particles,
which could occlude later particles or transparent draws behind them.
`DrawState::noDepthWrite` already implements this rule in both backends.

The builder writes `CLAMP_1` at `+0x30`, register ID `0x08` at `+0x38`.
Its `c.le.s` instructions at `0x01099e18` and `0x01099e54` select CLAMP
when `f12 <= 1` and `f13 <= 1`, independently for U and V; otherwise they
select REPEAT. The hook previously lost these arguments and used REPEAT on
both axes. It now captures them before the original call using the runtime’s
EE comparison macros, including their special float bit handling.

The packet constants and instruction addresses were checked against the
locally generated original function. No game instructions, assets, save data,
or generated game sources are included in the repository or test fixture.
The same omissions were present in the primary upstream source inspected at
[`77c962c9459948579f9fd8b3b3df6874a0d4bed3`](https://github.com/HenryKun55/celosia/blob/77c962c9459948579f9fd8b3b3df6874a0d4bed3/game/overrides/sotc_native.cpp).
This is a directed correction of the existing GPL-3.0 code, not a cherry-pick.

These contracts establish renderer defects independently of campaign progress.
They do **not** establish that an external altar’s particles or Hydrus’s
electricity use an affected material. Those scenes still need gameplay and
draw evidence before their reported symptoms can be called fixed.

## Regression fixture

`tests/native/particle_fidelity.cpp` executes the actual material and vertex
hooks with synthetic emitter data. A stub original function emits a small
reference TEST/CLAMP packet, records the incoming registers, and overwrites
caller-saved registers before returning. The fixture captures the real hook’s
`DrawState` and expanded vertices at the renderer boundary. It verifies:

- normal and subtractive particles keep depth testing and suppress Z writes;
- independent CLAMP/REPEAT modes, the inclusive threshold and EE float rules;
- unchanged additive blend, streak vertex counts, layer and ordinary defaults;
- unchanged arguments and exactly one call to each original function.

Run with an existing compiler kit:

```sh
bash tools/linux/test_particle_fidelity.sh /absolute/path/to/compiler-kit
```

The Linux CI runs the same fixture against baseline
`4396d9981d76a74118b14f82f22993a99a5adcba` and the candidate. It requires the
baseline to compile successfully, fail the depth/wrap assertions semantically,
and pass the input/blend controls. A compiler or setup failure does not count
as evidence of the original defect. The candidate must pass every check.

The [full CI run for `5ec48fe`](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/41272468970)
passed. The same fixture compiled in both versions: the baseline produced nine
semantic failures and passed six controls; the candidate passed all 15 checks.
The remaining CI checks, including fullscreen, GPU, input and card persistence,
also passed.

After the preceding QA gameplay session exited normally, the exact CI package
was installed as a separate candidate. A bounded local link reused 284 unchanged
guest objects after checking identical generator/configuration, headers,
toolchain, flags and other link inputs. Only the native particle translation
unit changed inside the base archive. The candidate kit is
`d357e14c82a02adeeca1aae3`. Its replay of Valus remained incomplete; those
attempts did not demonstrate a new product defect. Scene-specific particle
validation remains pending, and no new v0.2.8 package is complete.

The preceding `cb15` native defeated Valus, showed the darkened corpse and
post-death tendrils, and saved through the normal game interface. The package
selected for v0.2.7 has the same eleven native link inputs; its save reload is
the remaining first-colossus gate before progressing to Quadratus. The particle
patch and the altar/Hydrus scenes still require their own gameplay validation.
