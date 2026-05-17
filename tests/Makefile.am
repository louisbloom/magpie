# tests/Makefile.am - Unit tests for GNOME Mail.

UNIT_TESTS = test-arena test-accounts test-sidebar test-backend-contract test-message-list test-message-view test-mime

check_PROGRAMS = $(UNIT_TESTS)
TESTS          = $(UNIT_TESTS)

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    $(GTK_CFLAGS) \
    $(ADWAITA_CFLAGS) \
    $(GOA_CFLAGS) \
    $(JSON_GLIB_CFLAGS) \
    $(SOUP_CFLAGS) \
    $(GMIME_CFLAGS)

LDADD = \
    $(GTK_LIBS) \
    $(ADWAITA_LIBS) \
    $(GOA_LIBS) \
    $(JSON_GLIB_LIBS) \
    $(SOUP_LIBS) \
    $(GMIME_LIBS)

test_arena_SOURCES    = test-arena.c $(top_srcdir)/src/mail-arena.c
test_accounts_SOURCES = test-accounts.c

test_sidebar_SOURCES = \
    test-sidebar.c \
    mail-backend-fake.c \
    mail-backend-fake.h \
    $(top_srcdir)/src/mail-arena.c \
    $(top_srcdir)/src/mail-backend.c \
    $(top_srcdir)/src/mail-account.c \
    $(top_srcdir)/src/mail-sidebar.c \
    $(top_srcdir)/src/mail-backend-msgraph.c \
    $(top_srcdir)/src/mail-backend-imap.c

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
