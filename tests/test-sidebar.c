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
  MailAccount *acct; /* borrowed; owned by the sidebar after add_test_account */
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
  f->acct = acct;
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

/* Inspect a folder row and return the trailing badge label's text, or
 * NULL if no badge widget is present (unread==0). The row layout is:
 *   GtkListBoxRow -> GtkBox -> [GtkImage, GtkLabel (title), GtkLabel (badge)?] */
static char *
badge_text_at (GtkListBox *list_box,
               int row_index)
{
  GtkListBoxRow *row = gtk_list_box_get_row_at_index (list_box, row_index);
  if (row == NULL)
    return NULL;
  GtkWidget *hbox = gtk_list_box_row_get_child (row);
  if (hbox == NULL)
    return NULL;
  GtkWidget *icon = gtk_widget_get_first_child (hbox);
  if (icon == NULL)
    return NULL;
  GtkWidget *title = gtk_widget_get_next_sibling (icon);
  if (title == NULL)
    return NULL;
  GtkWidget *badge = gtk_widget_get_next_sibling (title);
  if (badge == NULL)
    return NULL;
  g_assert_true (GTK_IS_LABEL (badge));
  return g_strdup (gtk_label_get_text (GTK_LABEL (badge)));
}

static gboolean
title_has_heading_at (GtkListBox *list_box,
                      int row_index)
{
  GtkListBoxRow *row = gtk_list_box_get_row_at_index (list_box, row_index);
  GtkWidget *hbox = gtk_list_box_row_get_child (row);
  GtkWidget *icon = gtk_widget_get_first_child (hbox);
  GtkWidget *title = gtk_widget_get_next_sibling (icon);
  return gtk_widget_has_css_class (title, "heading");
}

/* When the backend emits FOLDER_COUNTS for a folder the sidebar shows,
 * the row's unread badge must update in place — no full reload. This
 * is the load-bearing assertion for requirement #1: clicking a mail
 * locally and an external mutt mark-read both reach this path. */
