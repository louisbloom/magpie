/* mail-backend-store.c - MailBackend backed by a local MailStore.
 *
 * The reads are synchronous (sqlite + filesystem) but we still
 * dispatch the GTask completion via g_idle_add so callers see the
 * same async semantics as msgraph/imap and our existing tests.
 *
 * Arena discipline (same contract as msgraph):
 *   - list_folders / list_messages reset the arena and refill it.
 *   - fetch_message_raw MUST NOT touch the arena: the sidebar and
 *     message-list panes hold borrowed pointers across fetches.
 *     See tests/test-backend-contract.c.
 */

#include "config.h"

#include "mail-backend-store.h"

#include <gtk/gtk.h>

typedef struct
{
  MailBackend base;
  MailStore *store; /* borrowed */
} MailBackendStore;

static void
reset_arena_and_buffers (MailBackendStore *self)
{
  mail_arena_reset (&self->base.fetch_arena);
  g_byte_array_set_size (self->base.response_buf, 0);
  g_string_truncate (self->base.path_buf, 0);
}

/* --- list_folders ------------------------------------------------- */

typedef struct
{
  MailBackendStore *self;
  GTask *task;
} ListFoldersCtx;

static gboolean
list_folders_run (gpointer p)
{
  ListFoldersCtx *ctx = p;
  if (!g_task_return_error_if_cancelled (ctx->task))
    {
      GError *error = NULL;
      GPtrArray *folders = mail_store_list_folders (ctx->self->store,
                                                    &ctx->self->base.fetch_arena, &error);
      if (folders == NULL)
        g_task_return_error (ctx->task, error);
      else
        g_task_return_pointer (ctx->task, folders, (GDestroyNotify) g_ptr_array_unref);
    }
  g_object_unref (ctx->task);
  g_free (ctx);
  return G_SOURCE_REMOVE;
}

static void
mb_store_list_folders_async (MailBackend *base,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
  MailBackendStore *self = (MailBackendStore *) base;
  reset_arena_and_buffers (self);
  ListFoldersCtx *ctx = g_new0 (ListFoldersCtx, 1);
  ctx->self = self;
  ctx->task = g_task_new (NULL, cancellable, callback, user_data);
  g_idle_add (list_folders_run, ctx);
}

static GPtrArray *
mb_store_list_folders_finish (MailBackend *base,
                              GAsyncResult *result,
                              GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- list_messages ------------------------------------------------ */

typedef struct
{
  MailBackendStore *self;
  char *folder_id;
  int top_n;
  GTask *task;
} ListMessagesCtx;

static gboolean
list_messages_run (gpointer p)
{
  ListMessagesCtx *ctx = p;
  if (!g_task_return_error_if_cancelled (ctx->task))
    {
      GError *error = NULL;
      GPtrArray *msgs = mail_store_list_messages (ctx->self->store,
                                                  ctx->folder_id, ctx->top_n,
                                                  &ctx->self->base.fetch_arena, &error);
      if (msgs == NULL)
        g_task_return_error (ctx->task, error);
      else
        g_task_return_pointer (ctx->task, msgs, (GDestroyNotify) g_ptr_array_unref);
    }
  g_object_unref (ctx->task);
  g_free (ctx->folder_id);
  g_free (ctx);
  return G_SOURCE_REMOVE;
}

static void
mb_store_list_messages_async (MailBackend *base,
                              const char *folder_id,
                              int top_n,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
  MailBackendStore *self = (MailBackendStore *) base;
  reset_arena_and_buffers (self);

  /* Defer the SQL query to the idle handler so it runs after the
   * async-callback contract (same shape as the fake backend). */
  ListMessagesCtx *ctx = g_new0 (ListMessagesCtx, 1);
  ctx->self = self;
  ctx->folder_id = g_strdup (folder_id);
  ctx->top_n = top_n;
  ctx->task = g_task_new (NULL, cancellable, callback, user_data);
  g_idle_add (list_messages_run, ctx);
}

static GPtrArray *
mb_store_list_messages_finish (MailBackend *base,
                               GAsyncResult *result,
                               GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- fetch_message_raw ------------------------------------------- */

typedef struct
{
  MailBackendStore *self;
  char *message_id;
  GTask *task;
} FetchRawCtx;

static gboolean
fetch_raw_run (gpointer p)
{
  FetchRawCtx *ctx = p;
  if (!g_task_return_error_if_cancelled (ctx->task))
    {
      /* MUST NOT touch self->base.fetch_arena — use a private one. */
      MailArena tmp;
      mail_arena_init (&tmp, 256);
      const char *dir_name = NULL;
      const char *filename = NULL;
      GError *error = NULL;
      gboolean found = mail_store_message_location (ctx->self->store, ctx->message_id,
                                                    &tmp, &dir_name, &filename, &error);
      if (!found && error == NULL)
        g_set_error (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "no local copy of message %s", ctx->message_id);

      GBytes *bytes = NULL;
      if (error == NULL)
        bytes = mail_store_read_raw (ctx->self->store, dir_name, filename, &error);

      mail_arena_destroy (&tmp);

      if (bytes == NULL)
        g_task_return_error (ctx->task, error);
      else
        g_task_return_pointer (ctx->task, bytes, (GDestroyNotify) g_bytes_unref);
    }
  g_object_unref (ctx->task);
  g_free (ctx->message_id);
  g_free (ctx);
  return G_SOURCE_REMOVE;
}

static void
mb_store_fetch_message_raw_async (MailBackend *base,
                                  const char *message_id,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  MailBackendStore *self = (MailBackendStore *) base;
  /* Crucially: no reset_arena here. */

  FetchRawCtx *ctx = g_new0 (FetchRawCtx, 1);
  ctx->self = self;
  ctx->message_id = g_strdup (message_id);
  ctx->task = g_task_new (NULL, cancellable, callback, user_data);
  g_idle_add (fetch_raw_run, ctx);
}

static GBytes *
mb_store_fetch_message_raw_finish (MailBackend *base,
                                   GAsyncResult *result,
                                   GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- destroy ------------------------------------------------------ */

static void
mb_store_destroy (MailBackend *base)
{
  MailBackendStore *self = (MailBackendStore *) base;
  /* self->store is borrowed; not freed here. */
  if (self->base.response_buf != NULL)
    g_byte_array_unref (self->base.response_buf);
  if (self->base.path_buf != NULL)
    g_string_free (self->base.path_buf, TRUE);
  mail_arena_destroy (&self->base.fetch_arena);
  g_free (self);
}

static const MailBackendVTable store_vt = {
  .list_folders_async = mb_store_list_folders_async,
  .list_folders_finish = mb_store_list_folders_finish,
  .list_messages_async = mb_store_list_messages_async,
  .list_messages_finish = mb_store_list_messages_finish,
  .fetch_message_raw_async = mb_store_fetch_message_raw_async,
  .fetch_message_raw_finish = mb_store_fetch_message_raw_finish,
  .destroy = mb_store_destroy,
};

MailBackend *
mail_backend_store_new (MailStore *store)
{
  g_return_val_if_fail (store != NULL, NULL);
  MailBackendStore *self = g_new0 (MailBackendStore, 1);
  self->base.vt = &store_vt;
  self->store = store;
  mail_arena_init (&self->base.fetch_arena, 4096);
  self->base.response_buf = g_byte_array_new ();
  self->base.path_buf = g_string_sized_new (256);
  return (MailBackend *) self;
}
