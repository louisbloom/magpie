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

/* Change-notification payload. Mirrors MailStoreChange (mail-store.h)
 * but expressed at the backend boundary so UI code does not need to
 * know about MailStore at all. MailBackendStore rebroadcasts every
 * underlying MailStoreChange via its backend's registry; other
 * backends (msgraph, imap) never emit — they have no local state to
 * invalidate. */
typedef enum
{
  MAIL_BACKEND_CHANGE_MESSAGE_FLAGS,
  MAIL_BACKEND_CHANGE_MESSAGE_ADDED,
  MAIL_BACKEND_CHANGE_MESSAGE_REMOVED,
  MAIL_BACKEND_CHANGE_FOLDER_COUNTS,
} MailBackendChangeKind;

typedef struct
{
  MailBackendChangeKind kind;
  const char *folder_id;
  const char *message_id;
  gboolean unread;
  int folder_unread;
  int folder_total;
} MailBackendChange;

typedef void (*MailBackendChangeCb) (MailBackend *backend,
                                     const MailBackendChange *change,
                                     gpointer user_data);

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

  /* Batched body fetch. Optional: backends that leave this slot NULL
   * still work — the public trampoline falls back to a sequential
   * chain of fetch_message_raw_async/finish calls. Backends that
   * implement it can amortise per-message round-trips (IMAP does one
   * UID FETCH for the whole batch).
   *
   * On success, the GPtrArray returned by the finish has the same
   * length as the input id array. Slot i is either a GBytes for the
   * message body or NULL if that specific message could not be
   * fetched (e.g. server forgot the UID between LIST and FETCH).
   * The GPtrArray's free-func is g_bytes_unref. */
  void (*fetch_messages_raw_async) (MailBackend *self,
                                    const char *const *message_ids,
                                    gsize n_ids,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);
  GPtrArray *(*fetch_messages_raw_finish) (MailBackend *self,
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
  /* Change-notification subscribers. Allocated lazily on the first
   * mail_backend_add_listener call; mail_backend_destroy frees the
   * array and invokes any registered GDestroyNotify hooks. */
  GArray *change_listeners; /* of MailBackendListener; lazy */
  guint next_listener_id;
  /* Subclasses extend by embedding MailBackend as their first member. */
};

void mail_backend_destroy (MailBackend *self);

/* Subscribe to change events emitted by @self. Returns a non-zero id
 * for paired removal, or 0 if @self is NULL. @notify (if non-NULL) is
 * called with @user_data on removal or backend destroy. */
guint mail_backend_add_listener (MailBackend *self,
                                 MailBackendChangeCb cb,
                                 gpointer user_data,
                                 GDestroyNotify notify);

void mail_backend_remove_listener (MailBackend *self, guint id);

/* Emit a change event to all current subscribers. Used by
 * MailBackendStore's rebroadcast bridge and by test code (the fake
 * backend exposes a thin wrapper for tests to drive UI updates
 * deterministically). Not used by callers outside backend
 * implementations. */
void mail_backend_emit_change (MailBackend *self,
                               const MailBackendChange *change);

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

/* Batched body fetch. If the backend's vtable slot is NULL the
 * trampoline serialises N fetch_message_raw_async calls and
 * assembles the parallel array — correct, just not faster. */
void mail_backend_fetch_messages_raw_async (MailBackend *self,
                                            const char *const *message_ids,
                                            gsize n_ids,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);
GPtrArray *mail_backend_fetch_messages_raw_finish (MailBackend *self,
                                                   GAsyncResult *result,
                                                   GError **error);

G_END_DECLS
