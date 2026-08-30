# Runs one program under every backend and asserts nothing observable differs.
#
#   cmake -DKHUDRA=<path> -DSOURCE=<file.khu> -DOUTPUT=<binary>
#         -P run_differential.cmake
#
# No .expected file: the interpreter is the reference. stdout, stderr and the
# exit code all have to agree, which is the invariant in docs/native.md, 5 --
# and unlike a golden it holds for programs that trap, too.
if(NOT DEFINED KHUDRA OR NOT DEFINED SOURCE OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "run_differential.cmake needs KHUDRA, SOURCE and OUTPUT")
endif()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

execute_process(
    COMMAND "${KHUDRA}" run "${SOURCE}"
    OUTPUT_VARIABLE vm_output ERROR_VARIABLE vm_errors RESULT_VARIABLE vm_code)

execute_process(
    COMMAND "${KHUDRA}" run --native "${SOURCE}"
    OUTPUT_VARIABLE jit_output ERROR_VARIABLE jit_errors RESULT_VARIABLE jit_code)

function(compare_backend name output errors code)
    if(NOT output STREQUAL vm_output)
        message(FATAL_ERROR
            "${name} stdout differs from the VM for ${SOURCE}\n"
            "--- vm ---\n${vm_output}"
            "--- ${name} ---\n${output}")
    endif()
    if(NOT errors STREQUAL vm_errors)
        message(FATAL_ERROR
            "${name} stderr differs from the VM for ${SOURCE}\n"
            "--- vm ---\n${vm_errors}"
            "--- ${name} ---\n${errors}")
    endif()
    if(NOT code STREQUAL vm_code)
        message(FATAL_ERROR
            "${name} exited ${code} but the VM exited ${vm_code} for ${SOURCE}")
    endif()
endfunction()

compare_backend("run --native" "${jit_output}" "${jit_errors}" "${jit_code}")

execute_process(
    COMMAND "${KHUDRA}" build "${SOURCE}" -o "${OUTPUT}"
    OUTPUT_VARIABLE build_output ERROR_VARIABLE build_errors RESULT_VARIABLE build_code)
if(NOT build_code EQUAL 0)
    message(FATAL_ERROR
        "khudra build ${SOURCE} exited ${build_code}\n"
        "--- stderr ---\n${build_errors}"
        "--- stdout ---\n${build_output}")
endif()

execute_process(
    COMMAND "${OUTPUT}"
    OUTPUT_VARIABLE aot_output ERROR_VARIABLE aot_errors RESULT_VARIABLE aot_code)

compare_backend("build" "${aot_output}" "${aot_errors}" "${aot_code}")
