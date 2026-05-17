/* mail-backend-imap.c - IMAP backend via libetpan + XOAUTH2.
 *
 * libetpan is synchronous; every IMAP method body runs in a worker
 * thread via g_task_run_in_thread() so the GTK main loop never blocks
 * on the network. A per-backend GMutex serialises all libetpan calls
 * on the single persistent connection.
 *
 * Auth: per-call GoaOAuth2Based access token (GOA refreshes on its
 * own), fed to mailimap_oauth2_authenticate() which frames SASL
 * XOAUTH2 for us. Works against Gmail's imap.gmail.com:993.
 *
 * Identifiers: list_messages returns IMAP-flavoured composite ids of
 * the form <UIDVALIDITY>\x01<UID>\x01<folder_name> via mail-imap-id;
 * fetch_message_raw decodes that, SELECTs the folder, verifies
 * UIDVALIDITY matches (so the row still refers to the message the
 * meta described — otherwise we error and the sync engine re-lists),
 * and FETCH UID BODY.PEEK[]s the raw RFC822 bytes.
 *
 * Dedup: ENVELOPE includes env_message_id (RFC 5322 §3.6.4
 * Message-ID), which is the value the sync engine uses as
 * content_key. Gmail preserves Message-ID byte-for-byte across every
 * label, so the same message in INBOX and [Gmail]/All Mail dedups to
 * a single body fetch.
 */

#include "config.h"

#include "mail-backend-imap.h"
#include "mail-imap-id.h"
#include "mail-mime.h"

#include <gmime/gmime.h>
#include <goa/goa.h>
#include <libetpan/libetpan.h>
#include <string.h>

typedef struct
{
  MailBackend base;
  GoaObject *goa_object;  /* ref'd; also serves as GTask source */
  GoaOAuth2Based *oauth2; /* borrowed from goa_object */
  mailimap *imap;         /* lazily constructed; protected by imap_lock */
  char *host;
  int port;
  gboolean use_ssl;
  gboolean use_tls;
  char *username;
  char *selected_mailbox; /* g_strdup'd; tracks libetpan's current SELECT */
  GMutex imap_lock;
} MailBackendIMAP;

/* --- destroy ---------------------------------------------------- */

static void
mb_imap_destroy (MailBackend *base)
{
  MailBackendIMAP *self = (MailBackendIMAP *) base;
  if (self->imap != NULL)
    {
      mailimap_logout (self->imap);
      mailimap_free (self->imap);
    }
  g_mutex_clear (&self->imap_lock);
  g_free (self->host);
  g_free (self->username);
  g_free (self->selected_mailbox);
  if (self->goa_object != NULL)
    g_object_unref (self->goa_object);
  if (self->base.response_buf != NULL)
    g_byte_array_unref (self->base.response_buf);
  if (self->base.path_buf != NULL)
    g_string_free (self->base.path_buf, TRUE);
  mail_arena_destroy (&self->base.fetch_arena);
  g_free (self);
}

/* --- arena/buffer reset (mirrors mail-backend-msgraph) --------- */

static void
reset_arena_and_buffers (MailBackendIMAP *self)
{
  mail_arena_reset (&self->base.fetch_arena);
  g_byte_array_set_size (self->base.response_buf, 0);
  g_string_truncate (self->base.path_buf, 0);
}

/* --- connection / auth (worker thread, mutex held) -------------- */

/* libetpan signals "operation succeeded" with three different codes:
 *   0 NO_ERROR                       — generic success
 *   1 NO_ERROR_AUTHENTICATED         — succeeded; session is auth'd
 *   2 NO_ERROR_NON_AUTHENTICATED     — succeeded; session is not auth'd
 *     (this is what mailimap_ssl_connect returns on a fresh socket)
 * Everything else is a real error. */
static gboolean
imap_rc_ok (int rc)
{
  return rc == MAILIMAP_NO_ERROR || rc == MAILIMAP_NO_ERROR_AUTHENTICATED || rc == MAILIMAP_NO_ERROR_NON_AUTHENTICATED;
}

