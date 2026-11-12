/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/conformance/conformance_table.c - the rules of diod's description.
 *
 * Every line of the table below is a rule taken from diod's protocol.md,
 * the excerpt of which lives in diod-protocol-excerpt.md next to this file.
 * The section column holds the heading of that excerpt, and the bench in
 * test_conformance_table.c fails when a heading is not there: an uncited
 * rule is not a cross-check.
 *
 * The case column names the function that proves the rule. A rule about
 * bytes is proved by reading the bytes of a message at the offsets of the
 * description; a rule about a sequence is proved by driving the production
 * client of proto9p/ against a real server (the reference server of
 * tests/ref9p/, or diod itself when it is installed).
 *
 * Covers P9-01, P9-02, P9-03, P9-07, P9-09 and gap 60.
 */
#include "conformance_table.h"

#include <stdlib.h>
#include <string.h>

/* The headings of the vendored excerpt, quoted once. */
#define SEC_DATA "### Data Structures"
#define SEC_VERSION "#### version -- negotiate protocol version"
#define SEC_FLUSH "#### flush -- abort a message"
#define SEC_WALK "#### walk -- descend a directory hierarchy"
#define SEC_READWRITE "### read, write -- transfer data from and to a file"
#define SEC_CLUNK "#### clunk -- destroy a fid"
#define SEC_ATTACH "#### attach, auth -- messages to establish a connection"
#define SEC_LERROR "#### lerror -- return error code"
#define SEC_STATFS "#### statfs -- get file system information"
#define SEC_LOPEN "#### lopen -- open a file"
#define SEC_LCREATE "#### lcreate -- create regular file"
#define SEC_SYMLINK "#### symlink -- create symbolic link"
#define SEC_READLINK "#### readlink -- read value of symbolic link"
#define SEC_GETATTR "#### getattr -- get file attributes"
#define SEC_SETATTR "#### setattr -- set file attributes"
#define SEC_READDIR "#### readdir -- read a directory"
#define SEC_MKDIR "### mkdir - create directory"
#define SEC_LINK "#### link - create hard link"
#define SEC_SESSION "### Sample Session"

