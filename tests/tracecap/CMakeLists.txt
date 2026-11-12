# SPDX-License-Identifier: Apache-2.0
#
# tests/tracecap/ - capture a real 9P exchange and render it as a QEMU trace.
#
# The trace fixture of host/tests/fixtures/ was written by hand once and was
# never regenerated (gap 55), so a change of the protocol library could keep
# passing on stale data. This module captures what the production client of
# proto9p/ really exchanges with the independent reference server of
# tests/ref9p/, and renders it in the format the parser of host/bench/
# trace.py reads.
#
# Two CTest cases gate the drift:
#   tracecap-fixture         the committed fixture is what the code produces;
#   tracecap-deterministic   two captures are byte-identical.
#
# The harness of tests/ref9p/ is reused as it is - it is compiled here, not
# copied - so the capture talks to the same reference server as the
# conformance benches, with the same framing.

get_filename_component(TRACECAP_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/host/tests/fixtures"
                       ABSOLUTE)

# The harness looks for python3 and the server script; give it the same two
# paths the conformance benches of tests/ref9p/ use.
add_compile_definitions(
    "PGW_REF9P_SERVER=\"${CMAKE_SOURCE_DIR}/tests/ref9p/ref9p_server.py\""
    "PGW_REF9P_PYTHON=\"${Python3_EXECUTABLE}\"")

pgw_library(pgw_tracecap
    SRCS tracecap.c trace_render.c ${CMAKE_SOURCE_DIR}/tests/ref9p/ref9p_harness.c
    DEPS pgw_proto9p pgw_common
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/ref9p)

add_executable(tracecap tracecap_cli.c)
target_link_libraries(tracecap PRIVATE pgw_tracecap)
pgw_apply_flags(tracecap)

# The drift gate: the committed fixture must match what the code produces now.
add_test(NAME tracecap-fixture COMMAND tracecap --check
         "${TRACECAP_FIXTURE_DIR}/qemu-9p-trace.log")
set_tests_properties(tracecap-fixture PROPERTIES
    TIMEOUT 120
    LABELS "trace;fixtures;regression")

# The capture must be reproducible: two runs, byte for byte.
add_test(NAME tracecap-deterministic COMMAND tracecap --verify-deterministic)
set_tests_properties(tracecap-deterministic PROPERTIES
    TIMEOUT 120
    LABELS "trace;fixtures")
