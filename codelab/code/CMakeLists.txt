# SPDX-License-Identifier: Apache-2.0
#
# Standalone build for the codelab's exercises.
#
# This file is deliberately NOT included by the repository's top-level
# CMakeLists.txt: the codelab must configure and build on its own, with nothing
# but a C11 compiler, so a reader can copy this directory somewhere else and it
# still works.
#
#   cmake -B /tmp/codelab-build -S codelab/code
#   cmake --build /tmp/codelab-build
#   ctest --test-dir /tmp/codelab-build --output-on-failure
#
# Every program here runs on the PC. The chapters that need QNX (the PCI calls,
# the interrupt thread, the resource manager) are shown as listings in the
# chapter text, and the parts of them that can be separated from the OS live
# here as portable modules with tests.
cmake_minimum_required(VERSION 3.16)

project(passerelle-codelab
    VERSION 1.0.0
    DESCRIPTION "Exercises for 'Writing a virtio driver for QNX 8.0'"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(CLAB_WERROR "Treat warnings as errors" ON)
option(CLAB_SANITIZERS "Build with ASan+UBSan" OFF)

enable_testing()

# The same flag set the main project uses, so a reader who goes on to read
# virtio/ finds the same warnings waiting for them.
set(CLAB_FLAGS -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual
               -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wconversion
               -Wsign-conversion)

function(clab_flags tgt)
    target_compile_options(${tgt} PRIVATE ${CLAB_FLAGS})
    if(CLAB_WERROR)
        target_compile_options(${tgt} PRIVATE -Werror)
    endif()
    if(CLAB_SANITIZERS)
        target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined
                                              -fno-sanitize-recover=all)
        target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# The portable modules the exercises are built on.
add_library(clab STATIC
    src/clab_irq.c
    src/clab_neg.c
    src/clab_p9.c
    src/clab_pci.c
    src/clab_vring.c
)
target_include_directories(clab PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
clab_flags(clab)

# One executable per chapter. Names match the chapter numbers so a reader can
# jump straight from the text to the code.
set(CLAB_EXERCISES
    ex01_env
    ex02_pci_caps
    ex03_negotiate
    ex04_vring
    ex05_irq
    ex06_first_byte
)

foreach(ex IN LISTS CLAB_EXERCISES)
    add_executable(${ex} src/${ex}.c)
    target_link_libraries(${ex} PRIVATE clab)
    clab_flags(${ex})
    add_test(NAME ${ex} COMMAND ${ex})
endforeach()

# The solution to every chapter's exercise, as a test.
add_executable(test_clab tests/test_clab.c)
target_link_libraries(test_clab PRIVATE clab)
clab_flags(test_clab)
add_test(NAME test_clab COMMAND test_clab)
