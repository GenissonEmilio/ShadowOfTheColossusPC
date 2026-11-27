# Shadow of the Colossus — Android

Android ARM64 adaptation of the North American PS2 SCUS-97472 port. The renderer
uses OpenGL ES 3.0, and the touch HUD feeds the same DualShock 2 input packets as
the game. The desktop launcher remains available separately.

## Playing on Android

1. Install the ARM64 APK that includes the on-device preparation kit. It
   requires Android 9 or later and OpenGL ES 3.0.
2. Copy your own North American **SCUS-97472** PS2 ISO to the phone. Open the
   launcher and select **Import ISO** (**Importar ISO**).
3. Keep the launcher open while the app extracts the disc and prepares the
   ARM64 game runner. The first build can take a while. Leave enough free
   storage for the temporary ISO copy, extracted game files and build cache.
4. Select **Play** (**Jogar**) when the launcher says the game is ready.

The kit compiles locally from the imported disc. Once the APK and ISO are on
the phone, preparation and subsequent launches can run offline. The launcher
shows progress during import and compilation and offers **Cancel**
(**Cancelar**). If preparation stops after extraction, select **Prepare game**
(**Preparar jogo**) to try again using the extracted files. Matching stages
and object files can be reused. Later launches reuse the verified runner for
the current disc and kit. [Preparation kit details](../../tools/android/kit/README.md)

The portable APK uses the application ID `com.genisson.sotc.mobile`. The
memory card stays in app-private `files/memcards` across disc imports, retries
and signed app updates under that ID. Back up your memory card before
uninstalling. Development APKs under `com.genisson.sotc` have separate storage.

### Interface language

Open **Language** (**Idioma**) in the launcher and select **Português**,
**English**, **日本語**, or **Español**. The launcher remembers the selection
and applies it while import or preparation continues. The initial language
follows the device when supported and otherwise defaults to Portuguese.

The port menu and HUD follow the launcher by default. To choose their language
independently, open **Menu → Display and sound → Menu language**. These
settings apply to the Android interface. The game uses the campaign language
from the SCUS-97472 disc.

## Controls

- Use the left stick to move Wander or Agro.
- Start a drag in an empty area on the right to move the camera. The same finger
  can cross buttons without activating them.
- **Focus** uses PS2 L1 and **Weapon** uses D-pad left or right.
- **Map** (**Mapa**) sends PS2 Start to open the original in-game map.
- The diamond **Menu** button opens the port options, the Android equivalent of
  the desktop F1 menu.
- Select **Menu → Touch controls → Move buttons** to edit positions. Tap **Menu**
  again to finish. The positions, size, opacity, camera sensitivity and axis
  inversion persist locally. **Reset controls** restores the defaults.

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

## Port options

| Page | Available settings |
| --- | --- |
| **Graphics quality** | Quality preset, render scale, anti-aliasing (FXAA), texture filtering, model detail and water detail. |
| **Graphics effects** | Bloom, fog, glow, depth of field, shadows, particles, screen blur, motion trails, reflections and water refraction. |
| **Display and sound** | Screen mode, VSync, FSR, frame generation, 30 FPS limit, FPS display, volume and menu language. |
| **Touch controls** | Button positions, size, opacity, camera sensitivity, axis inversion and reset. |
| **Save states** | Save, load or delete the quick slot and five manual slots. |

The menu reports capabilities from the active runner and renderer. FXAA, FSR,
frame generation and save states depend on those capabilities. Anisotropic
texture filtering is limited to the levels the GPU supports. Older
`.sotcplay` runners may offer fewer settings.

**Adaptive widescreen** is the default for landscape displays wider than 4:3.
It expands the horizontal view while preserving the proportions of the menus,
subtitles and HUD. Prerecorded movies use their original framing. Use
**Display and sound → Screen mode** to switch to centered 4:3, stretch or
integer scaling.

### Android save states

Open **Menu → Save states** on a compatible Android runner. The slot list
shows timestamps and sizes for existing states. The game processes a request
when it can safely run the operation.

Android writes `.sotcstate` files atomically in app-private
`files/savestates`. The Android v2 format records the runner’s GNU build ID
and the disc executable’s CRC32. Loading requires the same runner build and
a matching disc and memory card. Use in-game shrine saves on the memory card
to carry progress between builds.

## Optional legacy play bundle

Portable APKs without the preparation kit expose **Import game package**
(**Importar pacote do jogo**). For those builds, prepare a compatible private
`ShadowOfTheColossus.sotcplay` on a computer using your SCUS-97472 disc. Copy the
bundle and ISO to the phone and import both. Select **Play** when the launcher
validates the files. Portable APKs with the preparation kit can also reuse a
compatible package already installed in that app’s storage.

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

Without generated sources this builds the native runtime and test harness.

### Portable APK with on-device preparation

Build and stage the ROM-free kit, then pass its output directory as
`sotcOnDeviceKit`. [Kit build and APK verification](../../tools/android/kit/README.md)

For a signed release, configure its release keystore and run from
`PS2Recomp/android`. Set the version code and name for the release.

```sh
./gradlew assembleRelease -PsotcPortable=true \
  -PsotcOnDeviceKit=/work/sotc-apk-kit \
  -PsotcVersionCode="$VERSION_CODE" -PsotcVersionName="$VERSION_NAME"
```

Run the same release command with the kit property omitted to build the
portable variant that uses private `.sotcplay` packages.

### Private development builds

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
Private game builds require the SCUS SHA-256 property to verify the disc
executable. Check the full `NICO.DAT` hash separately against the validation
report when preparing a private build.

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

To run the launcher and control instrumentation, install the debug app and
test APKs for the same variant. Use this command for the development harness.

```sh
adb shell am instrument -w -e screenshot true \
  com.genisson.sotc.test/com.genisson.sotc.ControlsInstrumentation
```

For the portable APK, build with `assembleDebug assembleDebugAndroidTest
-PsotcPortable=true` and pass `sotcOnDeviceKit` to test the kit variant. Use
`com.genisson.sotc.mobile.test/com.genisson.sotc.ControlsInstrumentation` as
the instrumentation target.

Instrumentation exercises Android MotionEvent handling, control layout and
persistence, and the launcher language setting. The portable package tests
also cover archive structure, manifest parsing, SHA-256, ARM64 ELF alignment
and atomic package replacement.

Device validation for a fresh ISO-only installation is pending. It covers the
first full preparation, retry and offline relaunch, the in-game touch overlay,
FMV and audio, shrine gameplay, memory-card and state save/load, and frame timing.

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
