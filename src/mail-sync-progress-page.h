/* mail-sync-progress-page.h - Full-pane progress UI for an in-flight sync.
 *
 * AdwNavigationPage that the window pushes (via adw_navigation_view_replace)
 * when the selected account's MailSync becomes running. While shown,
 * the user cannot browse that account's folders/messages.
 *
 * The page reads ::progress + ::status off the bound MailSync and
 * provides a Cancel button that cancels the in-flight pass.
 */

#pragma once

#include "mail-sync.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define MAIL_TYPE_SYNC_PROGRESS_PAGE (mail_sync_progress_page_get_type ())
G_DECLARE_FINAL_TYPE (MailSyncProgressPage, mail_sync_progress_page, MAIL, SYNC_PROGRESS_PAGE, AdwNavigationPage)

GtkWidget *mail_sync_progress_page_new (void);

/* Bind a sync + a cancellable + a display identity. The page will
 * read progress/status off @sync until told otherwise. */
void mail_sync_progress_page_set_state (MailSyncProgressPage *self,
                                        MailSync *sync,
                                        GCancellable *cancellable,
                                        const char *account_identity);

G_END_DECLS
