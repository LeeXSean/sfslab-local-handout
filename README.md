# SFS Lab

A self-study version of the Shark File System lab from CMU 15-213 / 15-513.
Implement three file APIs, make them thread-safe, then explore parallel speedup.
This is an independent port, not an official release; see [NOTICE.md](NOTICE.md).

## Getting started

Use Linux or WSL. From the repository root:

```sh
make
make test
```

Read [the lab writeup](sfslab.pdf) and [the handout README](sfslab/README).
Edit `sfslab/sfs-disk.c`: `sfs_getpos`, `sfs_seek`, and `sfs_rename` are
intentionally unfinished. Add synchronization after those functions work.
The starter fails tests by design; concurrent failures vary until you add locks.

## Learning path

1. Implement the three APIs and pass the sequential tests.
2. Start with one mutex and pass all 12 correctness traces. Ordinary tests make
   real concurrent calls; ThreadSanitizer adds race diagnostics when available.
3. Optionally refine the locks and run `make -C sfslab grade` to compare speedup.
   The local 22-point benchmark is an exercise, not CMU's official grading scale.
   A correct coarse-lock implementation completes the basic exercise.

Use `main` for the basic lab. The `developer` branch adds file sizing,
expandable directories, and Unix-style unlink; these are optional extensions.

**This is the extension branch.** See [maintainer notes](docs/maintainer/DEVELOPER.md).

## Files and maintenance

- `sfslab/`: starter sources, test driver, and disk checker.
- `sfslab.pdf`: lab writeup; `writeup/sfslab.roff` is its source.
- `sfslab-handout.tar`: offline package. Extract in a new directory with
  `tar xf sfslab-handout.tar`, then run `make` inside `sfslab/`.

`make pdf dist` rebuilds the writeup and package. After `make`, run
`python3 tests/check_grader.py` to check the test infrastructure.
Each test run uses a private `sfs-test.*` directory; failure images are retained
there for inspection. `make clean` removes build products, not disk images.

Keep completed student solutions in your own private working copy.
