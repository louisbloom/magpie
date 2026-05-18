/* mail-backend-msgraph.c - Microsoft Graph mail backend.
 *
 * Strategy:
 *   - OAuth2 access token comes from GoaOAuth2Based; we re-fetch on
 *     every call (GOA caches & refreshes for us).
 *   - libsoup-3 issues GET requests against graph.microsoft.com.
 *   - json-glib parses the response body in-place from response_buf.
 *
 * Memory: all MailFolder / MailMessageMeta structs + their strings
 * live in the parent MailBackend's arena, which we reset on every
 * async entry. The HTTP response body lives in the persistent
 * response_buf (GByteArray) — see [[feedback-memory-reuse]].
 *
 * Note: GTask source is the GoaObject (a GObject) so the task can
 * outlive the backend struct without dereferencing freed memory.
 */

#include "config.h"

#include "mail-backend-msgraph.h"

#include <goa/goa.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#define MSGRAPH_BASE "https://graph.microsoft.com/v1.0"

/* Graph's documented per-page maximum for /me/mailFolders/{id}/messages
 * is 1000; anything larger is silently capped server-side. We always
 * request a full page and let the pagination loop walk @odata.nextLink
 * until the caller's total cap is hit or the link is absent. */
#define MSGRAPH_MESSAGES_PAGE_SIZE 1000

typedef struct
{
  MailBackend base;
  GoaObject *goa_object;  /* ref'd; also serves as GTask source */
  GoaOAuth2Based *oauth2; /* borrowed from goa_object */
  SoupSession *session;   /* owned */
} MailBackendMSGraph;

static void
mb_msgraph_destroy (MailBackend *base)
{
  MailBackendMSGraph *self = (MailBackendMSGraph *) base;
  if (self->session != NULL)
    g_object_unref (self->session);
  if (self->goa_object != NULL)
    g_object_unref (self->goa_object);
  if (self->base.response_buf != NULL)
    g_byte_array_unref (self->base.response_buf);
  if (self->base.path_buf != NULL)
    g_string_free (self->base.path_buf, TRUE);
  mail_arena_destroy (&self->base.fetch_arena);
  g_free (self);
}

/* --- shared URL/HTTP helpers ----------------------------------- */

/* Reset only the per-call HTTP scratch space. Safe for any operation. */
static void
reset_buffers (MailBackendMSGraph *self)
{
  g_byte_array_set_size (self->base.response_buf, 0);
  g_string_truncate (self->base.path_buf, 0);
}

/* Reset the arena AND the buffers. Only safe at the start of an
 * operation that *replaces* arena-borrowed structs (list_folders,
 * list_messages). fetch_message_raw must NOT call this, because the
 * message-list pane still holds MailMessageMeta pointers from the
 * preceding list_messages call. */
static void
reset_arena_and_buffers (MailBackendMSGraph *self)
{
  mail_arena_reset (&self->base.fetch_arena);
  reset_buffers (self);
}

static SoupMessage *
build_get (const char *url,
           const char *access_token)
{
  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  char bearer[2048];
  g_snprintf (bearer, sizeof bearer, "Bearer %s", access_token);
  soup_message_headers_append (headers, "Authorization", bearer);
  soup_message_headers_append (headers, "Accept", "application/json");
  return msg;
}

/* --- list_folders ---------------------------------------------- */

typedef struct
{
  char *url; /* current page URL; g_strdup */
  /* Accumulator state for paged responses: we keep a running GPtrArray
   * of MailFolder* pointing into the arena, growing across @odata.nextLink. */
  GPtrArray *folders; /* MailFolder* (arena-borrowed) */
} ListFoldersJob;

static void
list_folders_job_free (gpointer p)
{
  ListFoldersJob *job = p;
  g_free (job->url);
  if (job->folders != NULL)
    g_ptr_array_unref (job->folders);
  g_free (job);
}

