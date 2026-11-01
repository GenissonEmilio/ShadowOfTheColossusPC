# Transfer palette fidelity

The native texture decoder previously read the immutable palette in an NTO2
resource. The game's texture system prepares a separate palette for upload:
alpha effects modify a base copy, while palette scrolling may select another
copy. Reading and hashing only the file palette loses those changes even when
the renderer's upload cache correctly detects every byte it receives.

The shared palette selector now follows the active transfer selection. It
accepts bounded, aligned main RAM addresses and the runtime's direct/uncached
RAM aliases. Uninitialized or invalid transfer state falls back to a valid
file palette. An invalid file palette is rejected if there is no valid active
selection. Direct RGBA textures do not need a palette.

The optimized decoder, reference decoder, sampled key and complete content
key use this selection. Selecting the base copy unconditionally would still
lose palette scrolling; changing the decoder without changing the key would
retain stale GPU textures.

The comparison is based on the original `texTransResolve`, `texScaleClutAlpha`,
texture initialization and `texClutScrollOne` paths already described by the
project's renderer map. No external implementation or game data is included.
The patch retains the repository's GPL-3.0 license.

`tests/native/transfer_palette.cpp` creates its own textures, palettes and work
records. It checks zero alpha, an in-place alpha fade, active-buffer changes,
8-bit CSM1 ordering, changes outside a sparse palette sample, static cache
reuse, address validation/fallback and direct RGBA compatibility. Run with:

```sh
tools/linux/test_transfer_palette.sh artifacts/compiler
```

The test also accepts `SOTC_NATIVE_TEST_SOURCE` pointing to the earlier native
source, so the same semantic checks can demonstrate the previous failure.
CI compiles the same fixture against baseline
`83d8eff3c72902822c673dd60709e601777459bd` and requires specific semantic
failures, successful compilation and passing static-palette/RGBA controls.
It then requires the current source to pass. The fixture also checks the
accelerated RAM alias and that copying a `TexSource` freezes the palette
before a later guest mutation, as needed by asynchronous decoding.

[CI run 41259725998](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/41259725998)
completed successfully for patch `6191f4f77b7f2ae86ace41ebfae7c4e14a10ab3f`.
The baseline compiled and failed 13 of the same 22 semantic checks; the patch
compiled and passed all 22. The complete Linux workflow also passed. This
fixture intercepts the public renderer upload boundary; it does not create a
GPU context or test a game asset.

These synthetic checks do not validate sigils in combat, boss death effects,
Hydrus, collision, or any campaign completion. Gameplay evidence and saves
remain outside this document and outside the distributed package.
