# Repository convenience targets.  The handout build lives in sfslab/Makefile.

HANDOUT = sfslab
DIST = sfslab-handout.tar
DIST_MTIME = 2024-01-01 00:00Z

.PHONY: all
all:
	$(MAKE) -C $(HANDOUT)

.PHONY: test
test:
	$(MAKE) -C $(HANDOUT) test

.PHONY: clean
clean:
	$(MAKE) -C $(HANDOUT) clean

# Rebuild the handout tarball deterministically (requires GNU tar).
.PHONY: dist
dist:
	$(MAKE) -C $(HANDOUT) clean
	@tmp=$$(mktemp -d "$${TMPDIR:-/tmp}/sfslab-dist.XXXXXX"); \
	trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	mkdir "$$tmp/$(HANDOUT)"; \
	cp -R "$(HANDOUT)/." "$$tmp/$(HANDOUT)/"; \
	find "$$tmp/$(HANDOUT)" -type d -exec chmod 755 {} +; \
	find "$$tmp/$(HANDOUT)" -type f -exec chmod 644 {} +; \
	tar --sort=name --mtime='$(DIST_MTIME)' \
	  --owner=0 --group=0 --numeric-owner -C "$$tmp" -cf "$(DIST)" "$(HANDOUT)"
	@echo "built $(DIST)"

# Build the writeup PDF from its groff source.  The source carries its own
# macro definitions, so this needs only groff-base and ghostscript (ps2pdf),
# both standard on Linux.
.PHONY: pdf
pdf: sfslab.pdf

sfslab.pdf: writeup/sfslab.roff
	groff -t -Tps writeup/sfslab.roff | ps2pdf - $@
