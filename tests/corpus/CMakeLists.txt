# SPDX-License-Identifier: Apache-2.0
#
# tests/corpus/ - the stored divergences of the differential bench, and the
# bench that replays them (gap 62 of docs/gap-analysis.md).
#
# A *.pgwcase file here is a divergence that tests/e2e found and reduced to its
# shortest prefix (diff_reduce), saved by tools/corpus-save.sh with
# PGW_CORPUS_SAVE pointing at this directory. The format is documented in
# tests/e2e/corpus.h: seed + operation count + a digest of the operations, so
# the case replays byte for byte without storing the operations themselves.
#
# test_corpus runs in the normal CTest suite and fails if a stored divergence
# returns. It is strictly read-only: the replay itself happens in a temporary
# directory, so nothing is ever written to the repository tree. An empty
# corpus is valid - it only means no divergence has been captured yet.
#
# The corpus format and the digest live in tests/e2e/corpus.h, next to the
# bench that produces the cases; pgw_diff exports that directory, so this
# module needs no include path of its own.

pgw_test(test_corpus
    SRCS test_corpus.c
    DEPS pgw_diff
    LABELS "corpus;regression;e2e")

# Where the stored cases live: the source tree, read-only. The bench never
# writes there, and never anywhere else in the repository.
target_compile_definitions(test_corpus PRIVATE
    PGW_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

set_tests_properties(test_corpus PROPERTIES TIMEOUT 300)
