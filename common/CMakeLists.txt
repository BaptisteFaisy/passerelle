# SPDX-License-Identifier: Apache-2.0
add_library(pgw_common STATIC
    src/buf_reader.c
    src/buf_writer.c
    src/err.c
    src/flags.c
    src/log.c
    src/time.c
)
target_include_directories(pgw_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(pgw_common PUBLIC c_std_11)

add_library(pgw_test STATIC
    src/test.c
)
target_link_libraries(pgw_test PUBLIC pgw_common)

if(BUILD_TESTING)
    add_executable(test_common tests/test_common.c)
    target_link_libraries(test_common PRIVATE pgw_test)
    add_test(NAME common COMMAND test_common)
endif()
