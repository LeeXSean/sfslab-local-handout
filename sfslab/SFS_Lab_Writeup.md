# SFS Lab: Writing a Multithreaded Filesystem

> **Based on CMU 15-213/15-513 (Summer 2024)**
> Adapted for local development on Ubuntu 18.04
>
> **Disclaimer:** All original course materials, including the SFS filesystem
> codebase and lab design, are the intellectual property of Carnegie Mellon
> University and the 15-213/15-513 course staff. This adaptation is a personal
> self-study project by an enthusiast who deeply admires CMU's systems
> curriculum but has no opportunity to take the course on campus. No
> affiliation with CMU is implied. If this repository infringes on any
> rights, please open an issue and it will be taken down immediately.

---

## 1 Introduction

A file system is a data structure that the operating system uses to store, maintain, and refer to
meaningful data on the disk, and conversely to derive usable information from the raw data on the
disk. File systems allow programs to interact with data as abstract files, leaving the cumbersome
work of putting that data onto the physical disk to lower-level calls. File systems are also simple
but powerful tools for organization (with folders, linking, etc.) and security (with permissions).

In this lab, we provide you with a very basic file system, which does not implement folders,
linking, or permissions. In the Shark File System, as distributed in this lab, you may only create
and delete files, and read or write to them.

The first portion of this lab deals with file system capabilities. We ask you to implement a few
functions that introduce new abilities of the file system, allowing users to see and augment the
position of file descriptors, and to change file names with a single call.

The next portion of this lab asks you to improve the file system's performance and safety.
Currently, there is nothing to prevent two simultaneous calls to the file system from overwriting
or otherwise conflicting with each other. We would like for you to prevent conflicts like these
from happening, while still allowing for concurrent and fast access to the file system for
separate users.

## 2 Getting Started

### 2.1 Files

| File | Description |
|------|-------------|
| `sfs-disk.c` | **The file you will be modifying.** Contains the SFS implementation. |
| `sfs-api.h` | API specification header. Read this carefully -- do not modify. |
| `sfs-disk.h` | On-disk data structures and constants. Read-only (unless tackling optional challenges). |
| `sfs-support.c` | Low-level disk/mmap support routines. Provided -- do not modify. |
| `local/sfs-baseline-ref.c` | Naive reference implementation (handout code + one global mutex). Used by `make baseline` to calibrate the perf score for your machine. Do not modify. |
| `sfs-fsck.c` | Filesystem consistency checker. Run it on disk images to find structural bugs. |
| `local/test-sfs.c` | Standalone test driver and autograder. Exercises all API functions, includes TSan race detection. |
| `Makefile` | Build system. Builds `sfs-fsck`, `test-sfs`, and `test-sfs-baseline`. |

### 2.2 Building

On Ubuntu 18.04 (or any Linux with GCC):

```bash
make
```

This produces three executables:

- `sfs-fsck` -- filesystem consistency checker
- `test-sfs` -- test driver for your implementation
- `test-sfs-baseline` -- reference implementation (handout code + one global mutex) used to calibrate the perf score for your machine; see Section 5.1.

Before evaluating the performance score on a new machine, calibrate the
performance baseline:

```bash
make baseline
```

This runs `test-sfs-baseline` five times and caches the **median** throughput
to `.perf_baseline`, so transient noise (thermal throttling, background load,
scheduler jitter) doesn't skew your perf ratio. A `spread: min/max` line is
printed so you can tell at a glance whether the run was stable -- if min and
max differ by more than ~10%, the machine was probably not quiet; re-run on
an idle system. Override the sample count with `make baseline BASELINE_RUNS=N`
(odd N recommended).

Your perf score is then a ratio of your implementation's throughput to this
baseline, so results are comparable across laptops. You can defer this until
the A/B/C correctness tests pass; it is not needed for the starter health
check. Without a calibrated baseline the performance score is **capped at
5/10** (absolute throughput cannot show whether your locking *scales*), so run
`make baseline` -- or `make grade`, which does it for you -- before reading
anything into the perf number.

### 2.3 Running Tests

Assuming you have already run `make` (and `make baseline` before checking
performance):

```bash
./test-sfs
```

The test driver will exercise all SFS API functions and report pass/fail for each test.
Before you implement anything, you should see failures for `sfs_getpos`, `sfs_seek`, and
`sfs_rename`. As you implement each function, the corresponding tests will start passing.

