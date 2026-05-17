/* mail-refresh-button.h - Per-account refresh button.
 *
 * Looks like a normal GtkButton showing view-refresh-symbolic. When
 * its bound MailSync is running, the icon crossfades to a circular
 * progress ring driven by sync::progress. On completion the ring
 * crossfades to a checkmark (✓) or warning (⚠) for ~600 ms before
 * returning to the refresh icon.
 *
 * The button does NOT call mail_sync_run_async itself — it only
 * emits "clicked" so the parent can supply the remote backend and
 * local store the sync needs.
 */

#pragma once

#include "mail-sync.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MAIL_TYPE_REFRESH_BUTTON (mail_refresh_button_get_type ())
G_DECLARE_FINAL_TYPE (MailRefreshButton, mail_refresh_button, MAIL, REFRESH_BUTTON, GtkButton)

GtkWidget *mail_refresh_button_new (MailSync *sync);

void mail_refresh_button_set_sync (MailRefreshButton *self, MailSync *sync);
MailSync *mail_refresh_button_get_sync (MailRefreshButton *self);

G_END_DECLS