static gboolean
parse_folders_page (MailBackendMSGraph *self,
                    GBytes *body,
                    GPtrArray *out_folders,
                    char **out_next_link,
                    GError **error)
{
  gsize len = 0;
  const char *data = g_bytes_get_data (body, &len);
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, (gssize) len, error))
    return FALSE;

  JsonNode *root_node = json_parser_get_root (parser);
  if (root_node == NULL || JSON_NODE_TYPE (root_node) != JSON_NODE_OBJECT)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Graph response is not an object");
      return FALSE;
    }
  JsonObject *root = json_node_get_object (root_node);

  if (json_object_has_member (root, "error"))
    {
      JsonObject *err = json_object_get_object_member (root, "error");
      const char *code = json_object_has_member (err, "code") ? json_object_get_string_member (err, "code") : "unknown";
      const char *msg = json_object_has_member (err, "message") ? json_object_get_string_member (err, "message") : "";
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Graph error %s: %s", code, msg);
      return FALSE;
    }

  if (!json_object_has_member (root, "value"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Graph response missing 'value'");
      return FALSE;
    }
  JsonArray *value = json_object_get_array_member (root, "value");
  guint n = json_array_get_length (value);
  for (guint i = 0; i < n; i++)
    {
      JsonObject *o = json_array_get_object_element (value, i);
      if (json_object_has_member (o, "isHidden") && json_object_get_boolean_member (o, "isHidden"))
        continue;

      MailFolder *folder = mail_arena_alloc (&self->base.fetch_arena, sizeof *folder, _Alignof (MailFolder));
      folder->id = json_object_has_member (o, "id") ? mail_arena_strdup (&self->base.fetch_arena, json_object_get_string_member (o, "id")) : NULL;
      folder->display_name = json_object_has_member (o, "displayName") ? mail_arena_strdup (&self->base.fetch_arena, json_object_get_string_member (o, "displayName")) : NULL;
      folder->parent_id = json_object_has_member (o, "parentFolderId") ? mail_arena_strdup (&self->base.fetch_arena, json_object_get_string_member (o, "parentFolderId")) : NULL;
      folder->unread_count = json_object_has_member (o, "unreadItemCount") ? (int) json_object_get_int_member (o, "unreadItemCount") : 0;
      folder->total_count = json_object_has_member (o, "totalItemCount") ? (int) json_object_get_int_member (o, "totalItemCount") : 0;
      g_ptr_array_add (out_folders, folder);
    }

  *out_next_link = NULL;
  if (json_object_has_member (root, "@odata.nextLink"))
    *out_next_link = g_strdup (json_object_get_string_member (root, "@odata.nextLink"));

  return TRUE;
}

static void
issue_folders_request (GTask *task);

static void
on_folders_response (GObject *source_obj,
                     GAsyncResult *result,
                     gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  SoupSession *session = SOUP_SESSION (source_obj);
  MailBackendMSGraph *self = g_task_get_task_data (task);
  ListFoldersJob *job = (ListFoldersJob *) g_object_get_data (G_OBJECT (task), "job");
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) body = soup_session_send_and_read_finish (session, result, &error);
  if (body == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }

  /* Optional: surface non-200 statuses via the body's parsed error.code. */
  char *next_link = NULL;
  if (!parse_folders_page (self, body, job->folders, &next_link, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }

  if (next_link != NULL)
    {
      g_free (job->url);
      job->url = next_link;
      issue_folders_request (task);
      return;
    }

  g_task_return_pointer (task, g_ptr_array_ref (job->folders),
                         (GDestroyNotify) g_ptr_array_unref);
  g_object_unref (task);
}

static void
issue_folders_request (GTask *task)
{
  MailBackendMSGraph *self = g_task_get_task_data (task);
  ListFoldersJob *job = g_object_get_data (G_OBJECT (task), "job");
  const char *token = g_object_get_data (G_OBJECT (task), "access-token");

  g_autoptr (SoupMessage) msg = build_get (job->url, token);
  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT,
                                    g_task_get_cancellable (task),
                                    on_folders_response, task);
}

