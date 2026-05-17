/* mail-message-view.h - MIME-aware message viewer (pushed into the
 *                       right-pane NavigationView). */

#pragma once

#include "mail-backend.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Exclusive view-mode toggle. Bound to AdwToggleGroup::active-name via
 * the enum-nick string ("rendered", "plain", "source") in mail-window.c. */
typedef enum
{
  MAIL_MESSAGE_VIEW_MODE_RENDERED, /* best displayable part per RFC 2046 */
  MAIL_MESSAGE_VIEW_MODE_PLAIN,    /* force text/plain alternative */
  MAIL_MESSAGE_VIEW_MODE_SOURCE,   /* raw RFC822 bytes */
} MailMessageViewMode;

#define MAIL_TYPE_MESSAGE_VIEW_MODE (mail_message_view_mode_get_type ())
GType mail_message_view_mode_get_type (void);

#define MAIL_TYPE_MESSAGE_VIEW (mail_message_view_get_type ())
G_DECLARE_FINAL_TYPE (MailMessageView, mail_message_view, MAIL, MESSAGE_VIEW, GtkWidget)

GtkWidget *mail_message_view_new (void);

/* Fetch the raw RFC822 source of @message_id on @backend, parse it,
 * pick the best displayable part, and render. Cancels any in-flight
 * load on the same view. Always resets view-mode back to RENDERED. */
void mail_message_view_load (MailMessageView *self,
                             MailBackend *backend,
                             const char *message_id);

G_END_DECLS
