/* mail-backend.h - Provider-agnostic mail backend interface.
 *
 * Each provider (Microsoft Graph, IMAP, ...) supplies a vtable. All
 * transient parse output lives in the backend's owned MailArena, which
 * is reset between operations — see [[feedback-memory-reuse]].
 *
 * Lifetime: pointers returned by list_folders_finish / list_messages_finish
 * are valid only until the next call on the same backend.
 */

#pragma once

#include "mail-arena.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _MailBackend MailBackend;
typedef struct _MailBackendVTable MailBackendVTable;

typedef struct
{
  const char *id;
  const char *display_name;
  const char *parent_id; /* NULL for top-level */
  int unread_count;
  int total_count;
} MailFolder;

typedef struct
{
  const char *id;
  const char *subject;
  const char *from;
  gint64 received_unix;
  gboolean unread;
  /* Stable cross-folder identity for the message body. Used by the
   * sync engine to dedup: messages with the same content_key share a
   * single fetched body (hardlinked across folders in the local
   * Maildir tree). Allocated from the same arena as the other
   * fields; lifetime contract unchanged. NULL if the backend has no
   * such identity (the sync engine then treats the message as
   * unique and always fetches it).
   *
   * IMAP populates from ENVELOPE's Message-ID (RFC 5322 §3.6.4).
   * MS Graph populates from internetMessageId. */
  const char *content_key;
} MailMessageMeta;

struct _MailBackendVTable
{
  void (*list_folders_async) (MailBackend *self,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data);
  GPtrArray *(*list_folders_finish) (MailBackend *self,
                                     GAsyncResult *result,
                                     GError **error);

  void (*list_messages_async) (MailBackend *self,
                               const char *folder_id,
                               int top_n,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
  GPtrArray *(*list_messages_finish) (MailBackend *self,
                                      GAsyncResult *result,
                                      GError **error);

  void (*fetch_message_raw_async) (MailBackend *self,
                                   const char *message_id,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data);
  /* Returns a GBytes the caller must g_bytes_unref(), or NULL on error.
   * The bytes are owned by the caller and outlive the backend's arena;
   * callers retain them across the backend's next list_* call. */
  GBytes *(*fetch_message_raw_finish) (MailBackend *self,
                                       GAsyncResult *result,
                                       GError **error);

  void (*destroy) (MailBackend *self);
};

struct _MailBackend
{
  const MailBackendVTable *vt;
  MailArena fetch_arena;
  GByteArray *response_buf;
  GString *path_buf;
  /* Subclasses extend by embedding MailBackend as their first member. */
};

void mail_backend_destroy (MailBackend *self);

void mail_backend_list_folders_async (MailBackend *self,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);
GPtrArray *mail_backend_list_folders_finish (MailBackend *self,
                                             GAsyncResult *result,
                                             GError **error);

void mail_backend_list_messages_async (MailBackend *self,
                                       const char *folder_id,
                                       int top_n,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data);
GPtrArray *mail_backend_list_messages_finish (MailBackend *self,
                                              GAsyncResult *result,
                                              GError **error);

void mail_backend_fetch_message_raw_async (MailBackend *self,
                                           const char *message_id,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);
GBytes *mail_backend_fetch_message_raw_finish (MailBackend *self,
                                               GAsyncResult *result,
                                               GError **error);

G_END_DECLS
