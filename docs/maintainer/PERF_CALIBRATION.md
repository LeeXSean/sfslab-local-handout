# Performance Scoring Calibration

This note records how the performance ratio ladder in
`sfslab/local/test-sfs.c` (`score_perf_against_baseline`) was calibrated, so
the next recalibration can repeat the method instead of reverse-engineering
it. It contains measurements and method only -- no solution source. The
reference implementations used for calibration live outside the repository
and must never be committed or shipped in the handout tarball.

## Why the 2026-06 revision happened

Two compounding flaws made the old performance score reward the wrong thing:

1. **The timed region was mostly thread startup.** The old workload was
   8 threads x 100 iterations of open/write/seek/read/close with the clock
   started before `pthread_create`. Spawning and joining 8 no-op threads
   costs ~200-290us on the calibration machine -- *more than the filesystem
   work itself* (the whole 800-iteration run completed in ~350us). Measured
   spread at that size was 97-125% even with a start barrier; the reported
   ops/sec was scheduler noise.
2. **The ladder gave 10/10 at >= 0.90x.** The baseline binary
   (`test-sfs-baseline`) is the handout implementation under one global
   mutex. A correct student solution that keeps a single global mutex *is*
   the baseline, measures ~1.0x by construction, and therefore earned full
   performance marks. Fine-grained locking -- the lab's stated goal -- was
   never rewarded by a single point.

Verified empirically before the fix: a correct coarse-lock solution scored
22/22 (ratio 1.31-1.47x, pure jitter).

## The revised benchmark

Shape (see `PERF_*` constants in `local/test-sfs.c`, workload tag `v2`):

- 8 threads, each against its own file (`perf<N>.txt`).
- Per thread: 2000 sessions; one session = `sfs_open`, then 10 rounds of
  write/seek/getpos/read, then `sfs_close`. 672,000 API calls per run total;
  one reported "op" = one API call.
- Timing starts at a `pthread_barrier` released after all threads exist, so
  spawn cost is out of the measurement.
- 5 samples, median scored (`make baseline` does the same for the
  denominator). `.perf_baseline` records `WORKLOAD=v2`; a mismatching or
  missing tag falls back to absolute throughput capped at 5/10.

Amortizing open/close 1:10 against per-file I/O keeps the necessarily-shared
fd table from dominating while still charging for it, so the ratio chiefly
measures whether operations on *different files* actually run in parallel.

## Calibration measurements (2026-06-12)

Machine: WSL2, 20 hardware threads, images on ext4 (no `SFS_DISK_DIR`
override). All numbers are medians of 7 probe runs (probe = same workload in
a standalone driver) or of the 5-sample grader run; spreads in parentheses.

| implementation | throughput | ratio vs baseline |
|---|---|---|
| baseline: handout + one global mutex (stubs ENOSYS) | 12.1M calls/s (7.9%) | 1.00x |
| correct solution, one global mutex | 12.4M calls/s (5.7%) | **1.03x** |
| correct solution, fine-grained locking | 62.4M calls/s (6.1%) | **5.16x** |

The fine-grained calibration solution uses: a directory rwlock (read for
lookup/list, write for create/remove/rename), one mutex per directory slot
covering that file's size/blocks/data and fd positions, a mutex for the
open-file tables, and a leaf mutex for the freelist, with a fixed
lock order. It passes the full graded suite (12/12, TSan clean across the
3-seed schedule-fuzz sweep, fsck clean, S00/S01, model fuzz).

Also measured, to understand the floor: with the *old* 100-iteration
workload the same fine-grained solution was indistinguishable from coarse
(2.8M vs 2.2M "ops/sec", spreads ~100%); and an intermediate design that
keeps a global mutex around the fd table on every open/close tops out near
1.4x, which informed the 1.20/1.40 middle rungs.

## The ladder and its margins

| ratio | score | anchor |
|---|---|---|
| >= 2.50 | 10 | fine-grained measured 5.16x (2x margin); ~2.5-3x expected on 4-core machines, so 10/10 stays reachable on small CPUs |
| >= 1.80 | 9  | clearly past any global-lock design |
| >= 1.40 | 7  | partial parallelism (e.g. per-file data locks with a hot global fd path) |
| >= 1.20 | 5  | measurably better than one global lock |
| >= 0.85 | 3  | coarse-lock territory; the baseline itself is 1.0x and run-to-run ratio noise is well under +/-15% at this work size |
| < 0.85  | 0  | slower than the trivial solution |

Coarse measured 1.03x with <6% spread, so the 1.20 bar for the first paid
rung is several noise standard deviations away -- a global mutex cannot
fluke into 5/10.

## How to recalibrate (next time the workload or thresholds change)

1. Keep (or rebuild) two private reference solutions outside the repo:
   coarse global mutex, and per-file fine-grained as described above. Both
   must pass the full graded suite first.
2. Run `make baseline` and the full grader in each solution's tree,
   back-to-back in one quiet session (interleave runs if the machine
   drifts). Use the printed medians and spreads.
3. Bump `PERF_WORKLOAD_VERSION` in `local/test-sfs.c` *and* the `WORKLOAD=`
   line in the `baseline` Makefile target whenever the workload changes
   shape or size -- this invalidates every existing `.perf_baseline`
   gracefully (capped fallback + re-run message) instead of silently
   producing cross-workload ratios.
4. Place the 10/10 bar at no more than half what the fine-grained reference
   measures on the calibration machine, and sanity-check it against the
   smallest CPU you care about (parallel speedup is bounded by core count).
5. Update: writeup section 5.1 table, SCORING.md revision note, this file.
