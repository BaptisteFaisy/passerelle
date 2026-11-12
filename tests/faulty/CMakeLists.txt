# SPDX-License-Identifier: Apache-2.0
#
# tests/faulty/ - a deliberately faulty 9P2000.L server and the client bench
# that survives it (ROB-07, P9-09, ROB-06, gap 20 of docs/gap-analysis.md).
#
# faulty_server.py reuses the reference server of tests/ref9p/ (it imports it)
# and breaks one protocol rule per run. The C bench reuses the ref9p harness
# rather than writing a fourth process/socket helper: the same source file is
# compiled here with PGW_REF9P_SERVER pointing at the faulty server, so the
# spawn, the ready-pipe handshake, the framing and the kill-on-exit logic exist
# exactly once in the tree.

get_filename_component(PGW_FAULTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
get_filename_component(PGW_REF9P_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ref9p"
                       ABSOLUTE)

pgw_test(faulty_9p_client
    SRCS test_faulty.c
         ${PGW_REF9P_DIR}/ref9p_harness.c
         ${PGW_REF9P_DIR}/ref9p_truth.c
    DEPS pgw_proto9p
    LABELS "faulty;robustness")

target_include_directories(faulty_9p_client PRIVATE ${PGW_REF9P_DIR})
# The harness finds python3 and the server script next to itself, so the bench
# runs from any build directory and needs no PATH guess.
target_compile_definitions(faulty_9p_client PRIVATE
    PGW_REF9P_PYTHON="${Python3_EXECUTABLE}"
    PGW_REF9P_SERVER="${PGW_FAULTY_DIR}/faulty_server.py")

# Fourteen servers are started, one per armed fault; two of them are made to
# stall on purpose, which the client bounds with its own 300 ms timeout.
set_tests_properties(faulty_9p_client PROPERTIES TIMEOUT 120)
