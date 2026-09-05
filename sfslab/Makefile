# Makefile for the SFS Lab
#
# Students: you should not need to modify this file.

CC       = gcc

LDFLAGS  = -O2 -g -pthread
CFLAGS   = -std=c11 $(LDFLAGS) $(WARNINGS)
CPPFLAGS = -I. -D_GNU_SOURCE=1 -D_FORTIFY_SOURCE=2
WARNINGS = -Werror -Wall -Wextra -Wpedantic -Wconversion
WARNINGS += -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings
WARNINGS += -Wno-unused-parameter

PROGRAMS = sfs-fsck test-sfs test-sfs-baseline

all: $(PROGRAMS)
.PHONY: all

sfs-fsck: sfs-fsck.o
	$(CC) $(LDFLAGS) -o $@ $^

test-sfs: test-sfs.o sfs-disk.o sfs-support.o
	$(CC) $(LDFLAGS) -o $@ $^

test-sfs-baseline: test-sfs.o sfs-baseline-ref.o sfs-support.o
	$(CC) $(LDFLAGS) -o $@ $^

# Run the autograder against your implementation.
test: test-sfs sfs-fsck
	./test-sfs
.PHONY: test

# Developer-branch extensions; deliberately outside the 22-point score.
developer-test: test-sfs sfs-fsck
	./test-sfs --developer-only
.PHONY: developer-test

x-traces: test-sfs sfs-fsck
	./test-sfs --x-only
.PHONY: x-traces

# Baseline calibration: run the reference benchmark BASELINE_RUNS times
# and write the median to .perf_baseline, smoothing over per-run jitter
# (thermal, scheduler noise).  Keep BASELINE_RUNS odd so the median is
# one observed value, not an interpolation.  The performance score is
# the ratio of your throughput to this baseline; without a calibrated
# baseline it is capped at 5/10.  Override with:
#   make baseline BASELINE_RUNS=N
BASELINE_RUNS ?= 5

baseline: test-sfs-baseline
	@case "$(BASELINE_RUNS)" in \
	  ''|*[!0-9]*|0|*[02468]) \
	    echo "BASELINE_RUNS must be a positive odd integer" >&2; exit 2;; \
	esac
	@echo "Calibrating machine baseline ($(BASELINE_RUNS) runs, median)..."
	@rm -f .perf_baseline.tmp
	@i=1; while [ $$i -le $(BASELINE_RUNS) ]; do \
	  printf "  run %d/%d: " $$i $(BASELINE_RUNS); \
	  out=$$(./test-sfs-baseline --perf-only) || exit 1; \
	  printf '%s\n' "$$out"; \
	  printf '%s\n' "$$out" >> .perf_baseline.tmp; \
	  i=$$((i+1)); \
	done
	@sort -n .perf_baseline.tmp \
	  | awk -v n=$(BASELINE_RUNS) 'NR==int((n+1)/2){print; exit}' \
	  > .perf_baseline
	@printf 'SFS_DISK_DIR=%s\n' "$${SFS_DISK_DIR:-(unset)}" >> .perf_baseline
	@printf 'WORKLOAD=v4\n' >> .perf_baseline  # keep in sync with PERF_WORKLOAD_VERSION (test-sfs.c)
	@awk 'NR==1{min=$$1; max=$$1} \
	      {if($$1<min)min=$$1; if($$1>max)max=$$1} \
	      END{printf "  spread: min=%.2f max=%.2f (%.1f%% of midpoint)\n", \
	          min, max, 200*(max-min)/(max+min)}' .perf_baseline.tmp
	@rm -f .perf_baseline.tmp
	@echo "Baseline (median): $$(head -1 .perf_baseline) ops/sec  ->  .perf_baseline"
.PHONY: baseline

# Calibrate the baseline, then run the autograder.
grade: test-sfs test-sfs-baseline sfs-fsck
	$(MAKE) baseline
	./test-sfs --benchmark
.PHONY: grade

clean:
	-rm -f $(PROGRAMS) test-sfs-tsan *.o tsan_output.log .perf_baseline .perf_baseline.tmp
.PHONY: clean

## cut here ##
sfs-support.o: sfs-support.c sfs-disk.h
sfs-disk.o: sfs-disk.c sfs-api.h sfs-disk.h
sfs-fsck.o: sfs-fsck.c sfs-disk.h
test-sfs.o: test-sfs.c sfs-api.h sfs-disk.h
sfs-baseline-ref.o: sfs-baseline-ref.c sfs-api.h sfs-disk.h
