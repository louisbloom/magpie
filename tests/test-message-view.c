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
 *
 * WebKitWebView rendering itself is not covered here — instantiating a
 * web view headlessly is brittle and the body of an HTML render isn't
 * meaningfully testable without a real display. test_view_mode_transitions
 * verifies the property side of the toggle (mode reset, has-plain-part);
 * test_unsupported_shows_status_page verifies the AdwStatusPage branch
 * by probing the visible stack child.
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
test_natural_height_does_not_propagate_giant_buffer (void)
{
  /* Regression: opening a real RFC822 message (1500+ wrapped lines from
   * inlined base64 attachments) made the GtkScrolledWindow's child
   * propagate its 17 kpx natural height upward, producing repeated
   *   Adwaita-WARNING: AdwOverlaySplitView 0x… exceeds MailWindow
   *                    height: requested 17828 px, 700 px available
   * on every message click. Fix: GTK_POLICY_AUTOMATIC unconditionally
   * + gtk_scrolled_window_set_propagate_natural_{height,width}=FALSE.
   *
   * This test seeds a message body big enough to trigger the old path
   * (≥ 2000 lines), loads it, and asserts the view's natural height
   * is bounded by the scroller's min size, not the buffer's. */
  GString *body = g_string_new (NULL);
  g_string_append (body, "Subject: huge\r\n\r\n");
  for (int i = 0; i < 2000; i++)
    g_string_append_printf (body, "line %04d of a long body\r\n", i);
  FakeMessageSpec msgs[] = {
    { "huge", "huge", "a@b.c", 1700000000, FALSE, body->str },
  };

  MailBackend *fake = mail_backend_fake_new ();
  mail_backend_fake_set_messages (fake, "inbox", msgs, G_N_ELEMENTS (msgs));

  GtkWidget *window = gtk_window_new ();
  /* Cap the window small so a propagation regression would be glaring. */
  gtk_window_set_default_size (GTK_WINDOW (window), 800, 600);

  MailMessageView *view = MAIL_MESSAGE_VIEW (mail_message_view_new ());
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (view));
  gtk_window_present (GTK_WINDOW (window));
  pump_main_loop ();

  mail_message_view_load (view, fake, "huge");
  pump_main_loop ();

  /* Measure the view's natural vertical size. With the fix it should be
   * modest (bounded by the scroller's min); without it, it tracks the
   * buffer's full extent (tens of thousands of pixels). 1500 px is a
   * comfortable ceiling well above any plausible chrome and well below
   * the regression's 17 kpx. */
  int min_h = 0, nat_h = 0;
  gtk_widget_measure (GTK_WIDGET (view), GTK_ORIENTATION_VERTICAL, -1,
                      &min_h, &nat_h, NULL, NULL);
  g_assert_cmpint (nat_h, <, 1500);

  gtk_window_destroy (GTK_WINDOW (window));
  pump_main_loop ();
  mail_backend_destroy (fake);
  g_string_free (body, TRUE);
}

static void
test_view_mode_transitions (void)
{
  /* has-plain-part flips on every load completion; view-mode resets to
   * RENDERED on every load. This pins the toggle group's binding
   * contract: a refactor that drops the notify will silently break the
   * AdwToggleGroup's per-toggle sensitivity (plain disabled on
   * HTML-only) and the "go back to rendered after switching messages"
   * behaviour the user expects. */
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

  /* User flips to source mode. */
  g_object_set (view, "view-mode", MAIL_MESSAGE_VIEW_MODE_SOURCE, NULL);
  MailMessageViewMode mode = MAIL_MESSAGE_VIEW_MODE_RENDERED;
  g_object_get (view, "view-mode", &mode, NULL);
  g_assert_cmpint (mode, ==, MAIL_MESSAGE_VIEW_MODE_SOURCE);

  /* Loading a different message resets back to RENDERED. */
  mail_message_view_load (view, fake, "html");
  pump_main_loop ();
  g_object_get (view, "view-mode", &mode, NULL);
  g_assert_cmpint (mode, ==, MAIL_MESSAGE_VIEW_MODE_RENDERED);

  gtk_window_destroy (GTK_WINDOW (window));
  pump_main_loop ();
  mail_backend_destroy (fake);
}

static void
test_unsupported_shows_status_page (void)
{
  /* Top-level application/pdf — no displayable part. The view should
   * surface an AdwStatusPage rather than dumping raw base64 bytes. We
   * can't easily inspect AdwStatusPage from outside, so probe by
   * looking for the status icon in the descendant tree. */
  FakeMessageSpec msgs[] = {
    { "pdf", "Pdf only", "a@b.c", 1700000200, FALSE,
      "Subject: t\r\n"
      "Content-Type: application/pdf\r\n"
      "Content-Transfer-Encoding: base64\r\n"
      "\r\n"
      "SGVsbG8K\r\n" },
  };

  MailBackend *fake = mail_backend_fake_new ();
  mail_backend_fake_set_messages (fake, "inbox", msgs, G_N_ELEMENTS (msgs));

  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 400, 400);

  MailMessageView *view = MAIL_MESSAGE_VIEW (mail_message_view_new ());
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (view));
  gtk_window_present (GTK_WINDOW (window));
  pump_main_loop ();

  mail_message_view_load (view, fake, "pdf");
  pump_main_loop ();

  /* The visible GtkStack child has name "status" when the picker
   * returns UNSUPPORTED. Walk down from the view to find the stack. */
  GtkStack *stack = NULL;
  for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (view));
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      if (GTK_IS_STACK (child))
        {
          stack = GTK_STACK (child);
          break;
        }
    }
  g_assert_nonnull (stack);
  g_assert_cmpstr (gtk_stack_get_visible_child_name (stack), ==, "status");

  /* Plain mode also has no plain alternative here — still status page. */
  gboolean has_plain = TRUE;
  g_object_get (view, "has-plain-part", &has_plain, NULL);
  g_assert_false (has_plain);

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
  g_test_add_func ("/message-view/natural-height-does-not-propagate",
                   test_natural_height_does_not_propagate_giant_buffer);
  g_test_add_func ("/message-view/view-mode-transitions",
                   test_view_mode_transitions);
  g_test_add_func ("/message-view/unsupported-shows-status-page",
                   test_unsupported_shows_status_page);
  return g_test_run ();
}