static void
on_token_for_list_folders (GObject *source_obj,
                           GAsyncResult *result,
                           gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  MailBackendMSGraph *self = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;
  gchar *access_token = NULL;
  gint expires_in = 0;

  if (!goa_oauth2_based_call_get_access_token_finish (self->oauth2, &access_token, &expires_in, result, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }

  g_object_set_data_full (G_OBJECT (task), "access-token", access_token, g_free);
  issue_folders_request (task);
}

static void
mb_msgraph_list_folders_async (MailBackend *base,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
  MailBackendMSGraph *self = (MailBackendMSGraph *) base;
  reset_arena_and_buffers (self);

  GTask *task = g_task_new (self->goa_object, cancellable, callback, user_data);
  g_task_set_task_data (task, self, NULL); /* borrowed; backend outlives task because goa_object is ref'd by task */

  ListFoldersJob *job = g_new0 (ListFoldersJob, 1);
  job->url = g_strdup (MSGRAPH_BASE "/me/mailFolders?$top=100&includeHiddenFolders=true");
  job->folders = g_ptr_array_new ();
  g_object_set_data_full (G_OBJECT (task), "job", job, list_folders_job_free);

  goa_oauth2_based_call_get_access_token (self->oauth2, cancellable,
                                          on_token_for_list_folders, task);
}

static GPtrArray *
mb_msgraph_list_folders_finish (MailBackend *base,
                                GAsyncResult *result,
                                GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- list_messages --------------------------------------------- */

static char *
format_from (JsonObject *msg_obj,
             MailArena *arena)
{
  if (!json_object_has_member (msg_obj, "from"))
    return NULL;
  JsonNode *from_node = json_object_get_member (msg_obj, "from");
  if (from_node == NULL || JSON_NODE_TYPE (from_node) != JSON_NODE_OBJECT)
    return NULL;
  JsonObject *from = json_node_get_object (from_node);
  if (!json_object_has_member (from, "emailAddress"))
    return NULL;
  JsonObject *email = json_object_get_object_member (from, "emailAddress");
  const char *name = json_object_has_member (email, "name") ? json_object_get_string_member (email, "name") : NULL;
  const char *addr = json_object_has_member (email, "address") ? json_object_get_string_member (email, "address") : NULL;

  if (name == NULL && addr == NULL)
    return NULL;
  if (name != NULL && addr != NULL && !g_str_equal (name, addr))
    {
      g_autofree char *combined = g_strdup_printf ("%s <%s>", name, addr);
      return mail_arena_strdup (arena, combined);
    }
  return mail_arena_strdup (arena, name != NULL ? name : addr);
}

static gboolean
parse_messages_page (MailBackendMSGraph *self,
                     GBytes *body,
                     GPtrArray *out_messages,
                     char **out_next_link,
                     GError **error)
{
  gsize len = 0;
  const char *data = g_bytes_get_data (body, &len);
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, (gssize) len, error))
    return FALSE;

  JsonNode *root_node = json_parser_get_root (parser);
  if (root_node == NULL || JSON_NODE_TYPE (root_node) != JSON_NODE_OBJECT)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Graph response is not an object");
      return FALSE;
    }
  JsonObject *root = json_node_get_object (root_node);
  if (json_object_has_member (root, "error"))
    {
      JsonObject *err = json_object_get_object_member (root, "error");
      const char *code = json_object_has_member (err, "code") ? json_object_get_string_member (err, "code") : "unknown";
      const char *msg = json_object_has_member (err, "message") ? json_object_get_string_member (err, "message") : "";
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Graph error %s: %s", code, msg);
      return FALSE;
    }
  if (!json_object_has_member (root, "value"))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "Graph response missing 'value'");
      return FALSE;
    }
  JsonArray *value = json_object_get_array_member (root, "value");
  guint n = json_array_get_length (value);
  for (guint i = 0; i < n; i++)
    {
      JsonObject *o = json_array_get_object_element (value, i);
      MailMessageMeta *m = mail_arena_alloc (&self->base.fetch_arena, sizeof *m, _Alignof (MailMessageMeta));
      m->id = json_object_has_member (o, "id") ? mail_arena_strdup (&self->base.fetch_arena, json_object_get_string_member (o, "id")) : NULL;
      m->subject = json_object_has_member (o, "subject") ? mail_arena_strdup (&self->base.fetch_arena, json_object_get_string_member (o, "subject")) : NULL;
      m->from = format_from (o, &self->base.fetch_arena);
      m->unread = json_object_has_member (o, "isRead") ? !json_object_get_boolean_member (o, "isRead") : FALSE;
      m->content_key = json_object_has_member (o, "internetMessageId")
                           ? mail_arena_strdup (&self->base.fetch_arena, json_object_get_string_member (o, "internetMessageId"))
                           : NULL;
      m->received_unix = 0;
      if (json_object_has_member (o, "receivedDateTime"))
        {
          const char *iso = json_object_get_string_member (o, "receivedDateTime");
          g_autoptr (GDateTime) dt = g_date_time_new_from_iso8601 (iso, NULL);
          if (dt != NULL)
            m->received_unix = g_date_time_to_unix (dt);
        }
      g_ptr_array_add (out_messages, m);
    }

  *out_next_link = NULL;
  if (json_object_has_member (root, "@odata.nextLink"))
    *out_next_link = g_strdup (json_object_get_string_member (root, "@odata.nextLink"));
  return TRUE;
}

