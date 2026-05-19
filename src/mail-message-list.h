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

/* Reflect a "now read" change for @message_id in the currently
 * loaded list. No-op if the message isn't in the displayed folder.
 * Mutates the borrowed meta in place and re-binds the matching row so
 * the bold "heading" style drops off without a full reload. */
void mail_message_list_mark_read (MailMessageList *self,
                                  const char *message_id);

/* Filter the visible rows to messages where MailMessageMeta::unread is
 * TRUE. The underlying store is untouched; only the GtkListView sees
 * fewer rows. State persists across mail_message_list_load() calls. */
void mail_message_list_set_show_unread_only (MailMessageList *self,
                                             gboolean unread_only);
gboolean mail_message_list_get_show_unread_only (MailMessageList *self);

/* Test-only: borrowed pointer to the internal GtkListView. */
GtkListView *_mail_message_list_get_list_view_for_test (MailMessageList *self);

/* Test-only: borrowed pointer to the backing GListStore (item count
 * is the true row count regardless of how many widgets are realised). */
GListModel *_mail_message_list_get_model_for_test (MailMessageList *self);

/* Test-only: borrowed pointer to the filtered model that feeds the
 * GtkListView. Its item count reflects the visible row count under the
 * current unread-only filter setting. */
GListModel *_mail_message_list_get_filter_model_for_test (MailMessageList *self);

/* Test-only: borrowed MailMessageMeta for the row at @index, or NULL
 * if out of range. The pointer follows the backend arena's lifetime
 * (valid until the next load on this widget's current backend). */
const MailMessageMeta *_mail_message_list_get_meta_for_test (MailMessageList *self,
                                                             guint index);

/* Test-only: borrowed pointer to the internal state GtkStack. */
GtkStack *_mail_message_list_get_stack_for_test (MailMessageList *self);

/* Test-only: format a received_unix as the message list would, with
 * the caller-provided @now as the reference for "this year" vs
 * "older". Returned string is g_free()d by the caller. */
char *_mail_message_list_format_received_for_test (gint64 received_unix,
                                                   GDateTime *now);

G_END_DECLS
