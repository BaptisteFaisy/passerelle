/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/conformance/test_conformance_table.c - the table checks itself.
 *
 * The conformance table is a claim: "this rule of diod's description is
 * proved by that case". These cases check the claim:
 *
 *   - every rule cites a section that exists in the vendored excerpt, so an
 *     uncited rule cannot pass;
 *   - every rule names a case of the table, and every case is named by a
 *     rule, so a case nobody cites is a leftover, not a proof;
 *   - every case registered in the running binary is a case of the table,
 *     with a function behind it;
 *   - the excerpt says where it comes from, so a citation points at a
 *     document and not at a file of ours.
 *
 * The table is printed as well: the log of the bench then holds the whole
 * table, rule by rule.
 *
 * Covers gap 60.
 */
#define _POSIX_C_SOURCE 200809L

#include "conformance_table.h"

#include "pgw/test.h"

#include <stdio.h>
#include <string.h>

/* The lines of the excerpt that are exactly `section`. */
static bool excerpt_has_section(const char *path, const char *section)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof line, f) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' '))
            p[--len] = '\0';
        if (strcmp(p, section) == 0) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

/* The excerpt mentions the upstream document. */
static bool excerpt_mentions(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strstr(line, needle) != NULL) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

/* The rule: every rule cites a section of the excerpt. */
static void conf_table_every_rule_cites_a_section(void)
{
    const char *path = pgw_conf_excerpt_path();
    size_t n = pgw_conf_rule_count();
    PGW_ASSERT(n > 0);
    for (size_t i = 0; i < n; i++) {
        const pgw_conf_rule *r = pgw_conf_rule_at(i);
        PGW_ASSERT(r->section != NULL && r->section[0] != '\0');
        PGW_ASSERT(excerpt_has_section(path, r->section));
    }
}
PGW_TEST(conf_table_every_rule_cites_a_section);

/* The rule: every rule has a unique identifier and says something. */
static void conf_table_every_rule_has_a_unique_id(void)
{
    size_t n = pgw_conf_rule_count();
    for (size_t i = 0; i < n; i++) {
        const pgw_conf_rule *r = pgw_conf_rule_at(i);
        PGW_ASSERT(r->id != NULL && r->id[0] != '\0');
        PGW_ASSERT(r->statement != NULL && r->statement[0] != '\0');
        for (size_t j = i + 1; j < n; j++)
            PGW_ASSERT(strcmp(r->id, pgw_conf_rule_at(j)->id) != 0);
    }
}
PGW_TEST(conf_table_every_rule_has_a_unique_id);

/* The rule: every rule names a case of the table. */
static void conf_table_every_rule_names_a_case_of_the_table(void)
{
    size_t n = pgw_conf_rule_count();
    for (size_t i = 0; i < n; i++) {
        const pgw_conf_rule *r = pgw_conf_rule_at(i);
        PGW_ASSERT(r->case_name != NULL && r->case_name[0] != '\0');
        PGW_ASSERT(pgw_conf_case_exists(r->case_name));
    }
}
PGW_TEST(conf_table_every_rule_names_a_case_of_the_table);

/* The rule: every case of the table is named by a rule. */
static void conf_table_every_case_is_named_by_a_rule(void)
{
    size_t cases = pgw_conf_case_count();
    PGW_ASSERT(cases > 0);
    for (size_t i = 0; i < cases; i++) {
        const char *name = pgw_conf_case_name(i);
        bool cited = false;
        for (size_t j = 0; j < pgw_conf_rule_count(); j++) {
            if (strcmp(pgw_conf_rule_at(j)->case_name, name) == 0) {
                cited = true;
                break;
            }
        }
        PGW_ASSERT(cited);
    }
}
PGW_TEST(conf_table_every_case_is_named_by_a_rule);

/* The rule: every case registered in this binary is a case of the table, and
 * it has a function behind it. */
static void conf_table_every_registered_case_is_known(void)
{
    size_t n = pgw_conf_registered_count();
    PGW_ASSERT(n > 0);
    for (size_t i = 0; i < n; i++) {
        const pgw_conf_case *c = pgw_conf_registered_at(i);
        PGW_ASSERT(c != NULL);
        PGW_ASSERT(c->name != NULL && c->name[0] != '\0');
        PGW_ASSERT(c->run != NULL);
        PGW_ASSERT(pgw_conf_case_exists(c->name));
    }
}
PGW_TEST(conf_table_every_registered_case_is_known);

/* The rule: the excerpt says where it comes from. */
static void conf_table_the_excerpt_is_the_diod_document(void)
{
    const char *path = pgw_conf_excerpt_path();
    PGW_ASSERT(excerpt_mentions(path, pgw_conf_excerpt_url()));
    PGW_ASSERT(excerpt_mentions(path, "chaos/diod"));
}
PGW_TEST(conf_table_the_excerpt_is_the_diod_document);

/* The table itself, printed: the log of the bench holds it rule by rule. */
static void conf_table_prints_the_rules(void)
{
    size_t n = pgw_conf_rule_count();
    fprintf(stderr, "# the conformance table: %zu rules from diod's protocol.md\n", n);
    for (size_t i = 0; i < n; i++) {
        const pgw_conf_rule *r = pgw_conf_rule_at(i);
        fprintf(stderr, "# %-14s %-56s %s\n", r->id, r->section, r->case_name);
    }
    fflush(stderr);
}
PGW_TEST(conf_table_prints_the_rules);
