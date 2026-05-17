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
  gpointer backend;
} SignalCapture;

static void
on_folder_selected (MailSidebar *s,
                    gpointer backend,
                    const char *folder_id,
                    gpointer user_data)
{
  SignalCapture *cap = user_data;
  cap->count++;
  g_free (cap->folder_id);
  cap->folder_id = g_strdup (folder_id);
  cap->backend = backend;
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
  g_assert_true (cap.backend == f->fake);

  g_signal_handler_disconnect (f->sidebar, handler);
  g_free (cap.folder_id);
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
  g_test_add ("/sidebar/account-row-not-activatable",
              Fixture, NULL, fixture_set_up, test_account_row_is_not_activatable, fixture_tear_down);
  g_test_add ("/sidebar/folder-activation-emits-signal",
              Fixture, NULL, fixture_set_up, test_folder_activation_emits_signal, fixture_tear_down);

  return g_test_run ();
}
