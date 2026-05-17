/* mail-sync.h - One-shot reconciler from a remote backend to a local MailStore.
 *
 * A MailSync owns the *state* of an in-flight pass for a single
 * account; instances are typically owned 1:1 by MailAccount. While
 * a pass is running, "running" is TRUE and "progress" / "status"
 * stream live updates that drive the refresh button + progress page.
 *
 * Trigger: the user clicks the per-account refresh button. There is
 * no startup sync and no timer.
 *
 * Per pass (see plan):
 *   1. list_folders -> upsert into local; delete locally-only folders
 *   2. per folder, list_messages -> diff by remote_id; delete
 *      locally-only messages
 *   3. per new message, fetch_message_raw -> write_raw -> upsert row
 *
 * Out of scope for v1: delta queries (currently a full re-scan per
 * pass), local->remote push, parallel fetches.
 */

#pragma once

#include "mail-backend.h"
#include "mail-store.h"

#include <gio/gio.h>

G_BEGIN_DECLS

#define MAIL_TYPE_SYNC (mail_sync_get_type ())
G_DECLARE_FINAL_TYPE (MailSync, mail_sync, MAIL, SYNC, GObject)

MailSync *mail_sync_new (void);

/* Kick off a pass. @remote and @local are borrowed; the caller must
 * keep them alive until the GAsyncReadyCallback fires. @cancellable
 * is optional — if cancelled, the pass terminates with G_IO_ERROR_CANCELLED. */
void mail_sync_run_async (MailSync *self,
                          MailBackend *remote,
                          MailStore *local,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer user_data);

/* TRUE on success (including no-op when there is nothing to sync).
 * FALSE on error or cancellation, with @error set. */
gboolean mail_sync_run_finish (MailSync *self,
                               GAsyncResult *result,
                               GError **error);

gboolean mail_sync_is_running (MailSync *self);
double mail_sync_get_progress (MailSync *self);
const char *mail_sync_get_status (MailSync *self);

G_END_DECLS
