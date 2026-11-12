# The kept corpora

Two kinds of regression data live here, both replayed by the normal CTest
suite, on every pull request.

## `*.pgwcase` — the divergences of the differential bench (gap 62)

A `*.pgwcase` file is a divergence that `tests/e2e` found and reduced to its
shortest prefix (`diff_reduce`), saved by `tools/corpus-save.sh` with
`PGW_CORPUS_SAVE` pointing at this directory. The format is documented in
`tests/e2e/corpus.h`: a seed, an operation count and a digest of the
operations, so the case replays byte for byte without storing the operations
themselves. `tests/corpus/test_corpus.c` replays them.

## `fuzz/` — the inputs the fuzzing campaigns found interesting (gap 48)

The campaigns of `tests/fuzz/` regenerate their input from a seed, so nothing
of them survives a run: an input that made a decoder misbehave would simply
be found again, with nothing to replay. `fuzz/` keeps the interesting inputs
as **data**, and `tests/fuzz/fuzz_corpus_test.c` replays them in the normal
CTest suite (a few dozen small files, well under a second).

### The selection rule

One file per **(family, depth)**:

* the *family* is the kind of message the campaign built (`version`, `walk`,
  `read`, `getattr`, `statfs` for 9P; `init`, `lookup`, `getattr`, `read`,
  `statfs` for FUSE) plus a `noise` family for the pure random bytes;
* the *depth* is how far the input went into the decoders:
  `0` the header was refused, `1` the header was accepted but the type is
  unknown, `2` the type was known but the body was refused, `3` the message
  decoded entirely;
* the first input of a campaign that reaches a given depth wins, which makes
  the selection reproducible from the seed.

An input that made a decoder *misbehave* — a header accepted out of contract
— is not a corpus entry: it is a failure of the campaign, reported as such.
The fix that follows is what the corpus then keeps.

### The cap

The corpus is a regression artefact, not a data set. It stays small enough to
live in the repository:

* at most **4 files per family** (one per depth) and at most **16 families**;
* at most **512 bytes per file**, at most 256 files, at most 256 KiB in
  total;
* the committed corpus is **33 files** for about 2 KiB of data.

`tests/fuzz/fuzz_corpus_test.c` enforces the cap: a corpus that grew past it
fails the test instead of quietly getting heavier.

### Regenerating it

The campaigns write the corpus when `PGW_FUZZ_CORPUS_DIR` points at the
directory that must receive it. The seeds are fixed, so the result is
reproducible:

```sh
cmake -B build && cmake --build build -j8
PGW_FUZZ_CORPUS_DIR=tests/corpus/fuzz ./build/tests/fuzz_p9
PGW_FUZZ_CORPUS_DIR=tests/corpus/fuzz ./build/tests/fuzz_fuse
```

Two runs with the same seeds give the same files, byte for byte. The nightly
campaign (`nightly.yml`) runs the same campaigns with `PGW_FUZZ_CORPUS_DIR`
set and uploads the corpus as an artefact, so a maintainer can compare a
nightly corpus with the committed one and take what is new.
