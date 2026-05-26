/* tests/test-outbox.c - mail-outbox append regression tests.
 *
 * mail_outbox_append writes a debug copy of an outbound message to
 * <account_root>/Outbox.mbox so the user can verify it with mutt/less
 * before SMTP is wired up. The mbox file MUST be readable by mutt and
 * less; that means proper "From " envelopes and mboxrd ">From"
 * escaping in the body.
 */

#include "../src/mail-account.h"
#include "../src/mail-mime.h"
#include "../src/mail-outbox.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct
{
  char *root;
  char *home_orig;
  MailAccount *account;
} Fixture;

static void
fixture_set_up (Fixture *fx, gconstpointer unused)
{
  (void) unused;
  fx->root = g_dir_make_tmp ("spool-outbox-XXXXXX", NULL);
  g_assert_nonnull (fx->root);

  /* mail_outbox computes <home>/Mail/<identity>/Outbox.mbox. We hijack
   * $HOME so the temp dir is the home. */
  fx->home_orig = g_strdup (g_getenv ("HOME"));
  g_setenv ("HOME", fx->root, TRUE);

  /* Create the expected per-account directory layout. */
  g_autofree char *mail_dir = g_build_filename (fx->root, "Mail", "alice@example.com", NULL);
  g_assert_cmpint (g_mkdir_with_parents (mail_dir, 0700), ==, 0);

  fx->account = mail_account_new_for_test (NULL, "alice@example.com", "Test");
  g_assert_nonnull (fx->account);
}

static void
fixture_tear_down (Fixture *fx, gconstpointer unused)
{
  (void) unused;
  if (fx->account != NULL)
    mail_account_free (fx->account);

  /* Best-effort recursive rmdir of the temp tree. */
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) f = g_file_new_for_path (fx->root);
  GFileEnumerator *en = g_file_enumerate_children (f, "standard::name",
                                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
  /* Don't bother walking — just rm -rf via shell for simplicity. */
  if (en != NULL)
    g_object_unref (en);
  g_autofree char *cmd = g_strdup_printf ("rm -rf '%s'", fx->root);
  (void) g_spawn_command_line_sync (cmd, NULL, NULL, NULL, NULL);

  g_free (fx->root);
  if (fx->home_orig != NULL)
    {
      g_setenv ("HOME", fx->home_orig, TRUE);
      g_free (fx->home_orig);
    }
  else
    g_unsetenv ("HOME");
}

static char *
read_mbox (Fixture *fx)
{
  g_autofree char *path = g_build_filename (fx->root, "Mail", "alice@example.com", "Outbox.mbox", NULL);
  g_autofree char *contents = NULL;
  gsize len = 0;
  g_autoptr (GError) error = NULL;
  if (!g_file_get_contents (path, &contents, &len, &error))
    return NULL;
  return g_steal_pointer (&contents);
}

static void
test_basic_append (Fixture *fx, gconstpointer unused)
{
  (void) unused;
  g_autoptr (GError) error = NULL;
  gboolean ok = mail_outbox_append (fx->account,
                                    "bob@example.com",
                                    "Hello Bob",
                                    "First line\nSecond line\n",
                                    NULL,
                                    &error);
  g_assert_no_error (error);
  g_assert_true (ok);

  g_autofree char *mbox = read_mbox (fx);
  g_assert_nonnull (mbox);

  /* mbox envelope: starts with "From " at column 0. */
  g_assert_true (g_str_has_prefix (mbox, "From "));
  /* Headers and body present. */
  g_assert_true (strstr (mbox, "From: alice@example.com") != NULL);
  g_assert_true (strstr (mbox, "To: bob@example.com") != NULL);
  g_assert_true (strstr (mbox, "Subject: Hello Bob") != NULL);
  g_assert_true (strstr (mbox, "MIME-Version: 1.0") != NULL);
  g_assert_true (strstr (mbox, "Content-Type: text/plain") != NULL);
  g_assert_true (strstr (mbox, "First line") != NULL);
  g_assert_true (strstr (mbox, "Second line") != NULL);
}

