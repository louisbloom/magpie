/* tests/test-account-page.c - MailAccountPage rendering invariants.
 *
 * Pins that the page's "active sync" UI (visible ring + Cancel button)
 * only renders when the bound MailSync is actually running, and that
 * the inverse "Sync now" button shows in every other state. Original
 * bugs being guarded:
 *   - set_state once latched active UI whenever sync != NULL, so the
 *     page stayed in "Syncing" mode with a live Cancel button after
 *     the pass had completed (fixed by tracking :running).
 *   - The Sync-now button must emit MailAccountPage::sync-requested so
 *     the window can launch a new pass; before the action lived on a
 *     refresh button in the sidebar.
 */

#include "../src/mail-account-page.h"
#include "../src/mail-sync.h"

#include <adwaita.h>
#include <gtk/gtk.h>

static void
test_bound_to_non_running_sync_renders_idle (void)
{
  /* A fresh MailSync has :running == FALSE. Binding the page to such
   * a sync (the same state the page lands in *after* a pass completes)
   * must render idle UI: Cancel button hidden, Sync-now button visible. */
  MailAccountPage *page = MAIL_ACCOUNT_PAGE (mail_account_page_new ());
  g_object_ref_sink (page);

  MailSync *sync = mail_sync_new ();
  mail_account_page_set_state (page, sync, NULL, "user@example.com", "Microsoft 365");

  g_assert_false (_mail_account_page_is_cancel_visible_for_test (page));
  g_assert_true (_mail_account_page_is_sync_button_visible_for_test (page));

  g_object_unref (sync);
  g_object_unref (page);
}

static void
test_unbound_renders_idle (void)
{
  /* set_state with sync=NULL — the explicit "no sync bound" case. */
  MailAccountPage *page = MAIL_ACCOUNT_PAGE (mail_account_page_new ());
  g_object_ref_sink (page);

  mail_account_page_set_state (page, NULL, NULL, "user@example.com", "Microsoft 365");

  g_assert_false (_mail_account_page_is_cancel_visible_for_test (page));
  g_assert_true (_mail_account_page_is_sync_button_visible_for_test (page));
  /* Description matches the idle placeholder text. */
  g_assert_cmpstr (_mail_account_page_get_description_for_test (page),
                   ==,
                   "No sync in progress.");

  g_object_unref (page);
}

static void
test_subtitle_reflects_provider (void)
{
  /* The header bar's two-line title widget shows the account identity
   * on top and the provider name below, so the right pane keeps a
   * persistent indicator of which account it's about regardless of
   * sync state. */
  MailAccountPage *page = MAIL_ACCOUNT_PAGE (mail_account_page_new ());
  g_object_ref_sink (page);

  mail_account_page_set_state (page, NULL, NULL, "thomasc1971@hotmail.com", "Microsoft 365");
  g_assert_cmpstr (_mail_account_page_get_subtitle_for_test (page), ==, "Microsoft 365");

  /* Re-bind with a different provider; subtitle should update. */
  mail_account_page_set_state (page, NULL, NULL, "x@y.z", "Other Provider");
  g_assert_cmpstr (_mail_account_page_get_subtitle_for_test (page), ==, "Other Provider");

  /* NULL/empty provider renders as an empty subtitle, not "(null)". */
  mail_account_page_set_state (page, NULL, NULL, "x@y.z", NULL);
  g_assert_cmpstr (_mail_account_page_get_subtitle_for_test (page), ==, "");

  g_object_unref (page);
}

typedef struct
{
  guint count;
} SyncRequestedCapture;

static void
on_sync_requested_capture (MailAccountPage *page,
                           gpointer user_data)
{
  SyncRequestedCapture *cap = user_data;
  cap->count++;
}

static void
test_sync_button_emits_sync_requested (void)
{
  /* Clicking the Sync-now button must emit MailAccountPage::sync-
   * requested — the window's handler is what actually launches the
   * pass. Before the rewiring this action lived on a refresh button
   * in the sidebar; pin the new path. */
  MailAccountPage *page = MAIL_ACCOUNT_PAGE (mail_account_page_new ());
  g_object_ref_sink (page);
  mail_account_page_set_state (page, NULL, NULL, "user@example.com", "Microsoft 365");

  SyncRequestedCapture cap = { 0 };
  gulong handler = g_signal_connect (page, "sync-requested",
                                     G_CALLBACK (on_sync_requested_capture), &cap);

  GtkButton *btn = _mail_account_page_get_sync_button_for_test (page);
  g_assert_nonnull (btn);
  g_signal_emit_by_name (btn, "clicked");

  g_assert_cmpuint (cap.count, ==, 1);

  g_signal_handler_disconnect (page, handler);
  g_object_unref (page);
}

static void
test_sync_button_visibility_follows_active_state (void)
{
  /* The Sync-now button is visible iff the page is in idle state —
   * sync == NULL OR sync is bound but not running. Both shapes count
   * as idle. The window's auto-jump-on-sync-start flow depends on this
   * polarity: when a pass kicks off, on_running_notify drives the page
   * into active state where the Sync-now button must be gone. */
  MailAccountPage *page = MAIL_ACCOUNT_PAGE (mail_account_page_new ());
  g_object_ref_sink (page);

  /* sync == NULL → visible. */
  mail_account_page_set_state (page, NULL, NULL, "u@x.y", "P");
  g_assert_true (_mail_account_page_is_sync_button_visible_for_test (page));

  /* sync bound, not running → still visible. */
  MailSync *sync = mail_sync_new ();
  mail_account_page_set_state (page, sync, NULL, "u@x.y", "P");
  g_assert_true (_mail_account_page_is_sync_button_visible_for_test (page));

  g_object_unref (sync);
  g_object_unref (page);
}

int
main (int argc, char **argv)
{
  gtk_test_init (&argc, &argv, NULL);
  adw_init ();

  g_test_add_func ("/account-page/bound-to-non-running-renders-idle",
                   test_bound_to_non_running_sync_renders_idle);
  g_test_add_func ("/account-page/unbound-renders-idle",
                   test_unbound_renders_idle);
  g_test_add_func ("/account-page/subtitle-reflects-provider",
                   test_subtitle_reflects_provider);
  g_test_add_func ("/account-page/sync-button-emits-sync-requested",
                   test_sync_button_emits_sync_requested);
  g_test_add_func ("/account-page/sync-button-visibility-follows-active-state",
                   test_sync_button_visibility_follows_active_state);
  return g_test_run ();
}
