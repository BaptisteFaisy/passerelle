/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/opts.c - mount option parsing for both QNX drivers.
 *
 * Portable code (no QNX header): it is compiled by the PC build and exercised
 * by the driver tests, so a wrong bound is caught without a target.
 *
 * Covered requirements: FS-09 / SEC-01 (nosuid defaults to on), FS-13
 * (attrttl= is bounded by PGW_ATTR_CACHE_NS_MAX), P9-01 (msize= range),
 * ROB-05 (timeout= range).
 */
#include "pgw/drvopts.h"

#include "pgw/time.h"

#include <string.h>

/* The filesystem defaults (nosuid on, no cache, ...) are the layer 3's: they
 * live in rmfs, in pgw_fs_opts_default(). When the rmfs module is part of the
 * build the driver uses that function, so there is exactly one table. When it
 * is not (the PC syntax build, where only common and drivers are configured),
 * the same five values are set here so the parser stays testable on the PC;
 * the QNX build always links rmfs and never uses this fallback. */
#ifdef PGW_WITH_RMFS
#define opts_fs_default pgw_fs_opts_default
#else
static void opts_fs_default(pgw_fs_opts *fs)
{
    if (fs == NULL)
        return;
    fs->readonly = false;
    fs->noexec = false;
    fs->nosuid = true; /* SEC-01: no privilege elevation by default */
    fs->attr_ttl_ns = PGW_ATTR_CACHE_NS_DEFAULT;
    fs->max_inflight = PGW_INFLIGHT_DEFAULT;
}
#endif

#define OPTS_TAG_CAP 64u

bool pgw_opts_key_is(const char *key, size_t key_len, const char *name)
{
    size_t n = strlen(name);
    return key_len == n && memcmp(key, name, n) == 0;
}

static int digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

pgw_err pgw_opts_u32(const char *text, size_t len, uint32_t min, uint32_t max,
                     uint32_t *out)
{
    if (text == NULL || out == NULL || len == 0)
        return PGW_EINVAL;
    uint32_t base = 10;
    size_t i = 0;
    if (len > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        i = 2;
    }
    uint32_t v = 0;
    for (; i < len; i++) {
        int d = digit_value(text[i]);
        if (d < 0 || (uint32_t)d >= base)
            return PGW_EINVAL;
        if (v > (UINT32_MAX - (uint32_t)d) / base)
            return PGW_EINVAL; /* overflow: never wrap into a plausible value */
        v = v * base + (uint32_t)d;
    }
    if (v < min || v > max)
        return PGW_EINVAL;
    *out = v;
    return PGW_OK;
}

static bool copy_tag(char *dst, size_t cap, const char *src, size_t len)
{
    if (len == 0 || len >= cap || len >= OPTS_TAG_CAP)
        return false;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

/* --- the shared keys ------------------------------------------------------ */

typedef struct opt_flag {
    const char *name;
    bool *target;
    bool value;
} opt_flag;

static pgw_err parse_flag(const char *key, size_t key_len, const char *val,
                          size_t val_len, pgw_mount_opts *opts)
{
    /* ro / rw and the two "deny" switches are bare flags. nosuid is on by
     * default (SEC-01) and `suid` is the explicit opt-out. */
    const opt_flag flags[] = {
        { "ro", &opts->fs.readonly, true },
        { "rw", &opts->fs.readonly, false },
        { "noexec", &opts->fs.noexec, true },
        { "exec", &opts->fs.noexec, false },
        { "nosuid", &opts->fs.nosuid, true },
        { "suid", &opts->fs.nosuid, false },
        { "debug", &opts->debug, true },
    };
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        if (pgw_opts_key_is(key, key_len, flags[i].name)) {
            if (val != NULL && val_len != 0)
                return PGW_EINVAL; /* a bare flag takes no value */
            *flags[i].target = flags[i].value;
            return PGW_OK;
        }
    }
    return PGW_ENOSYS; /* not a bare flag: the caller tries the valued keys */
}

