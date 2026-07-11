# Repository convenience targets.  The handout build lives in sfslab/Makefile.

HANDOUT = sfslab
DIST = sfslab-handout.tar
DIST_MTIME = 2024-01-01 00:00Z
HANDOUT_FILES = .clang-format Makefile README sfs-api.h sfs-baseline-ref.c \
	sfs-disk.c sfs-disk.h sfs-fsck.c sfs-support.c test-sfs.c

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
# (requires GNU tar).  The PDF is packed as-is, never rebuilt, so the
# tarball does not depend on the PDF toolchain; the mtime check below
# warns when the writeup source looks newer than the packed PDF (a
# warning, not an error: fresh checkouts have uniform mtimes).
.PHONY: dist
dist:
	test -f sfslab.pdf
	@if [ writeup/sfslab.roff -nt sfslab.pdf ]; then \
	  echo "warning: sfslab.pdf is older than writeup/sfslab.roff;" >&2; \
	  echo "         run 'make pdf' first or the tarball ships a stale writeup" >&2; \
	fi
	@tmp=$$(mktemp -d "$${TMPDIR:-/tmp}/sfslab-dist.XXXXXX"); \
	trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	mkdir "$$tmp/$(HANDOUT)"; \
	cp $(addprefix $(HANDOUT)/,$(HANDOUT_FILES)) "$$tmp/$(HANDOUT)/"; \
	cp sfslab.pdf "$$tmp/"; \
	find "$$tmp" -type d -exec chmod 755 {} +; \
	find "$$tmp" -type f -exec chmod 644 {} +; \
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
