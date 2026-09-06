# sfslab

A personal self-study port of the Shark File System lab from CMU 15-213 / 15-513.
Implement three file APIs, make them thread-safe, then optionally explore parallel speedup.

## Contents

| Path | Contents |
| --- | --- |
| `sfslab/` | Starter sources, test driver, and disk checker |
| `sfslab.pdf` | Lab writeup |
| `writeup/sfslab.roff` | Source for the writeup |
| `sfslab-handout.tar` | Offline starter package |
| `tests/check_grader.py` | Checks for the test infrastructure |

## Usage

Use Linux or WSL. From the repository root:

```sh
make
make test
```

Read [the lab writeup](sfslab.pdf) and [the handout README](sfslab/README).
Edit `sfslab/sfs-disk.c`: `sfs_getpos`, `sfs_seek`, and `sfs_rename` are intentionally unfinished.

## Learning Path

1. Implement the three APIs and pass the sequential tests.
2. Start with one mutex and pass all 12 correctness traces. Ordinary tests make
   real concurrent calls; ThreadSanitizer adds race diagnostics when available.
3. Optionally refine the locks and run `make -C sfslab grade` to compare speedup.
   A correct coarse-lock implementation completes the basic exercise.

Use `main` for the basic lab. The `developer` branch adds file sizing,
expandable directories, and Unix-style unlink as optional extensions.

## Local Notes

- The starter fails tests by design; concurrent failures vary until locks are added.
- The local 22-point benchmark is an exercise, not CMU's official grading scale.
- Each run uses a private `sfs-test.*` directory and retains failure images there.
  `make clean` removes build products, not disk images.
- `make pdf dist` rebuilds the writeup and package. After `make`, run
  `python3 tests/check_grader.py` to check the test infrastructure.
- Extract the offline package in a new directory with `tar xf sfslab-handout.tar`,
  then run `make` inside `sfslab/`.
- Keep completed student solutions in your own private working copy.

## Sources

This is an independent port of CMU's Shark File System lab, not an official
release or a repository affiliated with the course. See [NOTICE.md](NOTICE.md)
for provenance and the boundary between the starter and completed solutions.