typedef struct
{
  char *url;
  GPtrArray *messages;
  int top_n;
  int accumulated;
} ListMessagesJob;

static void
list_messages_job_free (gpointer p)
{
  ListMessagesJob *job = p;
  g_free (job->url);
  if (job->messages != NULL)
    g_ptr_array_unref (job->messages);
  g_free (job);
}

static void
issue_messages_request (GTask *task);

static void
on_messages_response (GObject *source_obj,
                      GAsyncResult *result,
                      gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  MailBackendMSGraph *self = g_task_get_task_data (task);
  ListMessagesJob *job = g_object_get_data (G_OBJECT (task), "job");
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (source_obj), result, &error);
  if (body == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }

  char *next_link = NULL;
  if (!parse_messages_page (self, body, job->messages, &next_link, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }

  if (next_link != NULL && (int) job->messages->len < job->top_n)
    {
      g_free (job->url);
      job->url = next_link;
      issue_messages_request (task);
      return;
    }
  g_free (next_link);

  /* Truncate if we overshot. */
  if ((int) job->messages->len > job->top_n)
    g_ptr_array_set_size (job->messages, job->top_n);

  g_task_return_pointer (task, g_ptr_array_ref (job->messages),
                         (GDestroyNotify) g_ptr_array_unref);
  g_object_unref (task);
}

static void
issue_messages_request (GTask *task)
{
  MailBackendMSGraph *self = g_task_get_task_data (task);
  ListMessagesJob *job = g_object_get_data (G_OBJECT (task), "job");
  const char *token = g_object_get_data (G_OBJECT (task), "access-token");
  g_autoptr (SoupMessage) msg = build_get (job->url, token);
  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT,
                                    g_task_get_cancellable (task),
                                    on_messages_response, task);
}

