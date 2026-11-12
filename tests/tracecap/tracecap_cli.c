/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/tracecap/tracecap_cli.c - the command line of the trace capture.
 *
 * Three modes:
 *   tracecap                       write the rendered trace on stdout;
 *   tracecap --out FILE            write it in FILE;
 *   tracecap --check FILE          compare it with FILE and fail when they
 *                                   differ (this is the drift gate of gap 55);
 *   tracecap --verify-deterministic capture twice and fail when the two
 *                                   renderings are not byte-identical.
 *
 * Exit status: 0 when everything is fine, 1 when the fixture drifted or the
 * capture is not deterministic, 2 when the capture could not run at all.
 */
#define _POSIX_C_SOURCE 200809L
#include "tracecap.h"

#include "trace_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Render the capture into a memory buffer, so it can be compared or written
 * without touching the file system twice. */
static char *render_capture(size_t *out_len)
{
    tracecap_log log;
    tracecap_log_init(&log);
    if (tracecap_capture(&log) != PGW_OK) {
        fprintf(stderr, "tracecap: the capture failed\n");
        return NULL;
    }
    char *buf = NULL;
    size_t size = 0;
    FILE *mem = open_memstream(&buf, &size);
    if (mem == NULL) {
        tracecap_log_reset(&log);
        return NULL;
    }
    (void)tracecap_render(&log, mem);
    fclose(mem);
    tracecap_log_reset(&log);
    *out_len = size;
    return buf;
}

/* Index of the first byte that differs, or -1 when the two are equal. */
static int first_difference(const char *got, size_t got_len, const char *want,
                            size_t want_len)
{
    size_t i = 0;
    while (i < got_len && i < want_len && got[i] == want[i])
        i++;
    if (i == got_len && i == want_len)
        return -1;
    return (int)i;
}

static int check_against(const char *path)
{
    size_t len = 0;
    char *rendered = render_capture(&len);
    if (rendered == NULL)
        return 2;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "tracecap: cannot read %s\n", path);
        free(rendered);
        return 2;
    }
    char want[65536];
    size_t got = fread(want, 1u, sizeof want, f);
    fclose(f);
    int index = first_difference(rendered, len, want, got);
    int rc;
    if (index < 0) {
        fprintf(stdout, "tracecap: %s matches the current code\n", path);
        rc = 0;
    } else {
        size_t line = 1;
        for (size_t k = 0; k < (size_t)index; k++) {
            if (rendered[k] == '\n')
                line++;
        }
        fprintf(stderr, "tracecap: the fixture %s drifted at line %zu\n", path, line);
        fprintf(stderr, "  regenerated: %.120s\n",
                rendered + ((size_t)index > 40u ? (size_t)index - 40u : 0u));
        fprintf(stderr, "  committed:   %.120s\n",
                want + ((size_t)index > 40u ? (size_t)index - 40u : 0u));
        rc = 1;
    }
    free(rendered);
    return rc;
}

static int verify_deterministic(void)
{
    size_t len_a = 0;
    size_t len_b = 0;
    char *a = render_capture(&len_a);
    char *b = render_capture(&len_b);
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        return 2;
    }
    int rc = 0;
    if (len_a != len_b || memcmp(a, b, len_a) != 0) {
        fprintf(stderr, "tracecap: two captures differ (%zu and %zu bytes)\n", len_a,
                len_b);
        rc = 1;
    } else {
        fprintf(stdout, "tracecap: two captures are byte-identical (%zu bytes)\n", len_a);
    }
    free(a);
    free(b);
    return rc;
}

static int write_to(const char *path)
{
    size_t len = 0;
    char *rendered = render_capture(&len);
    if (rendered == NULL)
        return 2;
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "tracecap: cannot write %s\n", path);
        free(rendered);
        return 2;
    }
    size_t written = fwrite(rendered, 1u, len, f);
    fclose(f);
    free(rendered);
    if (written != len) {
        fprintf(stderr, "tracecap: short write on %s\n", path);
        return 2;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *out = NULL;
    const char *check = NULL;
    bool verify = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--check") == 0 && i + 1 < argc) {
            check = argv[++i];
        } else if (strcmp(argv[i], "--verify-deterministic") == 0) {
            verify = true;
        } else {
            fprintf(stderr, "usage: tracecap [--out FILE] [--check FILE] "
                            "[--verify-deterministic]\n");
            return 2;
        }
    }
    if (verify)
        return verify_deterministic();
    if (check != NULL)
        return check_against(check);
    if (out != NULL)
        return write_to(out);
    size_t len = 0;
    char *rendered = render_capture(&len);
    if (rendered == NULL)
        return 2;
    size_t written = fwrite(rendered, 1u, len, stdout);
    free(rendered);
    return (written == len) ? 0 : 2;
}
