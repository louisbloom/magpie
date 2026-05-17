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
 * heading is just the identity). When @sync is non-NULL the page
 * tracks ::progress and ::status off it; @cancellable is needed to
 * enable the Cancel button. @account_identity is required;
 * @account_provider populates the header subtitle and may be NULL or
 * empty. */
void mail_account_page_set_state (MailAccountPage *self,
                                  MailSync *sync,
                                  GCancellable *cancellable,
                                  const char *account_identity,
                                  const char *account_provider);

/* Test-only: heading label text + cancel button visibility + header
 * subtitle — used to pin that the page's "active" vs "idle" rendering
 * is driven by sync's :running state, not just by sync being non-NULL,
 * and that the subtitle reflects the bound provider name. */
const char *_mail_account_page_get_heading_text_for_test (MailAccountPage *self);
gboolean _mail_account_page_is_cancel_visible_for_test (MailAccountPage *self);
const char *_mail_account_page_get_subtitle_for_test (MailAccountPage *self);

G_END_DECLS