static const pgw_conf_rule g_rules[] = {
        {"DIOD-DATA-01", SEC_DATA,
         "Integers of 1, 2, 4 and 8 bytes travel little-endian, least significant byte "
         "first.",
         "conf_wire_integers_are_little_endian"},
        {"DIOD-DATA-02", SEC_DATA,
         "A string travels as length[2] followed by its bytes, with no terminating NULL.",
         "conf_wire_string_has_no_terminating_nul"},
        {"DIOD-DATA-03", SEC_DATA,
         "A qid is a 13 byte value: type[1] version[4] path[8].",
         "conf_wire_qid_is_thirteen_bytes"},

        {"DIOD-WIRE-01", SEC_VERSION,
         "Tversion is size[4] type[1] tag[2] msize[4] version[s].",
         "conf_wire_tversion_layout"},
        {"DIOD-WIRE-02", SEC_VERSION,
         "The version string of a 9P2000.L client is exactly 9P2000.L.",
         "conf_wire_tversion_layout"},
        {"DIOD-WIRE-03", SEC_VERSION,
         "size[4] counts the whole message, the size value itself included.",
         "conf_wire_size_field_is_the_message_length"},
        {"DIOD-WALK-01", SEC_WALK,
         "A fid is cloned to newfid by calling walk with nwname set to zero.",
         "conf_seq_walk_clone_returns_no_qid"},
        {"DIOD-WIRE-04", SEC_FLUSH, "Tflush is size[4] type[1] tag[2] oldtag[2].",
         "conf_wire_tflush_layout"},
        {"DIOD-WIRE-05", SEC_WALK,
         "Twalk is size[4] type[1] tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s]).",
         "conf_wire_twalk_layout"},
        {"DIOD-WIRE-06", SEC_READWRITE,
         "Tread is size[4] type[1] tag[2] fid[4] offset[8] count[4].",
         "conf_wire_tread_layout"},
        {"DIOD-WIRE-07", SEC_READWRITE,
         "Twrite is size[4] type[1] tag[2] fid[4] offset[8] count[4] data[count].",
         "conf_wire_twrite_layout"},
        {"DIOD-WIRE-08", SEC_READWRITE,
         "Rread is size[4] type[1] tag[2] count[4] data[count], Rwrite is size[4] "
         "type[1] tag[2] count[4].",
         "conf_wire_rread_and_rwrite_layout"},
        {"DIOD-WIRE-09", SEC_CLUNK, "Tclunk is size[4] type[1] tag[2] fid[4].",
         "conf_wire_tclunk_layout"},
        {"DIOD-WIRE-10", SEC_ATTACH,
         "Tattach is size[4] type[1] tag[2] fid[4] afid[4] uname[s] aname[s] n_uname[4].",
         "conf_wire_tattach_layout"},
        {"DIOD-WIRE-11", SEC_ATTACH,
         "Without an authentication handshake afid is P9_NOFID, that is ~0.",
         "conf_wire_tattach_layout"},
        {"DIOD-WIRE-12", SEC_ATTACH,
         "n_uname carries the uid of the user when it is not P9_NONUNAME.",
         "conf_wire_tattach_layout"},
        {"DIOD-WIRE-13", SEC_LERROR,
         "Rlerror is size[4] type[1] tag[2] ecode[4], and ecode is a Linux errno.",
         "conf_wire_rlerror_carries_a_linux_errno"},
        {"DIOD-WIRE-14", SEC_STATFS,
         "Rstatfs is size[4] type[1] tag[2] type[4] bsize[4] blocks[8] bfree[8] "
         "bavail[8] files[8] "
         "ffree[8] fsid[8] namelen[4].",
         "conf_wire_trstatfs_layout"},
        {"DIOD-WIRE-15", SEC_LOPEN,
         "Tlopen is size[4] type[1] tag[2] fid[4] flags[4], Rlopen is size[4] type[1] "
         "tag[2] qid[13] "
         "iounit[4].",
         "conf_wire_tlopen_layout"},
        {"DIOD-WIRE-16", SEC_LCREATE,
         "Tlcreate is size[4] type[1] tag[2] fid[4] name[s] flags[4] mode[4] gid[4], "
         "Rlcreate is "
         "size[4] type[1] tag[2] qid[13] iounit[4].",
         "conf_wire_tlcreate_layout"},
        {"DIOD-WIRE-17", SEC_GETATTR,
         "Tgetattr is size[4] type[1] tag[2] fid[4] request_mask[8].",
         "conf_wire_tgetattr_layout"},
        {"DIOD-WIRE-18", SEC_GETATTR,
         "Rgetattr is size[4] type[1] tag[2] valid[8] qid[13] mode[4] uid[4] gid[4] "
         "nlink[8] rdev[8] "
         "size[8] blksize[8] blocks[8] atime_sec[8] atime_nsec[8] mtime_sec[8] "
         "mtime_nsec[8] "
         "ctime_sec[8] ctime_nsec[8] btime_sec[8] btime_nsec[8] gen[8] data_version[8].",
         "conf_wire_rgetattr_layout"},
        {"DIOD-WIRE-19", SEC_SETATTR,
         "Tsetattr is size[4] type[1] tag[2] fid[4] valid[4] mode[4] uid[4] gid[4] "
         "size[8] "
         "atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8].",
         "conf_wire_tsetattr_layout"},
        {"DIOD-WIRE-20", SEC_READDIR,
         "Treaddir is size[4] type[1] tag[2] fid[4] offset[8] count[4], Rreaddir is "
         "size[4] "
         "type[1] tag[2] count[4] data[count].",
         "conf_wire_treaddir_layout"},
        {"DIOD-WIRE-21", SEC_READDIR,
         "A directory entry is the variable-length record qid[13] offset[8] type[1] "
         "name[s].",
         "conf_wire_readdir_record_layout"},
        {"DIOD-WIRE-22", SEC_MKDIR,
         "Tmkdir is size[4] type[1] tag[2] dfid[4] name[s] mode[4] gid[4], Rmkdir is "
         "size[4] "
         "type[1] tag[2] qid[13].",
         "conf_wire_tmkdir_layout"},
        {"DIOD-WIRE-23", SEC_SYMLINK,
         "Tsymlink is size[4] type[1] tag[2] fid[4] name[s] symtgt[s] gid[4], Rsymlink "
         "is size[4] "
         "type[1] tag[2] qid[13].",
         "conf_wire_tsymlink_layout"},
        {"DIOD-WIRE-24", SEC_READLINK,
         "Treadlink is size[4] type[1] tag[2] fid[4], Rreadlink is size[4] type[1] "
         "tag[2] target[s].",
         "conf_wire_treadlink_layout"},
        {"DIOD-WIRE-25", SEC_LINK,
         "Tlink is size[4] type[1] tag[2] dfid[4] fid[4] name[s], Rlink is size[4] "
         "type[1] tag[2].",
         "conf_wire_tlink_layout"},

        {"DIOD-SESS-01", SEC_SESSION,
         "A client negotiates the version and the msize, attaches, then reads the "
         "attributes of the "
         "mount point.",
         "conf_seq_version_attach_getattr"},
        {"DIOD-SESS-02", SEC_SESSION,
         "Opening a directory clones the fid with a walk of zero names and opens the "
         "clone.",
         "conf_seq_walk_clone_then_lopen"},
        {"DIOD-SESS-03", SEC_SESSION,
         "A readdir that returns no data ends the directory, and the fid is then "
         "clunked.",
         "conf_seq_readdir_ends_with_an_empty_reply"},
        {"DIOD-SESS-04", SEC_READDIR,
         "The first readdir starts at offset zero, the next one at the offset of the "
         "last entry.",
         "conf_seq_readdir_offset_is_the_last_entry_offset"},
        {"DIOD-SESS-05", SEC_SESSION,
         "Walking to a name that does not exist is answered with Rlerror and ecode 2, "
         "ENOENT.",
         "conf_diod_missing_name_is_enoent"},
        {"DIOD-WALK-02", SEC_WALK,
         "A walk that does not reach its target is an error for the client, and the "
         "session stays "
         "usable.",
         "conf_seq_walk_to_a_missing_name_fails"},
        {"DIOD-SESS-06", SEC_SESSION,
         "Creating a file clones the parent fid, creates the file in it, writes it and "
         "clunks it.",
         "conf_seq_create_write_clunk"},
        {"DIOD-SESS-07", SEC_SESSION,
         "Reading a file clones the fid, opens it, reads it and reads again to reach the "
         "end.",
         "conf_seq_read_then_eof_then_clunk"},
        {"DIOD-SESS-08", SEC_CLUNK,
         "After a clunk the fid is no longer usable by the client.",
         "conf_seq_clunk_releases_the_fid"},
        {"DIOD-SESS-09", SEC_VERSION,
         "No message the client sends is larger than the msize both sides agreed on.",
         "conf_seq_no_message_exceeds_the_negotiated_msize"},
        {"DIOD-SESS-10", SEC_ATTACH,
         "The attach selects the export named by aname: its files are the ones the "
         "client sees.",
         "conf_diod_attach_selects_the_export"},

        {"DIOD-SRV-01", SEC_SESSION,
         "Against the real diod server, version, attach and getattr succeed.",
         "conf_diod_version_attach_getattr"},
        {"DIOD-SRV-02", SEC_SESSION,
         "Against the real diod server, a directory is cloned, opened, read and clunked.",
         "conf_diod_readdir_session"},
        {"DIOD-SRV-04", SEC_SESSION,
         "Against the real diod server, a file is created, written and clunked.",
         "conf_diod_create_write_clunk"},
        {"DIOD-SRV-05", SEC_LERROR,
         "The ecode diod sends is a Linux errno the client can report.",
         "conf_diod_rlerror_is_a_linux_errno"},
};

