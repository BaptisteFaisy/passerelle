# SPDX-License-Identifier: Apache-2.0
#
# tests/fuse_e2e/ - the conformance bench and the directed cases through the
# FUSE adapter (gap 25 of docs/gap-analysis.md).
#
# The FUSE adapter of rmfs was built, but no bench had ever run through it:
# the differential bench of tests/e2e only knew the POSIX truth backend, and
# the directed cases of tests/cas only knew the POSIX backend and the 9P
# adapter. This directory closes that hole without copying anything:
#
#   fuse_mount.c       the mount: the reference FUSE server of tests/fuse_ref
#                      serves a real directory (the truth), a session is
#                      attached and rmfs is built over the FUSE adapter;
#   test_fuse_diff.c   the differential bench of tests/e2e, with the mount
#                      provider of diff.h installed: the same suites, the
#                      same verification, the FUSE adapter underneath;
#   test_fuse_cases.c  the directed cases of tests/cas (cas_cases.h), the
#                      same bodies over the FUSE adapter.
#
# Nothing outside this directory is edited: the coordinator registers it
# because it brings its own CMakeLists.txt.

get_filename_component(FUSE_REF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../fuse_ref" ABSOLUTE)
get_filename_component(CAS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cas" ABSOLUTE)

set(PGW_FUSE_REF_SERVER_PY "${FUSE_REF_DIR}/fuse_ref_server.py")

# The mount: the reference server (transport and truth) and rmfs over it.
pgw_library(pgw_fuse_mount
    SRCS
        fuse_mount.c
        ../fuse_ref/fuse_ref_harness.c
        ../fuse_ref/fuse_ref_truth.c
    DEPS pgw_protofuse pgw_rmfs pgw_test
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" "${FUSE_REF_DIR}" "${CAS_DIR}"
            "${CMAKE_SOURCE_DIR}/tests/e2e"
            "${CMAKE_SOURCE_DIR}/tests/posix_backend"
            "${CMAKE_SOURCE_DIR}/rmfs/tests"
)

pgw_test(test_fuse_diff
    SRCS test_fuse_diff.c
    DEPS pgw_diff pgw_fuse_mount
    LABELS "fuse;conformance")

# The directed cases over the FUSE adapter: the bodies live in tests/cas, so
# the truth helpers of tests/ref9p come with them.
pgw_test(test_fuse_cases
    SRCS test_fuse_cases.c ../ref9p/ref9p_truth.c
    DEPS pgw_cas_cases pgw_fuse_mount
    LABELS "fuse;directed;conformance")

foreach(t test_fuse_diff test_fuse_cases)
    target_include_directories(${t} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CAS_DIR}"
        "${CMAKE_SOURCE_DIR}/rmfs/tests")
    # Each case starts one reference server; the binary stays well under the
    # CI budget.
    set_tests_properties(${t} PROPERTIES TIMEOUT 180)
endforeach()

# The harness (compiled into the library) execs the interpreter CMake found
# and the server next to itself, so it works from any build directory.
target_compile_definitions(pgw_fuse_mount PRIVATE
    PGW_PYTHON3="${Python3_EXECUTABLE}"
    PGW_FUSE_REF_SERVER="${PGW_FUSE_REF_SERVER_PY}")