static void
on_token_for_list_messages (GObject *source_obj,
                            GAsyncResult *result,
                            gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  MailBackendMSGraph *self = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;
  gchar *access_token = NULL;
  gint expires_in = 0;

  if (!goa_oauth2_based_call_get_access_token_finish (self->oauth2, &access_token, &expires_in, result, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }
  g_object_set_data_full (G_OBJECT (task), "access-token", access_token, g_free);
  issue_messages_request (task);
}

static void
mb_msgraph_list_messages_async (MailBackend *base,
                                const char *folder_id,
                                int top_n,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
  MailBackendMSGraph *self = (MailBackendMSGraph *) base;
  reset_arena_and_buffers (self);

  GTask *task = g_task_new (self->goa_object, cancellable, callback, user_data);
  g_task_set_task_data (task, self, NULL);

  g_autofree char *escaped = g_uri_escape_string (folder_id, NULL, FALSE);
  ListMessagesJob *job = g_new0 (ListMessagesJob, 1);
  /* Always request a full Graph page; the caller's cap (top_n) is
   * enforced by the pagination loop in on_messages_response. */
  job->url = g_strdup_printf (MSGRAPH_BASE "/me/mailFolders/%s/messages?$top=%d&$orderby=receivedDateTime%%20desc&$select=id,subject,from,receivedDateTime,isRead,internetMessageId",
                              escaped, MSGRAPH_MESSAGES_PAGE_SIZE);
  job->messages = g_ptr_array_new ();
  job->top_n = top_n;
  g_object_set_data_full (G_OBJECT (task), "job", job, list_messages_job_free);

  goa_oauth2_based_call_get_access_token (self->oauth2, cancellable,
                                          on_token_for_list_messages, task);
}

static GPtrArray *
mb_msgraph_list_messages_finish (MailBackend *base,
                                 GAsyncResult *result,
                                 GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- fetch_message_raw ----------------------------------------- */

static void
on_raw_response (GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  g_autoptr (GError) error = NULL;

  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (source), result, &error);
  if (body == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }
  g_task_return_pointer (task, g_bytes_ref (body), (GDestroyNotify) g_bytes_unref);
  g_object_unref (task);
}

static void
on_token_for_fetch_raw (GObject *source,
                        GAsyncResult *result,
                        gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  MailBackendMSGraph *self = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;
  gchar *access_token = NULL;
  gint expires_in = 0;

  if (!goa_oauth2_based_call_get_access_token_finish (self->oauth2, &access_token, &expires_in, result, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }

  const char *url = g_object_get_data (G_OBJECT (task), "url");
  g_autoptr (SoupMessage) msg = build_get (url, access_token);
  /* $value returns RFC822 text; ask for it explicitly. */
  SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
  soup_message_headers_replace (headers, "Accept", "text/plain");
  g_free (access_token);
  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT,
                                    g_task_get_cancellable (task),
                                    on_raw_response, task);
}

static void
mb_msgraph_fetch_message_raw_async (MailBackend *base,
                                    const char *message_id,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
  MailBackendMSGraph *self = (MailBackendMSGraph *) base;
  /* Critical: do NOT reset the arena here. message-list rows still
   * hold MailMessageMeta pointers into it from the prior list_messages
   * call; resetting would dangle them and crash the row factory the
   * next time it runs. */
  reset_buffers (self);

  GTask *task = g_task_new (self->goa_object, cancellable, callback, user_data);
  g_task_set_task_data (task, self, NULL);

  g_autofree char *escaped = g_uri_escape_string (message_id, NULL, FALSE);
  char *url = g_strdup_printf (MSGRAPH_BASE "/me/messages/%s/$value", escaped);
  g_object_set_data_full (G_OBJECT (task), "url", url, g_free);

  goa_oauth2_based_call_get_access_token (self->oauth2, cancellable,
                                          on_token_for_fetch_raw, task);
}

static GBytes *
mb_msgraph_fetch_message_raw_finish (MailBackend *base,
                                     GAsyncResult *result,
                                     GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static const MailBackendVTable msgraph_vt = {
  .list_folders_async = mb_msgraph_list_folders_async,
  .list_folders_finish = mb_msgraph_list_folders_finish,
  .list_messages_async = mb_msgraph_list_messages_async,
  .list_messages_finish = mb_msgraph_list_messages_finish,
  .fetch_message_raw_async = mb_msgraph_fetch_message_raw_async,
  .fetch_message_raw_finish = mb_msgraph_fetch_message_raw_finish,
  .destroy = mb_msgraph_destroy,
};

MailBackend *
mail_backend_msgraph_new (GoaObject *goa_object)
{
  g_return_val_if_fail (GOA_IS_OBJECT (goa_object), NULL);

  GoaOAuth2Based *oauth2 = goa_object_peek_oauth2_based (goa_object);
  if (oauth2 == NULL)
    return NULL;

  MailBackendMSGraph *self = g_new0 (MailBackendMSGraph, 1);
  self->base.vt = &msgraph_vt;
  mail_arena_init (&self->base.fetch_arena, 4096);
  self->base.response_buf = g_byte_array_new ();
  self->base.path_buf = g_string_sized_new (256);
  self->goa_object = g_object_ref (goa_object);
  self->oauth2 = oauth2;
  self->session = soup_session_new ();
  soup_session_set_user_agent (self->session, "Magpie/0 ");
  return (MailBackend *) self;
}
