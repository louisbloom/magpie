/* mail-sync.c - State machine for one reconciliation pass.
 *
 * Three sequential phases, each one's slice of the [0,1] progress
 * range as documented in the plan:
 *   PHASE_FOLDERS   0.00 -> 0.05  one list_folders + upserts
 *   PHASE_LISTS     0.05 -> 0.20  one list_messages per folder
 *   PHASE_FETCH     0.20 -> 1.00  one fetch_message_raw per new id
 *
 * Each MailMessageMeta from the remote backend is valid only until
 * the *next* list_messages call (per the MailBackend lifetime
 * contract), so we copy the fields we care about into FetchItem
 * before moving on.
 */

#include "config.h"

#include "mail-sync.h"

typedef enum
{
  PHASE_IDLE = 0,
  PHASE_FOLDERS,
  PHASE_LISTS,
  PHASE_FETCH,
} Phase;

typedef struct
{
  char *folder_remote_id;
  char *folder_dir_name;
  char *message_remote_id;
  char *subject;
  char *from_addr;
  gint64 received_unix;
  gboolean unread;
} FetchItem;

static void
fetch_item_free (gpointer p)
{
  FetchItem *it = p;
  g_free (it->folder_remote_id);
  g_free (it->folder_dir_name);
  g_free (it->message_remote_id);
  g_free (it->subject);
  g_free (it->from_addr);
  g_free (it);
}

struct _MailSync
{
  GObject parent;

  /* Per-pass state. */
  MailBackend *remote;       /* borrowed */
  MailStore *local;          /* borrowed */
  GCancellable *cancellable; /* ref'd */
  GTask *task;               /* ref'd; the user's task */

  Phase phase;
  gboolean running;
  double progress;
  char *status;

  /* Folder traversal state. */
  GPtrArray *folder_remote_ids; /* g_strdup'd; sized at end of PHASE_FOLDERS */
  GPtrArray *folder_dir_names;  /* parallel to folder_remote_ids */
  guint folder_index;

  /* Fetch queue, built across PHASE_LISTS. */
  GPtrArray *pending_fetches; /* of FetchItem* */
  guint fetch_index;
};

enum
{
  PROP_0,
  PROP_RUNNING,
  PROP_PROGRESS,
  PROP_STATUS,
  N_PROPS,
};

static GParamSpec *properties[N_PROPS] = {
  NULL,
};

G_DEFINE_FINAL_TYPE (MailSync, mail_sync, G_TYPE_OBJECT)

/* --- forward decls ----------------------------------------------- */

static void start_folders (MailSync *self);
static void start_next_list (MailSync *self);
static void start_next_fetch (MailSync *self);
static void finish_pass (MailSync *self, GError *error /* takes ownership */);

static void on_folders_done (GObject *src, GAsyncResult *res, gpointer user_data);
static void on_messages_done (GObject *src, GAsyncResult *res, gpointer user_data);
static void on_fetch_done (GObject *src, GAsyncResult *res, gpointer user_data);

/* --- progress / status setters ----------------------------------- */

static void
set_progress (MailSync *self,
              double v)
{
  if (v < 0.0)
    v = 0.0;
  if (v > 1.0)
    v = 1.0;
  if (self->progress == v)
    return;
  self->progress = v;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROGRESS]);
}

static void
set_status (MailSync *self,
            char *text /* takes ownership */)
{
  g_free (self->status);
  self->status = text;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STATUS]);
}

static void
set_running (MailSync *self,
             gboolean v)
{
  if (self->running == v)
    return;
  self->running = v;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_RUNNING]);
}

/* --- public API -------------------------------------------------- */

MailSync *
mail_sync_new (void)
{
  return g_object_new (MAIL_TYPE_SYNC, NULL);
}

void
mail_sync_run_async (MailSync *self,
                     MailBackend *remote,
                     MailStore *local,
                     GCancellable *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
  g_return_if_fail (MAIL_IS_SYNC (self));
  g_return_if_fail (remote != NULL && local != NULL);

  if (self->running)
    {
      GTask *task = g_task_new (self, cancellable, callback, user_data);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PENDING,
                               "sync already in progress");
      g_object_unref (task);
      return;
    }

  self->remote = remote;
  self->local = local;
  self->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;
  self->task = g_task_new (self, cancellable, callback, user_data);

  self->folder_remote_ids = g_ptr_array_new_with_free_func (g_free);
  self->folder_dir_names = g_ptr_array_new_with_free_func (g_free);
  self->folder_index = 0;
  self->pending_fetches = g_ptr_array_new_with_free_func (fetch_item_free);
  self->fetch_index = 0;

  set_running (self, TRUE);
  set_progress (self, 0.0);
  set_status (self, g_strdup ("Fetching folder list…"));

  start_folders (self);
}

gboolean
mail_sync_run_finish (MailSync *self,
                      GAsyncResult *result,
                      GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  return g_task_propagate_boolean (G_TASK (result), error);
}

gboolean
mail_sync_is_running (MailSync *self)
{
  return self->running;
}

