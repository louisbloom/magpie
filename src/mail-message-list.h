/* mail-message-list.h - Message list pane (root of right-pane NavigationView). */

#pragma once

#include "mail-backend.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MAIL_TYPE_MESSAGE_LIST (mail_message_list_get_type ())
G_DECLARE_FINAL_TYPE (MailMessageList, mail_message_list, MAIL, MESSAGE_LIST, GtkWidget)

GtkWidget *mail_message_list_new (void);

/* Replace the current list with all messages in @folder_id on @backend.
 * Cancels any in-flight load. The list view virtualises row widgets,
 * so even tens of thousands of messages are inexpensive to display. */
void mail_message_list_load (MailMessageList *self,
                             MailBackend *backend,
                             const char *folder_id);

/* Emitted on row activation. message_id and subject are borrowed; the
 * receiver should duplicate before any operation that could reset the
 * backend's arena. */

/* Test-only: borrowed pointer to the internal GtkListView. */
GtkListView *_mail_message_list_get_list_view_for_test (MailMessageList *self);

/* Test-only: borrowed pointer to the backing GListStore (item count
 * is the true row count regardless of how many widgets are realised). */
GListModel *_mail_message_list_get_model_for_test (MailMessageList *self);

/* Test-only: borrowed pointer to the internal state GtkStack. */
GtkStack *_mail_message_list_get_stack_for_test (MailMessageList *self);

/* Test-only: format a received_unix as the message list would, with
 * the caller-provided @now as the reference for "this year" vs
 * "older". Returned string is g_free()d by the caller. */
char *_mail_message_list_format_received_for_test (gint64 received_unix,
                                                   GDateTime *now);

G_END_DECLS
