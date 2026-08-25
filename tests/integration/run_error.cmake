# Runs one expected-failure integration test.
#
#   cmake -DKHUDRA=<path> -DSOURCE=<file.khu> -DEXPECTED=<file.expected-error>
#         -P run_error.cmake
#
# The program must be rejected, and every non-empty line of EXPECTED must appear
# somewhere in the diagnostics. Lines rather than a whole-output comparison, so
# a test pins the wording that matters without freezing the surrounding layout.
if(NOT DEFINED KHUDRA OR NOT DEFINED SOURCE OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR "run_error.cmake needs KHUDRA, SOURCE and EXPECTED")
endif()

execute_process(
    COMMAND "${KHUDRA}" check "${SOURCE}"
    OUTPUT_VARIABLE actual_output
    ERROR_VARIABLE actual_errors
    RESULT_VARIABLE exit_code
)

if(exit_code EQUAL 0)
    message(FATAL_ERROR "${SOURCE} was accepted, but it should have been rejected")
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
            "diagnostics for ${SOURCE} do not mention:\n  ${trimmed}\n"
            "--- actual ---\n${actual_errors}")
    endif()
endforeach()
