/* mail-message-view.h - Raw RFC822 viewer (pushed into the right-pane NavigationView). */

#pragma once

#include "mail-backend.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MAIL_TYPE_MESSAGE_VIEW (mail_message_view_get_type ())
G_DECLARE_FINAL_TYPE (MailMessageView, mail_message_view, MAIL, MESSAGE_VIEW, GtkWidget)

GtkWidget *mail_message_view_new (void);

/* Fetch the raw RFC822 source of @message_id on @backend and display
 * it. Cancels any in-flight load on the same view. */
void mail_message_view_load (MailMessageView *self,
                             MailBackend *backend,
                             const char *message_id);

G_END_DECLS
