# SPDX-License-Identifier: Apache-2.0
#
# drivers/cmake/qnx-aarch64.cmake - CMake toolchain file for a QNX 8.0 guest
# on aarch64le (the `virt` machine of QEMU).
#
# Usage:
#   cmake -B build-qnx -DCMAKE_TOOLCHAIN_FILE=drivers/cmake/qnx-aarch64.cmake \
#         -DPGW_MODULES="common;virtio;proto9p;protofuse;rmfs;drivers"
#
# The compiler is `qcc`, the QNX 8.0 cross compiler; it is a wrapper around the
# gcc of the SDP, so the usual CMake variables apply. The two variables below
# are the ones that matter:
#
#   QNX_TARGET   where the SDP keeps the target's headers and libraries;
#   QNX_HOST     where the SDP keeps the host tools (mkqnximage, secpolcompile).
#
# Both default to the values of the SDP 8.0 installation layout; override them
# in the environment if the installation is elsewhere.

set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_PROCESSOR aarch64le)

# The SDP 8.0 layout: the target tree is under $QNX_HOST/target/qnx and the
# host tools under $QNX_HOST/host.
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

# qcc is the SDP's cross compiler: it sets the target, the sysroot and the
# runtime by itself, so no --sysroot is needed.
set(CMAKE_C_COMPILER qcc CACHE FILEPATH "QNX C compiler")
set(CMAKE_CXX_COMPILER q++ CACHE FILEPATH "QNX C++ compiler")
set(CMAKE_ASM_COMPILER qcc CACHE FILEPATH "QNX assembler")

# The linker and the archiver are the SDP's too; qcc drives them. The SDP 8.0
# has no `qar`: the archiver is named after the target.
set(CMAKE_LINKER qcc CACHE FILEPATH "QNX linker")
set(CMAKE_AR ntoaarch64-ar CACHE FILEPATH "QNX archiver")
set(CMAKE_RANLIB ntoaarch64-gcc-ranlib CACHE FILEPATH "QNX ranlib")

# The C library and the headers come from the target tree.
set(CMAKE_SYSROOT "${QNX_TARGET}")

set(CMAKE_FIND_ROOT_PATH "${QNX_TARGET}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The image is built by mkqnximage, which is a host tool of the SDP.
set(PGW_MKQNXIMAGE "${QNX_SDP_BIN}/mkqnximage" CACHE FILEPATH "mkqnximage")
set(PGW_SECPOLCOMPILE "${QNX_SDP_BIN}/secpolcompile" CACHE FILEPATH "secpolcompile")
set(PGW_SECPOLPUSH "${QNX_SDP_BIN}/secpolpush" CACHE FILEPATH "secpolpush")

# qcc's default target is gcc_ntox86_64, so the aarch64 build must select the
# aarch64le variant explicitly. qcc also defines __QNX__ and __QNXNTO__ itself
# (adding them again would be a redefinition, and the build treats warnings as
# errors), so the only initial flag here is the variant selection.
set(CMAKE_C_FLAGS_INIT "-Vgcc_ntoaarch64le -D_QNX_SOURCE=1")

message(STATUS "QNX target: ${QNX_TARGET}")
message(STATUS "QNX host tools: ${QNX_SDP_BIN}")
