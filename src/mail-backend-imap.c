/* mail-backend-imap.c - IMAP backend skeleton (libetpan).
 *
 * Compile-tested only in this initial step: the user's sole configured
 * GOA account is Microsoft 365 (no IMAP), so the runtime paths here
 * are not exercised. The file exists to validate that:
 *   1. libetpan headers compile against our CFLAGS,
 *   2. mailimap_new and mailimap_free link from -letpan,
 *   3. the vtable hooks satisfy the MailBackend contract.
 *
 * When the user adds a Gmail/Yahoo/IMAP account in GOA, replace the
 * NOT_SUPPORTED returns with real implementations following the same
 * arena / response_buf discipline as mail-backend-msgraph.c.
 */

#include "config.h"

#include "mail-backend-imap.h"

#include <goa/goa.h>
#include <gtk/gtk.h>
#include <libetpan/libetpan.h>
#include <string.h>

typedef struct
{
  MailBackend base;
  GoaObject *goa_object; /* ref'd; also serves as GTask source */
  mailimap *imap;        /* libetpan handle; lazily connected */
  /* Cached connection parameters read from GoaMail at construction. */
  char *host;
  int port;
  gboolean use_ssl;
  gboolean use_tls;
  char *username;
} MailBackendIMAP;

static void
mb_imap_destroy (MailBackend *base)
{
  MailBackendIMAP *self = (MailBackendIMAP *) base;
  if (self->imap != NULL)
    mailimap_free (self->imap);
  g_free (self->host);
  g_free (self->username);
  if (self->goa_object != NULL)
    g_object_unref (self->goa_object);
  if (self->base.response_buf != NULL)
    g_byte_array_unref (self->base.response_buf);
  if (self->base.path_buf != NULL)
    g_string_free (self->base.path_buf, TRUE);
  mail_arena_destroy (&self->base.fetch_arena);
  g_free (self);
}

static void
return_not_supported (GTask *task)
{
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "IMAP backend not yet implemented in this initial step");
  g_object_unref (task);
}

static void
mb_imap_list_folders_async (MailBackend *base,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
  MailBackendIMAP *self = (MailBackendIMAP *) base;
  GTask *task = g_task_new (self->goa_object, cancellable, callback, user_data);
  return_not_supported (task);
}

static GPtrArray *
mb_imap_list_folders_finish (MailBackend *base,
                             GAsyncResult *result,
                             GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
mb_imap_list_messages_async (MailBackend *base,
                             const char *folder_id,
                             int top_n,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
  MailBackendIMAP *self = (MailBackendIMAP *) base;
  GTask *task = g_task_new (self->goa_object, cancellable, callback, user_data);
  return_not_supported (task);
}

static GPtrArray *
mb_imap_list_messages_finish (MailBackend *base,
                              GAsyncResult *result,
                              GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
mb_imap_fetch_message_raw_async (MailBackend *base,
                                 const char *message_id,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  MailBackendIMAP *self = (MailBackendIMAP *) base;
  GTask *task = g_task_new (self->goa_object, cancellable, callback, user_data);
  return_not_supported (task);
}

static GBytes *
mb_imap_fetch_message_raw_finish (MailBackend *base,
                                  GAsyncResult *result,
                                  GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static const MailBackendVTable imap_vt = {
  .list_folders_async = mb_imap_list_folders_async,
  .list_folders_finish = mb_imap_list_folders_finish,
  .list_messages_async = mb_imap_list_messages_async,
  .list_messages_finish = mb_imap_list_messages_finish,
  .fetch_message_raw_async = mb_imap_fetch_message_raw_async,
  .fetch_message_raw_finish = mb_imap_fetch_message_raw_finish,
  .destroy = mb_imap_destroy,
};

MailBackend *
mail_backend_imap_new (GoaObject *goa_object)
{
  g_return_val_if_fail (GOA_IS_OBJECT (goa_object), NULL);

  GoaMail *mail = goa_object_peek_mail (goa_object);
  if (mail == NULL || !goa_mail_get_imap_supported (mail))
    return NULL;

  const char *host = goa_mail_get_imap_host (mail);
  const char *user = goa_mail_get_imap_user_name (mail);
  if (host == NULL || host[0] == '\0' || user == NULL || user[0] == '\0')
    return NULL;

  MailBackendIMAP *self = g_new0 (MailBackendIMAP, 1);
  self->base.vt = &imap_vt;
  mail_arena_init (&self->base.fetch_arena, 4096);
  self->base.response_buf = g_byte_array_new ();
  self->base.path_buf = g_string_sized_new (256);
  self->goa_object = g_object_ref (goa_object);
  self->host = g_strdup (host);
  self->username = g_strdup (user);
  self->use_ssl = goa_mail_get_imap_use_ssl (mail);
  self->use_tls = goa_mail_get_imap_use_tls (mail);
  self->port = self->use_ssl ? 993 : 143;
  self->imap = mailimap_new (0, NULL); /* link smoke; not connected */
  return (MailBackend *) self;
}
