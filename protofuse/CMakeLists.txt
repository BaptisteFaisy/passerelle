# SPDX-License-Identifier: Apache-2.0
#
# protofuse/CMakeLists.txt - Layer 2, FUSE protocol carried by virtio (virtiofs).
#
# Requirements VFS-02, VFS-03, VFS-04, VFS-05. Everything here is portable C11:
# the only way out of the module is the pgw_fuse_rpc_fn callback, so the whole
# protocol runs on the PC against an in-memory server (tests/fake_virtiofsd.c).

pgw_library(pgw_protofuse
    SRCS
        src/fusep_close.c
        src/fusep_decode.c
        src/fusep_decode2.c
        src/fusep_encode.c
        src/fusep_encode2.c
        src/fusep_init.c
        src/fusep_io.c
        src/fusep_node.c
        src/fusep_ops.c
        src/fusep_ops2.c
        src/fusep_refs.c
        src/fusep_reply.c
        src/fusep_session.c
    DEPS pgw_common
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# The internal headers (fusep_abi.h, fusep_internal.h, fusep_session.h) stay
# private to the library; only the tests are allowed to see them.
target_include_directories(pgw_protofuse PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(BUILD_TESTING)
    pgw_test(protofuse_codec
        SRCS tests/test_codec.c tests/test_codec_reply.c
        DEPS pgw_protofuse
    )
    # The fake virtiofsd is split in three files: transport, operations and the
    # simulated tree, so that no test file grows past the size rules.
    set(PGW_FAKE_SRCS
        tests/fake_virtiofsd.c
        tests/fake_virtiofsd_io.c
        tests/fake_virtiofsd_ops.c
        tests/fake_virtiofsd_tree.c
    )
    pgw_test(protofuse_session
        SRCS tests/test_session.c tests/test_session_errors.c ${PGW_FAKE_SRCS}
        DEPS pgw_protofuse
    )
    pgw_test(protofuse_refs
        SRCS tests/test_refs.c ${PGW_FAKE_SRCS}
        DEPS pgw_protofuse
    )
    foreach(t protofuse_codec protofuse_refs protofuse_session)
        target_include_directories(${t} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
        )
    endforeach()
endif()
