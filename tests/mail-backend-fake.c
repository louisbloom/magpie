/* tests/mail-backend-fake.c - In-memory MailBackend implementation.
 *
 * The fake stores its seeded data outside the arena (in g_strdup'd
 * fields owned by the fake). On each async entry it resets the arena,
 * copies the seeded data in, and schedules g_task_return on the main
 * context via g_idle_add — so consumers see the same observable
 * behaviour as the real backends.
 */

#include "mail-backend-fake.h"

#include <gtk/gtk.h>
#include <string.h>

typedef struct
{
  char *id;
  char *display_name;
  char *parent_id;
  int unread;
  int total;
} FakeFolderStored;

typedef struct
{
  char *id;
  char *subject;
  char *from;
  gint64 received_unix;
  gboolean unread;
  char *raw_rfc822;
  char *content_key;
} FakeMessageStored;

typedef struct
{
  char *folder_id;
  GArray *messages; /* of FakeMessageStored */
} FakeFolderMessages;

typedef struct
{
  MailBackend base;

  GArray *folders;         /* of FakeFolderStored */
  GArray *folder_messages; /* of FakeFolderMessages */

  /* Observation. */
  guint list_folders_calls;
  guint list_messages_calls;
  guint fetch_raw_calls;
  guint fetch_messages_raw_calls;
  char *last_folder_id;
  char *last_message_id;
} MailBackendFake;

static void
fake_folder_clear (gpointer p)
{
  FakeFolderStored *f = p;
  g_free (f->id);
  g_free (f->display_name);
  g_free (f->parent_id);
}

static void
fake_message_clear (gpointer p)
{
  FakeMessageStored *m = p;
  g_free (m->id);
  g_free (m->subject);
  g_free (m->from);
  g_free (m->raw_rfc822);
  g_free (m->content_key);
}

static void
fake_folder_messages_clear (gpointer p)
{
  FakeFolderMessages *fm = p;
  g_free (fm->folder_id);
  g_array_unref (fm->messages);
}

/* --- Helpers --------------------------------------------------- */

static FakeFolderMessages *
find_or_create_folder_messages (MailBackendFake *self,
                                const char *folder_id)
{
  for (guint i = 0; i < self->folder_messages->len; i++)
    {
      FakeFolderMessages *fm = &g_array_index (self->folder_messages, FakeFolderMessages, i);
      if (g_strcmp0 (fm->folder_id, folder_id) == 0)
        return fm;
    }
  FakeFolderMessages fm;
  fm.folder_id = g_strdup (folder_id);
  fm.messages = g_array_new (FALSE, FALSE, sizeof (FakeMessageStored));
  g_array_set_clear_func (fm.messages, fake_message_clear);
  g_array_append_val (self->folder_messages, fm);
  return &g_array_index (self->folder_messages, FakeFolderMessages,
                         self->folder_messages->len - 1);
}

static FakeFolderMessages *
find_folder_messages (MailBackendFake *self,
                      const char *folder_id)
{
  for (guint i = 0; i < self->folder_messages->len; i++)
    {
      FakeFolderMessages *fm = &g_array_index (self->folder_messages, FakeFolderMessages, i);
      if (g_strcmp0 (fm->folder_id, folder_id) == 0)
        return fm;
    }
  return NULL;
}

/* --- vtable: list_folders -------------------------------------- */

typedef struct
{
  GTask *task;            /* ref'd */
  GPtrArray *folders_out; /* of MailFolder*, arena-borrowed */
} IdleListFolders;

static gboolean
idle_complete_list_folders (gpointer p)
{
  IdleListFolders *ctx = p;
  if (g_task_return_error_if_cancelled (ctx->task))
    {
      g_ptr_array_unref (ctx->folders_out);
    }
  else
    {
      g_task_return_pointer (ctx->task, ctx->folders_out,
                             (GDestroyNotify) g_ptr_array_unref);
    }
  g_object_unref (ctx->task);
  g_free (ctx);
  return G_SOURCE_REMOVE;
}

