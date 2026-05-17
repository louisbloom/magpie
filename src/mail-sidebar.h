/* mail-sidebar.h - Account/folder sidebar widget. */

#pragma once

#include "mail-account.h"
#include "mail-backend.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MAIL_TYPE_SIDEBAR (mail_sidebar_get_type ())
G_DECLARE_FINAL_TYPE (MailSidebar, mail_sidebar, MAIL, SIDEBAR, GtkWidget)

GtkWidget *mail_sidebar_new (void);

/* Signals:
 *   folder-selected    (MailBackend* store_backend,
 *                       const char* folder_id,
 *                       MailAccount* account)
 *       — Emitted when the user activates a folder row. All pointers
 *         are borrowed; folder_id is valid until the sidebar reloads
 *         that folder's account.
 *
 *   account-added      (MailAccount* account)
 *       — Emitted each time an account row is appended (via GOA or via
 *         mail_sidebar_add_test_account). The window subscribes so it
 *         can wire account-level UI (e.g. notify::running on acct->sync).
 *
 *   refresh-requested  (MailAccount* account)
 *       — Emitted when the user clicks the refresh button on an
 *         account row. The sidebar does NOT start the sync itself;
 *         the window owns the cancellable and the progress page.
 */

/* Re-issue list_folders against the account's store_backend and
 * rebuild that account's folder rows. Call this after a sync pass
 * completes so freshly-synced folders appear immediately. */
void mail_sidebar_reload_folders (MailSidebar *self,
                                  MailAccount *acct);

/* ------------------------------------------------------------------
 * Test-only hooks.
 * ------------------------------------------------------------------ */

/* Append an externally-constructed account (bypassing GoaClient) and
 * trigger list_folders_async on its backend, same as the production
 * path. Takes ownership of @acct. */
void mail_sidebar_add_test_account (MailSidebar *self,
                                    MailAccount *acct);

/* Borrowed pointer to the internal GtkListBox so tests can walk the
 * rows. Returns NULL on a NULL self. */
GtkListBox *_mail_sidebar_get_list_box_for_test (MailSidebar *self);

G_END_DECLS
