# SPDX-License-Identifier: Apache-2.0
#
# proto9p - layer 2: the 9P2000.L protocol, portable C11.
#
# The library is split by concern so that no file grows past the project limit:
# encoders, decoders, identifier pools, the P9-04 translation glue and the
# session (lifecycle, namespace, path walking, chunked transfers).
#
# The tests are separate benches, one CTest case each:
#   codec      - encode/decode round trips of every message
#   validation - truncation at every length, trailing bytes, bad type/tag,
#                lying internal lengths (P9-09)
#   idpool     - a million alloc/free with no leak (P9-07)
#   session    - chunked read/write/readdir and a full session against an
#                in-memory server (P9-01, P9-05, P9-08, P9-10)

pgw_library(pgw_proto9p
    SRCS
        src/p9_enc.c
        src/p9_dec.c
        src/p9_dec_attr.c
        src/p9_idpool.c
        src/p9_xlate.c
        src/p9_session.c
        src/p9_session_ns.c
        src/p9_session_walk.c
        src/p9_session_io.c
    DEPS pgw_common
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(BUILD_TESTING)
    pgw_test(test_p9_codec
        SRCS tests/test_main.c tests/test_codec.c tests/test_rdecode.c
             tests/test_xlate.c
        DEPS pgw_proto9p
        LABELS "proto9p")
    target_include_directories(test_p9_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    pgw_test(test_p9_validation
        SRCS tests/test_main.c tests/test_validation.c
        DEPS pgw_proto9p
        LABELS "proto9p")
    target_include_directories(test_p9_validation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    pgw_test(test_p9_idpool
        SRCS tests/test_main.c tests/test_idpool.c
        DEPS pgw_proto9p
        LABELS "proto9p")
    target_include_directories(test_p9_idpool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    pgw_test(test_p9_session
        SRCS tests/test_main.c tests/test_session.c tests/test_session_io.c
             tests/test_bench.c tests/fake_server.c tests/fake_serve.c
             tests/fake_serve_meta.c tests/fake_dispatch.c
        DEPS pgw_proto9p
        LABELS "proto9p")
    target_include_directories(test_p9_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