See Section 5.1 for output-verbosity knobs (`-v` for full FAIL detail, `-q` for
scoreboard only).

If you only want to confirm that the offline package and starter-safe subset are
healthy, run:

```bash
make starter-safe
```

From the repository root, the Lua trace targets use the local Lua development
packages when available and fall back to Docker when those packages are missing.
The local Lua runner implements `lanes.gen` with pthreads and separate Lua
states, so Lua C traces make real concurrent calls into the SFS API.

You can also check a disk image for structural consistency:

```bash
./sfs-fsck [-v] <disk-image>
```

## 3 Overview

Your first item of business is to implement the `sfs_getpos` function. This function should just
return the position of a file descriptor within its file. This should be relatively straightforward,
but is a good warm-up to familiarize yourself with the file system codebase before moving on to
the rest of the assignment. The principal files of interest are `sfs-disk.c`, `sfs-disk.h`, and
`sfs-api.h`.

Next, you should implement the `sfs_seek` function. This function should move the file
descriptor's position forwards or backwards by the value passed into the argument. This will
require not just accessing data stored in the file system structure like you did for `sfs_getpos`,
but changing it as well.

After that, only `sfs_rename` remains unimplemented. This will also require changing data
stored in the file system structure.

Once you have implemented these three functions, it is time to move on to implementing
concurrency. We suggest approaching this in three gradations.

The first order of business is to make the system thread-safe in a very coarse way, only trying
to ensure correctness of accesses. This can be done by simply locking the entire file system for
any kind of access, and then unlocking once the request has been satisfied.

After making the file system thread-safe, you can start to think of ways to increase concurrent
accesses. There are two aspects to this: reading and writing, and separate files.

When reading, no data is being changed besides the current file descriptor's position, so
multiple file descriptors may read at the same time without any concern for overwriting each
other, whereas writing requires solitary access.

With separate files, accesses should not ever overlap, since the files have to be separate to
start with. So, reading or writing on one file need not prevent someone else from doing the same
with another file.

Which aspect you start with is up to you, but we would like for you to try to implement both.
If you can think of ways to improve concurrency even more, please go further!

## 4 The SFS Specification

You should only need to edit the `sfs-disk.c` file; however, some designs may need to modify
`sfs-disk.h`. The current top-level functions implemented in `sfs-disk.c`, accessible via
`sfs-api.h`, are:

### 4.1 Provided Functions

- **`int sfs_format(const char *diskName, size_t diskSize)`**
  Creates an SFS disk image on file `diskName`, creating it if it does not exist, allocating
  `diskSize` bytes for the image, and then mounts this disk to be used for all subsequent
  sfs-disk file routines.

- **`int sfs_mount(const char *diskName)`**
  Mounts the disk image, loading the state of a previously used file system.

- **`int sfs_unmount(void)`**
  Unmounts the current disk image. If any files are open, this will fail.

- **`int sfs_open(const char *fileName)`**
  Opens the file with name `fileName`, creating it if it does not exist, and returning a file
  descriptor.

- **`void sfs_close(int fd)`**
  Closes the given file descriptor. This call can't fail; it can only be inert for a file
  descriptor that doesn't exist.

- **`ssize_t sfs_write(int fd, const char *buf, size_t len)`**
  Writes up to `len` bytes of the string `buf` to the file descriptor given. On success,
  returns the number of bytes written. This call updates the position of `fd`.

- **`ssize_t sfs_read(int fd, char *buf, size_t len)`**
  Reads up to `len` bytes from the given file descriptor, placing them into `buf`. On success,
  returns the number of bytes read. This call updates the position of `fd`.

- **`int sfs_remove(const char *name)`**
  Attempts to remove the file `name`.

- **`int sfs_list(sfs_list_cookie *cookie, char filename_out[], size_t filename_space)`**
  Iteratively places file names into `filename_out[]` with each call, until there are no more
  files. Returns 0 upon successful retrieval, 1 when it has no more filenames to write out,
  and possibly a negative error code.

### 4.2 Functions You Must Implement

There are three functions in `sfs-disk.c` that are currently unimplemented. It is your first
order of business to implement these functions.

