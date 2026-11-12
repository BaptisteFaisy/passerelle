# SPDX-License-Identifier: Apache-2.0
#
# tests/ref9p/ - the reference 9P2000.L server and its conformance benches.
#
# The client of proto9p/ used to be tested only against an in-memory fake
# server, which made P9-01..P9-10 self-referential. Here the C benches start
# ref9p_server.py - a second, independent implementation of 9P2000.L - as a
# child process and talk to it over a TCP socket with a 4-byte big-endian
# length prefix, against a real directory whose contents give an independent
# truth for every assertion.
#
# Python3 is found by the socle (cmake/passerelle.cmake already calls
# find_package(Python3)); here we only bake its path and the server path into
# the benches so they run from any build directory, not just the one CMake
# happens to use as a working directory. A hand-built compile falls back to
# environment variables and relative paths (see ref9p_harness.c).

get_filename_component(REF9P_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

add_compile_definitions(
    "PGW_REF9P_SERVER=\"${REF9P_DIR}/ref9p_server.py\""
    "PGW_REF9P_PYTHON=\"${Python3_EXECUTABLE}\""
)

# The functional bench: full path, error codes and the fault-injection modes
# (P9-02, P9-03, P9-04, P9-07, P9-08, P9-09, P9-10).
pgw_test(test_ref9p
    SRCS test_main.c test_ref9p.c ref9p_harness.c ref9p_truth.c
    DEPS pgw_proto9p
    LABELS "ref9p;conformance")

# The wire-format bench: our encoders and decoders read at the offsets of the
# normative 9P2000.L message table (P9-01, P9-02, P9-03, P9-09, P9-10).
pgw_test(test_ref9p_fields
    SRCS test_main.c test_ref9p_fields.c test_ref9p_encode.c
         ref9p_fields.c ref9p_harness.c ref9p_truth.c
    DEPS pgw_proto9p
    LABELS "ref9p;conformance")