# SPDX-License-Identifier: Apache-2.0
#
# Passerelle - top level build.
#
# Modules are added only when their own CMakeLists.txt exists, so the tree
# builds at any stage of development and parallel work on separate modules
# never has to touch this file.
cmake_minimum_required(VERSION 3.16)

# The version has a single source of truth: the VERSION file at the root.
# It is read before project() so that the project version, the pkg-config
# file and the exported CMake package all carry the same string (the copies
# are checked by tools/release/test_version_sync.py).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PGW_VERSION_RAW)
string(STRIP "${PGW_VERSION_RAW}" PGW_VERSION)
if(NOT PGW_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR
        "VERSION file: '${PGW_VERSION}' is not a MAJOR.MINOR.PATCH version")
endif()

project(passerelle
    VERSION ${PGW_VERSION}
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

# Install and export (gap 37): the libraries, the public headers, the
# pkg-config file and the exported CMake package. PGW_INSTALL is ON by
# default and never changes the build itself.
include(passerelle-install)
if(PGW_INSTALL)
    pgw_install_package()
endif()

# Project rules (AGENTS.md): every source file under 500 lines, every function
# under 50 lines. Enforced as a test so `ctest` and CI both catch a violation.
add_test(NAME size-guard
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check-size.py"
            --root "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(size-guard PROPERTIES LABELS "rules")
