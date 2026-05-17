/* tests/test-message-view.c - MailMessageView regression tests.
 *
 * Drives the real MailMessageView widget with the fake backend so the
 * production fetch / buffer / property-notify path runs end-to-end. A
 * custom GLib log writer counts any "snapshot" or "allocation" warning
 * that fires while the view is mapped in a window and loading state
 * shuffles through it.
 *
 * Production trigger that motivated this test:
 *   "Trying to snapshot GtkGizmo without a current allocation"
 * fired intermittently on every message click. A gdb-captured bt
 * showed the warning descending through gtk_scrolled_window_snapshot
 * into the horizontal scrollbar's internal slider gizmo while
 * AdwNavigationView's page-swap animation re-evaluated AUTOMATIC
 * policy. The fix pins both panes' scrolled-window policies to
 * GTK_POLICY_ALWAYS (with overlay-scrolling off) so the scrollbar
 * subtree stays permanently allocated.
 *
 * Caveat (same as test-message-list's no-warnings test): the
 * specific snapshot warning only fires under the frame-clock-driven
 * snapshot path of a live display, which the offscreen backend
 * doesn't reproduce. We keep this as a defensive scaffold for the
 * adjacent class of warnings (allocation / unrealized children /
 * focus) that *do* fire headlessly.
 */

#include "mail-backend-fake.h"

#include "../src/mail-message-view.h"

#include <adwaita.h>
#include <gtk/gtk.h>
#include <string.h>

static guint suspect_warnings = 0;

static GLogWriterOutput
test_log_writer (GLogLevelFlags level,
                 const GLogField *fields,
                 gsize n_fields,
                 gpointer user_data)
{
  if (level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL))
    {
      for (gsize i = 0; i < n_fields; i++)
        {
          if (g_strcmp0 (fields[i].key, "MESSAGE") == 0 && fields[i].value != NULL)
            {
              const char *msg = (const char *) fields[i].value;
              if (strstr (msg, "snapshot") != NULL || strstr (msg, "allocation") != NULL || strstr (msg, "unrealized") != NULL)
                suspect_warnings++;
            }
        }
    }
  return g_log_writer_default (level, fields, n_fields, user_data);
}

static void
pump_main_loop (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static void
test_load_in_mapped_window (void)
{
  FakeMessageSpec msgs[] = {
    { "m1", "First subject", "alice@example.com", 1700000000, FALSE,
      "Subject: First subject\r\n"
      "From: alice@example.com\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "Body 1\r\n" },
    { "m2", "Second subject", "bob@example.com", 1700000100, FALSE,
      "Subject: Second subject\r\n"
      "From: bob@example.com\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "Body 2\r\n" },
  };

  suspect_warnings = 0;

  MailBackend *fake = mail_backend_fake_new ();
  mail_backend_fake_set_messages (fake, "inbox", msgs, G_N_ELEMENTS (msgs));

  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 400, 400);

  MailMessageView *view = MAIL_MESSAGE_VIEW (mail_message_view_new ());
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (view));

  gtk_window_present (GTK_WINDOW (window));
  pump_main_loop ();

  /* Two back-to-back loads to mimic the click-then-click-another path
   * — that's the production sequence that surfaced the warning. */
  mail_message_view_load (view, fake, "m1");
  pump_main_loop ();
  mail_message_view_load (view, fake, "m2");
  pump_main_loop ();

  gtk_window_destroy (GTK_WINDOW (window));
  pump_main_loop ();

  mail_backend_destroy (fake);

  g_assert_cmpuint (suspect_warnings, ==, 0);
}

static void
test_properties_emit_notify (void)
{
  /* has-plain-part flips on every load completion; show-plain resets
   * to FALSE on every load. This pins the toggle's binding contract so
   * a refactor that drops the notify won't silently break the
   * header-bar toggle's visibility / activation. */
  FakeMessageSpec msgs[] = {
    { "html", "Html only", "a@b.c", 1700000000, FALSE,
      "Subject: t\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n"
      "<p>html</p>\r\n" },
    { "plain", "Plain only", "a@b.c", 1700000100, FALSE,
      "Subject: t\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "plain body\r\n" },
  };

  MailBackend *fake = mail_backend_fake_new ();
  mail_backend_fake_set_messages (fake, "inbox", msgs, G_N_ELEMENTS (msgs));

  /* Map the view in a window so it goes through the realize+allocate
   * pass before we drive loads — closer to the production environment. */
  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 400, 400);

  MailMessageView *view = MAIL_MESSAGE_VIEW (mail_message_view_new ());
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (view));
  gtk_window_present (GTK_WINDOW (window));
  pump_main_loop ();

  guint has_plain_notifies = 0;
  g_signal_connect_swapped (view, "notify::has-plain-part",
                            G_CALLBACK (g_atomic_int_inc), &has_plain_notifies);

  /* HTML-only first: has-plain-part should end FALSE. */
  mail_message_view_load (view, fake, "html");
  pump_main_loop ();
  gboolean has_plain = TRUE;
  g_object_get (view, "has-plain-part", &has_plain, NULL);
  g_assert_false (has_plain);

  /* Plain-only next: has-plain-part should flip to TRUE and notify. */
  mail_message_view_load (view, fake, "plain");
  pump_main_loop ();
  g_object_get (view, "has-plain-part", &has_plain, NULL);
  g_assert_true (has_plain);

  g_assert_cmpuint (has_plain_notifies, >=, 2);

  gtk_window_destroy (GTK_WINDOW (window));
  pump_main_loop ();
  mail_backend_destroy (fake);
}

int
main (int argc,
      char *argv[])
{
  gtk_test_init (&argc, &argv, NULL);
  adw_init ();

  g_log_set_writer_func (test_log_writer, NULL, NULL);

  g_test_add_func ("/message-view/load-in-mapped-window",
                   test_load_in_mapped_window);
  g_test_add_func ("/message-view/properties-emit-notify",
                   test_properties_emit_notify);
  return g_test_run ();
}
