# SPDX-License-Identifier: Apache-2.0
#
# tests/interop/ - interoperability with a third-party 9P implementation.
#
# The benches of tests/ref9p/ prove that our client talks to a second
# implementation, but that implementation was written by us as well: both
# sides share the same assumptions, so a common misreading of the protocol
# would pass on both. This directory closes that hole (gap 59): the
# production client of proto9p/ is driven against diod, a third-party
# 9P2000.L server, and every result is compared with the truth on disk.
#
# diod is a package (Debian/Ubuntu: apt-get install diod), so the bench
# skips with an explicit reason when the server is not installed - the
# .github/workflows/interop.yml job installs it and runs the bench for
# real. A skip is never a pass.
#
# The truth helpers of tests/ref9p/ are reused as they are (no second
# implementation of the "what the file system really holds" side).

pgw_test(test_interop_diod
    SRCS test_interop_diod.c interop_diod.c interop_transport.c
         ${CMAKE_CURRENT_SOURCE_DIR}/../ref9p/ref9p_truth.c
    DEPS pgw_proto9p
    LABELS "interop;conformance")

target_include_directories(test_interop_diod PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ref9p)

# A missing dependency is reported as a skip, with the reason in the log.
set_tests_properties(test_interop_diod PROPERTIES
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "PGW_SKIP:")
