/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/qemu/qnx8/clockprobe.c - what the QNX 8.0 guest answers for the two
 * calls the driver's deadlines are built on.
 *
 * The driver waits for the 9P replies with a deadline taken from
 * clock_gettime(CLOCK_MONOTONIC) and yields with nanosleep(). If either fails
 * in the guest, a wait that should time out never does - the loop spins and
 * the guest stops answering its console. This probe prints what happens, at
 * boot, before anything else.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    struct timespec ts;
    errno = 0;
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("clockprobe: clock_gettime rc=%d errno=%d tv=%lld.%09ld\n", rc, errno,
           (long long)ts.tv_sec, (long long)ts.tv_nsec);
    fflush(stdout);

    struct timespec req = {0, 1000000L};
    struct timespec rem = {0, 0};
    errno = 0;
    rc = nanosleep(&req, &rem);
    printf("clockprobe: nanosleep rc=%d errno=%d\n", rc, errno);
    fflush(stdout);
    return 0;
}
