/* tests/test-message-list.c - MailMessageList regression tests.
 *
 * Drives the real MailMessageList widget with the fake backend so
 * gtk_list_box_bind_model runs the production row factory. A custom
 * GLib log writer counts any "markup" warning that fires during the
 * test; the markup-safety regression asserts the count is zero after
 * loading messages whose subjects and from-fields contain Pango
 * markup-trippy characters (& < > @).
 *
 * Production trigger that motivated this test:
 *   "Failed to set text 'Wise <noreply@wise.com> · 12:21' from markup
 *    due to error parsing markup: ... 'noreply@wise.com' is not a
 *    valid name: '@'"
 * AdwActionRow renders title/subtitle as Pango markup by default, so
 * the fix is adw_preferences_row_set_use_markup(row, FALSE) on every
 * row built in mail-message-list.c::build_row_widget.
 */

#include "mail-backend-fake.h"

#include "../src/mail-message-list.h"

#include <adwaita.h>
#include <gtk/gtk.h>
#include <string.h>

static guint markup_warnings = 0;

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
              if (strstr (msg, "markup") != NULL)
                markup_warnings++;
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

typedef struct
{
  MailMessageList *list;
  MailBackend *fake;
} Fixture;

static void
fixture_set_up (Fixture *f, gconstpointer ud)
{
  FakeMessageSpec msgs[] = {
    { "m1", "First subject", "alice@example.com", 1700000000, FALSE, "body1" },
    { "m2", "Second subject", "bob@example.com", 1700000100, TRUE, "body2" },
    { "m3", "Third subject", "carol@example.com", 1700000200, FALSE, "body3" },
  };

  markup_warnings = 0;
  f->fake = mail_backend_fake_new ();
  mail_backend_fake_set_messages (f->fake, "inbox", msgs, G_N_ELEMENTS (msgs));

  f->list = MAIL_MESSAGE_LIST (mail_message_list_new ());
  g_object_ref_sink (f->list);

  mail_message_list_load (f->list, f->fake, "inbox", 10);
  pump_main_loop ();
}

static void
fixture_tear_down (Fixture *f, gconstpointer ud)
{
  g_object_unref (f->list);
  pump_main_loop ();
  mail_backend_destroy (f->fake);
}

static void
test_rows_populated (Fixture *f, gconstpointer ud)
{
  GtkListBox *list_box = _mail_message_list_get_list_box_for_test (f->list);
  g_assert_nonnull (list_box);
  for (int i = 0; i < 3; i++)
    g_assert_nonnull (gtk_list_box_get_row_at_index (list_box, i));
  g_assert_null (gtk_list_box_get_row_at_index (list_box, 3));
}

static void
test_row_is_activatable (Fixture *f, gconstpointer ud)
{
  /* Regression: AdwActionRow defaults to activatable=FALSE. Without
   * an explicit opt-in, clicking a message row was a no-op. Same
   * class of bug as the sidebar's folder-row-activatable regression. */
  GtkListBox *list_box = _mail_message_list_get_list_box_for_test (f->list);
  for (int i = 0; i < 3; i++)
    {
      GtkListBoxRow *row = gtk_list_box_get_row_at_index (list_box, i);
      g_assert_nonnull (row);
      g_assert_true (gtk_list_box_row_get_activatable (row));
    }
}

typedef struct
{
  guint count;
  char *message_id;
  char *subject;
  gpointer backend;
} ActivationCapture;

static void
on_message_activated (MailMessageList *list,
                      gpointer backend,
                      const char *message_id,
                      const char *subject,
                      gpointer user_data)
{
  ActivationCapture *cap = user_data;
  cap->count++;
  g_free (cap->message_id);
  g_free (cap->subject);
  cap->message_id = g_strdup (message_id);
  cap->subject = g_strdup (subject);
  cap->backend = backend;
}

static void
test_activation_emits_signal (Fixture *f, gconstpointer ud)
{
  ActivationCapture cap = { 0 };
  gulong handler = g_signal_connect (f->list, "message-activated",
                                     G_CALLBACK (on_message_activated), &cap);

  GtkListBox *list_box = _mail_message_list_get_list_box_for_test (f->list);
  GtkListBoxRow *first = gtk_list_box_get_row_at_index (list_box, 0);
  g_assert_nonnull (first);

  g_signal_emit_by_name (list_box, "row-activated", first);

  g_assert_cmpuint (cap.count, ==, 1);
  g_assert_cmpstr (cap.message_id, ==, "m1");
  g_assert_cmpstr (cap.subject, ==, "First subject");
  g_assert_true (cap.backend == f->fake);

  g_signal_handler_disconnect (f->list, handler);
  g_free (cap.message_id);
  g_free (cap.subject);
}