/* Map a libetpan return code to a GError. Stream / parse errors smell
 * transport-fatal; the caller drops the connection on those. */
static void
set_imap_error (GError **error, int rc, const char *what)
{
  GIOErrorEnum code = G_IO_ERROR_FAILED;
  if (rc == MAILIMAP_ERROR_STREAM || rc == MAILIMAP_ERROR_PARSE || rc == MAILIMAP_ERROR_CONNECTION_REFUSED)
    code = G_IO_ERROR_BROKEN_PIPE;
  else if (rc == MAILIMAP_ERROR_LOGIN || rc == MAILIMAP_ERROR_BAD_STATE)
    code = G_IO_ERROR_PERMISSION_DENIED;
  g_set_error (error, G_IO_ERROR, code,
               "IMAP %s failed (libetpan rc=%d)", what, rc);
}

static gboolean
is_transport_fatal (int rc)
{
  return rc == MAILIMAP_ERROR_STREAM || rc == MAILIMAP_ERROR_PARSE || rc == MAILIMAP_ERROR_CONNECTION_REFUSED || rc == MAILIMAP_ERROR_BAD_STATE;
}

static void
drop_connection_locked (MailBackendIMAP *self)
{
  if (self->imap != NULL)
    {
      mailimap_free (self->imap);
      self->imap = NULL;
    }
  g_clear_pointer (&self->selected_mailbox, g_free);
}

static gboolean
ensure_connected_locked (MailBackendIMAP *self,
                         const char *access_token,
                         GError **error)
{
  if (self->imap != NULL)
    return TRUE;

  self->imap = mailimap_new (0, NULL);
  if (self->imap == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "mailimap_new returned NULL");
      return FALSE;
    }

  int rc;
  if (self->use_ssl)
    rc = mailimap_ssl_connect (self->imap, self->host, (uint16_t) self->port);
  else
    rc = mailimap_socket_connect (self->imap, self->host, (uint16_t) self->port);
  if (!imap_rc_ok (rc))
    {
      set_imap_error (error, rc, "connect");
      drop_connection_locked (self);
      return FALSE;
    }

  if (!self->use_ssl && self->use_tls)
    {
      rc = mailimap_socket_starttls (self->imap);
      if (!imap_rc_ok (rc))
        {
          set_imap_error (error, rc, "STARTTLS");
          drop_connection_locked (self);
          return FALSE;
        }
    }

  rc = mailimap_oauth2_authenticate (self->imap, self->username, access_token);
  if (!imap_rc_ok (rc))
    {
      set_imap_error (error, rc, "XOAUTH2");
      drop_connection_locked (self);
      return FALSE;
    }
  return TRUE;
}

static gboolean
select_mailbox_locked (MailBackendIMAP *self,
                       const char *mailbox,
                       GError **error)
{
  if (g_strcmp0 (self->selected_mailbox, mailbox) == 0)
    return TRUE;
  int rc = mailimap_select (self->imap, mailbox);
  if (!imap_rc_ok (rc))
    {
      set_imap_error (error, rc, "SELECT");
      if (is_transport_fatal (rc))
        drop_connection_locked (self);
      return FALSE;
    }
  g_free (self->selected_mailbox);
  self->selected_mailbox = g_strdup (mailbox);
  return TRUE;
}

/* --- folder helpers ------------------------------------------- */

static gboolean
mailbox_is_noselect (const struct mailimap_mailbox_list *mb)
{
  if (mb == NULL || mb->mb_flag == NULL)
    return FALSE;
  return mb->mb_flag->mbf_sflag == MAILIMAP_MBX_LIST_SFLAG_NOSELECT;
}