/* The cases of the whole table. The diod ones live in another binary: the
 * name is listed here so that the table bench can check that every case is
 * named by a rule, whichever binary implements it. */
static const char *const g_cases[] = {
        "conf_wire_integers_are_little_endian",
        "conf_wire_string_has_no_terminating_nul",
        "conf_wire_qid_is_thirteen_bytes",
        "conf_wire_tversion_layout",
        "conf_wire_size_field_is_the_message_length",
        "conf_wire_tflush_layout",
        "conf_wire_twalk_layout",
        "conf_wire_tread_layout",
        "conf_wire_twrite_layout",
        "conf_wire_rread_and_rwrite_layout",
        "conf_wire_tclunk_layout",
        "conf_wire_tattach_layout",
        "conf_wire_rlerror_carries_a_linux_errno",
        "conf_wire_trstatfs_layout",
        "conf_wire_tlopen_layout",
        "conf_wire_tlcreate_layout",
        "conf_wire_tgetattr_layout",
        "conf_wire_rgetattr_layout",
        "conf_wire_tsetattr_layout",
        "conf_wire_treaddir_layout",
        "conf_wire_readdir_record_layout",
        "conf_wire_tmkdir_layout",
        "conf_wire_tsymlink_layout",
        "conf_wire_treadlink_layout",
        "conf_wire_tlink_layout",
        "conf_seq_version_attach_getattr",
        "conf_seq_walk_clone_returns_no_qid",
        "conf_seq_walk_clone_then_lopen",
        "conf_seq_readdir_ends_with_an_empty_reply",
        "conf_seq_readdir_offset_is_the_last_entry_offset",
        "conf_seq_walk_to_a_missing_name_fails",
        "conf_seq_create_write_clunk",
        "conf_seq_read_then_eof_then_clunk",
        "conf_seq_clunk_releases_the_fid",
        "conf_seq_no_message_exceeds_the_negotiated_msize",
        "conf_diod_version_attach_getattr",
        "conf_diod_readdir_session",
        "conf_diod_missing_name_is_enoent",
        "conf_diod_create_write_clunk",
        "conf_diod_rlerror_is_a_linux_errno",
        "conf_diod_attach_selects_the_export",
};

