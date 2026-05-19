/* mail-compose-window.h - Plain-text compose dialog.
 *
 * AdwDialog with To/Subject entry rows and a monospace GtkTextView for
 * the body. On Send the message is appended to the account's
 * Outbox.mbox (see mail-outbox.h) and the dialog closes; failures
 * surface as a toast and keep the dialog open.
 *
 * The reply factory pre-fills the dialog from an original raw
 * RFC 5322 message:
 *   To:      Reply-To if present, else From
 *   Subject: "Re: " + original (idempotent)
 *   Body:    Gnus-style attribution + "> "-quoted text/plain
 *            (or HTML→text if plain absent)
 *
 * The dialog holds a borrowed MailAccount pointer; the caller is
 * responsible for keeping the account alive for the dialog's
 * lifetime (mail-window pins this via the active sidebar selection).
 */

#pragma once

#include "mail-account.h"

#include <adwaita.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MAIL_TYPE_COMPOSE_WINDOW (mail_compose_window_get_type ())
G_DECLARE_FINAL_TYPE (MailComposeWindow, mail_compose_window, MAIL, COMPOSE_WINDOW, AdwDialog)

/* Empty compose, no pre-fill. */
GtkWidget *mail_compose_window_new (MailAccount *account);

/* Reply compose, pre-filled from the original raw RFC 5322 bytes. */
GtkWidget *mail_compose_window_new_reply (MailAccount *account, GBytes *original_raw);

/* Test helper: synchronously trigger Send. Returns TRUE on success.
 * No-op if the window is not yet populated. */
gboolean mail_compose_window_send (MailComposeWindow *self, GError **error);

/* Test helpers — borrowed pointers, do not free. */
const char *mail_compose_window_peek_to (MailComposeWindow *self);
const char *mail_compose_window_peek_subject (MailComposeWindow *self);
char *mail_compose_window_dup_body (MailComposeWindow *self);

G_END_DECLS
