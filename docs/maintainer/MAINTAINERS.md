# Maintainer Notes

These notes are for maintaining the offline self-study port. They are not part
of the student solution.

## Student API Boundary

`sfs-api.h` is the lab contract. Keep it stable unless the whole handout,
autograder, trace runner, reference implementation, and writeup are changed
together.

The historical in-header TODO mentioned adding file-size operations such as
`fstat` and `ftruncate`. Do not make those required for the current local score.
They would add new allocation and truncation semantics that are outside the
existing lab surface.

If file-size operations are added later, keep them as optional local extensions:

- expose them through a separate target or report section;
- keep them out of the 22-point local score;
- add Lua runner bindings, C tests, reference behavior, and writeup text before
  publishing them.

The historical TODO also mentioned wrapping SFS file descriptors in a C newtype.
Keep the public API as `int fd`, because the existing handout, tests, and trace
style all use that shape. If fd confusion becomes a real maintenance issue,
prefer internal helper functions in the local test harness over changing the
student-facing function signatures.

## Inherited Support TODOs

`sfs-fsck.c` still contains small inherited FIXME comments around ASCII name
assumptions and block-device sizing. They are not part of the student task and
do not affect the local grading path. Treat them as support-tool maintenance,
not as assignment requirements.

## 2026-06 Revision Record

Landed 2026-06-12. Everything below is diagnostic-only except the one
deliberate scoring revision called out explicitly.

- **`freeBlocks` fix (handout + baseline-ref).** Prepending a freed chain to
  the free list did not re-aim the old head's `prev_block`, leaving the
  doubly linked free list inconsistent. `sfs-fsck` flags exactly this, so a
  spec-correct student solution failed A04/B01 through no fault of its own
  (B00 only survived because each remove was immediately followed by an open
  that reallocated the stale head). Fixed identically in `sfs-disk.c` and
  `local/sfs-baseline-ref.c`; the three `-ENOSYS` stubs are untouched, and
  the lab is now actually solvable to 12/12.
- **Deliberate performance scoring revision.** Weights unchanged (Perf is
  still 10 of 22). The old benchmark's timed region was dominated by thread
  spawn and the ladder gave 10/10 at >= 0.90x, so a coarse global mutex
  earned full marks. The v2 workload barriers out spawn cost and amortizes
  open/close over per-file I/O; the new ladder pays 3/10 at ~1.0x (coarse)
  and 10/10 from 2.5x up. Missing or workload-mismatched `.perf_baseline`
  now caps at 5/10. Method and measurements:
  `docs/maintainer/PERF_CALIBRATION.md`; student-facing text: writeup 5.1
  and SCORING.md.
- **New diagnostics (unscored).** S01 rename-atomicity stress trace (stress
  max 1 -> 2); seeded schedule fuzz (`--sched-fuzz[=SEED]`, used by the
  TSan sweep, which now runs three fuzzed schedules and reports the failing
  seed); differential model fuzz (`--model-fuzz[=SEED]`, `make model-fuzz`,
  fixed-seed `model_fuzz` section in report-json); failing-trace disk image
  snapshots (`fail_<id>_*.img`) paired with the new `sfs-fsck --dump` mode.
- **X traces.** The three "optional challenge" comments in `sfs-disk.c` now
  have unscored X00/X01/X02 traces (`make x-traces`; writeup section 7).
  They print after the scoreboard, appear in JSON as `x_traces` with
  `"graded": false`, and never appear in `--list-traces`, so the manifest
  contract is untouched. X02 intentionally conflicts with B02's documented
  `-EBUSY` semantics; the writeup frames it as a post-grading exploration.
- **TSan on hosts with high-entropy ASLR.** Persistent "unexpected memory
  mapping" failures retry under `setarch -R` automatically and otherwise
  degrade to `unavailable` (no penalty) instead of zeroing Category C.

## Core Next Work

- Run `docs/maintainer/RELEASE.md` before publishing a handout tarball.
- Keep `make lint-strict` in CI so shell scripts are ShellCheck-clean and the
  release toolchain includes clang-format. `make lint` may warn and skip
  optional tools on smaller local setups. Use `SFS_LINT_C_FORMAT_FILES` only for
  C files that are intentionally clang-format clean; do not mass-format the
  inherited handout sources without a deliberate review.
- Keep `make report-json` useful for dashboards and humans: it should exit 0
  when report generation succeeds, even if the starter or student solution has
  failing graded traces. Use `make report-json-strict` when CI should fail on
  the graded score.
- Keep `make dist-verify` as the release gate that rebuilds, extracts, and
  smoke-checks `sfslab-handout.tar`.
- Keep `make dist-repro-check` passing so release tarballs remain reproducible.
- Add more correctness traces before changing score weights.
- Keep the local 22-point score contract frozen unless there is a deliberate
  scoring revision; new checks should usually be diagnostics.
- Keep Lua trace coverage diagnostic until it matches the C autograder's
  concurrency signal.
- Use the trace manifest's starter metadata to keep smoke checks aligned with
  the Lua catalog.
- Keep schedule-sensitive concurrency churn in stress-only diagnostics, not in
  the main local score.
- Keep report formatting in `test-report.c` so `test-sfs.c` stays focused on
  trace behavior.
- Prefer local reporting and reproducibility improvements over expanding the
  public SFS API.
