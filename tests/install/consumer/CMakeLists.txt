# SPDX-License-Identifier: Apache-2.0
#
# tests/install/consumer - a downstream project that knows nothing about the
# Passerelle source tree: it only finds the installed package.
#
# It is configured with -DCMAKE_PREFIX_PATH=<prefix> and must find the
# package with find_package(Passerelle). It is never built as part of the
# Passerelle build: the install check configures it on its own, in a
# temporary directory.

cmake_minimum_required(VERSION 3.16)

project(pgw_consumer LANGUAGES C)

find_package(Passerelle REQUIRED)

if(NOT DEFINED Passerelle_VERSION)
    message(FATAL_ERROR "find_package(Passerelle) did not set Passerelle_VERSION")
endif()
message(STATUS "consumer: Passerelle_VERSION=${Passerelle_VERSION}")

add_executable(pgw_consumer consumer.c)
target_link_libraries(pgw_consumer PRIVATE Passerelle::pgw_common)
