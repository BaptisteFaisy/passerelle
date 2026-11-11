/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/test_main.c - entry point of every proto9p test bench.
 *
 * The harness in common/pgw/test.h registers cases at load time and provides
 * pgw_test_main(); each bench only has to supply this main.
 */
#include "pgw/test.h"

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
