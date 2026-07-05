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

# Rebuild the handout tarball deterministically from committed files
# (requires GNU tar).
.PHONY: dist
dist:
	test -f sfslab.pdf
	$(MAKE) -C $(HANDOUT) clean
	@tmp=$$(mktemp -d "$${TMPDIR:-/tmp}/sfslab-dist.XXXXXX"); \
	trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	mkdir "$$tmp/$(HANDOUT)"; \
	cp -R "$(HANDOUT)/." "$$tmp/$(HANDOUT)/"; \
	cp sfslab.pdf "$$tmp/"; \
	find "$$tmp/$(HANDOUT)" -type d -exec chmod 755 {} +; \
	find "$$tmp/$(HANDOUT)" -type f -exec chmod 644 {} +; \
	tar --sort=name --mtime='$(DIST_MTIME)' \
	  --owner=0 --group=0 --numeric-owner -C "$$tmp" -cf "$(DIST)" "$(HANDOUT)" sfslab.pdf
	@echo "built $(DIST)"

# Build the writeup PDF from its groff source.  The source carries its own
# macro definitions, so this needs only groff-base and ghostscript (ps2pdf),
# both standard on Linux.  The Omit* flags suppress ghostscript's embedded
# timestamps and document IDs, so rebuilding on the same toolchain is
# byte-identical and does not dirty the committed sfslab.pdf.  (Different
# ghostscript versions may still produce different bytes; CI therefore
# builds the PDF but does not compare it.)
.PHONY: pdf
pdf: sfslab.pdf

sfslab.pdf: writeup/sfslab.roff
	groff -t -Tps writeup/sfslab.roff | \
	  ps2pdf -dOmitInfoDate=true -dOmitXMP=true -dOmitID=true - $@
