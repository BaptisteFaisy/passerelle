#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# tools/corpus-save.sh - capture the divergences of the differential bench as
# regression cases (gap 62 of docs/gap-analysis.md).
#
# The bench of tests/e2e reduces any divergence to its shortest reproducible
# prefix (diff_reduce) and then only prints it. Run through this script, the
# reduced cases are also *persisted* into tests/corpus/, where test_corpus
# replays them on every CTest run: a divergence that was found once can then
# never come back unnoticed.
#
# Nothing is written unless this script asks for it: PGW_CORPUS_SAVE is the
# explicit request, and the normal test suite never sets it.
#
# Usage:
#   tools/corpus-save.sh                       # 100 000 ops, 3 seeds, ./build
#   BUILD_DIR=build-asan DIFF_OPS=1000000 tools/corpus-save.sh
#
# Exit status: 0 when at least one case was stored or the bench ran clean,
# 1 when the bench could not be run at all.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
CORPUS_DIR="${CORPUS_DIR:-$ROOT/tests/corpus}"
DIFF_OPS="${DIFF_OPS:-100000}"
BIN="$BUILD_DIR/tests/test_diff"

if [ ! -x "$BIN" ]; then
    echo "corpus-save: $BIN is not built."
    echo "             run: cmake -B build && cmake --build build -j8"
    exit 1
fi

mkdir -p "$CORPUS_DIR"
before=$(find "$CORPUS_DIR" -name '*.pgwcase' | wc -l)

echo "corpus-save: $DIFF_OPS differential operations per seed"
echo "corpus-save: reduced divergences go to $CORPUS_DIR"
PGW_CORPUS_SAVE="$CORPUS_DIR" PGW_DIFF_OPS="$DIFF_OPS" "$BIN"
rc=$?

after=$(find "$CORPUS_DIR" -name '*.pgwcase' | wc -l)
echo "corpus-save: $((after - before)) case(s) stored, $after in the corpus"
if [ "$((after - before))" -gt 0 ]; then
    echo "corpus-save: review them, then commit tests/corpus/ with the fix"
    find "$CORPUS_DIR" -name '*.pgwcase' -newermt '-10 minutes' -print
fi
# A failing bench is the expected outcome here: the whole point is to store
# what it reduced. Only a run that stored nothing keeps the bench's status.
if [ "$((after - before))" -gt 0 ]; then
    exit 0
fi
exit "$rc"
