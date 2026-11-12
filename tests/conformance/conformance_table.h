/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/conformance/conformance_table.h - the executable conformance table.
 *
 * Gap 60 asked for a cross-check against diod's description of the 9P2000.L
 * protocol turned into an executable test. This is that table: one entry per
 * rule of the description, each entry naming the section of the vendored
 * excerpt it comes from and the named case that proves it.
 *
 * The table is data, not prose, so the bench can check itself:
 *   - every rule cites a section that exists in diod-protocol-excerpt.md;
 *   - every rule names a case of the table, and every case is named by a
 *     rule;
 *   - every case that is registered in the running binary is known here.
 * A rule that nobody cites, or that cites nothing, is a failure of the
 * bench, not a silent gap.
 *
 * Covers P9-01 (the version and the msize), P9-02 (the message layouts),
 * P9-03 (the 9P2000.L operations), P9-07 (the fid and tag semantics),
 * P9-09 (the strict decoding) and gap 60.
 */
#ifndef CONFORMANCE_TABLE_H
#define CONFORMANCE_TABLE_H

#include <stdbool.h>
#include <stddef.h>

/* One rule of diod's protocol description. */
typedef struct pgw_conf_rule {
    const char *id;        /* unique identifier, e.g. "DIOD-WIRE-01" */
    const char *section;   /* the heading of the excerpt the rule comes from */
    const char *statement; /* the rule, in one sentence */
    const char *case_name; /* the named case that proves it */
} pgw_conf_rule;

/* One named case of the table: the function that proves the rule. */
typedef struct pgw_conf_case {
    const char *name;
    void (*run)(void);
} pgw_conf_case;

/* Register a case. The case is also a PGW_TEST of the same name, so the
 * table bench and the runner always run the same function. */
#define PGW_CONF_CASE(fn)                                                                \
    static const pgw_conf_case pgw_conf_case_##fn                                        \
            __attribute__((used, section("pgw_conf_cases"))) = {#fn, fn}

/* The rules, in the order of the description. */
size_t pgw_conf_rule_count(void);
const pgw_conf_rule *pgw_conf_rule_at(size_t index);
const pgw_conf_rule *pgw_conf_rule_find(const char *id);

/* The names of every case of the table, across the binaries that hold them. */
size_t pgw_conf_case_count(void);
const char *pgw_conf_case_name(size_t index);
bool pgw_conf_case_exists(const char *name);

/* The cases registered in the running binary. */
size_t pgw_conf_registered_count(void);
const pgw_conf_case *pgw_conf_registered_at(size_t index);

/* The excerpt the sections are cited from, and the upstream document. */
const char *pgw_conf_excerpt_path(void);
const char *pgw_conf_excerpt_url(void);

#endif /* CONFORMANCE_TABLE_H */
