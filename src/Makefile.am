# src/Makefile.am - Build the Magpie executable.

bin_PROGRAMS = magpie

magpie_SOURCES = \
    main.c \
    mail-arena.c \
    mail-arena.h \
    mail-application.c \
    mail-application.h \
    mail-window.c \
    mail-window.h \
    mail-sidebar.c \
    mail-sidebar.h \
    mail-message-list.c \
    mail-message-list.h \
    mail-message-view.c \
    mail-message-view.h \
    mail-mime.c \
    mail-mime.h \
    mail-imap-id.c \
    mail-imap-id.h \
    mail-imap-retry.c \
    mail-imap-retry.h \
    mail-account.c \
    mail-account.h \
    mail-store.c \
    mail-store.h \
    mail-backend.h \
    mail-backend.c \
    mail-backend-store.c \
    mail-backend-store.h \
    mail-sync.c \
    mail-sync.h \
    mail-progress-ring.c \
    mail-progress-ring.h \
    mail-account-page.c \
    mail-account-page.h \
    mail-eta.c \
    mail-eta.h \
    mail-backend-msgraph.c \
    mail-backend-msgraph.h \
    mail-backend-imap.c \
    mail-backend-imap.h \
    mail-resources.c

BUILT_SOURCES = magpie-version.h mail-resources.c
CLEANFILES    = magpie-version.h mail-resources.c

# Regenerate magpie-version.h on every `make` so the embedded version string
# tracks the current git HEAD (and grows a -dirty suffix when the tree has
# uncommitted changes). The recipe always runs, but the file is only
# rewritten when the version actually changes — so dependent .o files stay
# cached when nothing changed.
.PHONY: magpie_version_force
magpie-version.h: magpie_version_force
	@v=`"$(top_srcdir)/build-aux/git-version.sh" "$(top_srcdir)" 2>/dev/null`; \
	test -n "$$v" || v="$(MAGPIE_VERSION)"; \
	test -n "$$v" || v="unknown"; \
	printf '#ifndef MAGPIE_VERSION_H\n#define MAGPIE_VERSION_H\n#define MAGPIE_VERSION "%s"\n#endif\n' "$$v" > $@.tmp; \
	if cmp -s $@.tmp $@ 2>/dev/null; then \
	  rm -f $@.tmp; \
	else \
	  mv $@.tmp $@; \
	  echo "  GEN      $@ ($$v)"; \
	fi
magpie_version_force:

GRESOURCE_XML  = $(top_srcdir)/data/org.gnome.Magpie.gresource.xml
GRESOURCE_DEPS = \
    $(top_srcdir)/data/ui/window.ui \
    $(top_srcdir)/data/ui/style.css

mail-resources.c: $(GRESOURCE_XML) $(GRESOURCE_DEPS)
	$(GLIB_COMPILE_RESOURCES) --target=$@ --sourcedir=$(top_srcdir)/data \
	                          --generate-source --c-name=mail $(GRESOURCE_XML)

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -DMAGPIE_DATADIR=\"$(datadir)\" \
    $(GTK_CFLAGS) \
    $(ADWAITA_CFLAGS) \
    $(GOA_CFLAGS) \
    $(SOUP_CFLAGS) \
    $(JSON_GLIB_CFLAGS) \
    $(GMIME_CFLAGS) \
    $(SQLITE_CFLAGS) \
    $(WEBKIT_CFLAGS)

magpie_LDADD = \
    $(GTK_LIBS) \
    $(ADWAITA_LIBS) \
    $(GOA_LIBS) \
    $(SOUP_LIBS) \
    $(JSON_GLIB_LIBS) \
    $(GMIME_LIBS) \
    $(SQLITE_LIBS) \
    $(WEBKIT_LIBS) \
    $(LIBETPAN_LIBS)
