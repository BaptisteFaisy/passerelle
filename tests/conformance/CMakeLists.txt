# SPDX-License-Identifier: Apache-2.0
#
# tests/conformance/ - diod's protocol description, turned into an
# executable table.
#
# Gap 60 asked for a cross-check against diod's description of the 9P2000.L
# protocol. This directory holds it:
#
#   diod-protocol-excerpt.md  the excerpt of diod's protocol.md every rule
#                             is cited from (the upstream URL and the commit
#                             are in its header);
#   conformance_table.[ch]    the rules: id, section, statement, and the
#                             named case that proves each of them;
#   test_conformance_table.c  the table checks itself: a rule without a
#                             citation, a case nobody cites, is a failure;
#   test_conformance_wire*.c the rules that are statements about bytes,
#                             checked at the offsets of the description with
#                             the independent accessors of tests/ref9p/;
#   test_conformance_sequence.c the rules that are statements about a
#                             sequence, driven through the production client
#                             of proto9p/ against the reference server;
#   test_conformance_diod.c   the same rules against diod itself, when the
#                             package is installed (it skips with a reason
#                             when it is not).
#
# Nothing here re-implements a server: the harness and the truth helpers of
# tests/ref9p/ and tests/interop/ are reused as they are.
#
# The directory is registered automatically by tests/CMakeLists.txt: it brings
# its own CMakeLists.txt, so no other file has to be edited.

get_filename_component(CONFORMANCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

# The excerpt the rules are cited from, and the Python the reference server
# needs. The definitions of tests/ref9p/ are directory-scoped, so they are
# repeated here for the sources this directory compiles.
add_compile_definitions(
    "PGW_CONFORMANCE_EXCERPT=\"${CONFORMANCE_DIR}/diod-protocol-excerpt.md\""
    "PGW_REF9P_SERVER=\"${CMAKE_SOURCE_DIR}/tests/ref9p/ref9p_server.py\""
    "PGW_REF9P_PYTHON=\"${Python3_EXECUTABLE}\""
)

# The table itself and the rules that hold everywhere: the bytes of the
# description and the sequences of the reference server.
pgw_test(test_conformance
    SRCS test_main.c test_conformance_table.c test_conformance_wire.c
         test_conformance_wire_ops.c test_conformance_sequence.c
         conformance_table.c
         ${CMAKE_CURRENT_SOURCE_DIR}/../ref9p/ref9p_fields.c
         ${CMAKE_CURRENT_SOURCE_DIR}/../ref9p/ref9p_harness.c
         ${CMAKE_CURRENT_SOURCE_DIR}/../ref9p/ref9p_truth.c
    DEPS pgw_proto9p
    LABELS "conformance;diod")

target_include_directories(test_conformance PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ref9p)

# The rules that need the real server. The name carries "interop" so that
# the job of .github/workflows/interop.yml, which installs diod, runs it.
pgw_test(test_conformance_interop_diod
    SRCS test_conformance_diod.c conformance_table.c
         ${CMAKE_CURRENT_SOURCE_DIR}/../interop/interop_diod.c
         ${CMAKE_CURRENT_SOURCE_DIR}/../interop/interop_transport.c
         ${CMAKE_CURRENT_SOURCE_DIR}/../ref9p/ref9p_fields.c
         ${CMAKE_CURRENT_SOURCE_DIR}/../ref9p/ref9p_harness.c
         ${CMAKE_CURRENT_SOURCE_DIR}/../ref9p/ref9p_truth.c
    DEPS pgw_proto9p
    LABELS "conformance;diod;interop")

target_include_directories(test_conformance_interop_diod PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../interop
    ${CMAKE_CURRENT_SOURCE_DIR}/../ref9p)

# A missing diod is reported as a skip, with the reason in the log: an
# absent dependency is never a pass.
set_tests_properties(test_conformance_interop_diod PROPERTIES
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "PGW_SKIP:")
