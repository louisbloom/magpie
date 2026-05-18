/* mail-backend.c - Trampoline wrappers around the MailBackend vtable. */

#include "config.h"

#include "mail-backend.h"

#include <string.h>

typedef struct
{
  guint id;
  MailBackendChangeCb cb;
  gpointer user_data;
  GDestroyNotify notify;
} MailBackendListener;

static void
release_listeners (MailBackend *self)
{
  if (self->change_listeners == NULL)
    return;
  for (guint i = 0; i < self->change_listeners->len; i++)
    {
      MailBackendListener *l = &g_array_index (self->change_listeners, MailBackendListener, i);
      if (l->notify != NULL)
        l->notify (l->user_data);
    }
  g_array_free (self->change_listeners, TRUE);
  self->change_listeners = NULL;
}

void
mail_backend_destroy (MailBackend *self)
{
  if (self == NULL)
    return;
  /* Release listeners before the vt->destroy frees the embedding
   * struct — the GDestroyNotify hooks may reference fields owned by
   * the subclass and must run before that storage disappears. */
  release_listeners (self);
  self->vt->destroy (self);
}

guint
mail_backend_add_listener (MailBackend *self,
                           MailBackendChangeCb cb,
                           gpointer user_data,
                           GDestroyNotify notify)
{
  g_return_val_if_fail (self != NULL, 0);
  g_return_val_if_fail (cb != NULL, 0);

  if (self->change_listeners == NULL)
    self->change_listeners = g_array_new (FALSE, FALSE, sizeof (MailBackendListener));
  if (self->next_listener_id == 0)
    self->next_listener_id = 1;
  MailBackendListener l = {
    .id = self->next_listener_id++,
    .cb = cb,
    .user_data = user_data,
    .notify = notify,
  };
  g_array_append_val (self->change_listeners, l);
  return l.id;
}

void
mail_backend_remove_listener (MailBackend *self,
                              guint id)
{
  g_return_if_fail (self != NULL);
  if (id == 0 || self->change_listeners == NULL)
    return;
  for (guint i = 0; i < self->change_listeners->len; i++)
    {
      MailBackendListener *l = &g_array_index (self->change_listeners, MailBackendListener, i);
      if (l->id == id)
        {
          GDestroyNotify notify = l->notify;
          gpointer user_data = l->user_data;
          g_array_remove_index (self->change_listeners, i);
          if (notify != NULL)
            notify (user_data);
          return;
        }
    }
}

void
mail_backend_emit_change (MailBackend *self,
                          const MailBackendChange *change)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (change != NULL);
  if (self->change_listeners == NULL || self->change_listeners->len == 0)
    return;
  /* Snapshot the listener list so a callback that removes itself or
   * registers another listener doesn't break iteration. */
  guint n = self->change_listeners->len;
  MailBackendListener *snap = g_newa (MailBackendListener, n);
  memcpy (snap, self->change_listeners->data, n * sizeof (MailBackendListener));
  for (guint i = 0; i < n; i++)
    snap[i].cb (self, change, snap[i].user_data);
}

void
mail_backend_list_folders_async (MailBackend *self,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  self->vt->list_folders_async (self, cancellable, callback, user_data);
}

GPtrArray *
mail_backend_list_folders_finish (MailBackend *self,
                                  GAsyncResult *result,
                                  GError **error)
{
  return self->vt->list_folders_finish (self, result, error);
}

void
mail_backend_list_messages_async (MailBackend *self,
                                  const char *folder_id,
                                  int top_n,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  self->vt->list_messages_async (self, folder_id, top_n, cancellable, callback, user_data);
}

GPtrArray *
mail_backend_list_messages_finish (MailBackend *self,
                                   GAsyncResult *result,
                                   GError **error)
{
  return self->vt->list_messages_finish (self, result, error);
}

void
mail_backend_fetch_message_raw_async (MailBackend *self,
                                      const char *message_id,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
  self->vt->fetch_message_raw_async (self, message_id, cancellable, callback, user_data);
}

