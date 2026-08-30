# Runs one expected-runtime-failure integration test.
#
#   cmake -DKHUDRA=<path> -DSOURCE=<file.khu> -DEXPECTED=<file.expected-error>
#         [-DRUN_ARGS=run --native] -P run_runtime_error.cmake
#
# Unlike run_error.cmake, which rejects a program during `check`, this one
# exercises a runtime trap: the program must compile and run* but must fail at
# runtime (nonzero exit), and every non-empty line of EXPECTED must appear
# somewhere in the diagnostics. Lines rather than a whole-output comparison, so
# a test pins the wording that matters without freezing the surrounding layout.
# RUN_ARGS defaults to `run`; pass `run --native` to drive the native backend.
if(NOT DEFINED KHUDRA OR NOT DEFINED SOURCE OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR "run_runtime_error.cmake needs KHUDRA, SOURCE and EXPECTED")
endif()

if(NOT DEFINED RUN_ARGS)
    set(RUN_ARGS run)
endif()

separate_arguments(run_args_list UNIX_COMMAND "${RUN_ARGS}")

execute_process(
    COMMAND "${KHUDRA}" ${run_args_list} "${SOURCE}"
    OUTPUT_VARIABLE actual_output
    ERROR_VARIABLE actual_errors
    RESULT_VARIABLE exit_code
)

if(exit_code EQUAL 0)
    message(FATAL_ERROR "${SOURCE} ran to completion, but it should have trapped")
endif()

file(STRINGS "${EXPECTED}" expected_lines)
foreach(line ${expected_lines})
    string(STRIP "${line}" trimmed)
    if(trimmed STREQUAL "")
        continue()
    endif()
    string(FIND "${actual_errors}" "${trimmed}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR
            "runtime diagnostics for ${SOURCE} do not mention:\n  ${trimmed}\n"
            "--- actual ---\n${actual_errors}")
    endif()
endforeach()
