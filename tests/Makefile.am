# tests/Makefile.am - Unit tests for Spool.

UNIT_TESTS = test-arena test-accounts test-sidebar test-backend-contract test-message-list test-message-view test-mime test-imap-id test-imap-retry test-store test-backend-store test-sync test-eta test-account-page test-window test-maildir-watcher test-about test-quote test-html-to-text test-outbox test-compose-window

check_PROGRAMS = $(UNIT_TESTS)
TESTS          = $(UNIT_TESTS)

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_builddir)/src \
    $(GTK_CFLAGS) \
    $(ADWAITA_CFLAGS) \
    $(GOA_CFLAGS) \
    $(JSON_GLIB_CFLAGS) \
    $(SOUP_CFLAGS) \
    $(GMIME_CFLAGS) \
    $(SQLITE_CFLAGS) \
    $(WEBKIT_CFLAGS) \
    $(XML_CFLAGS)

LDADD = \
    $(GTK_LIBS) \
    $(ADWAITA_LIBS) \
    $(GOA_LIBS) \
    $(JSON_GLIB_LIBS) \
    $(SOUP_LIBS) \
    $(GMIME_LIBS) \
    $(SQLITE_LIBS) \
    $(WEBKIT_LIBS) \
    $(XML_LIBS)

test_arena_SOURCES    = test-arena.c $(top_srcdir)/src/mail-arena.c
test_accounts_SOURCES = test-accounts.c
test_eta_SOURCES      = test-eta.c $(top_srcdir)/src/mail-eta.c
test_window_SOURCES   = test-window.c
test_window_CPPFLAGS  = $(AM_CPPFLAGS) -DSPOOL_TOP_SRCDIR=\"$(abs_top_srcdir)\"
test_about_SOURCES    = test-about.c
test_about_CPPFLAGS   = $(AM_CPPFLAGS) -DSPOOL_TOP_SRCDIR=\"$(abs_top_srcdir)\"

test_account_page_SOURCES = \
    test-account-page.c \
    $(top_srcdir)/src/mail-account-page.c \
    $(top_srcdir)/src/mail-eta.c \
    $(top_srcdir)/src/mail-progress-ring.c \
    $(top_srcdir)/src/mail-sync.c \
    $(top_srcdir)/src/mail-store.c \
    $(top_srcdir)/src/mail-backend.c \
    $(top_srcdir)/src/mail-arena.c

test_sidebar_SOURCES = \
    test-sidebar.c \
    mail-backend-fake.c \
    mail-backend-fake.h \
    $(top_srcdir)/src/mail-arena.c \
    $(top_srcdir)/src/mail-backend.c \
    $(top_srcdir)/src/mail-account.c \
    $(top_srcdir)/src/mail-maildir-watcher.c \
    $(top_srcdir)/src/mail-sidebar.c \
    $(top_srcdir)/src/mail-store.c \
    $(top_srcdir)/src/mail-backend-store.c \
    $(top_srcdir)/src/mail-sync.c \
    $(top_srcdir)/src/mail-progress-ring.c \
    $(top_srcdir)/src/mail-backend-msgraph.c \
    $(top_srcdir)/src/mail-backend-imap.c \
    $(top_srcdir)/src/mail-imap-id.c \
    $(top_srcdir)/src/mail-imap-retry.c \
    $(top_srcdir)/src/mail-mime.c

test_sidebar_LDADD = $(LDADD) $(LIBETPAN_LIBS)

test_backend_contract_SOURCES = \
    test-backend-contract.c \
    mail-backend-fake.c \
    mail-backend-fake.h \
    $(top_srcdir)/src/mail-arena.c \
    $(top_srcdir)/src/mail-backend.c

test_message_list_SOURCES = \
    test-message-list.c \
    mail-backend-fake.c \
    mail-backend-fake.h \
    $(top_srcdir)/src/mail-arena.c \
    $(top_srcdir)/src/mail-backend.c \
    $(top_srcdir)/src/mail-message-list.c

test_message_list_LDADD = $(LDADD)

test_message_view_SOURCES = \
    test-message-view.c \
    mail-backend-fake.c \
    mail-backend-fake.h \
    $(top_srcdir)/src/mail-arena.c \
    $(top_srcdir)/src/mail-backend.c \
    $(top_srcdir)/src/mail-message-view.c \
    $(top_srcdir)/src/mail-mime.c

test_message_view_LDADD = $(LDADD)

test_mime_SOURCES = \
    test-mime.c \
    $(top_srcdir)/src/mail-mime.c