GBytes *
mail_backend_fetch_message_raw_finish (MailBackend *self,
                                       GAsyncResult *result,
                                       GError **error)
{
  return self->vt->fetch_message_raw_finish (self, result, error);
}

/* --- batched fetch trampoline + sequential fallback ---------------- */

/* When a backend doesn't supply fetch_messages_raw_async, we drive
 * the per-message vtable in sequence and assemble the parallel
 * GPtrArray ourselves. Correct everywhere; only fast where it's been
 * specialised (IMAP). */
typedef struct
{
  MailBackend *backend;
  char **ids; /* g_strdup'd, NULL-terminated; len == n_ids */
  gsize n_ids;
  gsize index;       /* next id to fetch */
  GPtrArray *bodies; /* len == n_ids; slots filled progressively */
} FallbackJob;

static void
fallback_job_free (gpointer p)
{
  FallbackJob *j = p;
  if (j == NULL)
    return;
  g_strfreev (j->ids);
  if (j->bodies != NULL)
    g_ptr_array_unref (j->bodies);
  g_free (j);
}

static void fallback_step (GTask *task);

static void
fallback_on_one_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  GTask *task = user_data;
  FallbackJob *job = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;
  GBytes *body = mail_backend_fetch_message_raw_finish (job->backend, res, &error);

  /* A per-message failure NULL-marks the slot and we keep going —
   * that matches the contract documented for the batched API:
   * "NULL entries mark missing messages". A cancellation propagates
   * as a real error. */
  if (body == NULL && error != NULL && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }
  g_ptr_array_index (job->bodies, job->index) = body; /* NULL ok */
  job->index++;

  if (job->index < job->n_ids)
    {
      fallback_step (task);
      return;
    }
  g_task_return_pointer (task, g_ptr_array_ref (job->bodies),
                         (GDestroyNotify) g_ptr_array_unref);
  g_object_unref (task);
}

static void
fallback_step (GTask *task)
{
  FallbackJob *job = g_task_get_task_data (task);
  mail_backend_fetch_message_raw_async (job->backend, job->ids[job->index],
                                        g_task_get_cancellable (task),
                                        fallback_on_one_done, task);
}

void
mail_backend_fetch_messages_raw_async (MailBackend *self,
                                       const char *const *message_ids,
                                       gsize n_ids,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
  if (self->vt->fetch_messages_raw_async != NULL)
    {
      self->vt->fetch_messages_raw_async (self, message_ids, n_ids,
                                          cancellable, callback, user_data);
      return;
    }

  /* GTask requires a GObject source; MailBackend is not one. NULL
   * is the standard sentinel. The fallback's GTask lifetime is
   * scoped to this call chain, so we don't need it to carry the
   * backend pointer — task_data carries the FallbackJob with @self
   * inside. */
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);
  if (n_ids == 0)
    {
      GPtrArray *empty = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
      g_task_return_pointer (task, empty, (GDestroyNotify) g_ptr_array_unref);
      g_object_unref (task);
      return;
    }

  FallbackJob *job = g_new0 (FallbackJob, 1);
  job->backend = self;
  job->ids = g_new0 (char *, n_ids + 1);
  for (gsize i = 0; i < n_ids; i++)
    job->ids[i] = g_strdup (message_ids[i]);
  job->n_ids = n_ids;
  job->bodies = g_ptr_array_new_full (n_ids, (GDestroyNotify) g_bytes_unref);
  g_ptr_array_set_size (job->bodies, n_ids); /* fills with NULLs */
  g_task_set_task_data (task, job, fallback_job_free);
  fallback_step (task);
}

GPtrArray *
mail_backend_fetch_messages_raw_finish (MailBackend *self,
                                        GAsyncResult *result,
                                        GError **error)
{
  /* The async trampoline routes through the vt slot when present and
   * uses its own GTask for the fallback path. Either way the finish
   * symmetry is the vt slot when defined, our own propagate
   * otherwise. */
  if (self->vt->fetch_messages_raw_async != NULL)
    return self->vt->fetch_messages_raw_finish (self, result, error);
  return g_task_propagate_pointer (G_TASK (result), error);
}