static void
test_no_warnings_when_realized_and_snapshotted (Fixture *f, gconstpointer ud)
{
  /* Smoke check: map the widget inside a real GtkWindow and cycle it
   * through loading→list. gtk_test_init makes Gtk-WARNINGs fatal, so
   * any warning fired in the headless render path aborts the test.
   *
   * Caveat: the original "Trying to snapshot GtkGizmo without a
   * current allocation" only fires under the frame-clock-driven
   * snapshot path of a live display, which the offscreen backend
   * doesn't reproduce. This test caught it once verified visually,
   * but is documenting-only against that specific class; we keep it
   * as a defensive scaffold for related warnings (focus, allocation,
   * unrealized children) that *do* fire headlessly. */
  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 400, 400);

  g_object_ref (f->list);
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (f->list));

  gtk_window_present (GTK_WINDOW (window));
  pump_main_loop ();

  mail_message_list_load (f->list, f->fake, "inbox", 10);
  pump_main_loop ();

  gtk_window_destroy (GTK_WINDOW (window));
  pump_main_loop ();
}

static void
test_empty_folder_shows_folder_empty_state (void)
{
  /* Regression: selecting a folder that loads successfully with zero
   * messages used to fall back to the initial "Select a folder"
   * status page, telling the user to pick a folder they had just
   * picked. The fix routes that case to a distinct "folder-empty"
   * status page ("No messages — This folder is empty."). */
  MailBackend *fake = mail_backend_fake_new ();
  /* Seed an empty folder so list_messages returns successfully but
   * with 0 rows (mail_backend_fake_set_messages with n=0). */
  mail_backend_fake_set_messages (fake, "empty-folder", NULL, 0);

  MailMessageList *list = MAIL_MESSAGE_LIST (mail_message_list_new ());
  g_object_ref_sink (list);

  mail_message_list_load (list, fake, "empty-folder", 10);
  pump_main_loop ();

  GtkStack *stack = _mail_message_list_get_stack_for_test (list);
  g_assert_nonnull (stack);
  g_assert_cmpstr (gtk_stack_get_visible_child_name (stack), ==, "folder-empty");

  g_object_unref (list);
  pump_main_loop ();
  mail_backend_destroy (fake);
}

static void
test_markup_special_chars (void)
{
  /* Each entry combines at least one of the four chars that broke the
   * production app: '&', '<', '>', '@'. */
  FakeMessageSpec msgs[] = {
    { "m1", "50 Spots Left & Last Day. Get 40% OFF", "Microsoft account team <account-security-noreply@accountprotection.microsoft.com>", 1700000000, FALSE, "body" },
    { "m2", "Subject with <angles>", "Wise <noreply@wise.com>", 1700000100, TRUE, "body" },
    { "m3", "Easy Thai System & Lifetime Access — only $3.20", "GNOME GitLab <gitlab-issues@gnome.org>", 1700000200, FALSE, "body" },
    { "m4", "Plain subject, weird from", "@just-an-at-sign", 1700000300, FALSE, "body" },
  };

  markup_warnings = 0;

  MailBackend *fake = mail_backend_fake_new ();
  mail_backend_fake_set_messages (fake, "inbox", msgs, G_N_ELEMENTS (msgs));

  MailMessageList *list = MAIL_MESSAGE_LIST (mail_message_list_new ());
  g_object_ref_sink (list);

  mail_message_list_load (list, fake, "inbox", G_N_ELEMENTS (msgs));
  pump_main_loop ();

  /* The list must have built rows for every message. */
  GtkListBox *list_box = _mail_message_list_get_list_box_for_test (list);
  g_assert_nonnull (list_box);
  for (guint i = 0; i < G_N_ELEMENTS (msgs); i++)
    g_assert_nonnull (gtk_list_box_get_row_at_index (list_box, (int) i));

  /* And the row factory must not have tripped any markup-parse
   * warnings while building those rows. */
  g_assert_cmpuint (markup_warnings, ==, 0);

  g_object_unref (list);
  pump_main_loop ();
  mail_backend_destroy (fake);
}

int
main (int argc,
      char *argv[])
{
  gtk_test_init (&argc, &argv, NULL);
  adw_init ();

  /* Install before any test so we see every warning, including those
   * fired inside bind_model factories. */
  g_log_set_writer_func (test_log_writer, NULL, NULL);

  g_test_add ("/message-list/rows-populated",
              Fixture, NULL, fixture_set_up, test_rows_populated, fixture_tear_down);
  g_test_add ("/message-list/row-is-activatable",
              Fixture, NULL, fixture_set_up, test_row_is_activatable, fixture_tear_down);
  g_test_add ("/message-list/activation-emits-signal",
              Fixture, NULL, fixture_set_up, test_activation_emits_signal, fixture_tear_down);
  g_test_add ("/message-list/no-warnings-when-realized",
              Fixture, NULL, fixture_set_up,
              test_no_warnings_when_realized_and_snapshotted,
              fixture_tear_down);
  g_test_add_func ("/message-list/empty-folder-shows-folder-empty-state",
                   test_empty_folder_shows_folder_empty_state);
  g_test_add_func ("/message-list/no-markup-warnings-for-special-chars",
                   test_markup_special_chars);
  return g_test_run ();
}
