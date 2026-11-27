# Android validation

## Standalone mobile update — 2026-11-27

This update starts from desktop v1.2.10 and includes the ARM64 generator from
PR #45. The portable APK additionally packages the ROM-free runtime archives,
Clang/LLD and CPython needed by the launcher to prepare its own runner. The
generator alone does not resolve issue #44.

Local checks on the Android 16 / Adreno 810 device:

| Check | Result |
| --- | --- |
| ARM64 runtime and preparation kit build | Passed, NDK 28.2.13676358, API 28 |
| Portable APK, instrumentation and lint | Passed; no lint errors |
| APK contents, signature and 16 KiB ZIP/ELF alignment | Passed; debug signing, no game inputs in APK |
| JVM input, ISO and repository suites | Passed; Windows symlink cases require privileges and were skipped |
| Installed-app instrumentation | 9 checks passed, including four launcher languages, Map/START separation, multitouch and layout persistence |
| Installed-app compiler smoke test | CPython helpers, C++ compilation, runtime link, read-only runner and native load probe passed under the app UID |
| GLES presentation shaders | Six production shader pairs passed pixel checks on Adreno 810 |
| Shared EGL frame ownership | Passed, including resize and bidirectional synchronization |
| Renderer alpha ordering and fallback | Shared textures and CPU fallback produced identical RGBA hashes with batching enabled or disabled |
| Bionic RNG and state identity | Six seeds × 2,051 draws matched; 1,024 draws replayed per seed; malformed state and same-CRC build-ID collision rejected |
| Preparation cancellation and retry | Passed during boot compilation; child processes stopped, imported disc and 112 completed objects preserved and reused |
| First standalone stage | Boot library generated, linked and loaded on the phone; KERNEL RAM capture completed |
| First ISO-only preparation and gameplay | In progress; not yet certified |

The renderer fixture reduced actual GLES draw calls from 15 to 5 in one probe
and from 16 to 6 in another, preserving the two RGBA hashes
`245dca6e65b092f4` and `17006dd50fc522e5`. This measures submission reduction
in those synthetic scenes, **not an FPS improvement for the full game**.

The mobile options menu uses the runtime's actual quality, effects, volume,
frame generation, FSR, FXAA, anisotropy and state APIs. Shader-dependent
options use the capabilities detected after GLES initialization. Save states
use Android format v2 with the complete GNU build ID; desktop format v1 is
unchanged. Normal game memory cards remain separate from compilation captures.

The initial standalone validation uses the owned SCUS-97472 ISO as the only
game input. No desktop-generated runner or module capture is imported for this
check. The historical measurements below concern earlier builds and must not
be attributed to this update.

Development record: 2026-11-23. Android validation is independent of the desktop
campaign results in the project README.

## Build and data

- Target: Android ARM64, API 28 minimum, GLES 3.0 minimum, 16 KiB ELF alignment.
- Connected test device: Android 16, Qualcomm Adreno 810, 2772 × 1280 landscape.
- Input disc: owned North American PS2 SCUS-97472; the original ISO was retained.
- The real Android loader capture is 33,554,432 bytes. Its SHA-256 is
  `68d0a156b2b7f60c03fedc22627fbede5120dcd5435a6fa74c8c18e9495935d9`.
- Preparation validated all 41,389 relocated words against that capture and
  generated 9,024 mapped functions, including 3,972 in GAMECORE.
- Phone and host hashes matched for both SCUS and the complete NICO.DAT file.
- FFmpeg 7.1.5 was built as static PIC ARM64 libraries with MPEG-2 video decoding
  and the MPEG video parser. The tarball signature and library architectures
  were checked; no game data was downloaded.

## Completed checks

| Check | Result |
| --- | --- |
| JVM touch input regression suite | Passed |
| ISO import regression suite | 10 checks passed |
| Real-disc Java import | 19 ISO files extracted in 47 s; all paths, sizes and SHA-256 hashes matched the reference |
| Import recovery and active-disc validation | Passed; active/fallback disc and saves preserved |
| Directory-link safety on Windows | NTFS junction checked; symlink creation restricted on this host |
| Android Java build and APK packaging | Passed |
| APK signature and 16 KiB ZIP alignment | Passed; APK Signature Scheme v2, ARM64 only |
| Android lint | 0 errors; 18 warnings, including untranslated/hardcoded text |
| Android workflow syntax | actionlint passed |
| Android MotionEvent/layout tests | 4 checks passed on the device |
| Label rendering regression | Passed; labels retain separate glyph advances |
| Main/effects/skinning GLES shader compilation | Validated statically |
| Native EGL renderer initialization | Passed on Adreno 810 |
| SCUS/KERNEL/MANAGER execution and GAMECORE loading | Passed on Android; real RAM captured |
| Game boot and initial 3D scene | FMV, title menu, New Game and initial 3D shrine displayed |
| Gameplay touch input | Movement, jump, pause/map and both camera directions checked |
| Native memory-card persistence | ARM64 probe wrote 512 bytes and another process verified them in a separate synthetic card |
| Background and return | 3D scene and HUD restored twice in the same process; held movement released |
| Exit and relaunch | Three clean exits; AAudio closed and no game process remained |

