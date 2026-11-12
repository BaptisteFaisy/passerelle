/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_decode.h - the depth of an input, shared by the two
 * families of decoders.
 *
 * The campaigns of this directory (ROB-06, P9-09) and the replay of the kept
 * corpus must exercise exactly the same code, so the decoders live in a
 * header instead of being written twice: fuzz_p9_decode.h for the 9P
 * decoders, fuzz_fuse_decode.h for the FUSE ones.
 *
 * The depth is how far an input went into the decoders. It is what the
 * corpus selection uses to keep the interesting inputs (see fuzz_corpus.h):
 * the shallower ones are already covered by the campaigns themselves.
 */
#ifndef FUZZ_DECODE_H
#define FUZZ_DECODE_H

/* How far an input went into the decoders. */
#define FUZZ_DEPTH_NONE 0 /* the header itself was refused */
#define FUZZ_DEPTH_HDR 1  /* the header was accepted, the type unknown */
#define FUZZ_DEPTH_BODY 2 /* the type was known, the body was refused */
#define FUZZ_DEPTH_FULL 3 /* a message was decoded entirely */

#endif /* FUZZ_DECODE_H */
