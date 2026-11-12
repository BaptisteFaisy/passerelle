# SPDX-License-Identifier: Apache-2.0
#
# tests/fuse_ref/ - a reference FUSE backend, and the client tests that use it.
#
# The reference server (fuse_ref_server.py) speaks the FUSE kernel wire
# protocol over a UNIX socket, framed with a 4-byte big-endian length per
# message, and serves a real directory. The tests here run the Passerelle FUSE
# client (protofuse/) against it, so VFS-01..VFS-05 are no longer checked only
# against the in-memory fake server of protofuse/tests/.
#
# Nothing outside this directory is touched: the coordinator wires these three
# targets into the root build through PGW_MODULES.

set(PGW_FUSE_REF_SERVER_PY "${CMAKE_CURRENT_SOURCE_DIR}/fuse_ref_server.py")

# Transport (fork/exec + socket + pgw_fuse_rpc_fn) and the disk-side oracle are
# shared by every test file here, so they are written exactly once.
set(PGW_FUSE_REF_HARNESS
    fuse_ref_harness.c
    fuse_ref_truth.c
)

pgw_test(fuse_ref_client
    SRCS test_fuse_ref.c ${PGW_FUSE_REF_HARNESS}
    DEPS pgw_protofuse
    LABELS "fuse;reference"
)
pgw_test(fuse_ref_namespace
    SRCS test_fuse_ref_ns.c ${PGW_FUSE_REF_HARNESS}
    DEPS pgw_protofuse
    LABELS "fuse;reference"
)
pgw_test(fuse_ref_robust
    SRCS test_fuse_ref_robust.c ${PGW_FUSE_REF_HARNESS}
    DEPS pgw_protofuse
    LABELS "fuse;reference;robustness"
)

foreach(t fuse_ref_client fuse_ref_namespace fuse_ref_robust)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # The test execs the interpreter CMake found and the server next to itself,
    # so it works from any build directory and needs no PATH guess.
    target_compile_definitions(${t} PRIVATE
        PGW_PYTHON3="${Python3_EXECUTABLE}"
        PGW_FUSE_REF_SERVER="${PGW_FUSE_REF_SERVER_PY}")
    # Each case starts one server; the whole binary stays well under a minute.
    set_tests_properties(${t} PROPERTIES TIMEOUT 60)
endforeach()