test_mime_LDADD = $(LDADD)

test_quote_SOURCES = \
    test-quote.c \
    $(top_srcdir)/src/mail-quote.c

test_quote_LDADD = $(LDADD)

test_html_to_text_SOURCES = \
    test-html-to-text.c \
    $(top_srcdir)/src/mail-html-to-text.c

test_html_to_text_CPPFLAGS = $(AM_CPPFLAGS) $(XML_CFLAGS)
test_html_to_text_LDADD = $(LDADD) $(XML_LIBS)

test_outbox_SOURCES = \
    test-outbox.c \
    $(top_srcdir)/src/mail-outbox.c \
    $(top_srcdir)/src/mail-mime.c \
    $(top_srcdir)/src/mail-account.c \
    $(top_srcdir)/src/mail-arena.c \
    $(top_srcdir)/src/mail-backend.c \
    $(top_srcdir)/src/mail-backend-store.c \
    $(top_srcdir)/src/mail-backend-msgraph.c \
    $(top_srcdir)/src/mail-backend-imap.c \
    $(top_srcdir)/src/mail-imap-id.c \
    $(top_srcdir)/src/mail-imap-retry.c \
    $(top_srcdir)/src/mail-maildir-watcher.c \
    $(top_srcdir)/src/mail-store.c \
    $(top_srcdir)/src/mail-sync.c

test_outbox_LDADD = $(LDADD) $(LIBETPAN_LIBS)

# Compose dialog uses the embedded gresource bundle for its template,
# so we link against the production mail-resources.c that src/ builds.
test_compose_window_SOURCES = \
    test-compose-window.c \
    $(top_srcdir)/src/mail-compose-window.c \
    $(top_srcdir)/src/mail-outbox.c \
    $(top_srcdir)/src/mail-html-to-text.c \
    $(top_srcdir)/src/mail-quote.c \
    $(top_srcdir)/src/mail-mime.c \
    $(top_srcdir)/src/mail-account.c \
    $(top_srcdir)/src/mail-arena.c \
    $(top_srcdir)/src/mail-backend.c \
    $(top_srcdir)/src/mail-backend-store.c \
    $(top_srcdir)/src/mail-backend-msgraph.c \
    $(top_srcdir)/src/mail-backend-imap.c \
    $(top_srcdir)/src/mail-imap-id.c \
    $(top_srcdir)/src/mail-imap-retry.c \
    $(top_srcdir)/src/mail-maildir-watcher.c \
    $(top_srcdir)/src/mail-store.c \
    $(top_srcdir)/src/mail-sync.c

test_compose_window_CPPFLAGS = $(AM_CPPFLAGS)
test_compose_window_LDADD = $(top_builddir)/src/mail-resources.$(OBJEXT) $(LDADD) $(LIBETPAN_LIBS)
test_compose_window_DEPENDENCIES = $(top_builddir)/src/mail-resources.$(OBJEXT)

test_imap_id_SOURCES = \
    test-imap-id.c \
    $(top_srcdir)/src/mail-imap-id.c

test_imap_id_LDADD = $(LDADD)

test_imap_retry_SOURCES = \
    test-imap-retry.c \
    $(top_srcdir)/src/mail-imap-retry.c

test_imap_retry_LDADD = $(LDADD)

test_store_SOURCES = \
    test-store.c \
    $(top_srcdir)/src/mail-arena.c \
    $(top_srcdir)/src/mail-store.c

test_store_LDADD = $(LDADD)

test_backend_store_SOURCES = \
    test-backend-store.c \
    $(top_srcdir)/src/mail-arena.c \
    $(top_srcdir)/src/mail-store.c \
    $(top_srcdir)/src/mail-backend.c \
    $(top_srcdir)/src/mail-backend-store.c

test_backend_store_LDADD = $(LDADD)

test_sync_SOURCES = \
    test-sync.c \
    mail-backend-fake.c \
    mail-backend-fake.h \
    $(top_srcdir)/src/mail-arena.c \
    $(top_srcdir)/src/mail-store.c \
    $(top_srcdir)/src/mail-backend.c \
    $(top_srcdir)/src/mail-sync.c

test_sync_LDADD = $(LDADD)

test_maildir_watcher_SOURCES = \
    test-maildir-watcher.c \
    $(top_srcdir)/src/mail-arena.c \
    $(top_srcdir)/src/mail-store.c \
    $(top_srcdir)/src/mail-maildir-watcher.c

test_maildir_watcher_LDADD = $(LDADD)
