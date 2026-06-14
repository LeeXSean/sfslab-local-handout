# -*- Makefile -*- for sfslab (local build)

-include .labname.mk

CC      = gcc

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

test-sfs: local/test-sfs.o local/test-report.o sfs-disk.o sfs-support.o
	$(CC) $(LDFLAGS) -o $@ $^

test-sfs-baseline: local/test-sfs.o local/test-report.o local/sfs-baseline-ref.o sfs-support.o
	$(CC) $(LDFLAGS) -o $@ $^

# Baseline calibration: run the benchmark BASELINE_RUNS times and write
# the median to .perf_baseline, smoothing over per-run jitter (thermal,
# scheduler noise). Keep BASELINE_RUNS odd so the median is one observed
# value, not an interpolation. Override with: make baseline BASELINE_RUNS=N
BASELINE_RUNS ?= 5
STRESS_RUNS ?= 10

baseline: test-sfs-baseline
	@echo "Calibrating machine baseline ($(BASELINE_RUNS) runs, median)..."
	@rm -f .perf_baseline.tmp
	@i=1; while [ $$i -le $(BASELINE_RUNS) ]; do \
	  printf "  run %d/%d: " $$i $(BASELINE_RUNS); \
	  ./test-sfs-baseline --perf-only | tee -a .perf_baseline.tmp; \
	  i=$$((i+1)); \
	done
	@sort -n .perf_baseline.tmp \
	  | awk -v n=$(BASELINE_RUNS) 'NR==int((n+1)/2){print; exit}' \
	  > .perf_baseline
	@printf 'SFS_DISK_DIR=%s\n' "$${SFS_DISK_DIR:-(unset)}" >> .perf_baseline
	@printf 'WORKLOAD=v2\n' >> .perf_baseline  # keep in sync with PERF_WORKLOAD_VERSION (local/test-sfs.c)
	@awk 'NR==1{min=$$1; max=$$1} \
	      {if($$1<min)min=$$1; if($$1>max)max=$$1} \
	      END{printf "  spread: min=%.2f max=%.2f (%.1f%% of midpoint)\n", \
	          min, max, 200*(max-min)/(max+min)}' .perf_baseline.tmp
	@rm -f .perf_baseline.tmp
	@echo "Baseline (median): $$(head -1 .perf_baseline) ops/sec  ->  .perf_baseline"
.PHONY: baseline

test: test-sfs sfs-fsck
	./test-sfs
.PHONY: test

json: test-sfs sfs-fsck
	./test-sfs --json
.PHONY: json

report-json: test-sfs sfs-fsck
	@sh local/report-json.sh
.PHONY: report-json

report-json-strict: test-sfs sfs-fsck
	@sh local/report-json.sh --strict
.PHONY: report-json-strict

doctor:
	@sh local/doctor.sh
.PHONY: doctor

lint:
	@sh local/lint.sh
.PHONY: lint

lint-strict:
	@sh local/lint.sh --strict
.PHONY: lint-strict

grade: test-sfs test-sfs-baseline sfs-fsck
	$(MAKE) baseline
	./test-sfs
.PHONY: grade

smoke: test-sfs sfs-fsck
	./test-sfs -q --smoke-only
.PHONY: smoke

starter-safe: starter-check smoke
	@if sh local/run-lua-with-fallback.sh starter-safe; then \
	  :; \
	else \
	  status=$$?; \
	  if [ $$status -eq 77 ]; then \
	    echo "starter-safe: Lua runner unavailable; skipping Lua starter-safe traces"; \
	  else \
	    exit $$status; \
	  fi; \
	fi
.PHONY: starter-safe

stress: test-sfs sfs-fsck
	@i=1; while [ $$i -le $(STRESS_RUNS) ]; do \
	  printf "stress run %d/%d\n" $$i $(STRESS_RUNS); \
	  ./test-sfs -q --concurrency-only || exit $$?; \
	  ./test-sfs -q --stress-only || exit $$?; \
	  i=$$((i+1)); \
	done
.PHONY: stress

# Differential fuzz against the in-memory reference model (diagnostic).
# Override the seed with: make model-fuzz MODEL_FUZZ_SEED=N
MODEL_FUZZ_SEED ?= 1

model-fuzz: test-sfs sfs-fsck
	./test-sfs --model-fuzz=$(MODEL_FUZZ_SEED)
.PHONY: model-fuzz

# Optional challenges (not scored); exits nonzero until all are achieved.
x-traces: test-sfs sfs-fsck
	./test-sfs --x-only
.PHONY: x-traces

check: starter-check trace-check manifest-check smoke
.PHONY: check

dist-check:
	@$(MAKE) clean >/dev/null
	@$(MAKE) handout-check starter-check trace-check manifest-check
	@$(MAKE) clean >/dev/null
.PHONY: dist-check

trace-check:
	@sh local/check-traces.sh
.PHONY: trace-check

trace-list: test-sfs
	./test-sfs --list-traces
.PHONY: trace-list

lua-runner:
	@sh local/build-lua-runner.sh
.PHONY: lua-runner

trace-run: lua-runner
	@sh local/run-lua-traces.sh
.PHONY: trace-run

trace-json: lua-runner
	@sh local/run-lua-traces.sh --json
.PHONY: trace-json

trace-smoke: lua-runner
	@sh local/run-lua-traces.sh --starter-safe
.PHONY: trace-smoke

manifest-check: test-sfs
	@sh local/manifest-check.sh
.PHONY: manifest-check

handout-check:
	@sh local/handout-check.sh
.PHONY: handout-check

starter-check:
	@sh local/starter-check.sh
.PHONY: starter-check

clean:
	-rm -f $(PROGRAMS) lua-sfs-runner test-sfs-tsan *.o local/*.o *.img trace.img tsan_output.log .perf_baseline .perf_baseline.tmp
.PHONY: clean

## cut here ##
sfs-support.o: sfs-support.c sfs-disk.h
sfs-disk.o: sfs-disk.c sfs-api.h sfs-disk.h
sfs-fsck.o: sfs-fsck.c sfs-disk.h
local/test-sfs.o: local/test-sfs.c local/test-report.h sfs-api.h
local/test-report.o: local/test-report.c local/test-report.h
local/sfs-baseline-ref.o: local/sfs-baseline-ref.c sfs-api.h sfs-disk.h
