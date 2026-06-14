# Release Checklist

Use this checklist before publishing or handing off `sfslab-handout.tar`.

Before running commands, review `docs/maintainer/DISTRIBUTION_REVIEW.md`. This
is a release-safety checklist, not legal advice. Run it from a clean worktree:
the dist targets may rewrite `sfslab-handout.tar`, and the final `git diff` /
clean-worktree gates are the authoritative stale-artifact checks in this
sequence.

```bash
make doctor
make lint-strict
make clean
make check
make trace-smoke
make report-json > /tmp/sfslab-report.json
python3 -m json.tool /tmp/sfslab-report.json >/dev/null
python3 docs/examples/compare-starter-report.py docs/examples/starter-report.json /tmp/sfslab-report.json
python3 docs/examples/test_compare_starter_report.py
make dist-verify
make dist-repro-check
git diff --exit-code -- sfslab-handout.tar
test -z "$(git status --porcelain=v1 --untracked-files=all)"
```

`make dist-verify` rebuilds `sfslab-handout.tar`, extracts it into a temporary
directory, runs `make check` from the extracted handout, and validates that
`make report-json` emits parseable JSON.

`make dist-repro-check` first checks the existing published tarball against a
fresh `make dist` result, then rebuilds the tarball a second time and compares
the two fresh SHA-256 hashes. The final `git diff` and clean-worktree gates catch
a stale committed tarball even when the rebuild itself is reproducible.

Expected starter status:

- `make lint-strict` passes when ShellCheck and clang-format are installed.
  C formatting is opt-in through `SFS_LINT_C_FORMAT_FILES` because the inherited
  handout C sources are not all clang-format clean.
- `make check` passes.
- `make trace-smoke` passes the starter-safe subset.
- `make report-json` exits 0 because report generation worked.
- The JSON score still reports the intentionally incomplete starter as not
  fully graded.
- `docs/examples/compare-starter-report.py` accepts the starter report's
  graded/core fields even when environment-dependent diagnostics differ.
- `docs/examples/test_compare_starter_report.py` passes.
- `sfs_getpos`, `sfs_seek`, and `sfs_rename` remain `-ENOSYS` stubs.
- `make stress` fails on the starter (S01 exercises the unimplemented
  `sfs_rename`); in `make report-json` this appears as
  `stress_diagnostics.exit_status != 0` and `diagnostics_passed: false`
  without failing report generation.
- `make model-fuzz` passes on the starter (unimplemented calls are modeled
  as no-ops and counted in the trace output).
- The default `./test-sfs` run ends with a `Category X` footer showing
  `Achieved: 0/3`; X traces never appear in `--list-traces`, so
  `make manifest-check` is unaffected.

## Version Tags

Use date-based release tags unless there is a stronger reason to use semantic
versioning:

```bash
git tag -a vYYYY.MM.DD -m "Release vYYYY.MM.DD"
git push origin vYYYY.MM.DD
```

If a second release is needed on the same date, add a numeric suffix such as
`vYYYY.MM.DD.2`.

## Release Notes Template

```markdown
## Summary

- One-line description of what changed for students.

## Verification

- `make doctor`
- `make lint-strict`
- `make check`
- `make trace-smoke`
- `make report-json > /tmp/sfslab-report.json`
- `python3 -m json.tool /tmp/sfslab-report.json >/dev/null`
- `python3 docs/examples/compare-starter-report.py docs/examples/starter-report.json /tmp/sfslab-report.json`
- `python3 docs/examples/test_compare_starter_report.py`
- `make dist-verify`
- `make dist-repro-check`
- `git diff --exit-code -- sfslab-handout.tar`
- `test -z "$(git status --porcelain=v1 --untracked-files=all)"`

## Starter Boundary

- `sfs_getpos`, `sfs_seek`, and `sfs_rename` remain intentionally incomplete.
- Starter graded/core report fields remain comparable to
  `docs/examples/starter-report.json`.

## Distribution Notes

- `docs/maintainer/DISTRIBUTION_REVIEW.md` reviewed.
- No completed `sfs-disk.c` solution included.
```
