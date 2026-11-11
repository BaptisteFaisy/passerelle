# SPDX-License-Identifier: Apache-2.0
#
# rmfs - layer 3, the protocol-neutral filesystem resource manager.
#
# The core builds and tests on its own (FS-01..FS-15, QNX-04, QNX-05, SEC-01):
# the protocol adapters are only added when their module is part of the build,
# so a half-finished protocol cannot break the layer-3 tests.

set(RMFS_SRCS
    src/rmfs.c
    src/rmfs_calls.c
    src/rmfs_ops_io.c
    src/rmfs_ops_meta.c
    src/path.c
    src/open_table.c
    src/attr_cache.c
    src/lock.c
    src/sched.c
)

pgw_library(pgw_rmfs
    SRCS ${RMFS_SRCS}
    DEPS pgw_common
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Adapters: only compiled against the real protocol libraries. They translate
# pgw_fs_backend_ops into pgw_p9_* / pgw_fuse_* session calls.
if(TARGET pgw_proto9p)
    target_sources(pgw_rmfs PRIVATE adapters/9p_adapter.c)
    target_link_libraries(pgw_rmfs PUBLIC pgw_proto9p)
endif()

if(TARGET pgw_protofuse)
    target_sources(pgw_rmfs PRIVATE adapters/fuse_adapter.c)
    target_link_libraries(pgw_rmfs PUBLIC pgw_protofuse)
endif()

if(BUILD_TESTING)
    # The fake backend implements pgw_fs_backend_ops over a real temporary
    # directory, so the tests exercise the layer without any protocol.
    set(RMFS_TEST_SUPPORT tests/fake_backend.c tests/fake_backend_ops.c)
    set(RMFS_TEST_MAIN tests/test_main.c)

    pgw_test(test_rmfs_path
        SRCS tests/test_rmfs_path.c ${RMFS_TEST_SUPPORT} ${RMFS_TEST_MAIN}
        DEPS pgw_rmfs)
    pgw_test(test_rmfs_open
        SRCS tests/test_rmfs_open.c ${RMFS_TEST_SUPPORT} ${RMFS_TEST_MAIN}
        DEPS pgw_rmfs)
    pgw_test(test_rmfs_meta
        SRCS tests/test_rmfs_meta.c ${RMFS_TEST_SUPPORT} ${RMFS_TEST_MAIN}
        DEPS pgw_rmfs)
    pgw_test(test_rmfs_perms
        SRCS tests/test_rmfs_perms.c ${RMFS_TEST_SUPPORT} ${RMFS_TEST_MAIN}
        DEPS pgw_rmfs)
    pgw_test(test_rmfs_fd
        SRCS tests/test_rmfs_fd.c ${RMFS_TEST_SUPPORT} ${RMFS_TEST_MAIN}
        DEPS pgw_rmfs)
    pgw_test(test_rmfs_cache
        SRCS tests/test_rmfs_cache.c ${RMFS_TEST_SUPPORT} ${RMFS_TEST_MAIN}
        DEPS pgw_rmfs)
    pgw_test(test_rmfs_sched
        SRCS tests/test_rmfs_sched.c ${RMFS_TEST_SUPPORT} ${RMFS_TEST_MAIN}
        DEPS pgw_rmfs)
    pgw_test(test_rmfs_locks
        SRCS tests/test_rmfs_locks.c ${RMFS_TEST_SUPPORT} ${RMFS_TEST_MAIN}
        DEPS pgw_rmfs)
endif()
