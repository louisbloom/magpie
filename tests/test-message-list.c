/* tests/test-message-list.c - MailMessageList regression tests.
 *
 * Drives the real MailMessageList widget with the fake backend so the
 * GtkListView factory runs the production setup/bind path. A custom
 * GLib log writer counts any "markup" warning that fires during the
 * test; the markup-safety regression asserts the count is zero after
 * loading messages whose subjects and from-fields contain Pango
 * markup-trippy characters (& < > @).
 *
 * The original production trigger ("Failed to set text ... from markup
 * due to error parsing markup ...") came from AdwActionRow rendering
 * its title/subtitle as Pango markup. The current factory uses plain
 * GtkLabels with gtk_label_set_text, so the markup path is never taken.
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

  mail_message_list_load (f->list, f->fake, "inbox");
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
  GListModel *model = _mail_message_list_get_model_for_test (f->list);
  g_assert_nonnull (model);
  g_assert_cmpuint (g_list_model_get_n_items (model), ==, 3);
}

static void
test_single_click_activates (Fixture *f, gconstpointer ud)
{
  /* Regression: under GtkListView we use single-click-activate, the
   * equivalent of the previous "AdwActionRow must be activatable"
   * fix on the GtkListBox path. */
  GtkListView *list_view = _mail_message_list_get_list_view_for_test (f->list);
  g_assert_nonnull (list_view);
  g_assert_true (gtk_list_view_get_single_click_activate (list_view));
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

  GtkListView *list_view = _mail_message_list_get_list_view_for_test (f->list);
  g_assert_nonnull (list_view);
  /* GtkListView::activate carries a position; the handler in
   * mail-message-list looks up the model item and emits
   * message-activated. */
  g_signal_emit_by_name (list_view, "activate", 0u);

  g_assert_cmpuint (cap.count, ==, 1);
  g_assert_cmpstr (cap.message_id, ==, "m1");
  g_assert_cmpstr (cap.subject, ==, "First subject");
  g_assert_true (cap.backend == f->fake);

  g_signal_handler_disconnect (f->list, handler);
  g_free (cap.message_id);
  g_free (cap.subject);
}

/* Regression: when the activation handler marks a message read, the
 * list pane should immediately drop the bold "heading" styling on
 * that row without waiting for the next folder reload. The
 * mark-read helper mutates the borrowed meta in place and emits
 * items-changed; assert that the meta seen by the row is now
 * unread=FALSE. (Asserting the CSS class would require a realised
 * row; the meta is the value factory_bind reads on every rebind, so
 * checking it is equivalent.) */
static void
test_mark_read_flips_unread (Fixture *f, gconstpointer ud)
{
  GListModel *model = _mail_message_list_get_model_for_test (f->list);
  g_assert_nonnull (model);

  /* Fixture seeds m2 as the single unread row. Find its index by
   * matching meta->id. */
  guint n = g_list_model_get_n_items (model);
  guint idx = G_MAXUINT;
  for (guint i = 0; i < n; i++)
    {
      const MailMessageMeta *meta = _mail_message_list_get_meta_for_test (f->list, i);
      if (meta != NULL && g_strcmp0 (meta->id, "m2") == 0)
        {
          idx = i;
          break;
        }
    }
  g_assert_cmpuint (idx, !=, G_MAXUINT);
  g_assert_true (_mail_message_list_get_meta_for_test (f->list, idx)->unread);

  mail_message_list_mark_read (f->list, "m2");
  pump_main_loop ();
  g_assert_false (_mail_message_list_get_meta_for_test (f->list, idx)->unread);

  /* Unknown id is a no-op (no crash, no state change). */
  mail_message_list_mark_read (f->list, "no-such-id");
  pump_main_loop ();
  g_assert_false (_mail_message_list_get_meta_for_test (f->list, idx)->unread);
}