static void
mb_fake_list_folders_async (MailBackend *base,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
  MailBackendFake *self = (MailBackendFake *) base;
  self->list_folders_calls++;

  mail_arena_reset (&self->base.fetch_arena);
  g_byte_array_set_size (self->base.response_buf, 0);
  g_string_truncate (self->base.path_buf, 0);

  GPtrArray *out = g_ptr_array_new ();
  for (guint i = 0; i < self->folders->len; i++)
    {
      FakeFolderStored *src = &g_array_index (self->folders, FakeFolderStored, i);
      MailFolder *dst = mail_arena_alloc (&self->base.fetch_arena, sizeof *dst, _Alignof (MailFolder));
      dst->id = mail_arena_strdup (&self->base.fetch_arena, src->id);
      dst->display_name = mail_arena_strdup (&self->base.fetch_arena, src->display_name);
      dst->parent_id = src->parent_id != NULL ? mail_arena_strdup (&self->base.fetch_arena, src->parent_id) : NULL;
      dst->unread_count = src->unread;
      dst->total_count = src->total;
      g_ptr_array_add (out, dst);
    }

  IdleListFolders *ctx = g_new (IdleListFolders, 1);
  ctx->task = g_task_new (NULL, cancellable, callback, user_data);
  ctx->folders_out = out;
  g_idle_add (idle_complete_list_folders, ctx);
}

