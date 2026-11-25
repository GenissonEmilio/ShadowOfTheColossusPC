# This directory is created solely for regenerable synthetic test output.
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${GENERATOR}" --signed-branch-fixture "${WORK}"
    RESULT_VARIABLE generated OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT generated EQUAL 0)
    message(FATAL_ERROR "Signed-branch fixture generation failed (${generated}):\n${output}\n${error}")
endif()
execute_process(COMMAND "${CXX}" -std=c++20 -O1 -msse4.1 -fno-strict-aliasing
    -DPS2X_STRICT_RETURN_DIAGNOSTICS=0 "-I${RUNTIME_INCLUDE}" "-I${IOP_INCLUDE}"
    "${WORK}/signed_branch_fixture.cpp" -o "${WORK}/signed_branch_fixture"
    RESULT_VARIABLE compiled OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT compiled EQUAL 0)
    message(FATAL_ERROR "Generated signed-branch code did not compile (${compiled}):\n${output}\n${error}")
endif()
execute_process(COMMAND "${WORK}/signed_branch_fixture"
    RESULT_VARIABLE executed OUTPUT_VARIABLE output ERROR_VARIABLE error)
message(STATUS "${output}")
if(NOT executed EQUAL 0)
    message(FATAL_ERROR "Generated signed-branch execution failed (${executed}):\n${output}\n${error}")
endif()
# Reproduce the old emitter's four narrow signed-branch expressions in the
# emitted source. Require the concrete material regression to fail, rather than
# accepting an unrelated compiler error or a crash as a useful negative control.
file(READ "${WORK}/signed_branch_fixture.cpp" baseline)
string(REPLACE "GPR_S64(ctx, 21)" "GPR_S32(ctx, 21)" baseline "${baseline}")
string(REPLACE "GPR_S64(ctx, 31)" "GPR_S32(ctx, 31)" baseline "${baseline}")
file(WRITE "${WORK}/signed_branch_before.cpp" "${baseline}")
execute_process(COMMAND "${CXX}" -std=c++20 -O1 -msse4.1 -fno-strict-aliasing
    -DPS2X_STRICT_RETURN_DIAGNOSTICS=0 "-I${RUNTIME_INCLUDE}" "-I${IOP_INCLUDE}"
    "${WORK}/signed_branch_before.cpp" -o "${WORK}/signed_branch_before"
    RESULT_VARIABLE compiled OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT compiled EQUAL 0)
    message(FATAL_ERROR "Pre-fix negative control did not compile (${compiled}):\n${output}\n${error}")
endif()
execute_process(COMMAND "${WORK}/signed_branch_before"
    RESULT_VARIABLE executed OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT executed EQUAL 1 OR NOT output MATCHES "FAIL material attribute=a00001000 expected=680000000 result=0")
    message(FATAL_ERROR "Pre-fix negative control did not reproduce the material regression (${executed}):\n${output}\n${error}")
endif()
message(STATUS "Pre-fix signed32 negative control reproduced the material regression as expected.")
# Preserve CTest's execution receipt, remove only these four named outputs.
file(REMOVE "${WORK}/signed_branch_fixture.cpp" "${WORK}/signed_branch_fixture"
    "${WORK}/signed_branch_before.cpp" "${WORK}/signed_branch_before")