/* Map a raw IMAP mailbox name to a friendlier display name. */
static char *
display_name_for (const char *raw, char delim)
{
  if (raw == NULL)
    return g_strdup ("(unnamed)");
  if (g_ascii_strcasecmp (raw, "INBOX") == 0)
    return g_strdup ("Inbox");
  const char *p = raw;
  /* Strip a Gmail-style "[Gmail]/" or "[Google Mail]/" prefix. */
  if (raw[0] == '[')
    {
      const char *close = strchr (raw, ']');
      if (close != NULL && delim != '\0' && close[1] == delim)
        p = close + 2;
    }
  if (delim != '\0')
    {
      const char *last = strrchr (p, delim);
      if (last != NULL)
        p = last + 1;
    }
  return g_strdup (p[0] != '\0' ? p : raw);
}

/* --- ENVELOPE → MailMessageMeta helpers ----------------------- */

static char *
arena_decode_header_text (MailArena *arena, const char *raw)
{
  if (raw == NULL)
    return NULL;
  char *decoded = g_mime_utils_header_decode_text (NULL, raw);
  if (decoded == NULL)
    return mail_arena_strdup (arena, raw);
  char *out = mail_arena_strdup (arena, decoded);
  g_free (decoded);
  return out;
}

static char *
arena_format_first_address (MailArena *arena, struct mailimap_env_from *env_from)
{
  if (env_from == NULL || env_from->frm_list == NULL)
    return NULL;
  clistiter *it = clist_begin (env_from->frm_list);
  if (it == NULL)
    return NULL;
  struct mailimap_address *a = clist_content (it);
  if (a == NULL)
    return NULL;
  const char *personal = a->ad_personal_name;
  const char *mailbox = a->ad_mailbox_name;
  const char *host = a->ad_host_name;

  if (mailbox == NULL && host == NULL)
    return personal != NULL ? arena_decode_header_text (arena, personal) : NULL;

  g_autofree char *decoded_personal = NULL;
  if (personal != NULL)
    decoded_personal = g_mime_utils_header_decode_text (NULL, personal);

  g_autofree char *combined = NULL;
  if (decoded_personal != NULL && decoded_personal[0] != '\0')
    combined = g_strdup_printf ("%s <%s@%s>", decoded_personal,
                                mailbox != NULL ? mailbox : "",
                                host != NULL ? host : "");
  else
    combined = g_strdup_printf ("%s@%s",
                                mailbox != NULL ? mailbox : "",
                                host != NULL ? host : "");
  return mail_arena_strdup (arena, combined);
}

static gint64
parse_envelope_date (const char *raw)
{
  if (raw == NULL)
    return 0;
  GDateTime *dt = g_mime_utils_header_decode_date (raw);
  if (dt == NULL)
    return 0;
  gint64 unix_secs = g_date_time_to_unix (dt);
  g_date_time_unref (dt);
  return unix_secs;
}

static gboolean
flags_contain_seen (struct mailimap_msg_att_dynamic *dyn)
{
  if (dyn == NULL || dyn->att_list == NULL)
    return FALSE;
  for (clistiter *i = clist_begin (dyn->att_list); i != NULL; i = clist_next (i))
    {
      struct mailimap_flag_fetch *ff = clist_content (i);
      if (ff == NULL || ff->fl_flag == NULL)
        continue;
      if (ff->fl_flag->fl_type == MAILIMAP_FLAG_SEEN)
        return TRUE;
    }
  return FALSE;
}

/* --- worker: list_folders ------------------------------------- */

