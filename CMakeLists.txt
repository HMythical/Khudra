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
# khu_vm -- interpreter, object model, GC, manual arenas (Phase 3 onward)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE KHU_VM_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/vm/*.cpp")
if(KHU_VM_SOURCES)
    add_library(khu_vm STATIC ${KHU_VM_SOURCES})
    target_include_directories(khu_vm PUBLIC "${CMAKE_SOURCE_DIR}/src/vm"
                                              "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(khu_vm PUBLIC khu_frontend khu_runtime_c)
    set(KHU_VM_TARGET khu_vm)
else()
    set(KHU_VM_TARGET "")
endif()

# ---------------------------------------------------------------------------
# khudra -- CLI driver
# ---------------------------------------------------------------------------
add_executable(khudra src/main/khu_main.cpp)
target_link_libraries(khudra PRIVATE khu_frontend khu_runtime_c ${KHU_VM_TARGET})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

file(GLOB_RECURSE KHU_UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/unit/*.cpp")
add_executable(khudra_unit_tests tests/test_main.cpp ${KHU_UNIT_TEST_SOURCES})
target_include_directories(khudra_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
# Tests read fixtures (example.khu, tests/integration/) from the source tree.
target_compile_definitions(khudra_unit_tests PRIVATE KHU_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(khudra_unit_tests PRIVATE khu_frontend khu_runtime_c ${KHU_VM_TARGET})

add_test(NAME unit COMMAND khudra_unit_tests)

# The CLI must report a real diagnostic (with a file:line) for stages that are
# not implemented yet -- Phase 0 acceptance.
add_test(NAME cli_help COMMAND khudra help)

# Golden-file integration tests: each tests/integration/<name>.khu is run and
# its stdout compared to <name>.expected.
file(GLOB KHU_GOLDEN_TESTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/integration/*.khu")
foreach(golden ${KHU_GOLDEN_TESTS})
    get_filename_component(golden_name "${golden}" NAME_WE)
    get_filename_component(golden_dir "${golden}" DIRECTORY)
    add_test(
        NAME golden_${golden_name}
        COMMAND ${CMAKE_COMMAND}
            -DKHUDRA=$<TARGET_FILE:khudra>
            -DSOURCE=${golden}
            -DEXPECTED=${golden_dir}/${golden_name}.expected
            -P ${CMAKE_SOURCE_DIR}/tests/integration/run_golden.cmake)
endforeach()

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
                -P ${CMAKE_SOURCE_DIR}/tests/integration/run_golden.cmake)
    endif()
endforeach()
