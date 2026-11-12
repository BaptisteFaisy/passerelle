# SPDX-License-Identifier: Apache-2.0
#
# tests/prop/ - property benches on the codec boundaries (gap 61 of
# docs/gap-analysis.md; requirements P9-09, VFS-03, ROB-06).
#
# prop.h/prop.c are the harness: a seeded generator, per-iteration
# reproducibility, shrinking of a failing case to a minimal one and a bounded
# iteration count. hypothesis is deliberately *not* a dependency: the harness
# is the guarantee, and it runs on every runner with nothing but a C compiler.
#
# Two benches, one per protocol layer, each fast enough for the pull-request
# gate (a few seconds). The long campaigns are registered DISABLED with an
# explicit TIMEOUT and the "long" label, so `make bench` and the out-of-PR
# workflow can run them without touching the 5-minute CI budget.
#
# The FUSE bench reuses protofuse/tests/codec_util.h to build replies with the
# same cursors a server writes with, so the wire layout exists once in the tree.

pgw_library(pgw_prop
    SRCS prop.c
    DEPS pgw_test
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

get_filename_component(PGW_PROTOFUSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../protofuse"
                       ABSOLUTE)

pgw_test(prop_p9
    SRCS prop_p9.c prop_p9_dec.c prop_p9_hostile.c
    DEPS pgw_prop pgw_proto9p
    LABELS "prop;codec")

pgw_test(prop_fuse
    SRCS prop_fuse.c prop_fuse_dec.c
    DEPS pgw_prop pgw_protofuse
    LABELS "prop;codec")

# codec_util.h lives with the protofuse tests and quotes the wire constants of
# the library, so both include directories are needed (same pattern as
# tests/faulty, which compiles the ref9p harness from its sibling directory).
target_include_directories(prop_fuse PRIVATE
    ${PGW_PROTOFUSE_DIR}/tests
    ${PGW_PROTOFUSE_DIR}/src)

# The long campaigns: 2 000 000 generated cases per encoder/decoder family.
# Out of the pull-request gate on purpose (CI budget 5 min).
add_test(NAME prop-p9-long COMMAND prop_p9)
set_tests_properties(prop-p9-long PROPERTIES
    DISABLED TRUE
    LABELS "long;prop"
    TIMEOUT 3600
    ENVIRONMENT "PGW_PROP_ITERS=2000000")

add_test(NAME prop-fuse-long COMMAND prop_fuse)
set_tests_properties(prop-fuse-long PROPERTIES
    DISABLED TRUE
    LABELS "long;prop"
    TIMEOUT 3600
    ENVIRONMENT "PGW_PROP_ITERS=2000000")
