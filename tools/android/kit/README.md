# Android on-device preparation kit

The kit lets the portable APK generate its ARM64 runner from an extracted
SCUS-97472 ISO. It includes the PS2 source generator, Clang and LLD, CPython
for the existing XFF analysis scripts, link inputs for API 28, and the
ROM-free runtime as PIC archives. It uses app-private storage for captures,
C++ files, object files and the resulting game library.

## Build

Use Android NDK 28.2.13676358, CMake 3.31.6, Ninja, Python 3.10 or newer and
the ARM64 FFmpeg prefix built by [build_ffmpeg.sh](../build_ffmpeg.sh).
Run from the repository root with your paths.

```sh
cmake -S PS2Recomp -B /work/sotc-kit-build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/sdk/ndk/28.2.13676358/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_ANDROID_KIT=ON \
  -DPS2X_ENABLE_FFMPEG=ON -DPS2X_ANDROID_FFMPEG_ROOT=/work/ffmpeg \
  -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_IOP_RPC_TRACE=OFF \
  -DPS2X_ENABLE_DEBUG_UI=OFF
cmake --build /work/sotc-kit-build --target sotc_android_kit --parallel 2
python3 tools/android/stage_on_device_kit.py \
  --ndk /sdk/ndk/28.2.13676358 --runtime-stage /work/sotc-kit-build/android-kit/stage \
  --downloads /work/sotc-kit-downloads --output /work/sotc-apk-kit

cd PS2Recomp/android
./gradlew assembleDebug -PsotcPortable=true -PsotcOnDeviceKit=/work/sotc-apk-kit
python3 ../../tools/android/verify_on_device_apk.py app/build/outputs/apk/debug/app-debug.apk
```

The staging script verifies the tool downloads against pinned SHA-256
digests and checks the packaged ARM64 ELF files for Android PIE or shared
library format and 16 KiB load segment alignment. The linked libraries
and headers come from the pinned NDK and the runtime build. The APK
includes the notices under `assets/licenses/`. The APK verification checks
the native library allowlist and checks the embedded kit for unexpected files.

## Preparation

The APK installer extracts the tools into Android's read-only native library
directory. The launcher starts the packaged executables there and uses CPython
to run the same function-map and XFF helpers as the desktop launcher. It then
uses each incomplete runner in a separate process to capture the relocated
module needed for the next stage. The process links a shared library, marks
it read-only and checks that it loads before it records a SHA-256 and
publishes the final runner.

Preparation uses up to two compiler workers based on available memory. A retry can reuse finished stages
and object files. Keep the launcher on screen during initial preparation.
Diagnostics are in `files/on-device/<kit-id>/work/<stage>/*.log` in the
app's private storage on debug builds. Each capture uses its own temporary
memory cards in `work/capture-memcards`. Gameplay uses the normal `files/memcards`.

The existing private play-bundle build remains available without the kit.
The on-device kit and the legacy player both target the SCUS-97472 game code.
