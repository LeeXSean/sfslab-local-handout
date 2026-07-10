# sfslab-local-handout

Offline self-study packaging for CMU 15-213 / 15-513's Shark File System
(SFS) lab. This is not an official CMU release; see `NOTICE.md`.

## Getting Started

Run this inside Linux, WSL, or a Linux container:

```bash
tar xf sfslab-handout.tar
cd sfslab
make
./test-sfs
```

Read `sfslab.pdf` (the lab writeup) first, then `sfslab/README`. The file
you edit is `sfslab/sfs-disk.c`.

The starter intentionally leaves `sfs_getpos`, `sfs_seek`, and `sfs_rename`
unimplemented, so a fresh handout fails part of the autograder. That is the
lab, not a packaging bug.

## Repository Layout

```text
sfslab-handout.tar   packaged copy of sfslab/ plus sfslab.pdf
sfslab.pdf           the lab writeup
sfslab/              handout working directory (source of the tarball)
writeup/             groff source for sfslab.pdf (make pdf)
```

Maintainer targets, from the repository root: `make dist` rebuilds the
tarball deterministically; `make pdf` rebuilds the writeup.

The `developer` branch additionally implements the historical file-size and
optional-design routes. They are unscored and exercised by `make
developer-test` and `make x-traces`; see `docs/maintainer/DEVELOPER.md`. Use
`main` for the course-shaped starter.

## Disclaimer

Original SFS lab materials belong to Carnegie Mellon University and the
15-213 / 15-513 course staff. This repository is an independent self-study
port with no CMU affiliation. Do not publish completed `sfs-disk.c`
solutions. See `NOTICE.md`.