The MotionEvent tests cover simultaneous joystick/grip/camera input, pointer ID
reordering, camera idle neutralization, button/camera separation, cancellation,
layout editing and persistence. Device gameplay checks additionally compared
up/down and horizontal drags against the scene. The adapter compensates the
game's 40% radial dead zone and its original axis signs. The inversion options
remain off by default and can be changed independently.

## Frame timing comparison

Both runs used the same complete ARM64 build, device, initial altar view, native
resolution and effects. The only differences were renderer overlap and reuse
of an unchanged presentation texture. Each row summarizes six consecutive
five-second windows after the scene had settled.

| Mode | Mean renderer frames published/s | Range | Mean game-thread render handoff | Texture uploads per 5 s |
| --- | ---: | ---: | ---: | ---: |
| Synchronous, repeat texture uploads | 21.6 | 20.6–23.0 | 25.9 ms | 300 |
| Parallel rendering, reuse unchanged texture | 34.0 | 33.4–36.4 | 8.7 ms | 167–182 |

This is about 57% more frames published by the renderer in this scene. The raw
six-window samples are in [the benchmark record](android-benchmark-2026-11-23.json).
The A/B runs were sequential; device temperature and other system load were not
controlled. Host presentation remained
approximately 60 Hz. These measurements do not guarantee the same performance
throughout the campaign or on other devices. Diagnostics are off by default;
the private A/B configuration was removed after testing.

## Lifecycle corrections retested

The capture build exposed a shutdown deadlock: Bionic's global stdio flush could
wait on a FILE used by a logger thread blocked in `fgets` on a pipe. The logger
now reads the pipe with `read(2)` and flushes stdout/stderr explicitly when
restoring them. The complete build also exposed raylib 5.5 destroying its audio
mutex before its AAudio device stopped. The Android patch now stops the device
first, following the upstream fix.

After both corrections, three game processes exited cleanly. The last two
included the parallel renderer, and the final APK was reinstalled and tested.
Logs confirmed successful audio shutdown with no FORTIFY/SIGABRT. The final
launcher returned to enabled Play/Import buttons, `pidof` returned no game PID,
and `dumpsys meminfo` found no game process. The existing card's hash was unchanged.

The 3D session used about 328 MiB PSS at one sample. This is an observation, not
a peak-memory guarantee. Destroying the game Activity releases its separate
native process after the runtime has stopped. A dismiss callback cannot unpause
the guest while its Activity is in the background.

## Scope and remaining validation

The native card probe checks page I/O and persistence across processes. It does
not validate Save/Load through the game's own menu. Full colossus combat and
climbing, the complete campaign, physical gamepads and foldable/multiwindow
behavior have not been certified. The resize patch was checked with the NDK and
the CMake integration; ordinary background/return was tested on the phone.

Android reported an active stereo AAudio stream at 48 kHz, and its own AudioTrack
diagnostics reported nonzero amplitudes. System volume was muted, so audible
quality was not assessed or changed. The FMV image was reviewed on the device.

This is a development APK, version `0.1.0-dev`, not a campaign-complete release.

## PR packaging and signing follow-up

The gameplay measurements and device checks above used the original AGP 8.7.3
APK, SHA-256 `4a809123ca2cc53d48a0b0c49d5725208a088ecdcf7173721f5c720131b66f18`.
The PR moves to AGP 8.10.1, which supports compile/target API 36. Gradle remains
8.12, JDK 17 and NDK 28.2.13676358. Local debug/instrumentation packaging passed;
the new lint run reported 0 errors and 17 warnings. The local SDK property path
was escaped correctly for the newer lint rule.

The separate PR APK, SHA-256
`85f9010e66e6947793d9bcb55ec64c6ead986f094f0be9f2b54e92ee90b37bc0`, passed
signature, certificate, ZIP alignment and native-library hash verification.
The packaged native library is byte-identical in both APKs. The phone was no
longer connected for this repackaging, so device testing was not repeated and
the original APK was preserved. Public certificate and verification metadata
are under `PS2Recomp/android/signing`; no private key is committed.

Release signing was tested with a temporary PKCS12 key: `assembleRelease`,
signature verification and 16 KiB ZIP alignment passed. The temporary key was
removed, and that probe APK was not installed or published. Negative checks
rejected absent signing configuration, a missing keystore, a game build without
its SCUS hash, and a nonexistent prebuilt library. These are packaging/configuration
checks, not production-key provisioning or additional gameplay tests.

The PR's ROM-free workflow runs the three JVM suites, compiles the ARM64 runtime
and instrumentation APK, runs lint, and checks its debug APK signature and ZIP
alignment. It does not execute instrumentation, load game data or certify the
campaign. GitHub check results belong to the PR; the tables above are local and
device evidence.

Private captures, logs, generated C++ and disc files are kept outside the
repository. The local APK is in the ignored `artifacts` directory. ROM-free CI
does not publish a playable APK.
