# Offline Self-Study Notes

The `sfslab/` directory is arranged to feel close to the CMU SFS handout while
still being runnable without CMU infrastructure. These notes are kept outside
the packaged handout so students see a smaller, assignment-style document set.

## What Matches the Handout Shape

- `sfs-disk.c` is the student file.
- `sfs-api.h`, `sfs-disk.h`, `sfs-support.c`, and `sfs-fsck.c` are provided
  support files.
- `.clang-format`, `.gitignore`, `.labname.mk`, and `README` mirror the kind
  of metadata present in a course handout.

## Local-Only Replacements

- `local/test-sfs.c` is a local C autograder. It replaces the course trace
  runner.
- `local/sfs-baseline-ref.c` is a coarse-lock reference used only for local
  performance calibration.
- `traces/` contains Lua-style trace fixtures for documentation and local
  execution.
- `make starter-safe` is the starter health check: it runs the C smoke traces
  and the starter-safe Lua traces. From the repository root, Lua traces fall
  back to Docker when local Lua development dependencies are missing.
- `make trace-smoke` executes the starter-safe Lua traces. `make trace-run`
  executes the full Lua-style catalog and is expected to fail on the
  unmodified skeleton. The local binding implements `lanes.gen` with pthreads
  and separate Lua states, so C traces make real concurrent calls into the SFS
  API.
- `make trace-json` reports the Lua trace catalog as separate diagnostic
  coverage, not as part of the local 22-point score.
- `make report-json` combines the local score, Lua trace coverage, stress
  diagnostics, and a fixed-seed model-fuzz section into one machine-readable
  report. Only the local autograder section is the graded 22-point score.
- The repository root `Dockerfile` provides a Linux toolchain for builds,
  ThreadSanitizer runs, and Lua trace execution. Root trace targets use it as
  a fallback when the host is missing Lua headers or `pkg-config`.
- `SCORING.md` explains why the local score is a self-study signal, not
  an official score prediction.
- `make smoke`, `make grade`, and `make trace-check` are convenience targets
  for local validation. `make check` is the normal development health check;
  `make dist-check` is the stricter package cleanliness check.
- `make stress` repeats the C concurrency traces and extra stress-only
  diagnostics (S00 directory churn, S01 rename atomicity) to expose
  nondeterministic failures after you have an implementation to test; it is
  not a starter health check.
- `make model-fuzz` runs a seed-reproducible random op sequence against an
  in-memory reference model, with periodic fsck checkpoints. Diagnostic only.
- `make x-traces` runs the unscored optional X challenges referenced by the
  "optional challenge" comments in `sfs-disk.c`.
- `make baseline` writes `.perf_baseline` (tagged with the benchmark workload
  version) so performance is scored against the current machine instead of
  CMU's Autolab hardware; without it the performance score is capped at 5/10.
- ThreadSanitizer is used locally as the race detector when available.

## Academic Integrity Boundary

Do not copy an existing solution into `sfs-disk.c` if your goal is self-study.
The offline grader is meant to help you test your own implementation, not to
turn a submitted or third-party solution into a polished answer.

The starter functions in `sfs-disk.c` intentionally remain incomplete:

- `sfs_getpos`
- `sfs_seek`
- `sfs_rename`

Treat those as the warm-up portion of the lab.
