# Shadow of the Colossus — Android

Android ARM64 adaptation of the North American PS2 SCUS-97472 port. The renderer
uses OpenGL ES 3.0, and the touch HUD feeds the same DualShock 2 input packets as
the game. The desktop launcher remains available separately.

## Controls

- Left stick: move Wander or Agro.
- Start a drag in an empty area on the right to move the camera. The same finger
  can cross buttons without activating them.
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
The touch camera filters small position jitter and increases the stick response
for deliberate swipes. Short, slow drags can move the camera past the game's
40% radial dead zone. Both axes can still be inverted independently.

## Display

**Tela cheia adaptada** is enabled by default for landscape displays wider
than 4:3. The game adapts its projection to show more of the scene horizontally
while keeping the menu, subtitles and indicators in their original proportions.
Prerecorded movies use their original framing. The diamond menu lets you
switch back to the centered 4:3 view. The display choice is saved separately
from the touch controls.

## Playing on Android

1. Install the public Android ARM64 APK from the same release as the desktop
   packages. It requires Android 9 or later and OpenGL ES 3.0.
2. On your computer, prepare `ShadowOfTheColossus.sotcplay` as described below
   using your own North American PS2 disc image (`SCUS-97472`).
3. Copy the `.sotcplay` bundle and your `.iso` to the phone. Use **Importar
   pacote do jogo** and **Importar meu disco** on the launcher, then select
   **Jogar**. Import requires space for the extracted game data.

The launcher activates each replacement after validating it. Saves are kept in
the app's `files/memcards` directory across bundle, disc and signed APK updates
under the same application ID. The public APK uses `com.genisson.sotc.mobile`
and has separate storage from the previous development APK. Keep a backup
before uninstalling an Android build, which removes its app-private data.

The game still has the desktop port's campaign limitations. Android builds must
be validated independently; compilation and HUD tests do not prove playability.

### Preparing the private play bundle on a computer

Use the source code from the same release as the APK. For the native build on
Linux x86-64, install Python 3.10 or later, Git, the Android SDK's NDK
`28.2.13676358` and CMake `3.31.6`. Set `ANDROID_HOME` to the SDK path or pass
`--sdk`. Obtain `compiler/bin/ps2_recomp` from the extracted desktop release and
build the [ARM64 static FFmpeg libraries](#building-ffmpeg) once. The Android
CMake build fetches open-source dependencies when they are absent locally.

From the source checkout root, run the following with your local paths. On
Linux, setting `LD_LIBRARY_PATH` lets the packaged recompiler find its bundled
libraries.

```sh
export LD_LIBRARY_PATH=/private/desktop-release/compiler/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
python3 tools/android/prepare_play_bundle.py \
  --iso /private/owned-disc.iso --work /private/android-game \
  --recompiler /private/desktop-release/compiler/bin/ps2_recomp \
  --ffmpeg-root /private/ffmpeg/install \
  --captures-dir /private/captures
```

The capture directory holds your private `kernel.bin`, `manager.bin` and
`gamecore.bin` files (32 MiB each). A completed desktop preparation can supply
them from `builds/<build_id>/captures` under the launcher's data directory. If
you have no captures, omit `--captures-dir` to try capturing from the local ISO
with a host C/C++ compiler and graphics development libraries. If the host
runner reaches MANAGER but cannot capture GAMECORE, use the Android development
capture procedure below for the missing stage. The script reuses matching
generated sources and incremental native builds in `--work`.

Windows x86-64 uses `python`, the corresponding desktop `ps2_recomp.exe`, and
the Windows SDK components. Reuse a complete set of private module captures for
the Windows workflow. The Windows host capture and full prepare command require
separate validation.

If you already built a complete compatible ARM64 game library from your own
disc, pass `--runner /private/libps2EntryRunner.so` instead of `--recompiler`,
`--ffmpeg-root` and `--captures-dir`. This route only needs Python and the SDK's
NDK. It checks the ISO, ARM64 ABI, Android API level, 16 KiB page alignment,
system library dependencies and the JNI and FFmpeg exports before packaging.

The result is `ShadowOfTheColossus.sotcplay` in `--work`. It contains the
locally built ARM64 runner and its manifest. Keep it private with your disc
image and captures.

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
From `PS2Recomp/android`, build the public variant using its release keystore.

```sh
./gradlew assembleRelease -PsotcPortable=true \
  -PsotcVersionCode=3 -PsotcVersionName=0.2.1
```

Use `./gradlew assembleDebug assembleDebugAndroidTest -PsotcPortable=true` to
build its debug launcher and importer instrumentation. The synthetic test
cases cover strict manifest parsing, archive entries, SHA-256, ELF architecture
and alignment, and atomic package replacement. Instrument the portable build
with `adb shell am instrument -w com.genisson.sotc.mobile.test/com.genisson.sotc.ControlsInstrumentation`
on a device with the portable debug APKs installed.

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
GAMECORE entry. To retrieve the capture from an installed debug build, use
`adb exec-out run-as com.genisson.sotc cat files/capture-gamecore.bin` and save
the output as `gamecore.bin` alongside `kernel.bin` and `manager.bin`. Use a
separate test device or application ID for debug builds signed with a different
key than the APK you use to play. The private play bundle build disables
capture mode.

For a game build, pass these Gradle properties:

```sh
./gradlew assembleDebug \
  -PsotcGeneratedDir=/private/work/gamecore/output \
  -PsotcBootSha256=SHA256_OF_SCUS_974_72 \
  -PsotcFfmpegRoot=/private/ffmpeg/install
```

### Building FFmpeg

Use the official FFmpeg 7.1.5 tarball and verify its SHA-256 is
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

Generated C++, captures, disc contents, `.sotcplay` packages, saves, local keys and APKs must remain
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
