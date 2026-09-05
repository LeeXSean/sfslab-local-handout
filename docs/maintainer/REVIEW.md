# Maintenance review — September 2026

## Scope and difficulty

Reviewed the main and developer handouts, API contracts, test driver, support
code, packaging, CI, and learning instructions. This is a targeted review, not
proof that all bugs are absent. Student API stubs and missing locks remain tasks.

CMU's [Fall 2023 synchronization lecture](https://www.cs.cmu.edu/afs/cs/academic/class/15213-f23/www/lectures/24-sync-advanced.pdf)
identifies three APIs (getpos, seek, rename) and synchronization as the lab work.
The [Fall 2025 overview](https://www.cs.cmu.edu/afs/cs/academic/class/15213-f25/www/lectures/01-overview.pdf)
also names parallelism and performance as learning goals. An official public
grader was not located; local thresholds cannot be certified equivalent.

The default path now checks correctness. One correct global mutex is enough
for this checkpoint. The existing performance scale is available explicitly
through `make grade`, as an optimization exercise. Developer extensions remain
on their own branch; none are required on main.

## Fixed

- The driver serialized concurrent calls, masking missing student locks and
  risking lock inversion with a directory lock held across list calls. Normal
  tests now exercise actual concurrent calls, even without ThreadSanitizer.
- Concurrent CHECK failures wrote a non-atomic shared flag. It is now atomic.
- Fixed test filenames could overwrite existing images or collide across runs.
  Each run now creates a private directory; clean no longer deletes image globs.
- An endless stderr stream could starve the trace timeout. Pipe draining now
  yields to the deadline check after a bounded amount of input.
- CI accepted an unexpectedly successful starter due to `&& ... || ...`
  exit-code handling. It now verifies the actual result explicitly.
- TSan allowed only 20 seconds for three traces with 30-second budgets each,
  and deleted failure diagnostics. The budget is now 120 seconds and failure
  logs are retained. Unsupported sanitizer runtimes are reported separately.
- Packaging could continue after a failed copy. It now stops on errors.
- The support `container_of` macro subtracted a byte offset after casting to
  a structure pointer. Arithmetic now occurs on a byte pointer. Current callers
  use an offset-zero header, so this was latent rather than an observed crash.

## Checks and limits

`python3 tests/check_grader.py` checks image preservation, actual concurrent
entry, atomic failure reporting, noisy-child timeouts, and starter exit status.
`make developer-test x-traces` checks all four implemented extension traces.
Both branches build with the existing warning-as-error settings.

SFS is a teaching library, not a hardened filesystem service. Mount checks the
image header; arbitrary corrupted images should be inspected with sfs-fsck,
not treated as safe input to the student implementation. Passing finite tests
is not a proof of race freedom or crash recovery.
