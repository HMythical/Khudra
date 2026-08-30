# Runs one golden-file integration test.
#
#   cmake -DKHUDRA=<path> -DSOURCE=<file.khu> -DEXPECTED=<file.expected>
#         [-DEXPECTED_ERR=<file.expected-err>] [-DBACKEND=native]
#         -P run_golden.cmake
#
# The program must exit 0 and its stdout must match EXPECTED byte for byte.
# When EXPECTED_ERR names a file that exists, its **stderr** has to match that
# one the same way -- the invariant covers both channels, so a golden that
# writes to khuStdErr pins both. A program with nothing to say on stderr simply
# has no such file.
# BACKEND=native runs the same program through the native backend instead of
# the interpreter; the goldens are shared, which is the point.
if(NOT DEFINED KHUDRA OR NOT DEFINED SOURCE OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR "run_golden.cmake needs KHUDRA, SOURCE and EXPECTED")
endif()

set(run_arguments run "${SOURCE}")
if(DEFINED BACKEND AND BACKEND STREQUAL "native")
    set(run_arguments run --native "${SOURCE}")
endif()

execute_process(
    COMMAND "${KHUDRA}" ${run_arguments}
    OUTPUT_VARIABLE actual_output
    ERROR_VARIABLE actual_errors
    RESULT_VARIABLE exit_code
)

if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR
        "khudra ${run_arguments} exited ${exit_code}\n"
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

if(DEFINED EXPECTED_ERR AND EXISTS "${EXPECTED_ERR}")
    file(READ "${EXPECTED_ERR}" expected_errors)
    if(NOT actual_errors STREQUAL expected_errors)
        message(FATAL_ERROR
            "stderr of ${SOURCE} does not match ${EXPECTED_ERR}\n"
            "--- expected ---\n${expected_errors}"
            "--- actual ---\n${actual_errors}")
    endif()
endif()
