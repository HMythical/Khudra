# Runs one golden-file integration test.
#
#   cmake -DKHUDRA=<path> -DSOURCE=<file.khu> -DEXPECTED=<file.expected>
#         -P run_golden.cmake
#
# The program must exit 0 and its stdout must match EXPECTED byte for byte.
if(NOT DEFINED KHUDRA OR NOT DEFINED SOURCE OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR "run_golden.cmake needs KHUDRA, SOURCE and EXPECTED")
endif()

execute_process(
    COMMAND "${KHUDRA}" run "${SOURCE}"
    OUTPUT_VARIABLE actual_output
    ERROR_VARIABLE actual_errors
    RESULT_VARIABLE exit_code
)

if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR
        "khudra run ${SOURCE} exited ${exit_code}\n"
        "--- stderr ---\n${actual_errors}"
        "--- stdout ---\n${actual_output}")
endif()

file(READ "${EXPECTED}" expected_output)

if(NOT actual_output STREQUAL expected_output)
    message(FATAL_ERROR
        "output of ${SOURCE} does not match ${EXPECTED}\n"
        "--- expected ---\n${expected_output}"
        "--- actual ---\n${actual_output}")
endif()