static void
test_large_folder_virtualises_rows (Fixture *f, gconstpointer ud)
{
  /* Regression: the prior GtkListBox + bind_model path instantiated a
   * widget per model item. With 10k-item inboxes that's catastrophic.
   * GtkListView only realises rows in the visible viewport plus a
   * small scroll buffer. Seed 5000 messages, load, present in a small
   * window, and assert the model holds 5000 but the list view only
   * realised a small constant number of row widgets. */
  GArray *specs = g_array_new (FALSE, FALSE, sizeof (FakeMessageSpec));
  GPtrArray *strings = g_ptr_array_new_with_free_func (g_free);
  for (int i = 0; i < 5000; i++)
    {
      char *id = g_strdup_printf ("m%04d", i);
      char *subj = g_strdup_printf ("Subject %04d", i);
      g_ptr_array_add (strings, id);
      g_ptr_array_add (strings, subj);
      FakeMessageSpec spec = {
        .id = id,
        .subject = subj,
        .from = "a@b.c",
        .received_unix = 1700000000 + i,
        .unread = FALSE,
        .raw_rfc822 = "x",
      };
      g_array_append_val (specs, spec);
    }
  MailBackend *fake = mail_backend_fake_new ();
  mail_backend_fake_set_messages (fake, "big", (FakeMessageSpec *) specs->data, specs->len);

  MailMessageList *list = MAIL_MESSAGE_LIST (mail_message_list_new ());
  g_object_ref_sink (list);

  /* Present in a real window so the GtkListView actually allocates and
   * realises the viewport. */
  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 600, 480);
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (list));
  gtk_window_present (GTK_WINDOW (window));
  pump_main_loop ();

  mail_message_list_load (list, fake, "big");
  pump_main_loop ();

  GListModel *model = _mail_message_list_get_model_for_test (list);
  g_assert_cmpuint (g_list_model_get_n_items (model), ==, 5000);

  /* Walk the list view's subtree and count hboxes that match the
   * factory's distinctive layout (have the "mail-title" widget data
   * set). With virtualisation a 5000-item model only realises the
   * visible viewport + a small overscan buffer. */
  guint realised = 0;
  GQueue stack = G_QUEUE_INIT;
  g_queue_push_tail (&stack, gtk_widget_get_first_child (GTK_WIDGET (_mail_message_list_get_list_view_for_test (list))));
  while (!g_queue_is_empty (&stack))
    {
      GtkWidget *w = g_queue_pop_head (&stack);
      while (w != NULL)
        {
          if (g_object_get_data (G_OBJECT (w), "mail-title") != NULL)
            realised++;
          GtkWidget *c = gtk_widget_get_first_child (w);
          if (c != NULL)
            g_queue_push_tail (&stack, c);
          w = gtk_widget_get_next_sibling (w);
        }
    }
  g_queue_clear (&stack);
  /* Confirmed in-session: virtualised count ≈ 205 for a 600×480 window;
   * any plausible viewport+overscan stays well under 500. A regression
   * to GtkListBox+bind_model would realise 5000. */
  g_assert_cmpuint (realised, >, 0);
  g_assert_cmpuint (realised, <, 500);

  gtk_window_destroy (GTK_WINDOW (window));
  pump_main_loop ();
  g_object_unref (list);
  pump_main_loop ();
  mail_backend_destroy (fake);
  g_array_unref (specs);
  g_ptr_array_unref (strings);
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

  mail_message_list_load (f->list, f->fake, "inbox");
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

  mail_message_list_load (list, fake, "empty-folder");
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

  mail_message_list_load (list, fake, "inbox");
  pump_main_loop ();

  /* The model must have one item per message. */
  GListModel *model = _mail_message_list_get_model_for_test (list);
  g_assert_nonnull (model);
  g_assert_cmpuint (g_list_model_get_n_items (model), ==, G_N_ELEMENTS (msgs));

  /* And the row factory must not have tripped any markup-parse
   * warnings while binding those rows. To exercise the bind callback
   * we need the list view to realise rows — map it in a window. */
  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 400, 400);
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (list));
  gtk_window_present (GTK_WINDOW (window));
  pump_main_loop ();
  g_assert_cmpuint (markup_warnings, ==, 0);
  gtk_window_destroy (GTK_WINDOW (window));
  pump_main_loop ();

  g_object_unref (list);
  pump_main_loop ();
  mail_backend_destroy (fake);
}

