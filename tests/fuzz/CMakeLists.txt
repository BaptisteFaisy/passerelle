# SPDX-License-Identifier: Apache-2.0
#
# tests/fuzz/ - the randomized-input campaigns and the corpus they keep.
#
# The campaigns themselves (fuzz_p9, fuzz_fuse) are registered by
# tests/CMakeLists.txt, which owns their sources; this module adds what
# belongs to the *kept* corpus (gap 48):
#
#   test_fuzz_corpus  replays the inputs of tests/corpus/fuzz/ in the normal
#                     CTest suite, so a decoder that used to misbehave on a
#                     stored input is caught again on every pull request.
#
# The corpus is written by the campaigns themselves when PGW_FUZZ_CORPUS_DIR
# points at the directory that must receive it; the selection rule and the
# cap are documented in tests/corpus/README.md.

pgw_test(test_fuzz_corpus
    SRCS fuzz_corpus_test.c
    DEPS pgw_proto9p pgw_protofuse
    LABELS "fuzz;corpus;regression")

# Where the kept corpus lives: the source tree, read-only. The replay never
# writes there.
target_compile_definitions(test_fuzz_corpus PRIVATE
    PGW_FUZZ_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/fuzz")
