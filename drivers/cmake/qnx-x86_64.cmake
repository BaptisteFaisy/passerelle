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

set(CMAKE_C_COMPILER nto-x86_64-gcc CACHE FILEPATH "QNX C compiler")
set(CMAKE_CXX_COMPILER nto-x86_64-g++ CACHE FILEPATH "QNX C++ compiler")
set(CMAKE_ASM_COMPILER nto-x86_64-as CACHE FILEPATH "QNX assembler")
set(CMAKE_LINKER nto-x86_64-ld CACHE FILEPATH "QNX linker")
set(CMAKE_AR nto-x86_64-ar CACHE FILEPATH "QNX archiver")
set(CMAKE_RANLIB nto-x86_64-ranlib CACHE FILEPATH "QNX ranlib")

set(CMAKE_SYSROOT "${QNX_TARGET}")

set(CMAKE_FIND_ROOT_PATH "${QNX_TARGET}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(PGW_MKQNXIMAGE "${QNX_SDP_BIN}/mkqnximage" CACHE FILEPATH "mkqnximage")
set(PGW_SECPOLCOMPILE "${QNX_SDP_BIN}/secpolcompile" CACHE FILEPATH "secpolcompile")
set(PGW_SECPOLPUSH "${QNX_SDP_BIN}/secpolpush" CACHE FILEPATH "secpolpush")

set(CMAKE_C_FLAGS_INIT "-D__QNX__ -D__QNXNTO__")

message(STATUS "QNX target: ${QNX_TARGET}")
message(STATUS "QNX host tools: ${QNX_SDP_BIN}")
