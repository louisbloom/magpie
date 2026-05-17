# Makefile.am - Top-level automake file for GNOME Mail

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