- **`ssize_t sfs_getpos(int fd)`**
  Should return the position of the file descriptor given, in bytes. Specifically, it should
  return the index of the byte we are on, where the first byte of the file is 0, the second
  is 1, and so on until the end of the file.

- **`ssize_t sfs_seek(int fd, ssize_t delta)`**
  Should move `fd`'s position by `delta`. If given the output `loc` from an earlier `getpos`
  call, `sfs_seek(fd, loc - sfs_getpos(fd))` should return `fd` to the position where the
  file descriptor was at that earlier call. `seek` should track with reads and writes to the
  file descriptor, according to how many bytes have been read or written. If the requested
  motion would put `fd` in a negative position, it should stop at 0. If the requested motion
  moves past the end of the file, it should stop at the end of the file.

- **`int sfs_rename(const char *old_name, const char *new_name)`**
  Should rename the file of name `old_name` to `new_name`. If `new_name` already exists,
  then delete and replace it.

For more details, such as expected return values and error codes, refer to the specifications
given in `sfs-api.h`. In addition to `sfs-disk.c` and `sfs-api.h`, `sfs-disk.h` will also be
worth reading and referencing as it defines some of the high-level design choices of the file
system.

## 5 Testing

### 5.1 Autograder

The primary testing tool is `test-sfs`, a standalone C autograder that mirrors the original CMU
grading structure. It runs categorized test traces and prints a scoreboard:

```bash
make              # builds sfs-fsck, test-sfs, test-sfs-baseline
make baseline     # before checking the perf score; see Section 2.2
./test-sfs
```

The autograder is organized into the same categories as the original:

| Category | Traces | Points | What it tests |
|----------|--------|--------|---------------|
| A (Feature Tests) | A00-A04 | 5 | format/mount, open/close/rw, getpos, seek, rename |
| B (Sequential Correctness) | B00-B03 | 4 | remove/list/reuse, multi-block seek + cross-boundary read, fd errors, open-file lifecycle, rename/list/reuse checks |
| C (Concurrent Correctness) | C00-C02 | 3 | separate-file writes, shared reads, r/w mix + open/close storm |
| Performance | benchmark | 10 | Concurrent throughput (only runs if correctness = 12/12) |
| Style | -- | 4 | Manual self-review (not auto-graded) |

Each correctness trace also runs `sfs-fsck` after unmounting its disk image.
This catches structural corruption such as orphaned blocks or inconsistent
block links even when the immediate API result appeared to be correct. When
this happens, the trace failure includes the first captured checker diagnostic.

Sample output (before implementing anything):

```
========================================
        SFS Lab Autograder
========================================

Category A (Feature Tests):
  A00 format_mount            PASS  [1/1]
  A01 open_close_rw           PASS  [1/1]
  A02 getpos                  FAIL  [0/1]
       -> FAIL [test-sfs.c:163]: initial getpos should be 0, got -38
       -> FAIL [test-sfs.c:167]: getpos after write(10) should be 10, got -38
       -> FAIL [test-sfs.c:169]: getpos(-1) should return -EBADF
       -> (... 1 more fail suppressed)
  A03 seek                    FAIL  [0/1]
  A04 rename                  FAIL  [0/1]
  Subtotal: 2/5

Category B (Sequential Correctness):
  B00 remove_list             PASS  [1/1]
  B01 multi_block_seek        FAIL  [0/1]
  B02 edge_cases              FAIL  [0/1]
  B03 rename_list_edges       FAIL  [0/1]
  Subtotal: 1/4

Category C (Concurrent Correctness):
  C00 separate_files          PASS  [1/1]
  C01 read_same_file          PASS  [1/1]
  C02 rw_mix_storm            PASS  [1/1]
  Subtotal: 3/3

Race Detection (ThreadSanitizer):
  (skipped -- normal correctness traces must all pass first)

Correctness: 6/12

Performance:
  (skipped -- correctness tests must all pass first)
  Score: 0/10

----------------------------------------
  Total: 6/22  (+ up to 4 style pts)
========================================
```

**Output verbosity:** FAIL diagnostics print grouped under the trace
they belong to -- the PASS/FAIL summary line always comes first, then any
FAIL detail for that trace, then the next trace. To keep a broken
implementation from flooding the screen, only the first 3 FAIL lines per
trace are shown; the rest are collapsed into `(... N more fails
suppressed)`.

