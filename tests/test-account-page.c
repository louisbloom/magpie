/* tests/test-account-page.c - MailAccountPage rendering invariants.
 *
 * Pins that the page's "active sync" UI (heading "Syncing X", visible
 * cancel button, ring) only renders when the bound MailSync is
 * actually running. The original bug: set_state latched active UI
 * whenever sync != NULL, so the page stayed in "Syncing X" mode with
 * the Cancel button after the pass had completed.
 */

#include "../src/mail-account-page.h"
#include "../src/mail-sync.h"

#include <adwaita.h>
#include <gtk/gtk.h>
#include <string.h>

static void
test_bound_to_non_running_sync_renders_idle (void)
{
  /* A fresh MailSync has :running == FALSE. Binding the page to such
   * a sync (the same state the page lands in *after* a pass completes,
   * since on_running_notify rebinds nothing) must render idle UI:
   * heading is the bare identity, Cancel button is hidden. Before the
   * fix the page would have rendered "Syncing user@example" with a
   * visible Cancel button because set_state only looked at sync != NULL. */
  MailAccountPage *page = MAIL_ACCOUNT_PAGE (mail_account_page_new ());
  g_object_ref_sink (page);

  MailSync *sync = mail_sync_new ();
  mail_account_page_set_state (page, sync, NULL, "user@example.com");

  const char *heading = _mail_account_page_get_heading_text_for_test (page);
  g_assert_nonnull (heading);
  g_assert_null (strstr (heading, "Syncing"));
  g_assert_cmpstr (heading, ==, "user@example.com");
  g_assert_false (_mail_account_page_is_cancel_visible_for_test (page));

  g_object_unref (sync);
  g_object_unref (page);
}

static void
test_unbound_renders_idle (void)
{
  /* set_state with sync=NULL — the explicit "no account selected" or
   * "fully idle" case. Heading is identity, no Cancel button. */
  MailAccountPage *page = MAIL_ACCOUNT_PAGE (mail_account_page_new ());
  g_object_ref_sink (page);

  mail_account_page_set_state (page, NULL, NULL, "user@example.com");

  const char *heading = _mail_account_page_get_heading_text_for_test (page);
  g_assert_cmpstr (heading, ==, "user@example.com");
  g_assert_false (_mail_account_page_is_cancel_visible_for_test (page));

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
  return g_test_run ();
}
