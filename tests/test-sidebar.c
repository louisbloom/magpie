/* tests/test-sidebar.c - Regression tests for the MailSidebar widget.
 *
 * Drives the real sidebar with a fake MailBackend so we exercise the
 * production control flow (GoaClient-less path: mail_sidebar_add_test_account
 * → sidebar_add_account → list_folders_async → list-store insert →
 * GtkListBox row construction via gtk_list_box_bind_model). The fake
 * idle-completes its async results, so tests pump the main context to
 * observe them.
 *
 * The original bug: AdwActionRow defaults to activatable=FALSE unless an
 * activatable-widget is set, which suppresses GtkListBox::row-activated
 * for folder rows. The fix is one line in build_row_widget; these tests
 * pin it down.
 */

#include "mail-backend-fake.h"

#include "../src/mail-account.h"
#include "../src/mail-sidebar.h"

#include <adwaita.h>
#include <gtk/gtk.h>

static void
pump_main_loop (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

typedef struct
{
  MailSidebar *sidebar;
  MailBackend *fake; /* borrowed; owned by the MailAccount we hand to the sidebar */
} Fixture;

static void
fixture_set_up (Fixture *f, gconstpointer ud)
{
  MailBackend *fake = mail_backend_fake_new ();
  FakeFolderSpec folders[] = {
    { "inbox", "Inbox", NULL, 5, 10 },
    { "drafts", "Drafts", NULL, 0, 3 },
  };
  mail_backend_fake_set_folders (fake, folders, G_N_ELEMENTS (folders));

  MailAccount *acct = mail_account_new_for_test (fake, "test@example", "Test Provider");

  f->fake = fake;
  f->sidebar = MAIL_SIDEBAR (mail_sidebar_new ());
  g_object_ref_sink (f->sidebar);

  mail_sidebar_add_test_account (f->sidebar, acct);
  pump_main_loop ();
}

static void
fixture_tear_down (Fixture *f, gconstpointer ud)
{
  g_object_unref (f->sidebar);
  /* Pump once more so any cancelled tasks drain. */
  pump_main_loop ();
}

static void
test_folders_populated (Fixture *f, gconstpointer ud)
{
  GtkListBox *list_box = _mail_sidebar_get_list_box_for_test (f->sidebar);
  g_assert_nonnull (list_box);

  /* Expected layout: [account, Inbox, Drafts]. */
  GtkListBoxRow *r0 = gtk_list_box_get_row_at_index (list_box, 0);
  GtkListBoxRow *r1 = gtk_list_box_get_row_at_index (list_box, 1);
  GtkListBoxRow *r2 = gtk_list_box_get_row_at_index (list_box, 2);
  GtkListBoxRow *r3 = gtk_list_box_get_row_at_index (list_box, 3);

  g_assert_nonnull (r0);
  g_assert_nonnull (r1);
  g_assert_nonnull (r2);
  g_assert_null (r3);

  /* The backend's list_folders_async should have fired exactly once. */
  g_assert_cmpuint (mail_backend_fake_list_folders_calls (f->fake), ==, 1);
}

static void
test_folder_row_is_activatable (Fixture *f, gconstpointer ud)
{
  /* Regression: AdwActionRow defaults to activatable=FALSE. If we don't
   * opt folder rows in explicitly, GtkListBox::row-activated never
   * fires for them and folder clicks become no-ops. */
  GtkListBox *list_box = _mail_sidebar_get_list_box_for_test (f->sidebar);
  GtkListBoxRow *folder = gtk_list_box_get_row_at_index (list_box, 1);
  g_assert_nonnull (folder);
  g_assert_true (gtk_list_box_row_get_activatable (folder));
}

static void
test_folder_row_is_compact (Fixture *f, gconstpointer ud)
{
  /* Regression: folder rows used AdwActionRow's two-line layout
   * (~52 px tall) which wasted vertical space in a list dominated by
   * folder names. The fix replaces folder rows with a hand-rolled
   * single-line GtkBox-in-GtkListBoxRow (~28-32 px). Assert the row's
   * natural vertical request is well below the old 52 px so a
   * regression to AdwActionRow would trip the bar. */
  GtkListBox *list_box = _mail_sidebar_get_list_box_for_test (f->sidebar);
  GtkListBoxRow *folder = gtk_list_box_get_row_at_index (list_box, 1);
  g_assert_nonnull (folder);
  int min = 0, nat = 0;
  gtk_widget_measure (GTK_WIDGET (folder), GTK_ORIENTATION_VERTICAL, -1,
                      &min, &nat, NULL, NULL);
  g_assert_cmpint (nat, <, 40);
}

static void
test_account_row_aligns_with_folder_rows (Fixture *f, gconstpointer ud)
{
  /* Regression: AdwActionRow brought its own internal prefix/suffix
   * padding, which knocked the provider icon out of horizontal
   * alignment with the folder icons below and pushed the refresh
   * button inboard from the folder rows' right edge. The fix hand-
   * rolls the account row with the same start/end margins as the
   * folder row's inner GtkBox so the icons and trailing controls line
   * up vertically. */
  GtkListBox *list_box = _mail_sidebar_get_list_box_for_test (f->sidebar);
  GtkListBoxRow *account = gtk_list_box_get_row_at_index (list_box, 0);
  GtkListBoxRow *folder = gtk_list_box_get_row_at_index (list_box, 1);
  g_assert_nonnull (account);
  g_assert_nonnull (folder);
  GtkWidget *account_box = gtk_list_box_row_get_child (account);
  GtkWidget *folder_box = gtk_list_box_row_get_child (folder);
  g_assert_nonnull (account_box);
  g_assert_nonnull (folder_box);
  g_assert_cmpint (gtk_widget_get_margin_start (account_box),
                   ==,
                   gtk_widget_get_margin_start (folder_box));
  g_assert_cmpint (gtk_widget_get_margin_end (account_box),
                   ==,
                   gtk_widget_get_margin_end (folder_box));
}

static void
test_account_row_is_not_activatable (Fixture *f, gconstpointer ud)
{
  /* Account rows are headers and must not respond to activation. */
  GtkListBox *list_box = _mail_sidebar_get_list_box_for_test (f->sidebar);
  GtkListBoxRow *account = gtk_list_box_get_row_at_index (list_box, 0);
  g_assert_nonnull (account);
  g_assert_false (gtk_list_box_row_get_activatable (account));
}

typedef struct
{
  guint count;
  char *folder_id;
  char *display_name;
  gpointer backend;
  gpointer account;
} SignalCapture;

static void
on_folder_selected (MailSidebar *s,
                    gpointer backend,
                    const char *folder_id,
                    gpointer account,
                    const char *display_name,
                    gpointer user_data)
{
  SignalCapture *cap = user_data;
  cap->count++;
  g_free (cap->folder_id);
  g_free (cap->display_name);
  cap->folder_id = g_strdup (folder_id);
  cap->display_name = g_strdup (display_name);
  cap->backend = backend;
  cap->account = account;
}

static void
test_folder_activation_emits_signal (Fixture *f, gconstpointer ud)
{
  SignalCapture cap = { 0 };
  gulong handler = g_signal_connect (f->sidebar, "folder-selected",
                                     G_CALLBACK (on_folder_selected), &cap);

  GtkListBox *list_box = _mail_sidebar_get_list_box_for_test (f->sidebar);
  GtkListBoxRow *inbox = gtk_list_box_get_row_at_index (list_box, 1);
  g_assert_nonnull (inbox);

  /* Simulate activation. GtkListBox emits row-activated when the row
   * is activated (single click, Enter, or explicit emit). */
  g_signal_emit_by_name (list_box, "row-activated", inbox);

  g_assert_cmpuint (cap.count, ==, 1);
  g_assert_cmpstr (cap.folder_id, ==, "inbox");
  g_assert_cmpstr (cap.display_name, ==, "Inbox");
  g_assert_true (cap.backend == f->fake);
  g_assert_nonnull (cap.account);

  g_signal_handler_disconnect (f->sidebar, handler);
  g_free (cap.folder_id);
  g_free (cap.display_name);
}

typedef struct
{
  guint count;
  gpointer account;
} AccountAddedCapture;

static void
on_account_added (MailSidebar *s,
                  gpointer acct,
                  gpointer user_data)
{
  AccountAddedCapture *cap = user_data;
  cap->count++;
  cap->account = acct;
}

static void
test_account_added_signal_fires_for_test_account (Fixture *f, gconstpointer ud)
{
  /* mail_sidebar_add_test_account ran in fixture_set_up before we
   * connected — so use a fresh sidebar to observe the signal. */
  AccountAddedCapture cap = { 0 };
  MailBackend *fake2 = mail_backend_fake_new ();
  MailAccount *acct = mail_account_new_for_test (fake2, "two@example.com", "Two");
  MailSidebar *sb = MAIL_SIDEBAR (mail_sidebar_new ());
  g_object_ref_sink (sb);
  g_signal_connect (sb, "account-added", G_CALLBACK (on_account_added), &cap);
  mail_sidebar_add_test_account (sb, acct);
  g_assert_cmpuint (cap.count, ==, 1);
  g_assert_true (cap.account == acct);
  pump_main_loop ();
  g_object_unref (sb);
}

static void
test_refresh_requested_signal_fires_on_button_click (Fixture *f, gconstpointer ud)
{
  /* The refresh button is added as an action-row suffix in build_row_widget.
   * Find it by walking the account row's descendants. */
  GtkListBox *list_box = _mail_sidebar_get_list_box_for_test (f->sidebar);
  GtkListBoxRow *account_row = gtk_list_box_get_row_at_index (list_box, 0);
  g_assert_nonnull (account_row);

  GtkWidget *button = NULL;
  for (GtkWidget *w = gtk_widget_get_first_child (GTK_WIDGET (account_row));
       w != NULL && button == NULL;
       w = gtk_widget_get_next_sibling (w))
    {
      /* Recursive descent until we find any GtkButton in the subtree. */
      GQueue stack = G_QUEUE_INIT;
      g_queue_push_tail (&stack, w);
      while (!g_queue_is_empty (&stack) && button == NULL)
        {
          GtkWidget *cur = g_queue_pop_head (&stack);
          if (GTK_IS_BUTTON (cur))
            {
              button = cur;
              break;
            }
          for (GtkWidget *c = gtk_widget_get_first_child (cur);
               c != NULL;
               c = gtk_widget_get_next_sibling (c))
            g_queue_push_tail (&stack, c);
        }
      g_queue_clear (&stack);
    }
  g_assert_nonnull (button);

  guint refresh_count = 0;
  g_signal_connect_swapped (f->sidebar, "refresh-requested",
                            G_CALLBACK (g_atomic_int_inc), &refresh_count);
  /* The button itself is sensitive iff sync != NULL. For test
   * accounts sync is NULL, so the button is insensitive and "clicked"
   * normally won't fire — but g_signal_emit_by_name forces it. */
  g_signal_emit_by_name (button, "clicked");
  g_assert_cmpuint (refresh_count, ==, 1);
}

int
main (int argc,
      char *argv[])
{
  gtk_test_init (&argc, &argv, NULL);
  adw_init ();

  g_test_add ("/sidebar/folders-populated",
              Fixture, NULL, fixture_set_up, test_folders_populated, fixture_tear_down);
  g_test_add ("/sidebar/folder-row-activatable",
              Fixture, NULL, fixture_set_up, test_folder_row_is_activatable, fixture_tear_down);
  g_test_add ("/sidebar/folder-row-compact",
              Fixture, NULL, fixture_set_up, test_folder_row_is_compact, fixture_tear_down);
  g_test_add ("/sidebar/account-row-aligns-with-folder-rows",
              Fixture, NULL, fixture_set_up, test_account_row_aligns_with_folder_rows, fixture_tear_down);
  g_test_add ("/sidebar/account-row-not-activatable",
              Fixture, NULL, fixture_set_up, test_account_row_is_not_activatable, fixture_tear_down);
  g_test_add ("/sidebar/folder-activation-emits-signal",
              Fixture, NULL, fixture_set_up, test_folder_activation_emits_signal, fixture_tear_down);
  g_test_add ("/sidebar/account-added-signal",
              Fixture, NULL, fixture_set_up,
              test_account_added_signal_fires_for_test_account, fixture_tear_down);
  g_test_add ("/sidebar/refresh-requested-signal",
              Fixture, NULL, fixture_set_up,
              test_refresh_requested_signal_fires_on_button_click, fixture_tear_down);

  return g_test_run ();
}
