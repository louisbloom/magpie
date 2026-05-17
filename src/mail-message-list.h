/* mail-message-list.h - Message list pane (root of right-pane NavigationView). */

#pragma once

#include "mail-backend.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MAIL_TYPE_MESSAGE_LIST (mail_message_list_get_type ())
G_DECLARE_FINAL_TYPE (MailMessageList, mail_message_list, MAIL, MESSAGE_LIST, GtkWidget)

GtkWidget *mail_message_list_new (void);

/* Replace the current list with the newest @top_n messages in
 * @folder_id on @backend. Cancels any in-flight load. */
void mail_message_list_load (MailMessageList *self,
                             MailBackend *backend,
                             const char *folder_id,
                             int top_n);

/* Emitted on row activation. message_id and subject are borrowed; the
 * receiver should duplicate before any operation that could reset the
 * backend's arena. */

/* Test-only: borrowed pointer to the internal GtkListBox. */
GtkListBox *_mail_message_list_get_list_box_for_test (MailMessageList *self);

/* Test-only: borrowed pointer to the internal state GtkStack. */
GtkStack *_mail_message_list_get_stack_for_test (MailMessageList *self);

G_END_DECLS
