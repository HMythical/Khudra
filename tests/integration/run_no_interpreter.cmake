# The ahead-of-time smoke test: a built binary must carry no interpreter.
#
#   cmake -DKHUDRA=<path> -DSOURCE=<file.khu> -DOUTPUT=<binary>
#         -DNM=<nm> -P run_no_interpreter.cmake
#
# `khudra build` links the memory systems and the image reader, and nothing from
# src/vm/vm.cpp. If a khu::vm::Vm symbol ever appears in the artifact, the
# native path has quietly grown a dependency on the thing it replaces.
if(NOT DEFINED KHUDRA OR NOT DEFINED SOURCE OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "run_no_interpreter.cmake needs KHUDRA, SOURCE and OUTPUT")
endif()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

execute_process(
    COMMAND "${KHUDRA}" build "${SOURCE}" -o "${OUTPUT}"
    OUTPUT_VARIABLE build_output ERROR_VARIABLE build_errors RESULT_VARIABLE build_code)
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

if(NOT DEFINED NM OR NM STREQUAL "NM-NOTFOUND")
    message(STATUS "nm is not available; skipping the interpreter-absence check")
    return()
endif()

execute_process(
    COMMAND "${NM}" -C "${binary}"
    OUTPUT_VARIABLE symbols ERROR_VARIABLE symbol_errors RESULT_VARIABLE symbol_code)
if(NOT symbol_code EQUAL 0)
    message(STATUS "nm could not read ${binary}; skipping the interpreter-absence check")
    return()
endif()

string(FIND "${symbols}" "khu::vm::Vm::" found)
if(NOT found EQUAL -1)
    message(FATAL_ERROR
        "the binary built from ${SOURCE} links the interpreter; `khudra build` is supposed to "
        "produce a native program with no VM in it")
endif()