- `./test-sfs -q` (`--quiet`) suppresses all FAIL detail -- useful when
  you only want the scoreboard.
- `./test-sfs -v` (`--verbose`) raises the cap to unlimited.

Flags can appear in any order and combine with the mode flags
(`--tsan-only`, `--stress-only`, `--model-fuzz`, `--x-only`, ...).
(`--perf-only` only emits a single ops/sec number, so verbosity has no
effect there.)

**Note on Category C:** After the normal A/B/C correctness traces all pass, the
autograder automatically compiles a ThreadSanitizer build and re-runs the concurrent
tests. The normal C traces serialize individual SFS API calls so partially
completed starters get stable functional feedback; the TSan rerun disables that
guard and makes real concurrent calls. The rerun sweeps **three seeded
schedule-fuzz runs** (seeds 1-3; see `--sched-fuzz` below), each perturbing
thread timing differently, so races that hide under one particular
interleaving still get a chance to surface; the failing seed is reported so
you can replay it. If TSan detects any data races, the Category C score is set
to 0 -- even if the normal traces appeared to pass. This mirrors the original
CMU ConTech-based race detection. If a normal correctness trace already
fails, TSan is skipped and the normal per-trace score is reported. If your
system does not support `-fsanitize=thread`, the TSan check is skipped without
penalty (on kernels whose address-space randomization confuses TSan, the
grader automatically retries under `setarch -R` first).

**Schedule fuzz (`--sched-fuzz[=SEED]`):** the concurrency traces can inject
seeded random `sched_yield()`s and microsleeps around SFS calls, shaking out
schedules the default timing never produces. The same seed reproduces the
same perturbation pattern, so a failure found under fuzz is replayable:
`./test-sfs --tsan-only --sched-fuzz=2` (env: `SFS_SCHED_FUZZ_SEED`). The
scored run does not fuzz; only the TSan sweep and explicit `--sched-fuzz`
invocations do, so no flakiness is added to the graded path.

The performance benchmark runs 8 threads, each against its own file. A thread
performs 2000 *sessions*: one `sfs_open`, then 10 rounds of
write/seek/getpos/read, then one `sfs_close` (672,000 API calls total per
run). Amortizing open/close over the I/O rounds makes the measurement
reflect how well operations on *different files* proceed in parallel -- the
thing this lab asks you to make scale -- rather than the necessarily-shared
fd table. Timing starts at a barrier after all 8 threads exist, so thread
startup cost is excluded. One "op" in every reported number is one SFS API
call. The benchmark is **sampled 5 times and the median ops/sec is scored**,
mirroring what `make baseline` does for calibration -- this keeps the numerator
and denominator of your ratio on the same statistical footing. A `spread`
percentage is printed (200 x (max - min) / (max + min)); the autograder also
prints a warning if spread exceeds 20%, pointing at Section 5.4 for mitigations.

Raw ops/sec is machine-dependent, so the score is instead a ratio against
a shipped reference implementation (the handout code wrapped with a single global
mutex) measured on the same machine via `make baseline`. Let
`ratio = student_ops / baseline_ops`:

| ratio | Score | Interpretation |
|-------|-------|----------------|
| >= 2.50 | 10/10 | Scaling that requires fine-grained locking |
| >= 1.80 | 9/10  | Strong parallelism |
| >= 1.40 | 7/10  | Meaningful parallelism |
| >= 1.20 | 5/10  | Slightly better than one global lock |
| >= 0.85 | 3/10  | Coarse-lock territory (this *is* the baseline) |
| < 0.85 | 0/10  | Slower than the naive baseline |

These bars are calibrated against real implementations (see
`docs/maintainer/PERF_CALIBRATION.md` for the measurements): a correct
solution that keeps one global mutex measures ~1.0x and earns 3/10, while a
per-file locking scheme measures ~4.5-5x on an 8-hardware-thread machine and
clears 10/10 with a wide margin. The 2.5x bar is deliberately below what
per-file locking achieves even on a 4-core machine, so full marks stay
reachable on small CPUs.

If `.perf_baseline` is missing -- or records a different grader workload
version than this grader measures (the file carries a `WORKLOAD=` tag so a
stale calibration is rejected instead of producing a meaningless ratio) --
the grader falls back to absolute throughput **capped at 5/10** and tells you
to re-run `make baseline`. Full performance credit always requires a
calibrated ratio.

