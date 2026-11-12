/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cas_main.c - the single entry point of the directed-case binaries.
 *
 * The harness (pgw/test.h) registers the cases of a binary at load time; this
 * file is the one `main` that runs them, so no case file repeats it (the same
 * pattern as rmfs/tests/test_main.c). Covers the directed cases of spec 7.4
 * (gaps 63, 64, 65, 66, 67, 69).
 */
#include "pgw/test.h"

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
