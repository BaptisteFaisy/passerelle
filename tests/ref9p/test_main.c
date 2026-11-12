/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ref9p/test_main.c - entry point shared by the reference benches.
 *
 * The harness in common/pgw/test.h registers cases at load time and provides
 * pgw_test_main(); each bench only has to supply this main.
 */
#include "pgw/test.h"

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
