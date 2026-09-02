cmake_minimum_required(VERSION 3.16)
project(khudra LANGUAGES C CXX)

# C++17 drives the front end, codegen and the VM core; the Procedure engine and
# runtime support in utils/ are C11 behind an extern "C" ABI (KHU-PLAN.md).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Address and undefined-behaviour sanitizers. The memory-systems tests are
# meant to be run under these (KHU-PLAN.md, Phase 5 acceptance):
#   cmake -S . -B build-asan -DKHU_SANITIZE=ON && ctest --test-dir build-asan
option(KHU_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(KHU_SANITIZE)
    if(MSVC)
        message(WARNING "KHU_SANITIZE is not supported with MSVC; ignoring")
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# ---------------------------------------------------------------------------
# khu_runtime_c -- C runtime: manual allocation, bool support, Procedure engine
# ---------------------------------------------------------------------------
file(GLOB KHU_RUNTIME_C_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/utils/*.c")
add_library(khu_runtime_c STATIC ${KHU_RUNTIME_C_SOURCES})
target_include_directories(khu_runtime_c PUBLIC "${CMAKE_SOURCE_DIR}/utils")
set_target_properties(khu_runtime_c PROPERTIES LINKER_LANGUAGE C)

# ---------------------------------------------------------------------------
# khu_util -- compiler-internal utilities (arrays, hashmap, arena, file, text)
# ---------------------------------------------------------------------------
file(GLOB KHU_UTIL_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/util/*.cpp")
add_library(khu_util STATIC ${KHU_UTIL_SOURCES})
target_include_directories(khu_util PUBLIC "${CMAKE_SOURCE_DIR}/src")

# ---------------------------------------------------------------------------
# Standard library -- lib/*.khu embedded into the toolchain
# ---------------------------------------------------------------------------
file(GLOB KHU_STDLIB_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/lib/*.khu")
list(SORT KHU_STDLIB_SOURCES)
set(KHU_STDLIB_GENERATED "${CMAKE_BINARY_DIR}/generated/khu_stdlib_sources.cpp")
add_custom_command(
    OUTPUT "${KHU_STDLIB_GENERATED}"
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${KHU_STDLIB_GENERATED}
        "-DSOURCES=${KHU_STDLIB_SOURCES}"
        -P "${CMAKE_SOURCE_DIR}/scripts/embed_stdlib.cmake"
    DEPENDS ${KHU_STDLIB_SOURCES} "${CMAKE_SOURCE_DIR}/scripts/embed_stdlib.cmake"
    COMMENT "Embedding the Khudra standard library"
    VERBATIM)

# ---------------------------------------------------------------------------
# khu_frontend -- diagnostics, lexer, parser, sema, codegen, bytecode
# ---------------------------------------------------------------------------
file(GLOB_RECURSE KHU_FRONTEND_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/lib/*.cpp")
add_library(khu_frontend STATIC ${KHU_FRONTEND_SOURCES} "${KHU_STDLIB_GENERATED}")
target_include_directories(khu_frontend PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(khu_frontend PUBLIC khu_util)

# ---------------------------------------------------------------------------
# khu_vm_core -- object model, heap, collector, manual arenas
#
# Split from the interpreter so the native backend can link the memory systems
# without linking a dispatcher it never enters: `khudra build` produces a binary
# whose link line has no interpreter on it at all (PLAN.md, Phase N-4).
# ---------------------------------------------------------------------------
file(GLOB_RECURSE KHU_VM_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/vm/*.cpp")
set(KHU_VM_INTERPRETER_SOURCES
    "${CMAKE_SOURCE_DIR}/src/vm/vm.cpp"
    "${CMAKE_SOURCE_DIR}/src/vm/proc_host.cpp")
set(KHU_VM_CORE_SOURCES ${KHU_VM_SOURCES})
list(REMOVE_ITEM KHU_VM_CORE_SOURCES ${KHU_VM_INTERPRETER_SOURCES})

if(KHU_VM_SOURCES)
    add_library(khu_vm_core STATIC ${KHU_VM_CORE_SOURCES})
    target_include_directories(khu_vm_core PUBLIC "${CMAKE_SOURCE_DIR}/src/vm"
                                                   "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(khu_vm_core PUBLIC khu_frontend khu_runtime_c)
    if(WIN32)
        # khuStdSystem's sockets are Winsock here (src/vm/platform.cpp), and
        # Winsock is not part of the default link on Windows the way the socket
        # calls are part of libc on Linux.
        target_link_libraries(khu_vm_core PUBLIC ws2_32)
    endif()

    add_library(khu_vm STATIC ${KHU_VM_INTERPRETER_SOURCES})
    target_link_libraries(khu_vm PUBLIC khu_vm_core)
    set(KHU_VM_TARGET khu_vm)
else()
    set(KHU_VM_TARGET "")
endif()

# ---------------------------------------------------------------------------
# Native backend -- raw memory execution (PLAN.md)
#
#   khu_native_cemit   bytecode::Module -> C translation unit
#   khu_native_rt      the runtime the emitted C links against: heap, collector,
#                      Procedure engine host, scanned-locals roots
#   khu_native_driver  the two wrappers: JIT (dlopen) and AOT (build)
#   khu_native_aot     main() for the standalone binaries `khudra build` writes
# ---------------------------------------------------------------------------

# The emitted C carries its own copy of the ABI header, so a translation unit
# builds without the Khudra source tree beside it.
set(KHU_NATIVE_ABI_HEADER "${CMAKE_SOURCE_DIR}/src/native/host/khu_native_abi.h")
set(KHU_NATIVE_ABI_GENERATED "${CMAKE_BINARY_DIR}/generated/khu_native_abi_source.cpp")
add_custom_command(
    OUTPUT "${KHU_NATIVE_ABI_GENERATED}"
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${KHU_NATIVE_ABI_GENERATED}
        -DHEADER=${KHU_NATIVE_ABI_HEADER}
        -P "${CMAKE_SOURCE_DIR}/scripts/embed_native_abi.cmake"
    DEPENDS "${KHU_NATIVE_ABI_HEADER}" "${CMAKE_SOURCE_DIR}/scripts/embed_native_abi.cmake"
    COMMENT "Embedding the native backend ABI"
    VERBATIM)

file(GLOB KHU_NATIVE_CEMIT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/native/cemit/*.cpp")
add_library(khu_native_cemit STATIC ${KHU_NATIVE_CEMIT_SOURCES} "${KHU_NATIVE_ABI_GENERATED}")
target_include_directories(khu_native_cemit PUBLIC "${CMAKE_SOURCE_DIR}/src/native")
target_link_libraries(khu_native_cemit PUBLIC khu_frontend)

add_library(khu_native_rt STATIC "${CMAKE_SOURCE_DIR}/src/native/host/native_host.cpp")
target_include_directories(khu_native_rt PUBLIC "${CMAKE_SOURCE_DIR}/src/native")
target_link_libraries(khu_native_rt PUBLIC khu_vm_core khu_runtime_c)

add_library(khu_native_aot STATIC "${CMAKE_SOURCE_DIR}/src/native/host/aot_main.cpp")
target_link_libraries(khu_native_aot PUBLIC khu_native_rt)

add_library(khu_native_driver STATIC
    "${CMAKE_SOURCE_DIR}/src/native/host/toolchain.cpp"
    "${CMAKE_SOURCE_DIR}/src/native/host/loader.cpp"
    "${CMAKE_SOURCE_DIR}/src/native/host/aot.cpp")
target_link_libraries(khu_native_driver PUBLIC khu_native_cemit khu_native_rt ${CMAKE_DL_LIBS})

# An ahead-of-time link needs the archives above. They live in the build tree;
# KHUDRA_NATIVE_LIB_DIR overrides that for an installed toolchain.
target_compile_definitions(khu_native_driver PRIVATE
    KHU_NATIVE_LIB_DIR="${CMAKE_BINARY_DIR}")
if(KHU_SANITIZE AND NOT MSVC)
    # A sanitized toolchain produces sanitized archives, so the binaries it
    # links have to agree.
    target_compile_definitions(khu_native_driver PRIVATE
        KHU_NATIVE_EXTRA_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g")
endif()

# ---------------------------------------------------------------------------
# khu_cli -- command-line surface, split out so the tests can parse arguments
# ---------------------------------------------------------------------------
add_library(khu_cli STATIC src/main/cli_options.cpp)
target_include_directories(khu_cli PUBLIC "${CMAKE_SOURCE_DIR}/src/main")

# ---------------------------------------------------------------------------
# khudra -- CLI driver
# ---------------------------------------------------------------------------
add_executable(khudra src/main/khu_main.cpp)
target_link_libraries(khudra PRIVATE khu_cli khu_native_driver khu_frontend khu_runtime_c
                      ${KHU_VM_TARGET})
# The lowered program is loaded into this process and calls back into it, so the
# host's khu_rt_* symbols have to be in the executable's dynamic table.
# ENABLE_EXPORTS is `-rdynamic` on POSIX, where the loaded object leaves those
# symbols dangling and the loader satisfies them from here.
set_target_properties(khudra PROPERTIES ENABLE_EXPORTS ON)
if(WIN32 AND NOT MSVC)
    # Windows has no dangling-symbol model: the emitted DLL *imports* khu_rt_*,
    # so this executable has to *export* them. MinGW's --export-all-symbols is
    # the whole of D3 (EXPANSION-PLAN.md, section 6); no cemit.cpp change is
    # needed on this path. MSVC would need a generated .def and an import
    # library instead, which is why it is the deferred D3b sub-phase.
    target_link_options(khudra PRIVATE "-Wl,--export-all-symbols")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

file(GLOB_RECURSE KHU_UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/unit/*.cpp")
add_executable(khudra_unit_tests tests/test_main.cpp ${KHU_UNIT_TEST_SOURCES})
target_include_directories(khudra_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
# Tests read fixtures (example.khu, tests/integration/) from the source tree.
target_compile_definitions(khudra_unit_tests PRIVATE KHU_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(khudra_unit_tests PRIVATE khu_cli khu_native_driver khu_frontend
                      khu_runtime_c ${KHU_VM_TARGET})
set_target_properties(khudra_unit_tests PROPERTIES ENABLE_EXPORTS ON)

add_test(NAME unit COMMAND khudra_unit_tests)

# The CLI must report a real diagnostic (with a file:line) for stages that are
# not implemented yet -- Phase 0 acceptance.
add_test(NAME cli_help COMMAND khudra help)

# Golden-file integration tests: each tests/integration/<name>.khu is run and
# its stdout compared to <name>.expected.
find_program(KHU_NM nm)
set(KHU_NATIVE_TEST_BIN "${CMAKE_BINARY_DIR}/native-tests")

# Registers the native variants of one golden program: the same .expected file
# run through `run --native` and through a binary written by `khudra build`,
# plus the differential that compares all three backends without a golden at all.
# `expected_err` names the file holding the program's expected stderr. It does
# not have to exist: a program with nothing to say on stderr has no such file,
# and the runners only compare the channel when one is there.
function(khu_add_native_tests name source expected expected_err)
    add_test(
        NAME golden_native_${name}
        COMMAND ${CMAKE_COMMAND}
            -DKHUDRA=$<TARGET_FILE:khudra>
            -DSOURCE=${source}
            -DEXPECTED=${expected}
            -DEXPECTED_ERR=${expected_err}
            -DBACKEND=native
            -P ${CMAKE_SOURCE_DIR}/tests/integration/run_golden.cmake)
    add_test(
        NAME build_${name}
        COMMAND ${CMAKE_COMMAND}
            -DKHUDRA=$<TARGET_FILE:khudra>
            -DSOURCE=${source}
            -DEXPECTED=${expected}
            -DEXPECTED_ERR=${expected_err}
            -DOUTPUT=${KHU_NATIVE_TEST_BIN}/build_${name}
            -P ${CMAKE_SOURCE_DIR}/tests/integration/run_build.cmake)
    add_test(
        NAME differential_${name}
        COMMAND ${CMAKE_COMMAND}
            -DKHUDRA=$<TARGET_FILE:khudra>
            -DSOURCE=${source}
            -DOUTPUT=${KHU_NATIVE_TEST_BIN}/differential_${name}
            -P ${CMAKE_SOURCE_DIR}/tests/integration/run_differential.cmake)
endfunction()

# Registers one golden program under all three backends. `prefix` keeps the
# test names of the subdirectories apart from the top-level ones.
function(khu_add_golden prefix golden)
    get_filename_component(golden_name "${golden}" NAME_WE)
    get_filename_component(golden_dir "${golden}" DIRECTORY)
    set(name "${prefix}${golden_name}")
    add_test(
        NAME golden_${name}
        COMMAND ${CMAKE_COMMAND}
            -DKHUDRA=$<TARGET_FILE:khudra>
            -DSOURCE=${golden}
            -DEXPECTED=${golden_dir}/${golden_name}.expected
            -DEXPECTED_ERR=${golden_dir}/${golden_name}.expected-err
            -P ${CMAKE_SOURCE_DIR}/tests/integration/run_golden.cmake)
    khu_add_native_tests(${name} ${golden}
        ${golden_dir}/${golden_name}.expected
        ${golden_dir}/${golden_name}.expected-err)
endfunction()

# The top-level goldens, plus the per-namespace standard-library goldens that
# PLAN.md puts under tests/integration/stdlib/. Both are picked up without
# editing this file.
file(GLOB KHU_GOLDEN_TESTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/integration/*.khu")
foreach(golden ${KHU_GOLDEN_TESTS})
    khu_add_golden("" "${golden}")
endforeach()

file(GLOB KHU_STDLIB_GOLDEN_TESTS CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/tests/integration/stdlib/*.khu")
foreach(golden ${KHU_STDLIB_GOLDEN_TESTS})
    khu_add_golden("stdlib_" "${golden}")
endforeach()

# khuStdSystem. These touch the file system, so they run where they can write:
# the build directory, which is where ctest puts a test's working directory
# anyway. Only success paths are golden -- an errno code and an errorMessage
# string are platform-specific, so neither ever appears in an .expected file
# (EXPANSION-PLAN.md, section 2.6).
file(GLOB KHU_SYSTEM_GOLDEN_TESTS CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/tests/integration/system/*.khu")

# `exit_code.khu` is the exception: it exits 7 on purpose, and run_golden.cmake
# requires a clean exit. The differential harness is the one that fits, because
# it compares the *exit code* across all three backends -- which is the whole
# claim khuStdSystem.exit makes.
set(KHU_SYSTEM_EXIT "${CMAKE_SOURCE_DIR}/tests/integration/system/exit_code.khu")
list(REMOVE_ITEM KHU_SYSTEM_GOLDEN_TESTS "${KHU_SYSTEM_EXIT}")

foreach(golden ${KHU_SYSTEM_GOLDEN_TESTS})
    khu_add_golden("system_" "${golden}")
endforeach()

add_test(
    NAME differential_system_exit_code
    COMMAND ${CMAKE_COMMAND}
        -DKHUDRA=$<TARGET_FILE:khudra>
        -DSOURCE=${KHU_SYSTEM_EXIT}
        -DOUTPUT=${KHU_NATIVE_TEST_BIN}/differential_system_exit_code
        -P ${CMAKE_SOURCE_DIR}/tests/integration/run_differential.cmake)

# The socket half of examples/http_server.khu, which no golden can cover: a
# real server, driven by a real curl, once per backend. The script exits 0 with
# a notice when curl is not installed, so a machine without it is not a failure.
add_test(
    NAME http_server_over_tcp
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/test_http_server.sh $<TARGET_FILE:khudra>)

# A built binary must be a native program, not a VM carrying one.
add_test(
    NAME native_binary_has_no_interpreter
    COMMAND ${CMAKE_COMMAND}
        -DKHUDRA=$<TARGET_FILE:khudra>
        -DSOURCE=${CMAKE_SOURCE_DIR}/examples/shapes.khu
        -DOUTPUT=${KHU_NATIVE_TEST_BIN}/no_interpreter
        -DNM=${KHU_NM}
        -P ${CMAKE_SOURCE_DIR}/tests/integration/run_no_interpreter.cmake)

# Expected-failure tests: each tests/integration/errors/<name>.khu must be
# rejected, mentioning every line of <name>.expected-error.
file(GLOB KHU_ERROR_TESTS CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/tests/integration/errors/*.khu")
foreach(bad ${KHU_ERROR_TESTS})
    get_filename_component(bad_name "${bad}" NAME_WE)
    get_filename_component(bad_dir "${bad}" DIRECTORY)
    add_test(
        NAME rejects_${bad_name}
        COMMAND ${CMAKE_COMMAND}
            -DKHUDRA=$<TARGET_FILE:khudra>
            -DSOURCE=${bad}
            -DEXPECTED=${bad_dir}/${bad_name}.expected-error
            -P ${CMAKE_SOURCE_DIR}/tests/integration/run_error.cmake)
endforeach()

# Runtime-failure tests: each tests/integration/runtime_errors/<name>.khu must
# compile but then trap at runtime on every backend, mentioning every line of
# <name>.expected-error on stderr. These cover faults that only show up while a
# program runs (a use-after-free, an out-of-range buffer access, ...), which the
# `check`-based run_error.cmake harness cannot see.
file(GLOB KHU_RUNTIME_ERROR_TESTS CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/tests/integration/runtime_errors/*.khu")
foreach(bad ${KHU_RUNTIME_ERROR_TESTS})
    get_filename_component(bad_name "${bad}" NAME_WE)
    get_filename_component(bad_dir "${bad}" DIRECTORY)
    add_test(
        NAME traps_${bad_name}
        COMMAND ${CMAKE_COMMAND}
            -DKHUDRA=$<TARGET_FILE:khudra>
            -DSOURCE=${bad}
            -DEXPECTED=${bad_dir}/${bad_name}.expected-error
            -DRUN_ARGS=run
            -P ${CMAKE_SOURCE_DIR}/tests/integration/run_runtime_error.cmake)
    add_test(
        NAME traps_native_${bad_name}
        COMMAND ${CMAKE_COMMAND}
            -DKHUDRA=$<TARGET_FILE:khudra>
            -DSOURCE=${bad}
            -DEXPECTED=${bad_dir}/${bad_name}.expected-error
            -DRUN_ARGS=run --native
            -P ${CMAKE_SOURCE_DIR}/tests/integration/run_runtime_error.cmake)
endforeach()

# Every sample program must pass semantic analysis, and -- where an expected
# output is recorded next to it -- produce exactly that output. The programs
# live in examples/ so they stay readable on their own; only the goldens live
# under tests/.
file(GLOB KHU_EXAMPLES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/examples/*.khu")
foreach(example ${KHU_EXAMPLES})
    get_filename_component(example_name "${example}" NAME_WE)
    add_test(NAME check_${example_name} COMMAND khudra check "${example}")

    set(example_golden "${CMAKE_SOURCE_DIR}/tests/integration/examples/${example_name}.expected")
    if(EXISTS "${example_golden}")
        add_test(
            NAME golden_example_${example_name}
            COMMAND ${CMAKE_COMMAND}
                -DKHUDRA=$<TARGET_FILE:khudra>
                -DSOURCE=${example}
                -DEXPECTED=${example_golden}
                -DEXPECTED_ERR=${CMAKE_SOURCE_DIR}/tests/integration/examples/${example_name}.expected-err
                -P ${CMAKE_SOURCE_DIR}/tests/integration/run_golden.cmake)
        khu_add_native_tests(example_${example_name} ${example} ${example_golden}
            "${CMAKE_SOURCE_DIR}/tests/integration/examples/${example_name}.expected-err")
    endif()
endforeach()
