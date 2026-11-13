/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/qemu/qnx8/fs6-demo.c - a program the guest executes from the share.
 *
 * FS-06 asks for one thing: a program served by the shared folder, executed
 * by the guest. This one is deliberately small: it prints what it is, reads a
 * file of the share *through the driver that serves it* and leaves a
 * distinctive exit status, so the console shows that the guest really ran
 * code that came from the PC.
 *
 * It is built for the target by the SDP's compiler (see build-image.sh) and
 * put in the shared folder; the guest runs it as `/host/fs6-demo`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS6_EXIT_CODE 42

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/host/from-host.txt";
    char line[128];

    printf("fs6-demo: running as uid %d, pid %d\n", (int)getuid(), (int)getpid());
    fflush(stdout);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        printf("fs6-demo: cannot open %s\n", path);
        fflush(stdout);
        return 1;
    }
    if (fgets(line, (int)sizeof line, f) == NULL)
        line[0] = '\0';
    (void)fclose(f);
    printf("fs6-demo: read through the driver: %s", line);
    fflush(stdout);
    return FS6_EXIT_CODE;
}
