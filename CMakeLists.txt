# SPDX-License-Identifier: Apache-2.0
#
# Passerelle - top level build.
#
# Modules are added only when their own CMakeLists.txt exists, so the tree
# builds at any stage of development and parallel work on separate modules
# never has to touch this file.
cmake_minimum_required(VERSION 3.16)

project(passerelle
    VERSION 0.1.0
    DESCRIPTION "Shared folder for QNX 8.0 guests under QEMU: 9P2000.L and virtiofs"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(PGW_SANITIZERS "Build with ASan+UBSan (ROB-06)" OFF)
option(PGW_WERROR     "Treat warnings as errors"       ON)
option(PGW_QNX_STUBS  "Build QNX-target files against tests/qnx-stub" OFF)
option(PGW_FUZZ       "Build fuzz harnesses"           OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(passerelle)

include(CTest)
enable_testing()

# Order matters: common is the shared base of every other layer.
# PGW_MODULES restricts the set (used by per-module build directories so that
# work in progress in another module cannot break this one).
set(PGW_ALL_MODULES common virtio proto9p protofuse rmfs drivers tests)
set(PGW_MODULES "${PGW_ALL_MODULES}" CACHE STRING "Modules to add to the build")

foreach(mod IN LISTS PGW_MODULES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${mod}/CMakeLists.txt")
        add_subdirectory(${mod})
    else()
        message(STATUS "module '${mod}' not present yet - skipped")
    endif()
endforeach()

# Project rules (AGENTS.md): every source file under 500 lines, every function
# under 50 lines. Enforced as a test so `ctest` and CI both catch a violation.
add_test(NAME size-guard
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check-size.py"
            --root "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(size-guard PROPERTIES LABELS "rules")
