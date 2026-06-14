# sfslab-local-handout

Offline self-study packaging for CMU 15-213 / 15-513's Shark File System
(SFS) lab.

This is not an official CMU release. The goal is to keep the student-facing
handout close to the course shape while replacing CMU-only infrastructure with
local tools.

## Who Should Read This

Students should start inside the handout:

```bash
cd sfslab
less README
```

The file to edit is `sfslab/sfs-disk.c`. The `docs/` directory is for local
packaging notes, report schemas, examples, and maintainer workflows; it is not
required for doing the lab.

## Package Quick Start

Run this inside Linux, WSL, or a Linux container:

```bash
tar xf sfslab-handout.tar
cd sfslab
make check
```

Then read `README` inside `sfslab/`.

## Useful Commands

From the repository root:

```bash
make doctor       # explain local toolchain readiness and fallbacks
make check        # development health check
make json         # local 22-point score as JSON
make report-json  # score plus diagnostics
make stress       # repeat concurrency traces + stress diagnostics (S00/S01)
make model-fuzz   # differential model fuzz (seed-reproducible diagnostic)
make x-traces     # optional unscored X challenges
make dist-verify  # rebuild and unpack-check sfslab-handout.tar
```

Docker wrappers such as `make docker-check` and `make docker-report-json` are
available when the local toolchain is not installed.

On Ubuntu or WSL, the fuller local toolchain is:

```bash
sudo apt-get install build-essential make shellcheck clang-format pkg-config lua5.4 liblua5.4-dev
```

## Project Layout

```text
sfslab-local-handout/
  sfslab/                 # handout-style working directory
  sfslab-handout.tar      # packaged copy of sfslab/
  docs/report-schema.md       # report-json field reference
  docs/offline-self-study.md  # local-only handout notes
  docs/examples/              # sample machine-readable reports
  docs/maintainer/            # release and repository maintenance notes
```

Inside `sfslab/`, the main files are:

- `README`: student starting point and recommended work order.
- `SFS_Lab_Writeup.md`: lab narrative.
- `SCORING.md`: local score explanation, useful after you start testing.
- `traces/`: executable Lua-style diagnostic traces, mostly for deeper checks.

## Starter Boundary

The local 22-point score is frozen at A=5, B=4, C=3, Performance=10. Lua trace
coverage, stress diagnostics, model fuzz, and the optional X traces are
reported separately and do not raise the main score. (The performance
*thresholds* inside those 10 points were deliberately recalibrated in 2026-06
so that fine-grained locking is what earns full performance credit; see
`sfslab/SCORING.md`.)

The starter intentionally leaves `sfs_getpos`, `sfs_seek`, and `sfs_rename`
incomplete. Those are part of the self-study work, not a packaging bug.
On a fresh starter, `make report-json` should produce valid JSON while reporting
that the graded score did not pass.

## Maintainers

Before publishing a new handout tarball, run the checklist in
`docs/maintainer/RELEASE.md`. The repository-only review material should not be
included inside `sfslab-handout.tar`.

## Disclaimer

Original SFS lab materials belong to Carnegie Mellon University and the
15-213 / 15-513 course staff. This repository is an independent self-study
port with no CMU affiliation. Do not publish completed `sfs-disk.c` solutions.
See `NOTICE.md` for the release boundary.
