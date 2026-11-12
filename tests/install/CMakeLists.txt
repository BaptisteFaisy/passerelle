# SPDX-License-Identifier: Apache-2.0
#
# tests/install - the out-of-tree install check (gap 38).
#
# The install rules of the top level CMakeLists.txt are proven here: the tree
# is installed into a throwaway prefix, then a downstream consumer is built
# from that prefix, once with the pkg-config flags and once with
# find_package(Passerelle).
#
# This directory brings its own CMakeLists.txt, so it is registered
# automatically by tests/CMakeLists.txt: adding the check costs this
# directory and no edit of any shared file.

add_test(NAME install-check
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/install_check.py"
            --source-dir "${CMAKE_SOURCE_DIR}"
            --build-dir "${CMAKE_BINARY_DIR}"
            --consumer-dir "${CMAKE_CURRENT_SOURCE_DIR}/consumer")
set_tests_properties(install-check PROPERTIES
    TIMEOUT 240
    LABELS "install")
