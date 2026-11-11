# SPDX-License-Identifier: Apache-2.0
#
# drivers/CMakeLists.txt - the QNX drivers.
#
# Two things are built here, and only here:
#
#   1. the real drivers, but only `if(QNX)`: the cross toolchain file (see
#      cmake/qnx-<arch>.cmake) names qcc, so the objects are QNX ones. On the PC
#      this part contributes nothing at all;
#   2. `pgw_qnx_syntax` targets, which compile the `*_qnx.c` files against
#      tests/qnx-stub/include so a broken QNX file is caught on the PC. They
#      build an OBJECT library: nothing is linked, so a missing QNX symbol can
#      never make the PC build fail.
#
# The portable part of the drivers (option parsing, the layer 3 backends) is
# compiled on the PC as `pgw_drivers_portable`, together with its tests.

include("${CMAKE_SOURCE_DIR}/cmake/passerelle.cmake")

set(PGW_DRIVER_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common")
set(PGW_SRC_ROOT "${CMAKE_SOURCE_DIR}")

# The include path every driver file needs: the project headers of the other
# layers and our own. The protocol layers are optional (other modules are
# written in parallel), so a missing one only removes the adapter that needs
# it, never the whole build.
set(PGW_DRIVER_INCLUDES
    "${PGW_DRIVER_COMMON_DIR}/include"
    "${PGW_SRC_ROOT}/common/include"
    "${PGW_SRC_ROOT}/rmfs/include"
    "${PGW_SRC_ROOT}/virtio/include"
    "${PGW_SRC_ROOT}/proto9p/include"
    "${PGW_SRC_ROOT}/protofuse/include")

# ---------------------------------------------------------------------------
# Portable half: option parsing and the layer 3 backends.
# ---------------------------------------------------------------------------

pgw_library(pgw_drivers_portable
    SRCS
        "${PGW_DRIVER_COMMON_DIR}/src/opts.c"
    DEPS pgw_common
    INCLUDES ${PGW_DRIVER_INCLUDES}
)

# The backends need the protocol libraries; they are optional at this point of
# the project (other modules are written in parallel), so each adapter is added
# only when its module is present.
if(TARGET pgw_proto9p)
    target_sources(pgw_drivers_portable PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/src/p9_backend.c")
    target_include_directories(pgw_drivers_portable PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/include")
    target_link_libraries(pgw_drivers_portable PUBLIC pgw_proto9p)
endif()

if(TARGET pgw_protofuse)
    target_sources(pgw_drivers_portable PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/src/fuse_backend.c")
    target_include_directories(pgw_drivers_portable PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/include")
    target_link_libraries(pgw_drivers_portable PUBLIC pgw_protofuse)
endif()

if(TARGET pgw_rmfs)
    target_link_libraries(pgw_drivers_portable PUBLIC pgw_rmfs)
    # The filesystem option defaults then come from rmfs itself, so there is
    # exactly one table (see drivers/common/src/opts.c).
    target_compile_definitions(pgw_drivers_portable PRIVATE PGW_WITH_RMFS=1)
endif()

# The option parser is portable and worth testing on the PC: the bounds it
# enforces are the ones SEC-01 and FS-13 rely on.
if(TARGET pgw_test)
    pgw_test(test_driver_opts
        SRCS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_opts.c"
        DEPS pgw_drivers_portable pgw_test
        LABELS "drivers")
endif()

# ---------------------------------------------------------------------------
# QNX-only half.
# ---------------------------------------------------------------------------

set(PGW_QNX_COMMON_SRCS
    "${PGW_DRIVER_COMMON_DIR}/src/attr_qnx.c"
    "${PGW_DRIVER_COMMON_DIR}/src/cred_qnx.c"
    "${PGW_DRIVER_COMMON_DIR}/src/ist_qnx.c"
    "${PGW_DRIVER_COMMON_DIR}/src/prio_qnx.c"
    "${PGW_DRIVER_COMMON_DIR}/src/privdrop_qnx.c"
    "${PGW_DRIVER_COMMON_DIR}/src/resmgr_connect_qnx.c"
    "${PGW_DRIVER_COMMON_DIR}/src/resmgr_dir_qnx.c"
    "${PGW_DRIVER_COMMON_DIR}/src/resmgr_io_qnx.c"
    "${PGW_DRIVER_COMMON_DIR}/src/resmgr_meta_qnx.c"
    "${PGW_DRIVER_COMMON_DIR}/src/resmgr_mount_qnx.c"
    "${PGW_DRIVER_COMMON_DIR}/src/srv_qnx.c"
    "${PGW_DRIVER_COMMON_DIR}/src/syslog_qnx.c"
    "${PGW_DRIVER_COMMON_DIR}/src/trace_qnx.c"
)

set(PGW_FS9P_QNX_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/src/dispatch_qnx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/src/irq_qnx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/src/mount_qnx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/src/privdrop_qnx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/src/syslog_qnx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/src/trace_qnx.c"
)

set(PGW_FSFUSE_QNX_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/src/dispatch_qnx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/src/irq_qnx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/src/mount_qnx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/src/privdrop_qnx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/src/syslog_qnx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/src/trace_qnx.c"
)

if(QNX)
    # The real build: the cross toolchain file provides qcc (or ntoaarch64-gcc
    # and friends), so these are QNX objects for QNX targets.
    add_executable(fs9p
        "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/src/main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/src/args.c"
        ${PGW_FS9P_QNX_SRCS}
        ${PGW_QNX_COMMON_SRCS})
    target_include_directories(fs9p PRIVATE
        ${PGW_DRIVER_INCLUDES}
        "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/include")
    target_link_libraries(fs9p PRIVATE pgw_drivers_portable)

    add_executable(fsfuse
        "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/src/main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/src/args.c"
        ${PGW_FSFUSE_QNX_SRCS}
        ${PGW_QNX_COMMON_SRCS})
    target_include_directories(fsfuse PRIVATE
        ${PGW_DRIVER_INCLUDES}
        "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/include")
    target_link_libraries(fsfuse PRIVATE pgw_drivers_portable)

    # The ABI check: on the target, the numbers the project quotes are compared
    # with the target's own headers (drivers/tests/qnx_abi_assert.c). A
    # mismatch is a compile error there, which is where it belongs.
    add_library(pgw_qnx_abi_assert OBJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/qnx_abi_assert.c")
    target_include_directories(pgw_qnx_abi_assert PRIVATE ${PGW_DRIVER_INCLUDES})
    pgw_apply_flags(pgw_qnx_abi_assert)
else()
    # The PC build: only a syntax check, never a link. The stub headers stand in
    # for the SDP ones, so a missing QNX symbol cannot break the PC build.
    pgw_qnx_syntax(pgw_qnx_syntax_common SRCS ${PGW_QNX_COMMON_SRCS})
    target_include_directories(pgw_qnx_syntax_common PRIVATE ${PGW_DRIVER_INCLUDES})

    pgw_qnx_syntax(pgw_qnx_syntax_fs9p
        SRCS ${PGW_FS9P_QNX_SRCS}
        DEPS pgw_qnx_syntax_common)
    target_include_directories(pgw_qnx_syntax_fs9p PRIVATE
        ${PGW_DRIVER_INCLUDES}
        "${CMAKE_CURRENT_SOURCE_DIR}/fs9p/include")

    pgw_qnx_syntax(pgw_qnx_syntax_fsfuse
        SRCS ${PGW_FSFUSE_QNX_SRCS}
        DEPS pgw_qnx_syntax_common)
    target_include_directories(pgw_qnx_syntax_fsfuse PRIVATE
        ${PGW_DRIVER_INCLUDES}
        "${CMAKE_CURRENT_SOURCE_DIR}/fsfuse/include")
endif()
