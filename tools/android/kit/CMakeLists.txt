set(repository "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(runtime "${repository}/PS2Recomp/ps2xRuntime")
set(kit "${CMAKE_CURRENT_BINARY_DIR}/stage")
file(GLOB overrides CONFIGURE_DEPENDS "${repository}/game/overrides/*.cpp")

if(PS2X_ENABLE_LTO OR NOT PS2X_ENABLE_FFMPEG)
    message(FATAL_ERROR "The Android kit requires FFmpeg and non-LTO archives")
endif()

add_library(sotc_android_base STATIC "${runtime}/src/main.cpp" ${overrides})
target_compile_features(sotc_android_base PRIVATE cxx_std_20)
target_compile_definitions(sotc_android_base PRIVATE SOTC_HAS_GAME=1 PS2X_VU1_TRUST_FPU=1)
target_compile_options(sotc_android_base PRIVATE -O3 -fno-strict-aliasing -ffile-prefix-map=${repository}=.)
target_link_libraries(sotc_android_base PRIVATE ps2_runtime)

add_executable(sotc_android_tool native_tool.c)
target_compile_options(sotc_android_tool PRIVATE -Wall -Wextra)
target_link_libraries(sotc_android_tool PRIVATE dl)

add_custom_target(sotc_android_kit
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${kit}/lib" "${kit}/include" "${kit}/tools"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:ps2_recomp>" "${kit}/tools/libSotcRecompile.so"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:sotc_android_tool>" "${kit}/tools/libSotcPrepare.so"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:sotc_android_base>" "${kit}/lib/libbase.a"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:ps2_runtime>" "${kit}/lib/libps2_runtime.a"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:ps2_iop>" "${kit}/lib/libps2_iop.a"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:raylib>" "${kit}/lib/libraylib.a"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${runtime}/include" "${kit}/include"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${repository}/PS2Recomp/ps2xIOP/include" "${kit}/include"
    COMMAND "${CMAKE_COMMAND}" -E copy "${sse2neon_SOURCE_DIR}/sse2neon.h" "${kit}/include/sse2neon.h"
    COMMAND "${CMAKE_COMMAND}" -E copy "${raylib_SOURCE_DIR}/src/raylib.h" "${kit}/include/raylib.h"
    COMMAND "${CMAKE_COMMAND}" -E copy "${raylib_SOURCE_DIR}/src/rlgl.h" "${kit}/include/rlgl.h"
    COMMAND "${CMAKE_COMMAND}" -E copy "${PS2X_ANDROID_FFMPEG_ROOT}/lib/libavcodec.a"
            "${PS2X_ANDROID_FFMPEG_ROOT}/lib/libavutil.a" "${PS2X_ANDROID_FFMPEG_ROOT}/lib/libswscale.a" "${kit}/lib"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${kit}/licenses"
    COMMAND "${CMAKE_COMMAND}" -E copy "${sse2neon_SOURCE_DIR}/LICENSE" "${kit}/licenses/sse2neon.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy "${raylib_SOURCE_DIR}/LICENSE" "${kit}/licenses/raylib.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy "${PS2X_ANDROID_FFMPEG_ROOT}/COPYING.LGPLv2.1" "${kit}/licenses/FFmpeg-LGPL-2.1.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy "${PS2X_ANDROID_FFMPEG_ROOT}/BUILD-FFMPEG.txt" "${kit}/licenses/BUILD-FFMPEG.txt"
    DEPENDS ps2_recomp sotc_android_tool sotc_android_base ps2_runtime ps2_iop raylib
    VERBATIM
)
