# CTest platform.version (platform/CMakeLists.txt, ADR 0024): runs PROGRAM --version and
# fails unless it exits 0 and prints exactly EXPECTED and a newline to stdout.
#
#   cmake -DPROGRAM=<MapleThief> -DEXPECTED="MapleThief <version>" -P version_test.cmake

execute_process(
    COMMAND "${PROGRAM}" --version
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${PROGRAM} --version exited with ${result}: ${error}")
endif()
if(NOT output STREQUAL "${EXPECTED}\n")
    message(FATAL_ERROR "${PROGRAM} --version printed '${output}', expected '${EXPECTED}'")
endif()
message(STATUS "${PROGRAM} --version: ${EXPECTED}")
