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
sfslab-handout.tar   packaged copy of sfslab/ (what a student downloads)
sfslab.pdf           the lab writeup
sfslab/              handout working directory (source of the tarball)
writeup/             groff source for sfslab.pdf (make pdf)
```

Maintainer targets, from the repository root: `make dist` rebuilds the
tarball deterministically; `make pdf` rebuilds the writeup.

## Disclaimer

Original SFS lab materials belong to Carnegie Mellon University and the
15-213 / 15-513 course staff. This repository is an independent self-study
port with no CMU affiliation. Do not publish completed `sfs-disk.c`
solutions. See `NOTICE.md`.