double
mail_sync_get_progress (MailSync *self)
{
  return self->progress;
}

const char *
mail_sync_get_status (MailSync *self)
{
  return self->status;
}

/* --- phase 1: list_folders --------------------------------------- */

static void
start_folders (MailSync *self)
{
  self->phase = PHASE_FOLDERS;
  mail_backend_list_folders_async (self->remote, self->cancellable,
                                   on_folders_done, self);
}

static void
on_folders_done (GObject *src,
                 GAsyncResult *res,
                 gpointer user_data)
{
  MailSync *self = user_data;
  GError *error = NULL;
  GPtrArray *folders = mail_backend_list_folders_finish (self->remote, res, &error);
  if (folders == NULL)
    {
      finish_pass (self, error);
      return;
    }

  /* Collect remote_ids seen this pass for the deletion diff. */
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < folders->len; i++)
    {
      MailFolder *f = g_ptr_array_index (folders, i);
      g_hash_table_add (seen, g_strdup (f->id));

      const char *dir_name = NULL;
      if (!mail_store_upsert_folder (self->local, f->id, f->display_name,
                                     f->parent_id, f->unread_count, f->total_count,
                                     &dir_name, &error))
        {
          /* folders array is borrowed; safe to leave to next list_*. */
          finish_pass (self, error);
          return;
        }
      g_ptr_array_add (self->folder_remote_ids, g_strdup (f->id));
      g_ptr_array_add (self->folder_dir_names, g_strdup (dir_name));
    }

  /* Anything in the store that the remote no longer reports -> drop. */
  GHashTable *existing = mail_store_folder_remote_ids (self->local, &error);
  if (existing == NULL)
    {
      finish_pass (self, error);
      return;
    }
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init (&it, existing);
  while (g_hash_table_iter_next (&it, &key, NULL))
    {
      const char *rid = key;
      if (!g_hash_table_contains (seen, rid))
        {
          if (!mail_store_delete_folder (self->local, rid, &error))
            {
              g_hash_table_unref (existing);
              finish_pass (self, error);
              return;
            }
        }
    }
  g_hash_table_unref (existing);

  set_progress (self, 0.05);
  if (self->folder_remote_ids->len == 0)
    {
      /* Nothing to scan or fetch. */
      set_progress (self, 1.0);
      set_status (self, g_strdup ("Up to date."));
      finish_pass (self, NULL);
      return;
    }
  self->phase = PHASE_LISTS;
  start_next_list (self);
}

/* --- phase 2: list_messages per folder --------------------------- */

static void
start_next_list (MailSync *self)
{
  const guint k = self->folder_index + 1;
  const guint n = self->folder_remote_ids->len;
  set_status (self, g_strdup_printf ("Scanning folders (%u / %u)…", k, n));

  const char *folder_id = g_ptr_array_index (self->folder_remote_ids, self->folder_index);
  /* G_MAXINT signals "no upper bound" — paginate until the backend
   * runs out of pages. Per-page sizing is the backend's concern. */
  mail_backend_list_messages_async (self->remote, folder_id, G_MAXINT,
                                    self->cancellable, on_messages_done, self);
}

static void
on_messages_done (GObject *src,
                  GAsyncResult *res,
                  gpointer user_data)
{
  MailSync *self = user_data;
  GError *error = NULL;
  GPtrArray *messages = mail_backend_list_messages_finish (self->remote, res, &error);
  if (messages == NULL)
    {
      finish_pass (self, error);
      return;
    }

  const char *folder_id = g_ptr_array_index (self->folder_remote_ids, self->folder_index);
  const char *folder_dir = g_ptr_array_index (self->folder_dir_names, self->folder_index);

  GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *existing = mail_store_message_remote_ids (self->local, folder_id, &error);
  if (existing == NULL)
    {
      g_hash_table_unref (seen);
      finish_pass (self, error);
      return;
    }

  /* Queue fetches for new ids. */
  for (guint i = 0; i < messages->len; i++)
    {
      MailMessageMeta *m = g_ptr_array_index (messages, i);
      g_hash_table_add (seen, g_strdup (m->id));
      if (g_hash_table_contains (existing, m->id))
        continue; /* update path deferred (no flag push-back in v1) */

      FetchItem *it = g_new0 (FetchItem, 1);
      it->folder_remote_id = g_strdup (folder_id);
      it->folder_dir_name = g_strdup (folder_dir);
      it->message_remote_id = g_strdup (m->id);
      it->subject = g_strdup (m->subject);
      it->from_addr = g_strdup (m->from);
      it->received_unix = m->received_unix;
      it->unread = m->unread;
      g_ptr_array_add (self->pending_fetches, it);
    }

  /* Drop locally-only ids within this window. */
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init (&it, existing);
  while (g_hash_table_iter_next (&it, &key, NULL))
    {
      const char *rid = key;
      if (!g_hash_table_contains (seen, rid))
        {
          if (!mail_store_delete_message (self->local, rid, &error))
            {
              g_hash_table_unref (existing);
              g_hash_table_unref (seen);
              finish_pass (self, error);
              return;
            }
        }
    }
  g_hash_table_unref (existing);
  g_hash_table_unref (seen);

  /* Advance folder cursor + progress. */
  self->folder_index++;
  const guint n = self->folder_remote_ids->len;
  set_progress (self, 0.05 + 0.15 * ((double) self->folder_index / (double) n));

  if (self->folder_index < n)
    {
      start_next_list (self);
      return;
    }

  /* Transition to PHASE_FETCH. */
  if (self->pending_fetches->len == 0)
    {
      set_progress (self, 1.0);
      set_status (self, g_strdup ("Up to date."));
      finish_pass (self, NULL);
      return;
    }
  self->phase = PHASE_FETCH;
  self->fetch_index = 0;
  start_next_fetch (self);
}

