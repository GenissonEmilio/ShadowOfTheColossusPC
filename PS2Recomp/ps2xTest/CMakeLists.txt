cmake_minimum_required(VERSION 3.21)

project(ps2xTest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(gs_cache)
endif()

# Static library with test logic (no main), used by ps2xStudio
add_library(ps2_test_function_table OBJECT
    src/test_function_table.cpp
)

target_include_directories(ps2_test_function_table PRIVATE
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
    ${CMAKE_SOURCE_DIR}/ps2xIOP/include
)

add_library(ps2_test_lib STATIC
    src/code_generator_tests.cpp
    src/r5900_decoder_tests.cpp
    src/elf_analyzer_tests.cpp
    src/pad_input_tests.cpp
    src/ps2_runtime_io_tests.cpp
    src/ps2_runtime_kernel_tests.cpp
    src/ps2_runtime_interrupt_tests.cpp
    src/ps2_memory_tests.cpp
    src/ps2_vu1_tests.cpp
    src/ps2_vu_tests.cpp
    src/ps2_gs_tests.cpp
    src/ps2_iop_tests.cpp
    src/ps2_sif_rpc_tests.cpp
    src/ps2_sif_dma_tests.cpp
    src/ps2_recompiler_tests.cpp
    src/ps2_runtime_expansion_tests.cpp
)

target_include_directories(ps2_test_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/ps2xRecomp/include
    ${CMAKE_SOURCE_DIR}/ps2xAnalyzer/include
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib
)

target_link_libraries(ps2_test_lib PRIVATE
    ps2_recomp_lib
    ps2_analyzer_lib
    ps2_runtime
    ps2_iop
)

if(UNIX AND NOT APPLE)
    target_link_libraries(ps2_test_lib PRIVATE ${CMAKE_DL_LIBS})
endif()

add_executable(ps2x_tests
    src/main.cpp
    $<TARGET_OBJECTS:ps2_test_function_table>
)

option(PRINT_GENERATED_CODE "Print generated code in tests" OFF)
if(PRINT_GENERATED_CODE)

    target_compile_definitions(ps2x_tests PRIVATE PRINT_GENERATED_CODE)
    target_compile_definitions(ps2_test_lib PRIVATE PRINT_GENERATED_CODE)
endif()

target_include_directories(ps2x_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/ps2xRecomp/include
    ${CMAKE_SOURCE_DIR}/ps2xAnalyzer/include
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
)

target_link_libraries(ps2x_tests PRIVATE
    ps2_test_lib
    ps2_recomp_lib
    ps2_analyzer_lib
    ps2_runtime
)

# Execute actual compiler output against the real context/register helpers.
# This narrow runner never starts the graphics/runtime test suites or needs ROMs.
if(BUILD_TESTING AND UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_test(NAME signed_branch_codegen_execution
        COMMAND ${CMAKE_COMMAND}
            "-DGENERATOR=$<TARGET_FILE:ps2x_tests>"
            "-DCXX=${CMAKE_CXX_COMPILER}"
            "-DRUNTIME_INCLUDE=${CMAKE_SOURCE_DIR}/ps2xRuntime/include"
            "-DIOP_INCLUDE=${CMAKE_SOURCE_DIR}/ps2xIOP/include"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/signed-branch-fixture"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/signed_branch_codegen_execution.cmake")
    set_tests_properties(signed_branch_codegen_execution PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()

# VU/GS tests intentionally instantiate large interpreter state.
# Windows executables default to a much smaller stack than Linux, and
# sanitizer/debug instrumentation can push these test frames over 1 MiB.
if(MSVC)
    target_link_options(ps2x_tests PRIVATE "/STACK:8388608")
elseif(MINGW)
    target_link_options(ps2x_tests PRIVATE "--stack,8388608")
endif()

if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(ps2x_tests)
endif()

include("${CMAKE_SOURCE_DIR}/ps2xRuntime/cmake/ReleaseMode.cmake")

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_test_lib)
    EnableFastReleaseMode(ps2x_tests)
endif()
