# Makefile.am - Top-level automake file for Magpie

SUBDIRS = src data tests

ACLOCAL_AMFLAGS = -I m4

EXTRA_DIST = \
    autogen.sh \
    build-aux/git-version.sh \
    build-aux/format-xml.sh \
    version \
    README.md \
    NEWS \
    AUTHORS \
    COPYING \
    .clang-format

.PHONY: format
format:
	@if command -v clang-format >/dev/null 2>&1; then \
	    echo "Formatting C with clang-format..."; \
	    find $(top_srcdir)/src $(top_srcdir)/tests \
	        \( -name '*.c' -o -name '*.h' \) -print0 \
	        | xargs -0 clang-format -i; \
	else \
	    echo "clang-format not found - skipping C." >&2; \
	fi
	@echo "Formatting XML / GtkBuilder with xmllint..."; \
	$(SHELL) $(top_srcdir)/build-aux/format-xml.sh $(top_srcdir)/data
	@if command -v prettier >/dev/null 2>&1; then \
	    echo "Formatting Markdown with prettier..."; \
	    find $(top_srcdir) \
	        -path '$(top_srcdir)/build' -prune -o \
	        -path '$(top_srcdir)/.git'  -prune -o \
	        -name '*.md' -print0 \
	        | xargs -0 -r prettier --log-level warn --write; \
	else \
	    echo "prettier not found - skipping Markdown." >&2; \
	fi

# Emit compile_commands.json for clangd. Runs from scratch so bear
# sees every gcc invocation; otherwise an incremental build skips
# already-compiled translation units and leaves them out of the DB.
# Builds tests too (via `make check`) so clangd has entries for the
# files under tests/ as well.
.PHONY: bear
bear:
	@if ! command -v bear >/dev/null 2>&1; then \
	    echo "bear not found (install bear)" >&2; exit 1; \
	fi; \
	echo "Cleaning for bear interception..."; \
	$(MAKE) clean >/dev/null; \
	echo "Building under bear -> $(abs_top_srcdir)/compile_commands.json"; \
	bear --output $(abs_top_srcdir)/compile_commands.json -- $(MAKE) check