/* The cases registered in the running binary. The anchor keeps the section
 * alive in every binary that links this file, even one with no case. */
extern const pgw_conf_case __start_pgw_conf_cases[];
extern const pgw_conf_case __stop_pgw_conf_cases[];

static const pgw_conf_case pgw_conf_case_anchor
        __attribute__((used, section("pgw_conf_cases"))) = {"", NULL};

#ifndef PGW_CONFORMANCE_EXCERPT
#define PGW_CONFORMANCE_EXCERPT ""
#endif

size_t pgw_conf_rule_count(void)
{
    return sizeof g_rules / sizeof g_rules[0];
}

const pgw_conf_rule *pgw_conf_rule_at(size_t index)
{
    return index < pgw_conf_rule_count() ? &g_rules[index] : NULL;
}

const pgw_conf_rule *pgw_conf_rule_find(const char *id)
{
    if (id == NULL)
        return NULL;
    for (size_t i = 0; i < pgw_conf_rule_count(); i++) {
        if (strcmp(g_rules[i].id, id) == 0)
            return &g_rules[i];
    }
    return NULL;
}

size_t pgw_conf_case_count(void)
{
    return sizeof g_cases / sizeof g_cases[0];
}

const char *pgw_conf_case_name(size_t index)
{
    return index < pgw_conf_case_count() ? g_cases[index] : NULL;
}

bool pgw_conf_case_exists(const char *name)
{
    if (name == NULL)
        return false;
    for (size_t i = 0; i < pgw_conf_case_count(); i++) {
        if (strcmp(g_cases[i], name) == 0)
            return true;
    }
    return false;
}

size_t pgw_conf_registered_count(void)
{
    size_t n = 0;
    for (const pgw_conf_case *c = __start_pgw_conf_cases; c < __stop_pgw_conf_cases;
         c++) {
        if (c->name != NULL && c->name[0] != '\0')
            n++;
    }
    return n;
}

const pgw_conf_case *pgw_conf_registered_at(size_t index)
{
    size_t n = 0;
    for (const pgw_conf_case *c = __start_pgw_conf_cases; c < __stop_pgw_conf_cases;
         c++) {
        if (c->name == NULL || c->name[0] == '\0')
            continue;
        if (n == index)
            return c;
        n++;
    }
    return NULL;
}

const char *pgw_conf_excerpt_path(void)
{
    const char *env = getenv("PGW_CONFORMANCE_EXCERPT");
    if (env != NULL && env[0] != '\0')
        return env;
    if (PGW_CONFORMANCE_EXCERPT[0] != '\0')
        return PGW_CONFORMANCE_EXCERPT;
    return "tests/conformance/diod-protocol-excerpt.md";
}

const char *pgw_conf_excerpt_url(void)
{
    return "https://github.com/chaos/diod/blob/master/protocol.md";
}
