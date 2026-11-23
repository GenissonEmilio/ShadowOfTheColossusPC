# Shadow of the Colossus — Android

Android ARM64 adaptation of the North American PS2 SCUS-97472 port. The renderer
uses OpenGL ES 3.0, and the touch HUD feeds the same DualShock 2 input packets as
the game. The desktop launcher remains available separately.

## Controls

- Left stick: move Wander or Agro.
- Drag an empty area on the right: camera. There is no fixed camera stick.
- Focar (L1), Arma (D-pad left/right), Mapa (Select), and Pausa (Start) retain
  their labeled controls.
- The diamond sigil opens touch settings. Move controls in edit mode and tap the
  sigil again to finish. Scale, opacity, sensitivity, axis inversion and positions
  persist locally. Restore controls resets the saved layout and camera options.

| PlayStation button | Touch icon | Action |
| --- | --- | --- |
| Cross (X) | Horse head | Call Agro on foot. While mounted, press to accelerate into a gallop and hold to help maintain speed. |
| Square | Sword | Attack with the sword. While gripping a colossus, hold to charge a stab. With the bow equipped, hold to draw and release to shoot. |
| Circle | Sword with a glowing tip | Raise the equipped sword in sunlight. Its reflection guides you toward the next colossus and helps locate important points on a colossus. |
| Triangle | Boot | Jump, hold for a higher jump when available, mount or dismount Agro, and leap between holds while climbing. |
| R1 | Gripping hand | Grab fur and structures to climb or hold on when a colossus moves. |

The input router tracks pointer IDs independently: gripping, moving, jumping and
looking can be combined. Camera movement expires when a finger stops. Cancel,
backgrounding and focus loss release held input. The HUD is a separate Android
application panel, because NativeActivity owns the game surface and input queue.
The touch camera compensates the original game's 40% radial dead zone. Its
default axis signs follow the viewing direction; the two inversion options
remain available independently.

## Importing your disc

Use **Importar meu disco** to select your own `.iso`. The importer validates the
SCUS executable and extracts to app-private storage using a bounded buffer.
A replacement is activated only after extraction and validation succeed. Saves
are stored separately in `files/memcards`; updates and disc replacement preserve
them. Space for both the temporary ISO and extracted data is needed during import.

The game still has the desktop port's campaign limitations. Android builds must
be validated independently; compilation and HUD tests do not prove playability.

## Development build

Requirements: JDK 17, Android Gradle Plugin 8.10.1 (pinned), Gradle 8.12 (wrapper),
Android SDK 36, build-tools 35.0.0, NDK 28.2.13676358 and SDK CMake 3.31.6.
Set `ANDROID_HOME` or `local.properties` (not committed).

```sh
cd PS2Recomp/android
./gradlew assembleDebug assembleDebugAndroidTest
```

Without generated sources this builds the native runtime and test harness. Its
Play button stays unavailable. It is not a playable distribution.

Prepare generated C++ from the owned disc outside the repository using
`tools/android/prepare_game.py --help`. That script builds and executes successive
host runners to capture relocated modules. Existing host restrictions must be
respected. To reuse already collected captures, pass the host recompiler and the
private files explicitly:

```sh
python tools/android/prepare_game.py --iso /private/owned-disc.iso \
  --work /private/work --recompiler /private/tools/ps2_recomp \
  --captures-dir /private/captures
```

The capture directory must contain the three 32 MiB files `kernel.bin`,
`manager.bin` and `gamecore.bin`. An Android development capture build can obtain
the final module when the earlier generated stages are available:

```sh
./gradlew assembleDebug -PsotcCaptureMissing=true \
  -PsotcGeneratedDir=/private/work/manager/output \
  -PsotcBootSha256=SHA256_OF_SCUS_974_72
```