static GPtrArray *
worker_list_folders (MailBackendIMAP *self, GError **error)
{
  clist *result = NULL;
  int rc = mailimap_list (self->imap, "", "*", &result);
  if (!imap_rc_ok (rc))
    {
      set_imap_error (error, rc, "LIST");
      if (is_transport_fatal (rc))
        drop_connection_locked (self);
      return NULL;
    }

  GPtrArray *out = g_ptr_array_new ();
  for (clistiter *it = clist_begin (result); it != NULL; it = clist_next (it))
    {
      struct mailimap_mailbox_list *mb = clist_content (it);
      if (mb == NULL || mailbox_is_noselect (mb))
        continue;
      MailFolder *f = mail_arena_alloc (&self->base.fetch_arena, sizeof *f,
                                        _Alignof (MailFolder));
      f->id = mail_arena_strdup (&self->base.fetch_arena, mb->mb_name);
      g_autofree char *display = display_name_for (mb->mb_name, mb->mb_delimiter);
      f->display_name = mail_arena_strdup (&self->base.fetch_arena, display);
      f->parent_id = NULL;
      f->unread_count = 0;
      f->total_count = 0;
      g_ptr_array_add (out, f);
    }
  mailimap_list_result_free (result);
  return out;
}

/* --- worker: list_messages ------------------------------------ */

/* Build a fetch type asking for UID, ENVELOPE, FLAGS. The fetch_att
 * adders take ownership of their argument, so we must not free the
 * individual atts after handing them over. */
static struct mailimap_fetch_type *
build_envelope_fetch_type (void)
{
  struct mailimap_fetch_type *ft = mailimap_fetch_type_new_fetch_att_list_empty ();
  mailimap_fetch_type_new_fetch_att_list_add (ft, mailimap_fetch_att_new_uid ());
  mailimap_fetch_type_new_fetch_att_list_add (ft, mailimap_fetch_att_new_envelope ());
  mailimap_fetch_type_new_fetch_att_list_add (ft, mailimap_fetch_att_new_flags ());
  return ft;
}

typedef struct
{
  guint32 uid;
  struct mailimap_envelope *env; /* borrowed from the libetpan response */
  gboolean seen;
} MsgRow;

static void
extract_msg_row (struct mailimap_msg_att *att, MsgRow *out)
{
  out->uid = 0;
  out->env = NULL;
  out->seen = FALSE;
  if (att == NULL || att->att_list == NULL)
    return;
  for (clistiter *it = clist_begin (att->att_list); it != NULL; it = clist_next (it))
    {
      struct mailimap_msg_att_item *item = clist_content (it);
      if (item == NULL)
        continue;
      if (item->att_type == MAILIMAP_MSG_ATT_ITEM_DYNAMIC)
        out->seen = flags_contain_seen (item->att_data.att_dyn);
      else if (item->att_type == MAILIMAP_MSG_ATT_ITEM_STATIC)
        {
          struct mailimap_msg_att_static *s = item->att_data.att_static;
          if (s == NULL)
            continue;
          if (s->att_type == MAILIMAP_MSG_ATT_UID)
            out->uid = s->att_data.att_uid;
          else if (s->att_type == MAILIMAP_MSG_ATT_ENVELOPE)
            out->env = s->att_data.att_env;
        }
    }
}