/* Pin the year-aware date formatting in the list. Backstory: the
 * subtitle column used to render every date older than today as
 * "Mon  D", so a message from 2025-02-16 and one from 2026-02-16
 * looked identical once the year had rolled over. format_received
 * now takes a third branch — when the message year differs from
 * the reference "now" year, the year is appended. */
static void
test_received_date_year_aware (void)
{
  /* Reference "now": 2026-05-18 14:00 local. */
  g_autoptr (GDateTime) now = g_date_time_new_local (2026, 5, 18, 14, 0, 0);
  g_assert_nonnull (now);

  /* This-year, earlier date: bare "Mon  D" with %e's leading space
   * for single-digit days — no year. */
  g_autoptr (GDateTime) feb_this_year = g_date_time_new_local (2026, 2, 16, 9, 30, 0);
  g_autofree char *s_this_year = _mail_message_list_format_received_for_test (
      g_date_time_to_unix (feb_this_year), now);
  g_assert_cmpstr (s_this_year, ==, "Feb 16");

  /* Previous year: year appended after a comma. */
  g_autoptr (GDateTime) feb_last_year = g_date_time_new_local (2025, 2, 16, 9, 30, 0);
  g_autofree char *s_last_year = _mail_message_list_format_received_for_test (
      g_date_time_to_unix (feb_last_year), now);
  g_assert_cmpstr (s_last_year, ==, "Feb 16, 2025");

  /* Several years back, end-of-year (two-digit day, no leading space). */
  g_autoptr (GDateTime) dec_2020 = g_date_time_new_local (2020, 12, 31, 23, 59, 0);
  g_autofree char *s_old = _mail_message_list_format_received_for_test (
      g_date_time_to_unix (dec_2020), now);
  g_assert_cmpstr (s_old, ==, "Dec 31, 2020");

  /* Same calendar day as "now": HH:MM time, not a date. */
  g_autoptr (GDateTime) today_morning = g_date_time_new_local (2026, 5, 18, 9, 7, 0);
  g_autofree char *s_today = _mail_message_list_format_received_for_test (
      g_date_time_to_unix (today_morning), now);
  g_assert_cmpuint (strlen (s_today), ==, 5);
  g_assert_true (s_today[2] == ':');

  /* received_unix=0 is the "no date" sentinel from the backends. */
  g_autofree char *s_empty = _mail_message_list_format_received_for_test (0, now);
  g_assert_cmpstr (s_empty, ==, "");
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
  g_test_add ("/message-list/single-click-activates",
              Fixture, NULL, fixture_set_up, test_single_click_activates, fixture_tear_down);
  g_test_add ("/message-list/activation-emits-signal",
              Fixture, NULL, fixture_set_up, test_activation_emits_signal, fixture_tear_down);
  g_test_add ("/message-list/mark-read-flips-unread",
              Fixture, NULL, fixture_set_up, test_mark_read_flips_unread, fixture_tear_down);
  g_test_add ("/message-list/large-folder-virtualises",
              Fixture, NULL, fixture_set_up, test_large_folder_virtualises_rows, fixture_tear_down);
  g_test_add ("/message-list/no-warnings-when-realized",
              Fixture, NULL, fixture_set_up,
              test_no_warnings_when_realized_and_snapshotted,
              fixture_tear_down);
  g_test_add_func ("/message-list/empty-folder-shows-folder-empty-state",
                   test_empty_folder_shows_folder_empty_state);
  g_test_add_func ("/message-list/no-markup-warnings-for-special-chars",
                   test_markup_special_chars);
  g_test_add_func ("/message-list/received-date-year-aware",
                   test_received_date_year_aware);
  return g_test_run ();
}