static void
test_folder_counts_event_updates_badge (Fixture *f, gconstpointer ud)
{
  GtkListBox *list_box = _mail_sidebar_get_list_box_for_test (f->sidebar);
  /* Fixture: [account, Inbox(unread=5), Drafts(unread=0)]. */
  g_autofree char *initial = badge_text_at (list_box, 1);
  g_assert_cmpstr (initial, ==, "5");
  g_assert_true (title_has_heading_at (list_box, 1));
  /* Drafts has unread=0, so no badge. */
  g_autofree char *drafts_initial = badge_text_at (list_box, 2);
  g_assert_null (drafts_initial);

  MailBackendChange tick_down = {
    .kind = MAIL_BACKEND_CHANGE_FOLDER_COUNTS,
    .folder_id = "inbox",
    .folder_unread = 3,
    .folder_total = 10,
  };
  mail_backend_emit_change (f->fake, &tick_down);
  pump_main_loop ();
  g_autofree char *after = badge_text_at (list_box, 1);
  g_assert_cmpstr (after, ==, "3");
  g_assert_true (title_has_heading_at (list_box, 1));

  /* Drop to zero: badge disappears, heading class clears. */
  MailBackendChange clear = {
    .kind = MAIL_BACKEND_CHANGE_FOLDER_COUNTS,
    .folder_id = "inbox",
    .folder_unread = 0,
    .folder_total = 10,
  };
  mail_backend_emit_change (f->fake, &clear);
  pump_main_loop ();
  g_autofree char *cleared = badge_text_at (list_box, 1);
  g_assert_null (cleared);
  g_assert_false (title_has_heading_at (list_box, 1));

  /* Events naming an unknown folder are dropped silently. */
  MailBackendChange unknown = {
    .kind = MAIL_BACKEND_CHANGE_FOLDER_COUNTS,
    .folder_id = "no-such-folder",
    .folder_unread = 99,
  };
  mail_backend_emit_change (f->fake, &unknown);
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
test_sidebar_natural_width_grows_with_identity (void)
{
  /* Regression: the sidebar must report a wider natural width when an
   * account with a longer identity is added. mail-window.c reads this
   * natural width and pins the AdwOverlaySplitView's min/max sidebar
   * width to it, so a long email name pushes the sidebar out to fit
   * without truncation. Build two sidebars side by side, one with a
   * short identity and one with a long one, and assert the long one
   * measures wider. */
  MailBackend *fake_short = mail_backend_fake_new ();
  FakeFolderSpec folders[] = {
    { "inbox", "Inbox", NULL, 0, 0 },
  };
  mail_backend_fake_set_folders (fake_short, folders, G_N_ELEMENTS (folders));
  MailAccount *acct_short = mail_account_new_for_test (fake_short, "a@b.c", "P");
  MailSidebar *sb_short = MAIL_SIDEBAR (mail_sidebar_new ());
  g_object_ref_sink (sb_short);
  mail_sidebar_add_test_account (sb_short, acct_short);
  pump_main_loop ();

  MailBackend *fake_long = mail_backend_fake_new ();
  mail_backend_fake_set_folders (fake_long, folders, G_N_ELEMENTS (folders));
  MailAccount *acct_long = mail_account_new_for_test (fake_long,
                                                      "really.long.email.address@some.company.example.com",
                                                      "P");
  MailSidebar *sb_long = MAIL_SIDEBAR (mail_sidebar_new ());
  g_object_ref_sink (sb_long);
  mail_sidebar_add_test_account (sb_long, acct_long);
  pump_main_loop ();

  int min_s = 0, nat_s = 0, min_l = 0, nat_l = 0;
  gtk_widget_measure (GTK_WIDGET (sb_short), GTK_ORIENTATION_HORIZONTAL, -1,
                      &min_s, &nat_s, NULL, NULL);
  gtk_widget_measure (GTK_WIDGET (sb_long), GTK_ORIENTATION_HORIZONTAL, -1,
                      &min_l, &nat_l, NULL, NULL);
  g_assert_cmpint (nat_l, >, nat_s);

  g_object_unref (sb_short);
  g_object_unref (sb_long);
  pump_main_loop ();
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

  /* Margins alone aren't enough: if the icon slots have different
   * widths, the title's start position differs between row kinds. The
   * account row uses a 24 px provider icon and the folder rows use a
   * 16 px symbolic icon; both occupy the same 24 px-wide slot via
   * gtk_widget_set_size_request so their centers (and the title text
   * that follows) line up. Check size-request widths, not pixel sizes
   * (which legitimately differ). */
  GtkWidget *account_icon = gtk_widget_get_first_child (account_box);
  GtkWidget *folder_icon = gtk_widget_get_first_child (folder_box);
  g_assert_true (GTK_IS_IMAGE (account_icon));
  g_assert_true (GTK_IS_IMAGE (folder_icon));
  int account_slot_w = 0, folder_slot_w = 0;
  gtk_widget_get_size_request (account_icon, &account_slot_w, NULL);
  gtk_widget_get_size_request (folder_icon, &folder_slot_w, NULL);
  g_assert_cmpint (account_slot_w, >, 0);
  g_assert_cmpint (account_slot_w, ==, folder_slot_w);
}

static void
test_account_row_is_selectable_and_activatable (Fixture *f, gconstpointer ud)
{
  /* Account rows are first-class navigation targets: clicking one
   * shows the account page in the right pane. */
  GtkListBox *list_box = _mail_sidebar_get_list_box_for_test (f->sidebar);
  GtkListBoxRow *account = gtk_list_box_get_row_at_index (list_box, 0);
  g_assert_nonnull (account);
  g_assert_true (gtk_list_box_row_get_activatable (account));
  g_assert_true (gtk_list_box_row_get_selectable (account));
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

  /* The sidebar dispatches on row-selected (covers user clicks and
   * programmatic gtk_list_box_select_row alike). */
  gtk_list_box_select_row (list_box, inbox);

  g_assert_cmpuint (cap.count, ==, 1);
  g_assert_cmpstr (cap.folder_id, ==, "inbox");
  g_assert_cmpstr (cap.display_name, ==, "Inbox");
  g_assert_true (cap.backend == f->fake);
  g_assert_nonnull (cap.account);

  g_signal_handler_disconnect (f->sidebar, handler);
  g_free (cap.folder_id);
  g_free (cap.display_name);
}

/* Re-clicking the already-selected folder must emit folder-selected
 * a second time so mail-window can pop the message viewer back off
 * the navigation stack. The bug surfaced as: user views a message,
 * then clicks the same Inbox row to "go back", and nothing happens
 * because GtkListBox::row-selected doesn't fire when the selection
 * isn't changing. The sidebar now also listens to row-activated
 * (with activate-on-single-click), which does fire on every click. */
static void
test_re_activation_of_selected_folder_emits_again (Fixture *f, gconstpointer ud)
{
  SignalCapture cap = { 0 };
  gulong handler = g_signal_connect (f->sidebar, "folder-selected",
                                     G_CALLBACK (on_folder_selected), &cap);

  GtkListBox *list_box = _mail_sidebar_get_list_box_for_test (f->sidebar);
  GtkListBoxRow *inbox = gtk_list_box_get_row_at_index (list_box, 1);
  g_assert_nonnull (inbox);

  /* First selection: standard row-selected path, one emission. */
  gtk_list_box_select_row (list_box, inbox);
  g_assert_cmpuint (cap.count, ==, 1);
  g_free (cap.folder_id);
  cap.folder_id = NULL;
  g_free (cap.display_name);
  cap.display_name = NULL;

  /* Re-activation of the same row: fires row-activated only
   * (selection unchanged). The sidebar's new row-activated handler
   * must dispatch the same way. */
  g_signal_emit_by_name (list_box, "row-activated", inbox);

  g_assert_cmpuint (cap.count, ==, 2);
  g_assert_cmpstr (cap.folder_id, ==, "inbox");
  g_assert_true (cap.backend == f->fake);

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

typedef struct
{
  guint count;
  gpointer account;
} AccountSelectedCapture;

static void
on_account_selected_capture (MailSidebar *s,
                             gpointer acct,
                             gpointer user_data)
{
  AccountSelectedCapture *cap = user_data;
  cap->count++;
  cap->account = acct;
}

static void
test_account_selection_emits_signal (Fixture *f, gconstpointer ud)
{
  /* Selecting the account row (via row-selected — which fires on both
   * user clicks and programmatic gtk_list_box_select_row) must emit
   * account-selected with the right MailAccount pointer. */
  AccountSelectedCapture cap = { 0 };
  gulong handler = g_signal_connect (f->sidebar, "account-selected",
                                     G_CALLBACK (on_account_selected_capture), &cap);

  GtkListBox *list_box = _mail_sidebar_get_list_box_for_test (f->sidebar);
  GtkListBoxRow *account_row = gtk_list_box_get_row_at_index (list_box, 0);
  g_assert_nonnull (account_row);

  gtk_list_box_select_row (list_box, account_row);

  g_assert_cmpuint (cap.count, ==, 1);
  g_assert_nonnull (cap.account);

  g_signal_handler_disconnect (f->sidebar, handler);
}

static void
test_mail_sidebar_select_account_programmatic (Fixture *f, gconstpointer ud)
{
  /* The window's auto-jump-on-sync-start path calls this API. Pin it. */
  AccountSelectedCapture cap = { 0 };
  gulong handler = g_signal_connect (f->sidebar, "account-selected",
                                     G_CALLBACK (on_account_selected_capture), &cap);

  /* fixture_set_up exposed the test account on the fixture; clear
   * any pre-selection so we pin that select_account() does the work. */
  GtkListBox *list_box = _mail_sidebar_get_list_box_for_test (f->sidebar);
  gtk_list_box_unselect_all (list_box);
  cap.count = 0;

  mail_sidebar_select_account (f->sidebar, f->acct);

  g_assert_cmpuint (cap.count, ==, 1);
  g_assert_true (cap.account == f->acct);

  g_signal_handler_disconnect (f->sidebar, handler);
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

/* Walk @root and find the first GtkImage whose icon-name matches
 * @needle (g_strcmp0). NULL if not present. */
static GtkImage *
find_image_by_icon_name (GtkWidget *root,
                         const char *needle)
{
  GQueue stack = G_QUEUE_INIT;
  g_queue_push_tail (&stack, root);
  while (!g_queue_is_empty (&stack))
    {
      GtkWidget *w = g_queue_pop_head (&stack);
      if (GTK_IS_IMAGE (w))
        {
          const char *name = gtk_image_get_icon_name (GTK_IMAGE (w));
          if (g_strcmp0 (name, needle) == 0)
            {
              g_queue_clear (&stack);
              return GTK_IMAGE (w);
            }
        }
      for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL;
           c = gtk_widget_get_next_sibling (c))
        g_queue_push_tail (&stack, c);
    }
  return NULL;
}

/* Walk @root and find the first GtkLabel whose text matches @needle.
 * NULL if not present. */
static GtkLabel *
find_label_by_text (GtkWidget *root,
                    const char *needle)
{
  GQueue stack = G_QUEUE_INIT;
  g_queue_push_tail (&stack, root);
  while (!g_queue_is_empty (&stack))
    {
      GtkWidget *w = g_queue_pop_head (&stack);
      if (GTK_IS_LABEL (w))
        {
          const char *text = gtk_label_get_text (GTK_LABEL (w));
          if (g_strcmp0 (text, needle) == 0)
            {
              g_queue_clear (&stack);
              return GTK_LABEL (w);
            }
        }
      for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL;
           c = gtk_widget_get_next_sibling (c))
        g_queue_push_tail (&stack, c);
    }
  return NULL;
}

/* Regression: the sidebar shows app branding (icon + "Spool") at the
 * top, left-aligned. Previously the sidebar carried an empty
 * AdwHeaderBar that was removed in commit a775bdb; this test pins the
 * subsequent reintroduction so an accidental future drop is caught. */
static void
test_sidebar_shows_branding (void)
{
  MailSidebar *sb = MAIL_SIDEBAR (mail_sidebar_new ());
  g_object_ref_sink (sb);

  GtkImage *icon = find_image_by_icon_name (GTK_WIDGET (sb), "org.gnome.Spool");
  g_assert_nonnull (icon);

  GtkLabel *label = find_label_by_text (GTK_WIDGET (sb), "Spool");
  g_assert_nonnull (label);
  /* Left-aligned along the same x-axis as account/folder row titles. */
  g_assert_cmpfloat (gtk_label_get_xalign (label), ==, 0.0f);

  g_object_unref (sb);
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
  g_test_add ("/sidebar/folder-counts-event-updates-badge",
              Fixture, NULL, fixture_set_up, test_folder_counts_event_updates_badge, fixture_tear_down);
  g_test_add ("/sidebar/account-row-aligns-with-folder-rows",
              Fixture, NULL, fixture_set_up, test_account_row_aligns_with_folder_rows, fixture_tear_down);
  g_test_add_func ("/sidebar/natural-width-grows-with-identity",
                   test_sidebar_natural_width_grows_with_identity);
  g_test_add ("/sidebar/account-row-selectable-and-activatable",
              Fixture, NULL, fixture_set_up, test_account_row_is_selectable_and_activatable, fixture_tear_down);
  g_test_add ("/sidebar/account-selection-emits-signal",
              Fixture, NULL, fixture_set_up, test_account_selection_emits_signal, fixture_tear_down);
  g_test_add ("/sidebar/account-select-programmatic",
              Fixture, NULL, fixture_set_up, test_mail_sidebar_select_account_programmatic, fixture_tear_down);
  g_test_add ("/sidebar/folder-reactivation-emits-again",
              Fixture, NULL, fixture_set_up, test_re_activation_of_selected_folder_emits_again, fixture_tear_down);
  g_test_add ("/sidebar/folder-activation-emits-signal",
              Fixture, NULL, fixture_set_up, test_folder_activation_emits_signal, fixture_tear_down);
  g_test_add ("/sidebar/account-added-signal",
              Fixture, NULL, fixture_set_up,
              test_account_added_signal_fires_for_test_account, fixture_tear_down);
  g_test_add_func ("/sidebar/shows-branding", test_sidebar_shows_branding);

  return g_test_run ();
}
