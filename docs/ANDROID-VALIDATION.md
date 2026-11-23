# Android validation

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
| Complete game execution | FMV, title menu, New Game and initial 3D shrine displayed |
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

| Mode | Mean native frames/s | Range | Mean game-thread render handoff | Texture uploads per 5 s |
| --- | ---: | ---: | ---: | ---: |
| Synchronous, repeat texture uploads | 21.6 | 20.6–23.0 | 25.9 ms | 300 |
| Parallel rendering, reuse unchanged texture | 34.0 | 33.4–36.4 | 8.7 ms | 167–182 |

This is about 57% more native frames in this scene. Host presentation remained
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

Private captures, logs, generated C++ and disc files are kept outside the
repository. The local APK is in the ignored `artifacts` directory. ROM-free CI
does not publish a playable APK.