static GPtrArray *
worker_list_messages (MailBackendIMAP *self,
                      const char *folder_id,
                      int top_n,
                      GError **error)
{
  if (!select_mailbox_locked (self, folder_id, error))
    return NULL;

  guint32 uidvalidity = 0;
  if (self->imap->imap_selection_info != NULL)
    uidvalidity = self->imap->imap_selection_info->sel_uidvalidity;

  /* 1:* — interval(1, 0) is libetpan's idiom for "highest". */
  struct mailimap_set *set = mailimap_set_new_interval (1, 0);
  struct mailimap_fetch_type *ft = build_envelope_fetch_type ();

  clist *result = NULL;
  int rc = mailimap_uid_fetch (self->imap, set, ft, &result);
  mailimap_set_free (set);
  mailimap_fetch_type_free (ft);
  if (!imap_rc_ok (rc))
    {
      set_imap_error (error, rc, "UID FETCH (ENVELOPE FLAGS)");
      if (is_transport_fatal (rc))
        drop_connection_locked (self);
      return NULL;
    }

  GPtrArray *out = g_ptr_array_new ();
  for (clistiter *it = clist_begin (result); it != NULL; it = clist_next (it))
    {
      struct mailimap_msg_att *att = clist_content (it);
      MsgRow row;
      extract_msg_row (att, &row);
      if (row.uid == 0 || row.env == NULL)
        continue;

      MailMessageMeta *m = mail_arena_alloc (&self->base.fetch_arena, sizeof *m,
                                             _Alignof (MailMessageMeta));
      g_autofree char *id = mail_imap_id_encode (uidvalidity, row.uid, folder_id);
      m->id = mail_arena_strdup (&self->base.fetch_arena, id);
      m->subject = arena_decode_header_text (&self->base.fetch_arena, row.env->env_subject);
      m->from = arena_format_first_address (&self->base.fetch_arena, row.env->env_from);
      m->received_unix = parse_envelope_date (row.env->env_date);
      m->unread = !row.seen;
      m->content_key = row.env->env_message_id != NULL
                           ? mail_arena_strdup (&self->base.fetch_arena, row.env->env_message_id)
                           : NULL;
      g_ptr_array_add (out, m);
    }
  mailimap_fetch_list_free (result);

  /* Truncate to top_n by sequence tail (newest first). For G_MAXINT
   * (the sync engine's "paginate to completion" sentinel) this is a
   * no-op. */
  if (top_n > 0 && (guint) top_n < out->len)
    {
      guint drop = out->len - (guint) top_n;
      /* The response is in ascending sequence/UID order; the newest
       * messages are at the end. Drop from the front. */
      g_ptr_array_remove_range (out, 0, drop);
    }

  return out;
}

/* --- worker: fetch_message_raw -------------------------------- */

static GBytes *
worker_fetch_message_raw (MailBackendIMAP *self,
                          guint32 uidvalidity_expected,
                          guint32 uid,
                          const char *folder_name,
                          GError **error)
{
  if (!select_mailbox_locked (self, folder_name, error))
    return NULL;

  if (self->imap->imap_selection_info == NULL || self->imap->imap_selection_info->sel_uidvalidity != uidvalidity_expected)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "UIDVALIDITY changed on the server; re-list required");
      return NULL;
    }

  struct mailimap_set *set = mailimap_set_new_single (uid);
  struct mailimap_section *section = mailimap_section_new (NULL); /* whole message */
  struct mailimap_fetch_att *att = mailimap_fetch_att_new_body_peek_section (section);
  struct mailimap_fetch_type *ft = mailimap_fetch_type_new_fetch_att (att);

  clist *result = NULL;
  int rc = mailimap_uid_fetch (self->imap, set, ft, &result);
  mailimap_set_free (set);
  mailimap_fetch_type_free (ft);
  if (!imap_rc_ok (rc))
    {
      set_imap_error (error, rc, "UID FETCH (BODY.PEEK[])");
      if (is_transport_fatal (rc))
        drop_connection_locked (self);
      return NULL;
    }

  GBytes *bytes = NULL;
  for (clistiter *it = clist_begin (result); it != NULL && bytes == NULL; it = clist_next (it))
    {
      struct mailimap_msg_att *m = clist_content (it);
      if (m == NULL || m->att_list == NULL)
        continue;
      for (clistiter *j = clist_begin (m->att_list); j != NULL && bytes == NULL; j = clist_next (j))
        {
          struct mailimap_msg_att_item *item = clist_content (j);
          if (item == NULL || item->att_type != MAILIMAP_MSG_ATT_ITEM_STATIC)
            continue;
          struct mailimap_msg_att_static *s = item->att_data.att_static;
          if (s == NULL || s->att_type != MAILIMAP_MSG_ATT_BODY_SECTION)
            continue;
          const char *body = s->att_data.att_body_section->sec_body_part;
          gsize len = s->att_data.att_body_section->sec_length;
          if (body != NULL)
            bytes = g_bytes_new (body, len);
        }
    }
  mailimap_fetch_list_free (result);

  if (bytes == NULL && (error == NULL || *error == NULL))
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "IMAP FETCH BODY.PEEK[] returned no body section");
  return bytes;
}

