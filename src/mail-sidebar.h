/* mail-sidebar.h - Account/folder sidebar widget. */

#pragma once

#include "mail-account.h"
#include "mail-backend.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MAIL_TYPE_SIDEBAR (mail_sidebar_get_type ())
G_DECLARE_FINAL_TYPE (MailSidebar, mail_sidebar, MAIL, SIDEBAR, GtkWidget)

GtkWidget *mail_sidebar_new (void);

/*
 * Emitted when the user activates a folder row. backend is borrowed
 * (owned by the corresponding MailAccount); folder_id is borrowed and
 * valid until the sidebar reloads that folder's account.
 */

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
