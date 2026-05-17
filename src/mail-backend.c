/* mail-backend.c - Trampoline wrappers around the MailBackend vtable. */

#include "config.h"

#include "mail-backend.h"

void
mail_backend_destroy (MailBackend *self)
{
  if (self == NULL)
    return;
  self->vt->destroy (self);
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
