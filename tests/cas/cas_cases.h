/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cas_cases.h - the case bodies shared by the adapters under test.
 *
 * A case body takes a cas_view (the layer under test, the directory that
 * holds the truth and the caller's identity) and nothing else, so the very
 * same assertions run over the POSIX backend, over the 9P adapter and over
 * the FUSE adapter. The binaries that drive them live in tests/cas (POSIX
 * and 9P) and in tests/fuse_e2e (FUSE): the bodies are written once, here.
 *
 * Covers ROB-04 (gap 19: a file replaced on the PC while it is open) and
 * the hard-link semantics of gap 68 (link count, identity, unlink, rename
 * and link onto an existing target, and the attribute cache that must not
 * keep a stale link count).
 */
#ifndef PGW_TESTS_CAS_CASES_H
#define PGW_TESTS_CAS_CASES_H

#include "cas.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- ROB-04: the file is replaced on the PC while it is open ------------- */

/* The editor pattern: a new file is renamed over the old one while a
 * descriptor is open on the old one. The descriptor keeps the old inode, a
 * new descriptor sees the new file, and the attributes of the name describe
 * the new file. */
void cas_case_replace(const cas_view *v);

/* The same inode, emptied and rewritten: the open descriptor sees the new
 * content, because the inode did not change. */
void cas_case_truncate(const cas_view *v);

/* The file is unlinked while it is open: the name disappears, the inode
 * survives until the descriptor is closed. */
void cas_case_unlink(const cas_view *v);

/* The attribute cache must not serve the size or the mtime of a file that
 * the PC replaced behind the share's back. */
void cas_case_cache_stale(const cas_view *v);

/* --- gap 68: the hard-link semantics ------------------------------------- */

/* Two names, one inode: the link count of both, the identity, and a write
 * through one name seen through the other. */
void cas_case_link_identity(const cas_view *v);

/* Unlinking one name leaves the other intact, with a decremented count. */
void cas_case_link_unlink(const cas_view *v);

/* Renaming a name onto an existing one, and renaming a name onto another
 * name of the same inode (POSIX: nothing happens, both names stay). */
void cas_case_link_rename(const cas_view *v);

/* Linking onto a name that already exists is refused, and the link count of
 * the file does not change. */
void cas_case_link_target(const cas_view *v);

/* The attribute cache must not keep a stale link count, neither after a link
 * made through the share nor after an unlink made on the PC. */
void cas_case_link_cache(const cas_view *v);

#ifdef __cplusplus
}
#endif

#endif /* PGW_TESTS_CAS_CASES_H */
