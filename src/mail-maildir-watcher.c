/* mail-maildir-watcher.c - GFileMonitor-based external mutation watcher.
 *
 * Per-folder cur/ monitor. On any event, schedule a single debounce
 * timer per folder; when it fires, call
 * mail_store_reconcile_folder_from_disk. The reconciler emits
 * MailStoreChange events for drifted rows; MailBackendStore relays
 * them to the UI.
 *
 * The 120 ms debounce keeps a rename storm (e.g. mutt marking 30
 * messages read in one keystroke) to a single reconcile pass.
 */

#include "config.h"

#include "mail-maildir-watcher.h"

#include <gio/gio.h>
#include <glib.h>

#define MAIL_MAILDIR_WATCHER_DEBOUNCE_MS 120

typedef struct
{
  MailMaildirWatcher *parent; /* borrowed */
  char *folder_remote_id;     /* g_strdup'd */
  GFileMonitor *monitor;      /* owned; NULL while running test-trigger */
  guint debounce_source_id;   /* 0 when no timer pending */
} WatcherEntry;

struct _MailMaildirWatcher
{
  MailStore *store;    /* borrowed; outlives the watcher */
  GHashTable *folders; /* char *folder_remote_id -> WatcherEntry* (owned) */
};

static void
watcher_entry_free (gpointer p)
{
  WatcherEntry *e = p;
  if (e == NULL)
    return;
  if (e->debounce_source_id != 0)
    g_source_remove (e->debounce_source_id);
  if (e->monitor != NULL)
    {
      g_file_monitor_cancel (e->monitor);
      g_object_unref (e->monitor);
    }
  g_free (e->folder_remote_id);
  g_free (e);
}

static gboolean
run_reconcile (gpointer data)
{
  WatcherEntry *e = data;
  e->debounce_source_id = 0;
  g_autoptr (GError) error = NULL;
  if (!mail_store_reconcile_folder_from_disk (e->parent->store,
                                              e->folder_remote_id, &error))
    g_warning ("mail-maildir-watcher: reconcile %s failed: %s",
               e->folder_remote_id,
               error != NULL ? error->message : "(no error)");
  return G_SOURCE_REMOVE;
}

static void
schedule_reconcile (WatcherEntry *e)
{
  if (e->debounce_source_id != 0)
    return; /* timer already pending; coalesce */
  e->debounce_source_id = g_timeout_add (MAIL_MAILDIR_WATCHER_DEBOUNCE_MS,
                                         run_reconcile, e);
}

static void
on_file_event (GFileMonitor *monitor,
               GFile *file,
               GFile *other_file,
               GFileMonitorEvent event,
               gpointer user_data)
{
  (void) monitor;
  (void) file;
  (void) other_file;
  WatcherEntry *e = user_data;

  switch (event)
    {
    case G_FILE_MONITOR_EVENT_RENAMED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_DELETED:
      /* Filename-affecting events are the only ones the reconciler
       * cares about. CHANGES_DONE_HINT and content events are
       * Maildir-irrelevant. */
      schedule_reconcile (e);
      break;
    default:
      break;
    }
}

MailMaildirWatcher *
mail_maildir_watcher_new (MailStore *store)
{
  g_return_val_if_fail (store != NULL, NULL);
  MailMaildirWatcher *self = g_new0 (MailMaildirWatcher, 1);
  self->store = store;
  self->folders = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, watcher_entry_free);
  return self;
}

gboolean
mail_maildir_watcher_watch_folder (MailMaildirWatcher *self,
                                   const char *folder_remote_id,
                                   GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (folder_remote_id != NULL, FALSE);

  if (g_hash_table_contains (self->folders, folder_remote_id))
    return TRUE; /* already watching */

  g_autofree char *dir_name = mail_store_folder_dir_name (self->store,
                                                          folder_remote_id);
  if (dir_name == NULL)
    return FALSE;

  /* Run the initial reconcile synchronously before arming the monitor.
   * This picks up drift that accumulated while spool was closed —
   * the user opens the app, the sidebar lists folders with stale
   * unread counts from sqlite, the watcher starts and immediately
   * fires FOLDER_COUNTS through the reconciler so the badges correct
   * themselves on the next iteration of the main loop. */
  if (!mail_store_reconcile_folder_from_disk (self->store, folder_remote_id, error))
    return FALSE;

  g_autofree char *cur_path = g_build_filename (mail_store_root (self->store),
                                                dir_name, "cur", NULL);
  g_autoptr (GFile) gfile = g_file_new_for_path (cur_path);
  g_autoptr (GError) monitor_err = NULL;
  /* WATCH_MOVES collapses rename-pairs (delete + create) into a single
   * RENAMED event, which is the common cur/ flag-change pattern. */
  GFileMonitor *monitor = g_file_monitor_directory (gfile, G_FILE_MONITOR_WATCH_MOVES,
                                                    NULL, &monitor_err);
  if (monitor == NULL)
    {
      /* The folder dir may not exist yet (freshly upserted folder
       * before its cur/ was created). Skip silently — sync will
       * create the dirs and the next watch_folder call wires up. */
      if (g_error_matches (monitor_err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return TRUE;
      g_propagate_error (error, g_steal_pointer (&monitor_err));
      return FALSE;
    }

  WatcherEntry *e = g_new0 (WatcherEntry, 1);
  e->parent = self;
  e->folder_remote_id = g_strdup (folder_remote_id);
  e->monitor = monitor;
  g_signal_connect (monitor, "changed", G_CALLBACK (on_file_event), e);
  g_hash_table_insert (self->folders, g_strdup (folder_remote_id), e);
  return TRUE;
}

void
mail_maildir_watcher_unwatch_folder (MailMaildirWatcher *self,
                                     const char *folder_remote_id)
{
  g_return_if_fail (self != NULL);
  if (folder_remote_id == NULL)
    return;
  g_hash_table_remove (self->folders, folder_remote_id);
}

void
mail_maildir_watcher_free (MailMaildirWatcher *self)
{
  if (self == NULL)
    return;
  g_hash_table_destroy (self->folders);
  g_free (self);
}

gboolean
mail_maildir_watcher_trigger_for_test (MailMaildirWatcher *self,
                                       const char *folder_remote_id)
{
  g_return_val_if_fail (self != NULL && folder_remote_id != NULL, FALSE);
  WatcherEntry *e = g_hash_table_lookup (self->folders, folder_remote_id);
  if (e == NULL)
    return FALSE;
  /* Cancel any pending debounce and run the reconcile path
   * synchronously so tests can deterministically observe the emit. */
  if (e->debounce_source_id != 0)
    {
      g_source_remove (e->debounce_source_id);
      e->debounce_source_id = 0;
    }
  g_autoptr (GError) error = NULL;
  if (!mail_store_reconcile_folder_from_disk (self->store, folder_remote_id, &error))
    {
      g_warning ("mail-maildir-watcher: trigger reconcile %s failed: %s",
                 folder_remote_id,
                 error != NULL ? error->message : "(no error)");
      return FALSE;
    }
  return TRUE;
}
