# tests/Makefile.am - Unit tests for GNOME Mail.

UNIT_TESTS = test-arena test-accounts test-sidebar test-backend-contract test-message-list test-message-view test-mime test-imap-id test-store test-backend-store test-sync test-eta test-account-page

check_PROGRAMS = $(UNIT_TESTS)
TESTS          = $(UNIT_TESTS)

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    $(GTK_CFLAGS) \
    $(ADWAITA_CFLAGS) \
    $(GOA_CFLAGS) \
    $(JSON_GLIB_CFLAGS) \
    $(SOUP_CFLAGS) \
    $(GMIME_CFLAGS) \
    $(SQLITE_CFLAGS) \
    $(WEBKIT_CFLAGS)

LDADD = \
    $(GTK_LIBS) \
    $(ADWAITA_LIBS) \
    $(GOA_LIBS) \
    $(JSON_GLIB_LIBS) \
    $(SOUP_LIBS) \
    $(GMIME_LIBS) \
    $(SQLITE_LIBS) \
    $(WEBKIT_LIBS)

test_arena_SOURCES    = test-arena.c $(top_srcdir)/src/mail-arena.c
test_accounts_SOURCES = test-accounts.c
test_eta_SOURCES      = test-eta.c $(top_srcdir)/src/mail-eta.c

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
    $(top_srcdir)/src/mail-sidebar.c \
    $(top_srcdir)/src/mail-store.c \
    $(top_srcdir)/src/mail-backend-store.c \
    $(top_srcdir)/src/mail-sync.c \
    $(top_srcdir)/src/mail-progress-ring.c \
    $(top_srcdir)/src/mail-backend-msgraph.c \
    $(top_srcdir)/src/mail-backend-imap.c \
    $(top_srcdir)/src/mail-imap-id.c \
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

test_imap_id_SOURCES = \
    test-imap-id.c \
    $(top_srcdir)/src/mail-imap-id.c

test_imap_id_LDADD = $(LDADD)

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