Each Category C trace uses its own disk image and the parent cleans up
concurrency images after timeout/crash paths, which keeps repeated stress runs
from inheriting leftover state from the previous trace.

**Failure disk images:** when a trace fails, the autograder snapshots the
trace's surviving disk image(s) as `fail_<trace>_<image>.img` next to the
originals and prints the path, so you can inspect the exact corrupted state
with `./sfs-fsck --dump fail_B01_test.img` (Section 5.2) instead of
re-running under a debugger. The copies are skipped in `--json`/`--tsan-only`
runs and can be forced on/off with `SFS_KEEP_FAILED_DISKS=1/0`; `make clean`
removes them along with the other images.

`make stress` is for testing an implementation after you start fixing
concurrency. It repeats the scored C concurrency traces and then runs
stress-only diagnostics: **S00** (directory churn while another thread lists)
and **S01** (rename atomicity: a writer loops `rename("incoming", "stable")`
while observers repeatedly open and read `"stable"` -- if an observer ever
sees the file empty, the rename exposed a window in which the destination
name did not exist, which the specification forbids; see the FAQ). These
diagnostics are intentionally outside the 22-point score because they are
more schedule-sensitive than the core correctness signal. `make stress` is
stricter than the starter health check, and the unmodified skeleton
may fail it (S01 exercises `sfs_rename`, which the starter has not
implemented yet).

**Deadlock protection:** Each Category C trace runs in a forked child with a
30-second wall-clock budget; the perf benchmark gets 60 seconds. If a child
exceeds its budget (e.g. from a deadlock), the parent `SIGKILL`s it and scores
that trace/benchmark as 0. You will not need Ctrl+C to recover.

### 5.2 sfs-fsck

The filesystem consistency checker `sfs-fsck` can verify the structural integrity of a disk
image. This is useful for debugging: if your implementation corrupts the on-disk data structures,
`sfs-fsck` will tell you what went wrong.

```bash
./sfs-fsck test.img
./sfs-fsck -v test.img       # verbose output
./sfs-fsck --dump test.img   # human-readable image dump, then the checks
```

It checks for:

- Mislabeled blocks
- Invalid directory entries
- File length disagreeing with number of allocated blocks
- Inconsistent doubly linked lists
- Circular linked lists
- Blocks on more than one list simultaneously
- Orphaned blocks (not on any list)

`--dump` (`-d`) prints what the image actually contains before judging it:
the superblock summary, the free list chain, every directory entry with its
block chain and size, and a per-type block tally. The dump is deliberately
defensive -- chains are walked with a step cap, out-of-range block IDs are
annotated instead of crashing, and long chains are elided -- so it stays
useful on precisely the corrupted images you will want to point it at, such
as the `fail_<trace>_*.img` snapshots the autograder keeps for failing
traces (Section 5.1).

### 5.3 Concurrency Testing

The autograder automatically compiles a ThreadSanitizer build and re-runs the Category C
traces after the normal A/B/C correctness suite passes. If TSan detects data races, the
C score is set to 0. If any normal correctness trace fails, TSan is skipped so the first
fix remains the visible correctness failure.

If you want to run TSan manually for more detailed output:

```bash
gcc -std=c11 -g -fsanitize=thread -pthread -D_GNU_SOURCE=1 \
    -I. -o test-sfs-tsan local/test-sfs.c local/test-report.c \
    sfs-disk.c sfs-support.c
./test-sfs-tsan --tsan-only
./test-sfs-tsan --tsan-only --sched-fuzz=2   # replay the seed the grader reported
```

ThreadSanitizer will print the exact locations of any data races it detects. This is the
local equivalent of the ConTech-based `raceTool` used in the original CMU lab.

### 5.4 Running inside Docker Desktop on Windows

