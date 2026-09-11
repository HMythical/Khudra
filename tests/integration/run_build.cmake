# Builds one program into a standalone native executable and runs it.
#
#   cmake -DKHUDRA=<path> -DSOURCE=<file.khu> -DEXPECTED=<file.expected>
#         [-DEXPECTED_ERR=<file.expected-err>] -DOUTPUT=<binary>
#         [-DFLAGS="..."] -P run_build.cmake
#
# This is the ahead-of-time proof: the artifact is a program in its own right,
# so it is executed directly, with no toolchain in the picture, and has to match
# the same golden the interpreter matches.
# FLAGS holds toolchain flags to insert before the source -- `--allow-kernel`
# for the kernel goldens (PLAN.md, section 9.6).
if(NOT DEFINED KHUDRA OR NOT DEFINED SOURCE OR NOT DEFINED EXPECTED OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "run_build.cmake needs KHUDRA, SOURCE, EXPECTED and OUTPUT")
endif()

if(NOT DEFINED FLAGS)
    set(FLAGS "")
endif()

separate_arguments(flags_list UNIX_COMMAND "${FLAGS}")

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

execute_process(
    COMMAND "${KHUDRA}" build ${flags_list} "${SOURCE}" -o "${OUTPUT}"
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_errors
    RESULT_VARIABLE build_code
)

if(NOT build_code EQUAL 0)
    message(FATAL_ERROR
        "khudra build ${SOURCE} exited ${build_code}\n"
        "--- stderr ---\n${build_errors}"
        "--- stdout ---\n${build_output}")
endif()

# `khudra build` writes `<output>.exe` on Windows and `<output>` everywhere
# else, so the artifact to run is whichever of the two is on disk. Resolving it
# here keeps the harness one script rather than two.
set(binary "${OUTPUT}")
if(NOT EXISTS "${binary}" AND EXISTS "${OUTPUT}.exe")
    set(binary "${OUTPUT}.exe")
endif()

execute_process(
    COMMAND "${binary}"
    OUTPUT_VARIABLE actual_output
    ERROR_VARIABLE actual_errors
    RESULT_VARIABLE exit_code
)

if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR
        "the built program for ${SOURCE} exited ${exit_code}\n"
        "--- stderr ---\n${actual_errors}"
        "--- stdout ---\n${actual_output}")
endif()

file(READ "${EXPECTED}" expected_output)

if(NOT actual_output STREQUAL expected_output)
    message(FATAL_ERROR
        "output of the built ${SOURCE} does not match ${EXPECTED}\n"
        "--- expected ---\n${expected_output}"
        "--- actual ---\n${actual_output}")
endif()

if(DEFINED EXPECTED_ERR AND EXISTS "${EXPECTED_ERR}")
    file(READ "${EXPECTED_ERR}" expected_errors)
    if(NOT actual_errors STREQUAL expected_errors)
        message(FATAL_ERROR
            "stderr of the built ${SOURCE} does not match ${EXPECTED_ERR}\n"
            "--- expected ---\n${expected_errors}"
            "--- actual ---\n${actual_errors}")
    endif()
endif()