/* --- phase 3: fetch_message_raw per new id ----------------------- */

static void
start_next_fetch (MailSync *self)
{
  const guint k = self->fetch_index + 1;
  const guint n = self->pending_fetches->len;
  set_status (self, g_strdup_printf ("Downloading messages (%u / %u)…", k, n));

  FetchItem *it = g_ptr_array_index (self->pending_fetches, self->fetch_index);
  mail_backend_fetch_message_raw_async (self->remote, it->message_remote_id,
                                        self->cancellable, on_fetch_done, self);
}

static void
on_fetch_done (GObject *src,
               GAsyncResult *res,
               gpointer user_data)
{
  MailSync *self = user_data;
  GError *error = NULL;
  g_autoptr (GBytes) bytes = mail_backend_fetch_message_raw_finish (self->remote, res, &error);
  if (bytes == NULL)
    {
      finish_pass (self, error);
      return;
    }

  FetchItem *it = g_ptr_array_index (self->pending_fetches, self->fetch_index);
  g_autofree char *filename = NULL;
  if (!mail_store_write_raw (self->local, it->folder_dir_name, bytes, !it->unread,
                             &filename, &error))
    {
      finish_pass (self, error);
      return;
    }
  if (!mail_store_upsert_message (self->local, it->folder_remote_id, it->message_remote_id,
                                  filename, it->subject, it->from_addr,
                                  it->received_unix, it->unread, NULL, &error))
    {
      finish_pass (self, error);
      return;
    }

  self->fetch_index++;
  const guint n = self->pending_fetches->len;
  set_progress (self, 0.20 + 0.80 * ((double) self->fetch_index / (double) n));

  if (self->fetch_index < n)
    {
      start_next_fetch (self);
      return;
    }
  set_status (self, g_strdup ("Up to date."));
  finish_pass (self, NULL);
}

/* --- finish ------------------------------------------------------ */

static void
finish_pass (MailSync *self,
             GError *error /* takes ownership */)
{
  GTask *task = self->task;
  self->task = NULL;

  /* Clear per-pass state. */
  g_clear_object (&self->cancellable);
  self->remote = NULL;
  self->local = NULL;
  g_clear_pointer (&self->folder_remote_ids, g_ptr_array_unref);
  g_clear_pointer (&self->folder_dir_names, g_ptr_array_unref);
  g_clear_pointer (&self->pending_fetches, g_ptr_array_unref);
  self->phase = PHASE_IDLE;

  set_running (self, FALSE);

  if (task != NULL)
    {
      if (error != NULL)
        g_task_return_error (task, error);
      else
        g_task_return_boolean (task, TRUE);
      g_object_unref (task);
    }
  else if (error != NULL)
    g_error_free (error);
}

/* --- GObject ----------------------------------------------------- */

static void
mail_sync_init (MailSync *self)
{
  self->status = g_strdup ("");
}

static void
mail_sync_get_property (GObject *object,
                        guint prop_id,
                        GValue *value,
                        GParamSpec *pspec)
{
  MailSync *self = MAIL_SYNC (object);
  switch (prop_id)
    {
    case PROP_RUNNING:
      g_value_set_boolean (value, self->running);
      break;
    case PROP_PROGRESS:
      g_value_set_double (value, self->progress);
      break;
    case PROP_STATUS:
      g_value_set_string (value, self->status);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mail_sync_finalize (GObject *object)
{
  MailSync *self = MAIL_SYNC (object);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->folder_remote_ids, g_ptr_array_unref);
  g_clear_pointer (&self->folder_dir_names, g_ptr_array_unref);
  g_clear_pointer (&self->pending_fetches, g_ptr_array_unref);
  g_free (self->status);
  G_OBJECT_CLASS (mail_sync_parent_class)->finalize (object);
}

static void
mail_sync_class_init (MailSyncClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->get_property = mail_sync_get_property;
  object_class->finalize = mail_sync_finalize;

  properties[PROP_RUNNING] = g_param_spec_boolean ("running", NULL, NULL,
                                                   FALSE,
                                                   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_PROGRESS] = g_param_spec_double ("progress", NULL, NULL,
                                                   0.0, 1.0, 0.0,
                                                   G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_STATUS] = g_param_spec_string ("status", NULL, NULL, "",
                                                 G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, properties);
}
