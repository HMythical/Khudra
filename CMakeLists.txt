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
# khu_frontend -- diagnostics, lexer, parser, sema, codegen, bytecode
# ---------------------------------------------------------------------------
file(GLOB_RECURSE KHU_FRONTEND_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/lib/*.cpp")
add_library(khu_frontend STATIC ${KHU_FRONTEND_SOURCES})
target_include_directories(khu_frontend PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(khu_frontend PUBLIC khu_util)

# ---------------------------------------------------------------------------
# khu_vm -- interpreter, object model, GC, manual arenas (Phase 3 onward)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE KHU_VM_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/vm/*.cpp")
if(KHU_VM_SOURCES)
    add_library(khu_vm STATIC ${KHU_VM_SOURCES})
    target_include_directories(khu_vm PUBLIC "${CMAKE_SOURCE_DIR}/src/vm")
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
target_link_libraries(khudra_unit_tests PRIVATE khu_frontend khu_runtime_c ${KHU_VM_TARGET})

add_test(NAME unit COMMAND khudra_unit_tests)

# The CLI must report a real diagnostic (with a file:line) for stages that are
# not implemented yet -- Phase 0 acceptance.
add_test(NAME cli_help COMMAND khudra help)
