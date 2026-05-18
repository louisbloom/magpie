/* mail-maildir-watcher.h - Watch a Maildir tree for external mutation.
 *
 * One GFileMonitor per folder's cur/ directory. When the kernel
 * reports a change, the watcher schedules a short debounce
 * (MAIL_MAILDIR_WATCHER_DEBOUNCE_MS) and then runs the per-folder
 * reconciler against MailStore. Drifted rows surface as
 * MailStoreChange events, which the existing MailBackendStore bridge
 * rebroadcasts to UI subscribers — so an external mutt mark-read
 * shows up in the sidebar badge and message-list boldness without any
 * sync needing to run.
 *
 * Initial reconcile: when a folder is added (mail_maildir_watcher_watch_folder)
 * the reconciler runs once *before* the monitor arms, so any drift
 * that accumulated while magpie was closed is picked up at startup.
 *
 * Threading: every event and the debounce timer run on the GLib
 * default main context; the reconciler runs on the same context.
 * Subscribers may safely touch widgets.
 *
 * Limits: each watched folder consumes one inotify watch descriptor.
 * Default /proc/sys/fs/inotify/max_user_watches is 8192; typical
 * magpie usage is a few hundred. NFS / SMB don't reliably deliver
 * inotify events — local-fs Maildir is assumed.
 */

#pragma once

#include "mail-store.h"

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _MailMaildirWatcher MailMaildirWatcher;

MailMaildirWatcher *mail_maildir_watcher_new (MailStore *store);

/* Begin watching the cur/ directory of @folder_remote_id. Resolves
 * dir_name via the store, runs an initial reconcile synchronously,
 * then arms a GFileMonitor on cur/. A second call for the same
 * folder is a no-op. Returns FALSE only if the folder is unknown to
 * the store (in which case @error is left clear). */
gboolean mail_maildir_watcher_watch_folder (MailMaildirWatcher *self,
                                            const char *folder_remote_id,
                                            GError **error);

/* Stop watching @folder_remote_id. Drops the GFileMonitor and any
 * pending debounce timer. Idempotent. */
void mail_maildir_watcher_unwatch_folder (MailMaildirWatcher *self,
                                          const char *folder_remote_id);

/* Tear down all monitors and pending timers. Safe to pass NULL. */
void mail_maildir_watcher_free (MailMaildirWatcher *self);

/* Test seam: invoke the per-folder reconcile path synchronously
 * without involving the kernel or a wall-clock wait. Returns TRUE if
 * the folder is watched (so the test knows it was actually invoked). */
gboolean mail_maildir_watcher_trigger_for_test (MailMaildirWatcher *self,
                                                const char *folder_remote_id);

G_END_DECLS
