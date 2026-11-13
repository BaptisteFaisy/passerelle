# SPDX-License-Identifier: Apache-2.0
#
# drivers/cmake/qnx-x86_64.cmake - CMake toolchain file for a QNX 8.0 guest on
# x86_64 (the `microvm` machine of QEMU).
#
# Usage:
#   cmake -B build-qnx -DCMAKE_TOOLCHAIN_FILE=drivers/cmake/qnx-x86_64.cmake \
#         -DPGW_MODULES="common;virtio;proto9p;protofuse;rmfs;drivers"
#
# Same as qnx-aarch64.cmake, with the x86_64 target: the cross compiler is
# `nto-x86_64-gcc` (the raw gcc of the SDP), because qcc's default target is
# aarch64le. The other SDP tools (mkqnximage, secpolcompile) are the same.

set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(DEFINED ENV{QNX_TARGET})
    set(QNX_TARGET "$ENV{QNX_TARGET}" CACHE PATH "SDP target directory")
else()
    set(QNX_TARGET "/opt/qnx800/host/linux/x86_64/target/qnx" CACHE PATH "SDP target directory")
endif()

if(DEFINED ENV{QNX_HOST})
    set(QNX_HOST "$ENV{QNX_HOST}" CACHE PATH "SDP host directory")
else()
    set(QNX_HOST "/opt/qnx800/host/linux/x86_64" CACHE PATH "SDP host directory")
endif()

set(QNX_SDP_BIN "${QNX_HOST}/bin" CACHE PATH "SDP host tools")

set(CMAKE_C_COMPILER qcc CACHE FILEPATH "QNX C compiler")
set(CMAKE_CXX_COMPILER q++ CACHE FILEPATH "QNX C++ compiler")
set(CMAKE_ASM_COMPILER qcc CACHE FILEPATH "QNX assembler")

# The linker and the archiver are the SDP's too; qcc drives the linker. The
# SDP 8.0 has no `qar`: the archiver is named after the target.
set(CMAKE_LINKER qcc CACHE FILEPATH "QNX linker")
set(CMAKE_AR ntox86_64-ar CACHE FILEPATH "QNX archiver")
set(CMAKE_RANLIB ntox86_64-gcc-ranlib CACHE FILEPATH "QNX ranlib")

set(CMAKE_SYSROOT "${QNX_TARGET}")

set(CMAKE_FIND_ROOT_PATH "${QNX_TARGET}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The host tools of the SDP 8.0 are not all in the same place: the compiler
# and secpolcompile live in $QNX_HOST/usr/bin, while mkqnximage lives in the
# shared host/common/bin (the environment script puts both on PATH). The paths
# are looked up, not guessed, so the toolchain works wherever the SDP sits.
find_program(PGW_QNX_MKQNXIMAGE NAMES mkqnximage
             HINTS "${QNX_HOST}/usr/bin" "${QNX_HOST}/../common/bin" "${QNX_SDP_BIN}")
find_program(PGW_QNX_SECPOLCOMPILE NAMES secpolcompile
             HINTS "${QNX_HOST}/usr/bin" "${QNX_HOST}/../common/bin" "${QNX_SDP_BIN}")
find_program(PGW_QNX_SECPOLPUSH NAMES secpolpush
             HINTS "${QNX_HOST}/usr/bin" "${QNX_HOST}/../common/bin" "${QNX_SDP_BIN}")
set(PGW_MKQNXIMAGE "${PGW_QNX_MKQNXIMAGE}" CACHE FILEPATH "mkqnximage")
set(PGW_SECPOLCOMPILE "${PGW_QNX_SECPOLCOMPILE}" CACHE FILEPATH "secpolcompile")
set(PGW_SECPOLPUSH "${PGW_QNX_SECPOLPUSH}" CACHE FILEPATH "secpolpush")

# qcc compiles in strict ISO mode here (the portable flags set -std=c11 and
# feature-test macros), which hides the QNX extensions the driver uses
# (PROT_NOCACHE, MAP_PHYS, mmap_device_io, mem_offset64). _QNX_SOURCE is the
# documented way to expose them; it implies all the __EXT_* families.
set(CMAKE_C_FLAGS_INIT "-D_QNX_SOURCE=1")

message(STATUS "QNX target: ${QNX_TARGET}")
message(STATUS "QNX host tools: ${QNX_SDP_BIN}")