If you run the lab inside a Docker container whose working directory is
bind-mounted from a Windows host (Docker Desktop's default setup), two classes
of issue come up often enough to be worth calling out.

**Jitter and high spread.** Docker Desktop on Windows routes filesystem I/O
through a 9p / virtiofs / gRPC-FUSE layer; every `open`/`write`/`unlink` in the
SFS disk image crosses that layer. Combined with Windows Defender scanning
the mounted directory and the Hyper-V scheduler preempting the Linux VM,
single-run variance of 40-70% on the perf benchmark is normal rather than a
bug in your implementation. The 5-sample median absorbs most of this. If you
still see the "spread exceeds 20%" warning, pick one (easiest first):

1. **Move the disk images off the bind mount** with the `SFS_DISK_DIR` env
   var. The disk images (`test.img`, the `test_conc_*.img` family,
   `test_model.img`, `test_perf.img`) dominate I/O cost; pointing them at
   the container's overlay FS (`/tmp`) keeps them off the Windows bind
   mount entirely:
   ```bash
   export SFS_DISK_DIR=/tmp
   make baseline          # recalibrate in the new location
   ./test-sfs             # scored run
   ```
   The autograder prints `Disk images redirected via SFS_DISK_DIR: /tmp` to
   stderr when the variable is set, so you can confirm it took effect. On
   a typical Docker-on-Windows setup this alone drops spread from 40-70%
   into the single digits. The value is recorded into `.perf_baseline`; if
   your calibration and scored run disagree on `SFS_DISK_DIR`, the scorer
   prints a loud warning and tells you to recalibrate -- otherwise the ratio
   would be measuring two different filesystems.
2. **Raise the sample count.** `make baseline BASELINE_RUNS=11` tightens
   calibration at the cost of a longer one-time setup. For the scored run,
   bump `PERF_SAMPLE_RUNS` in `local/test-sfs.c` and rebuild.
3. **Exclude the project directory from Windows Defender** (Settings ->
   Virus & threat protection -> Exclusions). Removes one randomized source of
   latency spikes.
4. **Move the whole project to WSL2's native filesystem** (e.g.
   `\\wsl$\Ubuntu\home\...`). The bind mount disappears entirely and spread
   typically drops into single digits. Most robust fix, but requires
   relocating your workspace.

**Bind-mount ghost directories.** If you delete and re-extract the handout
while the container is running, you may see contradictory state across
syscalls: `ls` reports the directory missing, but `mv` or `rm -rf` claims it
still exists, and `stat` may give a different answer again. This is the
mount driver's metadata cache disagreeing with itself -- not a bug you wrote.
Fixes, easiest first:

1. Extract under a fresh name: `tar xf sfslab-handout.tar && mv sfslab work-dir`.
2. Restart the container (`docker restart <name>` from the host, then re-enter).
3. Long-term, move the project off the Windows bind mount (option 4 above).

### 5.5 Differential model fuzz

`make model-fuzz` (or `./test-sfs --model-fuzz[=SEED]`) runs a seeded random
sequence of ~2000 operations in lockstep against your implementation and a
small in-memory reference model, comparing every return value, every byte
read, and the directory listing. Every 250 operations it closes all files,
unmounts, runs `sfs-fsck` on the image, and remounts. The disk is a single
page -- the smallest image `sfs_format` accepts -- so `ENOSPC` paths are
exercised for real.

This is a *diagnostic*, not part of the 22-point score, but it is very good
at catching the long tail the fixed traces cannot enumerate: off-by-one
positions at block boundaries, wrong error codes, size accounting drift.
On a mismatch it prints the seed, the operation number, and a log of the
most recent operations; the same seed replays the identical sequence
(`make model-fuzz MODEL_FUZZ_SEED=N`). The three unimplemented starter
functions are treated as no-ops (counted and noted), so the fuzz is usable
from your first build onward. `make report-json` includes a fixed-seed
model-fuzz section so reports stay reproducible.

## 6 Concurrency Guide

Once the three basic functions are implemented, the next challenge is making the file system
thread-safe. Here is a suggested progression:

### 6.1 Coarse-Grained Locking

Start by adding a single global mutex that protects the entire file system. Lock it at the
beginning of every API function and unlock it at the end. This ensures correctness but allows
no concurrency -- it is exactly what the `test-sfs-baseline` reference does, so expect a perf
ratio near 1.0x and a performance score of 3/10. That is the intended first checkpoint, not
the destination.

### 6.2 Reader-Writer Locks

`pthread_rwlock_t` lets multiple readers proceed concurrently while writers get exclusivity.
Be careful about where it actually helps, though: `sfs_read` *advances the file descriptor's
position*, so on its own a single global rwlock buys almost nothing on the scored workload --
nearly every operation still needs the write side. Reader-writer locks pay off for structures
that are genuinely read-mostly, such as the directory during name lookups, usually in
combination with the per-file locks of Section 6.3.