static GPtrArray *
mb_fake_list_folders_finish (MailBackend *base,
                             GAsyncResult *result,
                             GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- vtable: list_messages ------------------------------------- */

typedef struct
{
  GTask *task;
  GPtrArray *messages_out;
} IdleListMessages;

static gboolean
idle_complete_list_messages (gpointer p)
{
  IdleListMessages *ctx = p;
  if (g_task_return_error_if_cancelled (ctx->task))
    g_ptr_array_unref (ctx->messages_out);
  else
    g_task_return_pointer (ctx->task, ctx->messages_out,
                           (GDestroyNotify) g_ptr_array_unref);
  g_object_unref (ctx->task);
  g_free (ctx);
  return G_SOURCE_REMOVE;
}

static void
mb_fake_list_messages_async (MailBackend *base,
                             const char *folder_id,
                             int top_n,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
  MailBackendFake *self = (MailBackendFake *) base;
  self->list_messages_calls++;
  g_free (self->last_folder_id);
  self->last_folder_id = g_strdup (folder_id);

  mail_arena_reset (&self->base.fetch_arena);
  g_byte_array_set_size (self->base.response_buf, 0);
  g_string_truncate (self->base.path_buf, 0);

  GPtrArray *out = g_ptr_array_new ();
  FakeFolderMessages *fm = find_folder_messages (self, folder_id);
  if (fm != NULL)
    {
      guint count = MIN ((guint) top_n, fm->messages->len);
      for (guint i = 0; i < count; i++)
        {
          FakeMessageStored *src = &g_array_index (fm->messages, FakeMessageStored, i);
          MailMessageMeta *dst = mail_arena_alloc (&self->base.fetch_arena, sizeof *dst, _Alignof (MailMessageMeta));
          dst->id = mail_arena_strdup (&self->base.fetch_arena, src->id);
          dst->subject = mail_arena_strdup (&self->base.fetch_arena, src->subject);
          dst->from = mail_arena_strdup (&self->base.fetch_arena, src->from);
          dst->received_unix = src->received_unix;
          dst->unread = src->unread;
          dst->content_key = src->content_key != NULL
                                 ? mail_arena_strdup (&self->base.fetch_arena, src->content_key)
                                 : NULL;
          g_ptr_array_add (out, dst);
        }
    }

  IdleListMessages *ctx = g_new (IdleListMessages, 1);
  ctx->task = g_task_new (NULL, cancellable, callback, user_data);
  ctx->messages_out = out;
  g_idle_add (idle_complete_list_messages, ctx);
}

static GPtrArray *
mb_fake_list_messages_finish (MailBackend *base,
                              GAsyncResult *result,
                              GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- vtable: fetch_message_raw --------------------------------- */

typedef struct
{
  GTask *task;
  char *raw; /* owned; freed in idle */
} IdleFetchRaw;

static gboolean
idle_complete_fetch_raw (gpointer p)
{
  IdleFetchRaw *ctx = p;
  if (g_task_return_error_if_cancelled (ctx->task))
    {
      /* nothing */
    }
  else
    {
      g_task_return_pointer (ctx->task, ctx->raw, g_free);
      ctx->raw = NULL;
    }
  g_object_unref (ctx->task);
  g_free (ctx->raw);
  g_free (ctx);
  return G_SOURCE_REMOVE;
}

static void
mb_fake_fetch_message_raw_async (MailBackend *base,
                                 const char *message_id,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  MailBackendFake *self = (MailBackendFake *) base;
  self->fetch_raw_calls++;
  g_free (self->last_message_id);
  self->last_message_id = g_strdup (message_id);

  /* Walk every seeded folder looking for the message id. */
  char *raw_copy = NULL;
  for (guint i = 0; i < self->folder_messages->len && raw_copy == NULL; i++)
    {
      FakeFolderMessages *fm = &g_array_index (self->folder_messages, FakeFolderMessages, i);
      for (guint j = 0; j < fm->messages->len; j++)
        {
          FakeMessageStored *m = &g_array_index (fm->messages, FakeMessageStored, j);
          if (g_strcmp0 (m->id, message_id) == 0 && m->raw_rfc822 != NULL)
            {
              raw_copy = g_strdup (m->raw_rfc822);
              break;
            }
        }
    }
  if (raw_copy == NULL)
    raw_copy = g_strdup ("");

  IdleFetchRaw *ctx = g_new (IdleFetchRaw, 1);
  ctx->task = g_task_new (NULL, cancellable, callback, user_data);
  ctx->raw = raw_copy;
  g_idle_add (idle_complete_fetch_raw, ctx);
}

static GBytes *
mb_fake_fetch_message_raw_finish (MailBackend *base,
                                  GAsyncResult *result,
                                  GError **error)
{
  char *raw = g_task_propagate_pointer (G_TASK (result), error);
  if (raw == NULL)
    return NULL;
  gsize len = strlen (raw);
  return g_bytes_new_take (raw, len);
}

/* --- vtable: fetch_messages_raw (batched) ---------------------- */

typedef struct
{
  GTask *task;
  GPtrArray *bodies; /* of GBytes*, parallel to input ids; NULL slots ok */
} IdleFetchBatch;

static gboolean
idle_complete_fetch_batch (gpointer p)
{
  IdleFetchBatch *ctx = p;
  if (g_task_return_error_if_cancelled (ctx->task))
    g_ptr_array_unref (ctx->bodies);
  else
    g_task_return_pointer (ctx->task, ctx->bodies, (GDestroyNotify) g_ptr_array_unref);
  g_object_unref (ctx->task);
  g_free (ctx);
  return G_SOURCE_REMOVE;
}

static void
mb_fake_fetch_messages_raw_async (MailBackend *base,
                                  const char *const *message_ids,
                                  gsize n_ids,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  MailBackendFake *self = (MailBackendFake *) base;
  self->fetch_messages_raw_calls++;

  /* Build the result array eagerly — same fake semantics as the
   * per-message path, just collected. Each lookup tracks the
   * per-message call counter so callers can distinguish "sync used
   * the batched API" from "sync used the fallback that loops the
   * per-message API". */
  GPtrArray *bodies = g_ptr_array_new_full (n_ids, (GDestroyNotify) g_bytes_unref);
  for (gsize i = 0; i < n_ids; i++)
    {
      GBytes *body = NULL;
      for (guint f = 0; f < self->folder_messages->len && body == NULL; f++)
        {
          FakeFolderMessages *fm = &g_array_index (self->folder_messages, FakeFolderMessages, f);
          for (guint j = 0; j < fm->messages->len; j++)
            {
              FakeMessageStored *m = &g_array_index (fm->messages, FakeMessageStored, j);
              if (g_strcmp0 (m->id, message_ids[i]) == 0 && m->raw_rfc822 != NULL)
                {
                  body = g_bytes_new (m->raw_rfc822, strlen (m->raw_rfc822));
                  break;
                }
            }
        }
      g_ptr_array_add (bodies, body); /* NULL on miss; matches contract */
    }

  IdleFetchBatch *ctx = g_new (IdleFetchBatch, 1);
  ctx->task = g_task_new (NULL, cancellable, callback, user_data);
  ctx->bodies = bodies;
  g_idle_add (idle_complete_fetch_batch, ctx);
}

static GPtrArray *
mb_fake_fetch_messages_raw_finish (MailBackend *base,
                                   GAsyncResult *result,
                                   GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- destroy ---------------------------------------------------- */

static void
mb_fake_destroy (MailBackend *base)
{
  MailBackendFake *self = (MailBackendFake *) base;
  if (self->folders != NULL)
    g_array_unref (self->folders);
  if (self->folder_messages != NULL)
    g_array_unref (self->folder_messages);
  g_free (self->last_folder_id);
  g_free (self->last_message_id);
  if (self->base.response_buf != NULL)
    g_byte_array_unref (self->base.response_buf);
  if (self->base.path_buf != NULL)
    g_string_free (self->base.path_buf, TRUE);
  mail_arena_destroy (&self->base.fetch_arena);
  g_free (self);
}

static const MailBackendVTable fake_vt = {
  .list_folders_async = mb_fake_list_folders_async,
  .list_folders_finish = mb_fake_list_folders_finish,
  .list_messages_async = mb_fake_list_messages_async,
  .list_messages_finish = mb_fake_list_messages_finish,
  .fetch_message_raw_async = mb_fake_fetch_message_raw_async,
  .fetch_message_raw_finish = mb_fake_fetch_message_raw_finish,
  .fetch_messages_raw_async = mb_fake_fetch_messages_raw_async,
  .fetch_messages_raw_finish = mb_fake_fetch_messages_raw_finish,
  .destroy = mb_fake_destroy,
};

/* --- Public API ------------------------------------------------- */

MailBackend *
mail_backend_fake_new (void)
{
  MailBackendFake *self = g_new0 (MailBackendFake, 1);
  self->base.vt = &fake_vt;
  mail_arena_init (&self->base.fetch_arena, 4096);
  self->base.response_buf = g_byte_array_new ();
  self->base.path_buf = g_string_sized_new (256);
  self->folders = g_array_new (FALSE, FALSE, sizeof (FakeFolderStored));
  g_array_set_clear_func (self->folders, fake_folder_clear);
  self->folder_messages = g_array_new (FALSE, FALSE, sizeof (FakeFolderMessages));
  g_array_set_clear_func (self->folder_messages, fake_folder_messages_clear);
  return (MailBackend *) self;
}

void
mail_backend_fake_set_folders (MailBackend *backend,
                               const FakeFolderSpec *folders,
                               gsize n)
{
  MailBackendFake *self = (MailBackendFake *) backend;
  g_array_set_size (self->folders, 0); /* clear, runs clear-func */
  for (gsize i = 0; i < n; i++)
    {
      FakeFolderStored row;
      row.id = g_strdup (folders[i].id);
      row.display_name = g_strdup (folders[i].display_name);
      row.parent_id = folders[i].parent_id != NULL ? g_strdup (folders[i].parent_id) : NULL;
      row.unread = folders[i].unread;
      row.total = folders[i].total;
      g_array_append_val (self->folders, row);
    }
}

void
mail_backend_fake_set_messages (MailBackend *backend,
                                const char *folder_id,
                                const FakeMessageSpec *msgs,
                                gsize n)
{
  MailBackendFake *self = (MailBackendFake *) backend;
  FakeFolderMessages *fm = find_or_create_folder_messages (self, folder_id);
  g_array_set_size (fm->messages, 0);
  for (gsize i = 0; i < n; i++)
    {
      FakeMessageStored row;
      row.id = g_strdup (msgs[i].id);
      row.subject = g_strdup (msgs[i].subject);
      row.from = g_strdup (msgs[i].from);
      row.received_unix = msgs[i].received_unix;
      row.unread = msgs[i].unread;
      row.raw_rfc822 = msgs[i].raw_rfc822 != NULL ? g_strdup (msgs[i].raw_rfc822) : NULL;
      row.content_key = msgs[i].content_key != NULL ? g_strdup (msgs[i].content_key) : NULL;
      g_array_append_val (fm->messages, row);
    }
}

guint
mail_backend_fake_list_folders_calls (MailBackend *backend)
{
  return ((MailBackendFake *) backend)->list_folders_calls;
}

guint
mail_backend_fake_list_messages_calls (MailBackend *backend)
{
  return ((MailBackendFake *) backend)->list_messages_calls;
}

guint
mail_backend_fake_fetch_raw_calls (MailBackend *backend)
{
  return ((MailBackendFake *) backend)->fetch_raw_calls;
}

guint
mail_backend_fake_fetch_messages_raw_calls (MailBackend *backend)
{
  return ((MailBackendFake *) backend)->fetch_messages_raw_calls;
}

const char *
mail_backend_fake_last_folder_id (MailBackend *backend)
{
  return ((MailBackendFake *) backend)->last_folder_id;
}

const char *
mail_backend_fake_last_message_id (MailBackend *backend)
{
  return ((MailBackendFake *) backend)->last_message_id;
}
