/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/conformance/test_main.c - entry point of the conformance bench.
 *
 * The harness of common/pgw/test.h registers the cases at load time and
 * provides pgw_test_main(); the bench only has to supply this main. The
 * bench that needs diod has its own main, because it skips when the server
 * is not installed.
 */
#include "pgw/test.h"

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
