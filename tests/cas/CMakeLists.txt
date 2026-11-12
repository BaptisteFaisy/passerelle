# SPDX-License-Identifier: Apache-2.0
#
# tests/cas/ - the directed cases of spec 7.4 and the robustness depth gaps.
#
# This directory brings its own CMakeLists.txt, so tests/CMakeLists.txt picks
# it up automatically and no shared file is edited (CONTRIBUTING.md 3.9).
#
# Six gaps of docs/gap-analysis.md are covered, each by its own binary so a
# failure names the case:
#
#   test_cas_big_file    gap 64, the 5 GiB file (sparse, never materialised)
#   test_cas_big_dir     gap 65, the 100 000-entry directory
#   test_cas_path_len    gap 66, the maximum path length
#   test_cas_exotic      gap 67, exotic names (UTF-8, NFC/NFD, punctuation)
#   test_cas_load        gap 63, a mount under a continuous workload
#   test_cas_fsync       gap 69, what fsync really proves
#
# The scale of a case comes from its environment, so the same binary runs the
# short variant inside the 5-minute CI budget and the full spec variant as a
# DISABLED test of the long campaigns (make bench / the nightly workflow).
#
# The client-side cases reuse the reference 9P server of tests/ref9p: its
# harness and truth helpers are compiled into the binaries that need them, so
# nothing here duplicates that module.

get_filename_component(REF9P_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ref9p" ABSOLUTE)
find_package(Threads REQUIRED)

add_compile_definitions(
    "PGW_REF9P_SERVER=\"${REF9P_DIR}/ref9p_server.py\""
    "PGW_REF9P_PYTHON=\"${Python3_EXECUTABLE}\"")

# The 9P-side fixture: the reference server, a session and the layer on top of
# the 9P adapter (the "client view" of the directed cases).
set(CAS_9P_SRCS cas_9p.c ../ref9p/ref9p_harness.c)
# cas_main.c is the single entry point; cas_common.c the fixture; the truth
# helpers of tests/ref9p are reused as they are, so no case re-implements a
# path join or a directory listing.
set(CAS_MAIN cas_main.c cas_common.c ../ref9p/ref9p_truth.c)

include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${REF9P_DIR}"
    "${CMAKE_SOURCE_DIR}/rmfs/tests")

pgw_test(test_cas_big_file
    SRCS test_cas_big_file.c ${CAS_MAIN} ${CAS_9P_SRCS}
    DEPS pgw_diff pgw_proto9p
    LABELS "directed;conformance")

# The 5 GiB variant of spec 7.4. Never a pull-request gate (CI budget): the
# case is sparse, so it costs disk *nothing*, but the long campaigns want the
# literal number of the specification.
add_test(NAME cas-big-file-5gib COMMAND test_cas_big_file)
set_tests_properties(cas-big-file-5gib PROPERTIES
    DISABLED TRUE
    LABELS "long;directed"
    TIMEOUT 900
    ENVIRONMENT "PGW_CAS_BIG_BYTES=5368709120")

pgw_test(test_cas_big_dir
    SRCS test_cas_big_dir.c ${CAS_MAIN} ${CAS_9P_SRCS}
    DEPS pgw_diff pgw_proto9p
    LABELS "directed;conformance")

# The 100 000 entries of spec 7.4, out of the pull-request gate.
add_test(NAME cas-big-dir-100k COMMAND test_cas_big_dir)
set_tests_properties(cas-big-dir-100k PROPERTIES
    DISABLED TRUE
    LABELS "long;directed"
    TIMEOUT 1800
    ENVIRONMENT "PGW_CAS_DIR_ENTRIES=100000")

pgw_test(test_cas_path_len
    SRCS test_cas_path_len.c ${CAS_MAIN} ${CAS_9P_SRCS}
    DEPS pgw_diff pgw_proto9p
    LABELS "directed;conformance")

pgw_test(test_cas_exotic
    SRCS test_cas_exotic.c ${CAS_MAIN} ${CAS_9P_SRCS}
    DEPS pgw_diff pgw_proto9p
    LABELS "directed;conformance")

pgw_test(test_cas_fsync
    SRCS test_cas_fsync.c ${CAS_MAIN} ${CAS_9P_SRCS}
    DEPS pgw_diff pgw_proto9p
    LABELS "directed;conformance")

pgw_test(test_cas_load
    SRCS test_cas_load.c ${CAS_MAIN}
    DEPS pgw_diff
    LABELS "directed;conformance")
target_link_libraries(test_cas_load PRIVATE Threads::Threads)

# The long campaign of gap 63: more clients, more time, out of the pull-request
# gate (CI budget, 5 minutes per job).
add_test(NAME cas-load-long COMMAND test_cas_load)
set_tests_properties(cas-load-long PROPERTIES
    DISABLED TRUE
    LABELS "long;directed"
    TIMEOUT 900
    ENVIRONMENT "PGW_CAS_LOAD_THREADS=8;PGW_CAS_LOAD_SECONDS=30")