/* --- async glue: token + thread + result ---------------------- */

typedef enum
{
  IMAP_OP_LIST_FOLDERS,
  IMAP_OP_LIST_MESSAGES,
  IMAP_OP_FETCH_RAW,
} ImapOp;

typedef struct
{
  ImapOp op;
  char *access_token;
  /* list_messages */
  char *folder_id;
  int top_n;
  /* fetch_message_raw */
  guint32 fetch_uidvalidity;
  guint32 fetch_uid;
  char *fetch_folder_name;
} ImapJob;

static void
imap_job_free (gpointer p)
{
  ImapJob *j = p;
  if (j == NULL)
    return;
  g_free (j->access_token);
  g_free (j->folder_id);
  g_free (j->fetch_folder_name);
  g_free (j);
}

static void
worker_thread (GTask *task,
               gpointer source,
               gpointer task_data,
               GCancellable *cancellable)
{
  MailBackendIMAP *self = g_task_get_source_tag (task);
  ImapJob *job = task_data;
  GError *error = NULL;

  g_mutex_lock (&self->imap_lock);

  if (!ensure_connected_locked (self, job->access_token, &error))
    {
      g_mutex_unlock (&self->imap_lock);
      g_task_return_error (task, error);
      return;
    }

  switch (job->op)
    {
    case IMAP_OP_LIST_FOLDERS:
      {
        GPtrArray *folders = worker_list_folders (self, &error);
        g_mutex_unlock (&self->imap_lock);
        if (folders == NULL)
          g_task_return_error (task, error);
        else
          g_task_return_pointer (task, folders, (GDestroyNotify) g_ptr_array_unref);
        return;
      }
    case IMAP_OP_LIST_MESSAGES:
      {
        GPtrArray *messages = worker_list_messages (self, job->folder_id, job->top_n, &error);
        g_mutex_unlock (&self->imap_lock);
        if (messages == NULL)
          g_task_return_error (task, error);
        else
          g_task_return_pointer (task, messages, (GDestroyNotify) g_ptr_array_unref);
        return;
      }
    case IMAP_OP_FETCH_RAW:
      {
        GBytes *bytes = worker_fetch_message_raw (self, job->fetch_uidvalidity,
                                                  job->fetch_uid, job->fetch_folder_name,
                                                  &error);
        g_mutex_unlock (&self->imap_lock);
        if (bytes == NULL)
          g_task_return_error (task, error);
        else
          g_task_return_pointer (task, bytes, (GDestroyNotify) g_bytes_unref);
        return;
      }
    }
  g_mutex_unlock (&self->imap_lock);
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "unknown IMAP op");
}

static void
on_token_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GTask *task = user_data;
  MailBackendIMAP *self = g_task_get_source_tag (task);
  ImapJob *job = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;
  gchar *access_token = NULL;
  gint expires_in = 0;
  if (!goa_oauth2_based_call_get_access_token_finish (self->oauth2, &access_token,
                                                      &expires_in, result, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }
  job->access_token = access_token; /* takes ownership; freed in imap_job_free */
  g_task_run_in_thread (task, worker_thread);
  g_object_unref (task);
}

static GTask *
new_imap_task (MailBackendIMAP *self,
               ImapJob *job,
               GCancellable *cancellable,
               GAsyncReadyCallback callback,
               gpointer user_data)
{
  GTask *task = g_task_new (self->goa_object, cancellable, callback, user_data);
  /* The backend is the source tag (not the source object — that's
   * the GOA object so the task survives a backend destroy mid-flight
   * via the ref the task already holds). */
  g_task_set_source_tag (task, self);
  g_task_set_task_data (task, job, imap_job_free);
  return task;
}

/* --- vtable: list_folders ------------------------------------- */