static void
test_two_appends_make_two_envelopes (Fixture *fx, gconstpointer unused)
{
  (void) unused;
  g_assert_true (mail_outbox_append (fx->account, "a@x", "one", "a", NULL, NULL));
  g_assert_true (mail_outbox_append (fx->account, "b@x", "two", "b", NULL, NULL));

  g_autofree char *mbox = read_mbox (fx);
  g_assert_nonnull (mbox);

  /* Two "From " envelopes (line starts). */
  int envelopes = 0;
  const char *p = mbox;
  while (p != NULL)
    {
      const char *hit = (p == mbox && g_str_has_prefix (p, "From ")) ? p : strstr (p, "\nFrom ");
      if (hit == NULL)
        break;
      envelopes++;
      p = hit + 1;
    }
  g_assert_cmpint (envelopes, ==, 2);
}

static void
test_mboxrd_from_escaping (Fixture *fx, gconstpointer unused)
{
  (void) unused;
  /* A body line that starts with "From " or ">From " must get a '>'
   * prefix so mutt doesn't confuse it with the next envelope. */
  g_assert_true (mail_outbox_append (fx->account,
                                     "bob@x",
                                     "from-test",
                                     "intro line\n"
                                     "From hell with love\n"
                                     ">From the deep\n"
                                     "end\n",
                                     NULL, NULL));

  g_autofree char *mbox = read_mbox (fx);
  g_assert_nonnull (mbox);
  /* Original "From hell" is escaped as ">From hell". */
  g_assert_true (strstr (mbox, "\n>From hell with love\n") != NULL);
  /* Original ">From the deep" becomes ">>From the deep". */
  g_assert_true (strstr (mbox, "\n>>From the deep\n") != NULL);
}

static void
test_reply_headers (Fixture *fx, gconstpointer unused)
{
  (void) unused;
  static const char raw[] =
      "From: Alice <alice@example.com>\r\n"
      "Subject: original\r\n"
      "Message-ID: <orig@example.com>\r\n"
      "References: <prev@example.com>\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "x\r\n";
  MailMimeReplySource *src = mail_mime_extract_reply_source ((const guint8 *) raw, sizeof raw - 1);
  g_assert_nonnull (src);
  g_assert_true (mail_outbox_append (fx->account, "alice@example.com",
                                     "Re: original", "body", src, NULL));
  mail_mime_reply_source_free (src);

  g_autofree char *mbox = read_mbox (fx);
  g_assert_nonnull (mbox);
  g_assert_true (strstr (mbox, "In-Reply-To: <orig@example.com>") != NULL);
  g_assert_true (strstr (mbox, "References:") != NULL);
  /* References should include both the prior chain and the new id. */
  g_assert_true (strstr (mbox, "<prev@example.com>") != NULL);
  g_assert_true (strstr (mbox, "<orig@example.com>") != NULL);
}

static void
test_message_id_present (Fixture *fx, gconstpointer unused)
{
  (void) unused;
  g_assert_true (mail_outbox_append (fx->account, "bob@x", "t", "b", NULL, NULL));
  g_autofree char *mbox = read_mbox (fx);
  g_assert_nonnull (mbox);
  g_assert_true (strstr (mbox, "Message-Id:") != NULL || strstr (mbox, "Message-ID:") != NULL);
  g_assert_true (strstr (mbox, "Date:") != NULL);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/outbox/basic-append", Fixture, NULL, fixture_set_up, test_basic_append, fixture_tear_down);
  g_test_add ("/outbox/two-appends-make-two-envelopes", Fixture, NULL, fixture_set_up, test_two_appends_make_two_envelopes, fixture_tear_down);
  g_test_add ("/outbox/mboxrd-from-escaping", Fixture, NULL, fixture_set_up, test_mboxrd_from_escaping, fixture_tear_down);
  g_test_add ("/outbox/reply-headers", Fixture, NULL, fixture_set_up, test_reply_headers, fixture_tear_down);
  g_test_add ("/outbox/message-id-present", Fixture, NULL, fixture_set_up, test_message_id_present, fixture_tear_down);

  return g_test_run ();
}
