# SPDX-License-Identifier: Apache-2.0
#
# cmake/passerelle.cmake - shared build helpers.
#
# Every module declares its library and its tests through these two functions so
# that warning flags, sanitizers and include paths are identical everywhere and
# are written exactly once (no duplication between modules).

find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(pgw_apply_flags tgt)
    target_compile_options(${tgt} PRIVATE
        -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes
        -Wmissing-prototypes -Wvla -Wconversion -Wsign-conversion
        -fno-omit-frame-pointer)
    if(PGW_WERROR)
        target_compile_options(${tgt} PRIVATE -Werror)
    endif()
    if(PGW_SANITIZERS)
        target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined
                                              -fno-sanitize-recover=all)
        target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# pgw_library(<name> SRCS a.c b.c [DEPS lib...] [INCLUDES dir...])
function(pgw_library name)
    cmake_parse_arguments(ARG "" "" "SRCS;DEPS;INCLUDES" ${ARGN})
    if(NOT ARG_SRCS)
        message(FATAL_ERROR "pgw_library(${name}): SRCS is required")
    endif()
    add_library(${name} STATIC ${ARG_SRCS})
    target_compile_features(${name} PUBLIC c_std_11)
    foreach(inc IN LISTS ARG_INCLUDES)
        target_include_directories(${name} PUBLIC ${inc})
    endforeach()
    if(ARG_DEPS)
        target_link_libraries(${name} PUBLIC ${ARG_DEPS})
    endif()
    pgw_apply_flags(${name})
endfunction()

# pgw_test(<name> SRCS t.c [DEPS lib...]) - registers a CTest case.
function(pgw_test name)
    cmake_parse_arguments(ARG "" "" "SRCS;DEPS;LABELS" ${ARGN})
    add_executable(${name} ${ARG_SRCS})
    target_link_libraries(${name} PRIVATE pgw_test ${ARG_DEPS})
    pgw_apply_flags(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
    if(ARG_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABELS}")
    endif()
endfunction()

# pgw_qnx_syntax(<name> SRCS a.c ...) - compile QNX-target files on the PC
# against tests/qnx-stub, so a broken driver is caught before an SDP build.
function(pgw_qnx_syntax name)
    cmake_parse_arguments(ARG "" "" "SRCS;DEPS" ${ARGN})
    add_library(${name} OBJECT ${ARG_SRCS})
    target_compile_features(${name} PRIVATE c_std_11)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/qnx-stub/include")
    target_compile_definitions(${name} PRIVATE PGW_QNX_STUB_BUILD=1)
    if(ARG_DEPS)
        target_link_libraries(${name} PRIVATE ${ARG_DEPS})
    endif()
    pgw_apply_flags(${name})
endfunction()
