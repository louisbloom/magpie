/* mail-account-page.h - Full-pane account page (sync status + identity).
 *
 * AdwNavigationPage shown when the user selects an account in the
 * sidebar, OR when an account's MailSync becomes running (the sidebar
 * is auto-jumped to the account row and this page is swapped into the
 * right pane). The page currently surfaces sync status only — progress
 * ring, status text, and a Cancel button — but is named "account page"
 * because additional account-scoped sub-views (folder counts, last-sync
 * time, account preferences) will land here without further rename.
 *
 * The page is bound via mail_account_page_set_state; passing sync=NULL
 * renders the idle state for an account that isn't currently syncing.
 */

#pragma once

#include "mail-sync.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define MAIL_TYPE_ACCOUNT_PAGE (mail_account_page_get_type ())
G_DECLARE_FINAL_TYPE (MailAccountPage, mail_account_page, MAIL, ACCOUNT_PAGE, AdwNavigationPage)

GtkWidget *mail_account_page_new (void);

/* Bind the page to an account. @sync may be NULL — in that case the
 * page renders the idle state (no progress ring, no cancel button,
 * the Sync-now button is visible). When @sync is non-NULL the page
 * tracks ::progress / ::status / ::running off it; @cancellable is
 * needed to enable the Cancel button. @account_identity is required;
 * @account_provider populates the header subtitle and may be NULL or
 * empty. */
void mail_account_page_set_state (MailAccountPage *self,
                                  MailSync *sync,
                                  GCancellable *cancellable,
                                  const char *account_identity,
                                  const char *account_provider);

/* Signals:
 *   sync-requested ()
 *       — Emitted when the user clicks the page's "Sync now" button.
 *         The window subscribes and starts a pass against the bound
 *         account (the page itself does not own the cancellable or
 *         touch the sync engine). */

/* Borrowed pointer to the start-of-header sidebar-toggle button.
 * The window binds its `active` property to the parent split view's
 * `show-sidebar` so the page can collapse/uncollapse the sidebar
 * just like the message-list and message-view pages. */
GtkToggleButton *mail_account_page_get_sidebar_toggle (MailAccountPage *self);

/* Test-only accessors for the rendering invariants the unit tests pin. */
gboolean _mail_account_page_is_cancel_visible_for_test (MailAccountPage *self);
gboolean _mail_account_page_is_sync_button_visible_for_test (MailAccountPage *self);
GtkButton *_mail_account_page_get_sync_button_for_test (MailAccountPage *self);
const char *_mail_account_page_get_subtitle_for_test (MailAccountPage *self);
const char *_mail_account_page_get_description_for_test (MailAccountPage *self);

G_END_DECLS
