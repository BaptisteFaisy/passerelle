/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/test_main.c - Entry point shared by every rmfs test binary.
 *
 * The harness (pgw/test.h) registers cases at load time; this file is the
 * single `main` that runs them, so no test file repeats it.
 */
#include "pgw/test.h"

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
