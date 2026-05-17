/* tests/test-imap-id.c - mail_imap_id_{encode,decode} round-trip. */

#include "../src/mail-imap-id.h"

#include <glib.h>

static void
test_round_trip_inbox (void)
{
  g_autofree gchar *id = mail_imap_id_encode (12345, 7, "INBOX");
  g_assert_nonnull (id);

  guint32 vu = 0, uid = 0;
  const char *folder = NULL;
  g_assert_true (mail_imap_id_decode (id, &vu, &uid, &folder));
  g_assert_cmpuint (vu, ==, 12345);
  g_assert_cmpuint (uid, ==, 7);
  g_assert_cmpstr (folder, ==, "INBOX");
}

static void
test_round_trip_gmail_label (void)
{
  /* Folder names with brackets, slashes, spaces — Gmail's label
   * naming convention. The SOH delimiter is safe because IMAP
   * mailbox names use printable bytes only. */
  g_autofree gchar *id = mail_imap_id_encode (999999, 4242, "[Gmail]/All Mail");
  guint32 vu = 0, uid = 0;
  const char *folder = NULL;
  g_assert_true (mail_imap_id_decode (id, &vu, &uid, &folder));
  g_assert_cmpuint (vu, ==, 999999);
  g_assert_cmpuint (uid, ==, 4242);
  g_assert_cmpstr (folder, ==, "[Gmail]/All Mail");
}

static void
test_extremes (void)
{
  g_autofree gchar *id = mail_imap_id_encode (G_MAXUINT32, G_MAXUINT32, "x");
  guint32 vu = 0, uid = 0;
  const char *folder = NULL;
  g_assert_true (mail_imap_id_decode (id, &vu, &uid, &folder));
  g_assert_cmpuint (vu, ==, G_MAXUINT32);
  g_assert_cmpuint (uid, ==, G_MAXUINT32);
  g_assert_cmpstr (folder, ==, "x");
}

static void
test_malformed (void)
{
  guint32 vu = 0, uid = 0;
  const char *folder = NULL;

  /* NULL */
  g_assert_false (mail_imap_id_decode (NULL, &vu, &uid, &folder));
  /* No SOH at all */
  g_assert_false (mail_imap_id_decode ("12345:7:INBOX", &vu, &uid, &folder));
  /* Only one SOH */
  g_assert_false (mail_imap_id_decode ("12345\x01"
                                       "7INBOX",
                                       &vu, &uid, &folder));
  /* Empty uidvalidity */
  g_assert_false (mail_imap_id_decode ("\x01"
                                       "7\x01"
                                       "INBOX",
                                       &vu, &uid, &folder));
  /* Empty uid */
  g_assert_false (mail_imap_id_decode ("12345\x01\x01"
                                       "INBOX",
                                       &vu, &uid, &folder));
  /* Empty folder */
  g_assert_false (mail_imap_id_decode ("12345\x01"
                                       "7\x01",
                                       &vu, &uid, &folder));
  /* Non-numeric uidvalidity */
  g_assert_false (mail_imap_id_decode ("abc\x01"
                                       "7\x01"
                                       "INBOX",
                                       &vu, &uid, &folder));
  /* Overflow uidvalidity */
  g_assert_false (mail_imap_id_decode ("99999999999\x01"
                                       "7\x01"
                                       "INBOX",
                                       &vu, &uid, &folder));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/imap-id/round-trip-inbox", test_round_trip_inbox);
  g_test_add_func ("/imap-id/round-trip-gmail-label", test_round_trip_gmail_label);
  g_test_add_func ("/imap-id/extremes", test_extremes);
  g_test_add_func ("/imap-id/malformed", test_malformed);
  return g_test_run ();
}
