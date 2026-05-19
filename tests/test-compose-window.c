/* tests/test-compose-window.c - MailComposeWindow regression tests.
 *
 * Exercise the reply factory end-to-end: parse a raw RFC 5322 message,
 * pre-fill To/Subject/body, and verify that Send writes the expected
 * shape to the account's Outbox.mbox.
 */

#include "../src/mail-account.h"
#include "../src/mail-compose-window.h"

#include <adwaita.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
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
  fx->root = g_dir_make_tmp ("magpie-compose-XXXXXX", NULL);
  g_assert_nonnull (fx->root);

  fx->home_orig = g_strdup (g_getenv ("HOME"));
  g_setenv ("HOME", fx->root, TRUE);

  g_autofree char *mail_dir = g_build_filename (fx->root, "Mail", "me@example.com", NULL);
  g_assert_cmpint (g_mkdir_with_parents (mail_dir, 0700), ==, 0);

  fx->account = mail_account_new_for_test (NULL, "me@example.com", "Test");
  g_assert_nonnull (fx->account);
}

static void
fixture_tear_down (Fixture *fx, gconstpointer unused)
{
  (void) unused;
  if (fx->account != NULL)
    mail_account_free (fx->account);

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
read_outbox (Fixture *fx)
{
  g_autofree char *path = g_build_filename (fx->root, "Mail", "me@example.com", "Outbox.mbox", NULL);
  g_autofree char *contents = NULL;
  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return NULL;
  return g_steal_pointer (&contents);
}

static GBytes *
sample_message_bytes (void)
{
  static const char raw[] =
      "From: Alice Example <alice@example.com>\r\n"
      "To: me@example.com\r\n"
      "Subject: Hello there\r\n"
      "Message-ID: <orig@example.com>\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "Hi me,\r\n"
      "\r\n"
      "Can you take a look at the patch?\r\n";
  return g_bytes_new_static (raw, sizeof raw - 1);
}

static void
test_reply_prefills_fields (Fixture *fx, gconstpointer unused)
{
  (void) unused;
  g_autoptr (GBytes) raw = sample_message_bytes ();
  GtkWidget *dialog = mail_compose_window_new_reply (fx->account, raw);
  g_assert_nonnull (dialog);

  /* To = original From (no Reply-To set). */
  g_assert_cmpstr (mail_compose_window_peek_to (MAIL_COMPOSE_WINDOW (dialog)), ==, "alice@example.com");
  /* Subject prefixed with "Re: ", idempotent on repeat. */
  g_assert_cmpstr (mail_compose_window_peek_subject (MAIL_COMPOSE_WINDOW (dialog)), ==, "Re: Hello there");

  g_autofree char *body = mail_compose_window_dup_body (MAIL_COMPOSE_WINDOW (dialog));
  g_assert_nonnull (body);
  /* Attribution and quoted body. */
  g_assert_true (strstr (body, "Alice Example <alice@example.com> writes:") != NULL);
  g_assert_true (strstr (body, "> Hi me,") != NULL);
  g_assert_true (strstr (body, "> Can you take a look at the patch?") != NULL);
  /* Empty-line prefix: ">", no trailing space. */
  g_assert_true (strstr (body, "\n>\n") != NULL);

  g_object_unref (g_object_ref_sink (dialog));
}

static void
test_send_writes_outbox (Fixture *fx, gconstpointer unused)
{
  (void) unused;
  g_autoptr (GBytes) raw = sample_message_bytes ();
  GtkWidget *dialog = mail_compose_window_new_reply (fx->account, raw);
  g_assert_nonnull (dialog);

  g_autoptr (GError) error = NULL;
  gboolean ok = mail_compose_window_send (MAIL_COMPOSE_WINDOW (dialog), &error);
  g_assert_no_error (error);
  g_assert_true (ok);

  g_autofree char *mbox = read_outbox (fx);
  g_assert_nonnull (mbox);
  g_assert_true (g_str_has_prefix (mbox, "From "));
  g_assert_true (strstr (mbox, "Subject: Re: Hello there") != NULL);
  g_assert_true (strstr (mbox, "To: alice@example.com") != NULL);
  g_assert_true (strstr (mbox, "From: me@example.com") != NULL);
  /* The reply context propagates In-Reply-To. */
  g_assert_true (strstr (mbox, "In-Reply-To: <orig@example.com>") != NULL);
  /* Body content is preserved. */
  g_assert_true (strstr (mbox, "Alice Example <alice@example.com> writes:") != NULL);
  g_assert_true (strstr (mbox, "> Hi me,") != NULL);

  g_object_unref (g_object_ref_sink (dialog));
}

static void
test_html_only_uses_html_to_text (Fixture *fx, gconstpointer unused)
{
  (void) unused;
  static const char raw[] =
      "From: Alice <alice@example.com>\r\n"
      "Subject: html only\r\n"
      "Message-ID: <h1@example.com>\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n"
      "<p>greetings,</p><p>see attached idea</p>\r\n";
  g_autoptr (GBytes) bytes = g_bytes_new_static (raw, sizeof raw - 1);
  GtkWidget *dialog = mail_compose_window_new_reply (fx->account, bytes);
  g_assert_nonnull (dialog);

  g_autofree char *body = mail_compose_window_dup_body (MAIL_COMPOSE_WINDOW (dialog));
  g_assert_nonnull (body);
  /* HTML must be converted to plain text *before* quoting — no raw
   * tags in the quote. */
  g_assert_true (strstr (body, "<p>") == NULL);
  g_assert_true (strstr (body, "> greetings,") != NULL);
  g_assert_true (strstr (body, "> see attached idea") != NULL);

  g_object_unref (g_object_ref_sink (dialog));
}

int
main (int argc, char *argv[])
{
  gtk_test_init (&argc, &argv, NULL);
  adw_init ();

  g_test_add ("/compose-window/reply-prefills-fields",
              Fixture, NULL, fixture_set_up, test_reply_prefills_fields, fixture_tear_down);
  g_test_add ("/compose-window/send-writes-outbox",
              Fixture, NULL, fixture_set_up, test_send_writes_outbox, fixture_tear_down);
  g_test_add ("/compose-window/html-only-uses-html-to-text",
              Fixture, NULL, fixture_set_up, test_html_only_uses_html_to_text, fixture_tear_down);

  return g_test_run ();
}