This maps to CMake's `SOTC_ANDROID_CAPTURE_MODULES=ON`. The runtime writes
`files/capture-gamecore.bin` in app-private storage; copy it to the private capture
directory as `gamecore.bin` before running `prepare_game.py --captures-dir`.
Capture mode is a development stage and intentionally stops at the missing
GAMECORE entry. It is disabled for the complete build.

For a game build, pass these Gradle properties:

```sh
./gradlew assembleDebug \
  -PsotcGeneratedDir=/private/work/gamecore/output \
  -PsotcBootSha256=SHA256_OF_SCUS_974_72 \
  -PsotcFfmpegRoot=/private/ffmpeg/install
```

For FMVs, use the official FFmpeg 7.1.5 tarball and verify its SHA-256 is
`de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f`.
[FFmpeg 7.1.5 source](https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz)

From the repository root, build from a clean extraction of the tarball.

```sh
ANDROID_NDK_HOME=/path/to/ndk/28.2.13676358 \
  bash tools/android/build_ffmpeg.sh /private/ffmpeg-7.1.5 /private/ffmpeg/install
```

Use GNU make and a host C compiler on Linux. On Windows, use Git Bash and put
LLVM-MinGW's `x86_64-w64-mingw32-clang` on `PATH`. The script uses two build jobs
by default. It installs the three ARM64 static PIC libraries, headers, LGPL 2.1
license and a record of the configure command. Pass the install directory as
`sotcFfmpegRoot`. The ROM-free test harness can omit FFmpeg.

For Windows, invoke `gradlew.bat`. `-PsotcBuildDir=D:/short/build` avoids long native
build paths. To package an already built library, use
`-PsotcPrebuiltDir=/private/jniLibs`, `-PsotcHasGame=true` and
`-PsotcBootSha256=SHA256_OF_SCUS_974_72`; that directory must contain
`arm64-v8a/libps2EntryRunner.so` and any non-system shared dependencies. Prebuilt
mode does not rebuild or verify native compilation flags.
Game builds require the SCUS SHA-256 property. The app verifies that executable;
the full `NICO.DAT` hash in the validation report is a separate, one-time check.

Public development certificates, APK verification metadata and private release
signing configuration are documented in [APK signing](signing/README.md).

Generated C++, captures, disc contents, saves, local keys and APKs must remain
outside version control. The CI job builds the ROM-free harness and runs JVM
tests and lint. It compiles instrumentation; executing those tests needs a device.
It does not publish a game APK.

## Tests

`tests/android/TouchInputStateTest.java` runs on the desktop JVM. It covers pointer
ownership, simultaneous actions, camera sensitivity/inversion, neutralization and
cancellation. `DiscImageImporterTest.java` checks extraction, wrong editions,
executable mismatch, malformed extents, path traversal and cancellation.

After installing both debug APKs on a device:

```sh
adb shell am instrument -w -e screenshot true \
  com.genisson.sotc.test/com.genisson.sotc.ControlsInstrumentation
```

This runs Android MotionEvent and layout/persistence tests in landscape. Real
NativeActivity overlay visibility, gameplay camera direction, audio, FMVs,
save/reload and frame timing still require tests with the owned game.

`tools/android/test_memory_card.sh CMAKE_DEPS_DIR` builds an ARM64 probe with the
NDK and verifies a synthetic card page in two separate processes on a connected
device. It uses temporary files outside the game's save directory and removes
them afterwards. It does not exercise the game's Save/Load menu.

For frame timing comparisons, `files/renderer.env` in the app's private storage
accepts only `SOTC_RENDER_THREAD`, `SOTC_REUSE_NATIVE_FRAME` and
`SOTC_ANDROID_FRAME_STATS`, each as `0` or `1`. Restart the game process after a
change. Compare `0/0/1` with `1/1/1` in the same scene and at the same resolution.
The optional five-second log separates published native frames from host
presentation, texture uploads, reused frames and readback/handoff time. Normal
defaults enable parallel rendering and texture reuse, with diagnostics disabled.
