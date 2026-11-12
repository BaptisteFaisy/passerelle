# SPDX-License-Identifier: Apache-2.0
#
# tests/ - the conformance and randomized-input benches.
#
# Three things live here:
#   posix_backend/  a pgw_fs_backend_ops implementation over a real directory,
#                   i.e. the "truth" side of the differential bench;
#   e2e/            the differential suite (spec 7.2) and the directed cases
#                   (spec 7.4), both runnable on the PC without QNX;
#   fuzz/           randomized-input campaigns against the wire decoders
#                   (ROB-06, P9-09).
#
# Every test registered here is bounded to a few seconds so the CI stays inside
# its 5-minute budget. The long campaigns of spec 7.6 (1 000 000 differential
# operations, hours of fuzzing) are registered but DISABLED: they run from the
# out-of-PR workflow or by hand, never on a pull request.
#
# A subdirectory that brings its own CMakeLists.txt is registered automatically
# (see the end of this file): adding a bench costs one directory, never an
# edit of this file.

add_library(pgw_posix_backend STATIC
    posix_backend/backend_file.c
    posix_backend/backend_tree.c
)
target_include_directories(pgw_posix_backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/posix_backend)
target_link_libraries(pgw_posix_backend PUBLIC pgw_rmfs)
pgw_apply_flags(pgw_posix_backend)

add_library(pgw_diff STATIC
    e2e/diff_ops.c
    e2e/diff_verify.c
    e2e/diff_verify_tree.c
    e2e/testfs.c
)
target_include_directories(pgw_diff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/e2e)
target_link_libraries(pgw_diff PUBLIC pgw_posix_backend pgw_test)
pgw_apply_flags(pgw_diff)

# --- differential + directed conformance (spec 7.2, 7.4) -------------------
pgw_test(test_diff SRCS e2e/test_diff.c DEPS pgw_diff LABELS "e2e;conformance")
pgw_test(test_directed SRCS e2e/test_directed.c DEPS pgw_diff
         LABELS "e2e;conformance")

# The 1 000 000-operation campaign of spec 7.6. Disabled by default: it is a
# nightly / pre-submission run, never a pull-request gate (CI budget 5 min).
add_test(NAME e2e-long COMMAND test_diff)
set_tests_properties(e2e-long PROPERTIES
    DISABLED TRUE
    LABELS "long;conformance"
    TIMEOUT 3600
    ENVIRONMENT "PGW_DIFF_OPS=1000000")

# --- randomized input campaigns (ROB-06) -----------------------------------
pgw_test(fuzz_p9 SRCS fuzz/fuzz_p9.c DEPS pgw_proto9p LABELS "fuzz")
pgw_test(fuzz_fuse SRCS fuzz/fuzz_fuse.c DEPS pgw_protofuse LABELS "fuzz")

add_test(NAME fuzz-p9-long COMMAND fuzz_p9)
set_tests_properties(fuzz-p9-long PROPERTIES
    DISABLED TRUE
    LABELS "long;fuzz"
    TIMEOUT 3600
    ENVIRONMENT "PGW_FUZZ_ITERS=5000000")

add_test(NAME fuzz-fuse-long COMMAND fuzz_fuse)
set_tests_properties(fuzz-fuse-long PROPERTIES
    DISABLED TRUE
    LABELS "long;fuzz"
    TIMEOUT 3600
    ENVIRONMENT "PGW_FUZZ_ITERS=5000000")

# Coverage-guided mode, only when the toolchain provides libFuzzer. Never a
# requirement: the deterministic campaigns above run everywhere.
option(PGW_LIBFUZZER "Build the fuzz harnesses for -fsanitize=fuzzer" OFF)
if(PGW_LIBFUZZER)
    add_executable(fuzz_p9_lf fuzz/fuzz_p9.c)
    target_link_libraries(fuzz_p9_lf PRIVATE pgw_proto9p pgw_common)
    target_compile_definitions(fuzz_p9_lf PRIVATE PGW_LIBFUZZER=1)
    target_compile_options(fuzz_p9_lf PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_p9_lf PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# --- test modules: picked up automatically -------------------------------
# Every subdirectory that brings its own CMakeLists.txt is a test module and
# is registered here without this file being edited. A new bench (a faulty
# server, property tests, a vhost harness) then costs one directory and
# touches nothing else, so parallel work never collides on this file.
file(GLOB pgw_test_subdirs LIST_DIRECTORIES TRUE RELATIVE
     ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(subdir IN LISTS pgw_test_subdirs)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/CMakeLists.txt")
        add_subdirectory(${subdir})
    endif()
endforeach()