### 6.3 Per-File Locking

Accesses to separate files should not block each other. Consider a lock per open file (or per
directory entry) so that operations on different files can proceed in parallel. This is the
level of granularity the performance score is calibrated around; getting it right (plus
short critical sections on the shared structures below) is what clears the 10/10 bar.

### 6.4 Tips

- Start coarse, verify correctness, then refine.
- Use `sfs-fsck` after concurrent tests to verify disk image integrity.
- ThreadSanitizer (`-fsanitize=thread`) is your best friend for finding races.
- Be careful with operations that touch global state: the free list, the directory, and the
  open file tables. Each can have its own small lock -- but fix a global lock *order* and
  stick to it everywhere, or two threads taking the same locks in opposite orders will
  deadlock (the 30-second trace timeout will catch you, but the report is less helpful
  than a design that cannot deadlock).
- After any locking change, run `make stress` and `make model-fuzz`, not just `./test-sfs`.

## 7 Optional challenges (X traces)

The handout source contains three "optional challenge" comments (in
`createFile`, `sfs_open`, and `sfs_remove` in `sfs-disk.c`). Each now has a
matching **X trace** that tells you whether you actually pulled it off. They
are deliberately *not* part of the 22-point score: the default `./test-sfs`
run shows them after the scoreboard as achieved (`PASS`) or not (`--`)
without affecting your total or the exit status, and they never appear in
`--list-traces`. For full diagnostics and kept failure images, run:

```bash
make x-traces          # = ./test-sfs --x-only
```

- **X00 `empty_file_blocks`** -- make empty files consume *zero* data blocks
  (today every file occupies at least one block so a nonzero `first_block`
  can mark the entry live). The trace creates 5 empty files on a minimal
  one-page disk and then grows another file to use **every** data block --
  which only fits if the empty files are block-free. Note that the on-disk
  convention for "this entry is live" must change, so this trace skips the
  `sfs-fsck` pass and you should update `sfs-fsck.c` to match your new
  convention (the checker's directory-entry rules are written assuming the
  old one).
- **X01 `dir_expansion`** -- allow more than `DIR_ENTRIES_PER_BLOCK` (15)
  files by chaining additional directory blocks from the superblock
  (`next_rootdir` is already in the format, and `sfs-fsck` already walks
  it). The trace creates 20 files, lists them all, spot-checks contents,
  and requires a clean fsck afterwards.
- **X02 `unix_remove`** -- implement Unix delete semantics: removing an open
  file succeeds, the name disappears from the directory immediately, the
  surviving fd can still read and write, re-opening the name creates a
  distinct new file, and the storage is reclaimed once the last fd closes
  (fsck must find no orphaned blocks). **Heads up:** this deliberately
  contradicts the graded B02 trace, which encodes the handout's documented
  `-EBUSY` behavior. Achieving X02 means giving up that B02 expectation --
  treat it as a post-grading exploration (or keep it on a branch), exactly
  the kind of design trade-off the comment in `sfs_remove` is inviting you
  to think about.

## Appendix: FAQ

### "No space left on device"

Each test sizes the disk appropriately for correct execution. If your implementation has an error
or a race, it may end up using additional space and thereby cause an operation to fail with
`ENOSPC`.

### How do I debug disk corruption?

Run `sfs-fsck -v` on your disk image. It will report exactly which blocks are mislinked,
orphaned, or mislabeled. You can also add `printf` statements in `sfs-disk.c` to trace
block allocation and deallocation.

### What if `sfs_seek` goes past the end of the file?

Per the specification in `sfs-api.h`, if seeking would move the position past the end of the
file, it should be clamped to the file size. If it would go negative, clamp to 0. This is
different from the Unix `lseek` system call.

### Does `sfs_rename` need to be atomic?

Yes. If `new_name` already exists, the replacement should be atomic -- there should be no gap
in which a concurrent thread can observe `new_name` not existing. For the single-threaded
implementation this is straightforward; for the concurrent version, you will need appropriate
locking. The S01 stress diagnostic (`make stress`, Section 5.1) hunts for exactly this gap:
its observers open-and-read the rename destination in a loop and flag any moment the name
resolved to a freshly created empty file.