static void
mb_imap_list_folders_async (MailBackend *base,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
  MailBackendIMAP *self = (MailBackendIMAP *) base;
  reset_arena_and_buffers (self);

  ImapJob *job = g_new0 (ImapJob, 1);
  job->op = IMAP_OP_LIST_FOLDERS;
  GTask *task = new_imap_task (self, job, cancellable, callback, user_data);
  goa_oauth2_based_call_get_access_token (self->oauth2, cancellable,
                                          on_token_ready, task);
}

static GPtrArray *
mb_imap_list_folders_finish (MailBackend *base,
                             GAsyncResult *result,
                             GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- vtable: list_messages ------------------------------------ */

static void
mb_imap_list_messages_async (MailBackend *base,
                             const char *folder_id,
                             int top_n,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
  MailBackendIMAP *self = (MailBackendIMAP *) base;
  reset_arena_and_buffers (self);

  ImapJob *job = g_new0 (ImapJob, 1);
  job->op = IMAP_OP_LIST_MESSAGES;
  job->folder_id = g_strdup (folder_id);
  job->top_n = top_n;
  GTask *task = new_imap_task (self, job, cancellable, callback, user_data);
  goa_oauth2_based_call_get_access_token (self->oauth2, cancellable,
                                          on_token_ready, task);
}

static GPtrArray *
mb_imap_list_messages_finish (MailBackend *base,
                              GAsyncResult *result,
                              GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- vtable: fetch_message_raw -------------------------------- */

static void
mb_imap_fetch_message_raw_async (MailBackend *base,
                                 const char *message_id,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  MailBackendIMAP *self = (MailBackendIMAP *) base;
  /* fetch_message_raw must NOT reset the arena (the UI still holds
   * MailMessageMeta pointers from the preceding list_messages). */
  g_byte_array_set_size (self->base.response_buf, 0);

  guint32 uidvalidity = 0, uid = 0;
  const char *folder_name = NULL;
  if (!mail_imap_id_decode (message_id, &uidvalidity, &uid, &folder_name))
    {
      GTask *task = g_task_new (self->goa_object, cancellable, callback, user_data);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                               "malformed IMAP message id");
      g_object_unref (task);
      return;
    }

  ImapJob *job = g_new0 (ImapJob, 1);
  job->op = IMAP_OP_FETCH_RAW;
  job->fetch_uidvalidity = uidvalidity;
  job->fetch_uid = uid;
  job->fetch_folder_name = g_strdup (folder_name);
  GTask *task = new_imap_task (self, job, cancellable, callback, user_data);
  goa_oauth2_based_call_get_access_token (self->oauth2, cancellable,
                                          on_token_ready, task);
}

static GBytes *
mb_imap_fetch_message_raw_finish (MailBackend *base,
                                  GAsyncResult *result,
                                  GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- vtable + constructor ------------------------------------- */

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

  GoaOAuth2Based *oauth2 = goa_object_peek_oauth2_based (goa_object);
  if (oauth2 == NULL)
    return NULL; /* IMAP w/o XOAUTH2 isn't supported in this initial step */

  /* The worker thread uses GMime to decode ENVELOPE header text;
   * GMime crashes if any of its utilities is called before init. */
  mail_mime_ensure_init ();

  MailBackendIMAP *self = g_new0 (MailBackendIMAP, 1);
  self->base.vt = &imap_vt;
  mail_arena_init (&self->base.fetch_arena, 4096);
  self->base.response_buf = g_byte_array_new ();
  self->base.path_buf = g_string_sized_new (256);
  self->goa_object = g_object_ref (goa_object);
  self->oauth2 = oauth2;
  self->host = g_strdup (host);
  self->username = g_strdup (user);
  self->use_ssl = goa_mail_get_imap_use_ssl (mail);
  self->use_tls = goa_mail_get_imap_use_tls (mail);
  self->port = self->use_ssl ? 993 : 143;
  g_mutex_init (&self->imap_lock);
  return (MailBackend *) self;
}
