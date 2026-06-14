# Root convenience targets. The handout build still lives in sfslab/Makefile.

HANDOUT = sfslab
DIST = sfslab-handout.tar
DIST_MTIME = 2024-01-01 00:00Z
DOCKER_IMAGE ?= sfslab-offline
DOCKER_WORKDIR ?= /work
DOCKER_RUN = docker run --rm -v "$(CURDIR):$(DOCKER_WORKDIR)" -w $(DOCKER_WORKDIR) $(DOCKER_IMAGE)

.PHONY: all
all:
	$(MAKE) -C $(HANDOUT)

.PHONY: test
test:
	$(MAKE) -C $(HANDOUT) test

.PHONY: json
json:
	@$(MAKE) -s -C $(HANDOUT) json

.PHONY: report-json
report-json:
	@$(MAKE) -s -C $(HANDOUT) report-json

.PHONY: report-json-strict
report-json-strict:
	@$(MAKE) -s -C $(HANDOUT) report-json-strict

.PHONY: lint
lint:
	$(MAKE) -C $(HANDOUT) lint

.PHONY: lint-strict
lint-strict:
	$(MAKE) -C $(HANDOUT) lint-strict

.PHONY: doctor
doctor:
	$(MAKE) -C $(HANDOUT) doctor

.PHONY: grade
grade:
	$(MAKE) -C $(HANDOUT) grade

.PHONY: smoke
smoke:
	$(MAKE) -C $(HANDOUT) smoke

.PHONY: starter-safe
starter-safe:
	$(MAKE) -C $(HANDOUT) starter-safe

.PHONY: stress
stress:
	$(MAKE) -C $(HANDOUT) stress

.PHONY: model-fuzz
model-fuzz:
	$(MAKE) -C $(HANDOUT) model-fuzz

.PHONY: x-traces
x-traces:
	$(MAKE) -C $(HANDOUT) x-traces

.PHONY: check
check:
	$(MAKE) -C $(HANDOUT) check

.PHONY: dist-check
dist-check:
	$(MAKE) -C $(HANDOUT) dist-check
	@if [ -f "$(DIST)" ]; then \
	  sh $(HANDOUT)/local/dist-verify.sh $(DIST); \
	else \
	  echo "dist-check: $(DIST) is missing; run 'make dist' first"; \
	  exit 1; \
	fi

.PHONY: dist-verify
dist-verify: dist
	@sh $(HANDOUT)/local/dist-verify.sh $(DIST)

.PHONY: dist-repro-check
dist-repro-check:
	@sh $(HANDOUT)/local/dist-repro-check.sh $(DIST)

.PHONY: trace-check
trace-check:
	$(MAKE) -C $(HANDOUT) trace-check

.PHONY: trace-list
trace-list:
	@$(MAKE) -s -C $(HANDOUT) trace-list

.PHONY: lua-runner
lua-runner:
	$(MAKE) -C $(HANDOUT) lua-runner

.PHONY: trace-run
trace-run:
	@sh $(HANDOUT)/local/run-lua-with-fallback.sh run

.PHONY: trace-json
trace-json:
	@sh $(HANDOUT)/local/run-lua-with-fallback.sh json

.PHONY: trace-smoke
trace-smoke:
	@sh $(HANDOUT)/local/run-lua-with-fallback.sh smoke

.PHONY: manifest-check
manifest-check:
	$(MAKE) -C $(HANDOUT) manifest-check

.PHONY: handout-check
handout-check:
	$(MAKE) -C $(HANDOUT) handout-check

.PHONY: starter-check
starter-check:
	$(MAKE) -C $(HANDOUT) starter-check

.PHONY: clean
clean:
	$(MAKE) -C $(HANDOUT) clean

.PHONY: dist
dist:
	$(MAKE) -C $(HANDOUT) clean
	$(MAKE) -C $(HANDOUT) dist-check
	$(MAKE) -C $(HANDOUT) clean
	@tmp=$$(mktemp -d "$${TMPDIR:-/tmp}/sfslab-dist.XXXXXX"); \
	trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	mkdir "$$tmp/$(HANDOUT)"; \
	cp -R "$(HANDOUT)/." "$$tmp/$(HANDOUT)/"; \
	find "$$tmp/$(HANDOUT)" -type d -exec chmod 755 {} +; \
	find "$$tmp/$(HANDOUT)" -type f -exec chmod 644 {} +; \
	find "$$tmp/$(HANDOUT)/local" -type f -name '*.sh' -exec chmod 755 {} +; \
	if tar --version 2>/dev/null | grep -qi 'gnu tar'; then \
	  tar --sort=name --mtime='$(DIST_MTIME)' \
	    --owner=0 --group=0 --numeric-owner -C "$$tmp" -cf "$(DIST)" "$(HANDOUT)"; \
	else \
	  tar -C "$$tmp" -cf "$(DIST)" "$(HANDOUT)"; \
	fi

.PHONY: docker-image
docker-image:
	docker build -t $(DOCKER_IMAGE) .

.PHONY: docker-shell
docker-shell: docker-image
	docker run --rm -it -v "$(CURDIR):$(DOCKER_WORKDIR)" -w $(DOCKER_WORKDIR)/$(HANDOUT) $(DOCKER_IMAGE)

.PHONY: docker-check
docker-check: docker-image
	$(DOCKER_RUN) sh -c 'make clean && make check'

.PHONY: docker-dist-check
docker-dist-check: docker-image
	$(DOCKER_RUN) sh -c 'make clean && make dist-check'

.PHONY: docker-starter-safe
docker-starter-safe: docker-image
	$(DOCKER_RUN) sh -c 'make clean && make starter-safe'

.PHONY: docker-stress
docker-stress: docker-image
	$(DOCKER_RUN) sh -c 'make clean && make stress'

.PHONY: docker-trace-smoke
docker-trace-smoke:
	@docker build -q -t $(DOCKER_IMAGE) . >/dev/null
	@$(DOCKER_RUN) sh -c 'make -s clean >/dev/null && cd $(HANDOUT) && make -s lua-runner >/dev/null && sh local/run-lua-traces.sh --starter-safe; status=$$?; make -s clean >/dev/null; exit $$status'

.PHONY: docker-trace-run
docker-trace-run:
	@docker build -q -t $(DOCKER_IMAGE) . >/dev/null
	@$(DOCKER_RUN) sh -c 'make -s clean >/dev/null && cd $(HANDOUT) && make -s lua-runner >/dev/null && sh local/run-lua-traces.sh; status=$$?; make -s clean >/dev/null; exit $$status'

.PHONY: docker-trace-json
docker-trace-json:
	@docker build -q -t $(DOCKER_IMAGE) . >/dev/null
	@$(DOCKER_RUN) sh -c 'make -s clean >/dev/null && cd $(HANDOUT) && make -s lua-runner >/dev/null && sh local/run-lua-traces.sh --json; status=$$?; make -s clean >/dev/null; exit $$status'

.PHONY: docker-report-json
docker-report-json:
	@docker build -q -t $(DOCKER_IMAGE) . >/dev/null
	@$(DOCKER_RUN) sh -c 'make -s clean >/dev/null && cd $(HANDOUT) && make -s report-json'