static pgw_err parse_value(const char *key, size_t key_len, const char *val,
                           size_t val_len, pgw_mount_opts *opts)
{
    uint32_t n = 0;
    if (val == NULL || val_len == 0)
        return PGW_EINVAL;
    if (pgw_opts_key_is(key, key_len, "tag")) {
        return copy_tag(opts->tag, sizeof opts->tag, val, val_len) ? PGW_OK
                                                                  : PGW_EINVAL;
    }
    if (pgw_opts_key_is(key, key_len, "msize")) {
        if (pgw_opts_u32(val, val_len, PGW_MSIZE_MIN, PGW_MSIZE_MAX, &n) != PGW_OK)
            return PGW_EINVAL;
        opts->msize = n;
        return PGW_OK;
    }
    if (pgw_opts_key_is(key, key_len, "timeout")) {
        if (pgw_opts_u32(val, val_len, 0, PGW_TIMEOUT_MS_MAX, &n) != PGW_OK)
            return PGW_EINVAL;
        opts->timeout_ms = n;
        return PGW_OK;
    }
    if (pgw_opts_key_is(key, key_len, "attrttl")) {
        /* Milliseconds on the command line (the cache decision D4 is expressed
         * in ms); stored in nanoseconds like every other duration. */
        if (pgw_opts_u32(val, val_len, 0, UINT32_MAX, &n) != PGW_OK)
            return PGW_EINVAL;
        uint64_t ns = pgw_ms_to_ns(n);
        if (ns > PGW_ATTR_CACHE_NS_MAX)
            return PGW_EINVAL;
        opts->fs.attr_ttl_ns = ns;
        return PGW_OK;
    }
    if (pgw_opts_key_is(key, key_len, "irq")) {
        if (pgw_opts_u32(val, val_len, 0, (uint32_t)INT32_MAX, &n) != PGW_OK)
            return PGW_EINVAL;
        opts->irq = (int32_t)n;
        return PGW_OK;
    }
    if (pgw_opts_key_is(key, key_len, "requests")) {
        if (pgw_opts_u32(val, val_len, 1, PGW_INFLIGHT_MAX, &n) != PGW_OK)
            return PGW_EINVAL;
        opts->fs.max_inflight = n;
        return PGW_OK;
    }
    return PGW_ENOSYS; /* not a valued key we know */
}

pgw_err pgw_opts_parse(const char *list, pgw_mount_opts *opts, pgw_opt_hook hook)
{
    if (list == NULL || opts == NULL)
        return PGW_EINVAL;
    size_t pos = 0;
    while (list[pos] != '\0') {
        size_t end = pos;
        while (list[end] != '\0' && list[end] != ',')
            end++;
        if (end == pos)
            return PGW_EINVAL; /* empty element: "ro,,nosuid" or a trailing "," */
        const char *item = list + pos;
        size_t item_len = end - pos;
        const char *eq = memchr(item, '=', item_len);
        size_t key_len = eq != NULL ? (size_t)(eq - item) : item_len;
        const char *val = eq != NULL ? eq + 1 : NULL;
        size_t val_len = eq != NULL ? item_len - key_len - 1u : 0u;
        if (key_len == 0)
            return PGW_EINVAL;
        pgw_err rc = parse_flag(item, key_len, val, val_len, opts);
        if (rc == PGW_ENOSYS)
            rc = parse_value(item, key_len, val, val_len, opts);
        if (rc == PGW_ENOSYS) {
            pgw_err hrc = PGW_ENOSYS;
            if (hook == NULL || !hook(opts, item, key_len, val, val_len, &hrc))
                return PGW_EINVAL; /* a typo must not change behaviour */
            rc = hrc;
        }
        if (rc != PGW_OK)
            return rc;
        if (list[end] != ',') {
            pos = end;
            break;
        }
        pos = end + 1u;
        if (list[pos] == '\0')
            return PGW_EINVAL; /* a trailing comma is a typo, not an option */
    }
    return PGW_OK;
}

void pgw_opts_default(pgw_mount_opts *opts)
{
    if (opts == NULL)
        return;
    memset(opts, 0, sizeof *opts);
    opts_fs_default(&opts->fs); /* nosuid on, cache off (SEC-01, FS-13) */
    opts->timeout_ms = PGW_TIMEOUT_MS_DEFAULT;
    opts->msize = PGW_MSIZE_DEFAULT;
    opts->irq = -1; /* let layer 1 pick, MSI-X first (VIO-04) */
    opts->debug = false;
    opts->mount_point = NULL;
    opts->tag[0] = '\0';
}
