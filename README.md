# sfslab

A self-study port of CMU 15-213 / 15-513's Shark File System lab.
Complete three file APIs, then make them thread-safe.

[Writeup](sfslab.pdf) · [Starter](sfslab/) · [Offline handout](sfslab-handout.tar)

## Use

Linux or WSL. Read the writeup and [handout README](sfslab/README), then implement
`sfs_getpos`, `sfs_seek`, and `sfs_rename` in [sfs-disk.c](sfslab/sfs-disk.c).
Run from the repository root:

```sh
make
make test
```

The starter fails tests until the APIs and synchronization are implemented.
A single mutex and all 12 correctness traces complete the basic exercise.
The concurrent traces make real parallel calls; ThreadSanitizer adds race
diagnostics when available.

## Notes

- Use `main` for the basic lab. The `developer` branch adds optional file sizing,
  directory growth, and Unix-style unlink.
- `make -C sfslab grade` measures optional parallel speedup. Its local 22-point
  scale is not CMU's official grading scale.
- Test runs use private `sfs-test.*` directories and retain failure images.
  `make clean` removes build products, not disk images.
- Extract the offline handout in a new directory with `tar xf sfslab-handout.tar`,
  then run `make` inside `sfslab/`.
- `make pdf dist` rebuilds the writeup and handout. After a build,
  `python3 tests/check_grader.py` checks the test infrastructure.

Keep completed student solutions private. This is an independent port;
[NOTICE.md](NOTICE.md) records its provenance and the starter/solution boundary.
