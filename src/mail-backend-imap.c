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
#include "mail-imap-retry.h"
#include "mail-mime.h"
#include "spool-version.h"

#include <gmime/gmime.h>
#include <goa/goa.h>
#include <libetpan/libetpan.h>
#include <libetpan/mailimap_id.h>
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
  /* OAuth token cache. Refilled inside acquire_token_locked from a
   * synchronous GOA call when missing or near expiry. Sharing the
   * token across many fetches eliminates the per-message DBus hop to
   * org.gnome.OnlineAccounts that used to dominate fetch latency. */
  char *cached_token;
  gint64 cached_token_expires_unix; /* g_get_real_time / G_USEC_PER_SEC */
  GMutex imap_lock;
} MailBackendIMAP;

/* Refresh the cached token unconditionally if its remaining lifetime
 * is below this. Gmail's tokens live ~3600s; 60s of slack keeps a
 * batch from straddling an expiry. */
#define IMAP_TOKEN_REFRESH_SLACK_SECONDS 60

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
  g_free (self->cached_token);
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

/* Returns a borrowed pointer to the cached token (valid until the
 * lock is released). Refreshes via a sync DBus call to GOA when the
 * cache is empty or within the slack window. The token outlives the
 * function call only as long as the caller holds imap_lock; copy if
 * you need it after releasing. */
static const char *
acquire_token_locked (MailBackendIMAP *self,
                      GCancellable *cancellable,
                      GError **error)
{
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  if (self->cached_token != NULL && self->cached_token_expires_unix > now + IMAP_TOKEN_REFRESH_SLACK_SECONDS)
    return self->cached_token;

  g_clear_pointer (&self->cached_token, g_free);
  self->cached_token_expires_unix = 0;

  gchar *token = NULL;
  gint expires_in = 0;
  if (!goa_oauth2_based_call_get_access_token_sync (self->oauth2, &token,
                                                    &expires_in, cancellable,
                                                    error))
    return NULL;
  self->cached_token = token;
  self->cached_token_expires_unix = now + expires_in;
  return self->cached_token;
}

/* Drop the cached token (used after the server rejects auth so the
 * next acquire forces a refresh through GOA). */
static void
invalidate_token_locked (MailBackendIMAP *self)
{
  g_clear_pointer (&self->cached_token, g_free);
  self->cached_token_expires_unix = 0;
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

  /* RFC 2971 ID exchange: best-effort. Advertising ourselves shows up
   * in Gmail's IMAP "Last account activity" log so the user can tell
   * which client connected. Servers that don't support ID (or refuse
   * the command) return NO/BAD, which leaves session state intact —
   * we ignore the result. */
  if (mailimap_has_id (self->imap))
    {
      char *server_name = NULL;
      char *server_version = NULL;
      mailimap_id_basic (self->imap, "Spool", SPOOL_VERSION,
                         &server_name, &server_version);
      free (server_name);
      free (server_version);
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

/* Walk a STATUS response's info list and copy MESSAGES/UNSEEN into
 * the caller's slots. Other attributes (RECENT, UIDNEXT…) are
 * ignored — we don't request them. */
static void
status_extract_counts (struct mailimap_mailbox_data_status *data,
                       int *unread_out,
                       int *total_out)
{
  if (data == NULL || data->st_info_list == NULL)
    return;
  for (clistiter *it = clist_begin (data->st_info_list); it != NULL; it = clist_next (it))
    {
      struct mailimap_status_info *info = clist_content (it);
      if (info == NULL)
        continue;
      if (info->st_att == MAILIMAP_STATUS_ATT_UNSEEN)
        *unread_out = (int) info->st_value;
      else if (info->st_att == MAILIMAP_STATUS_ATT_MESSAGES)
        *total_out = (int) info->st_value;
    }
}

/* Issue STATUS for one mailbox to fill unread/total. A transport-
 * fatal rc propagates upward so run_with_auth_retry can reconnect
 * and retry. A non-transport failure (a quirky server returning NO
 * for one mailbox) logs a warning and returns TRUE with the counts
 * left at their initial zero — the user still gets counts for the
 * other folders rather than the whole list_folders failing. */
static gboolean
fetch_folder_status_locked (MailBackendIMAP *self,
                            const char *mailbox,
                            struct mailimap_status_att_list *att_list,
                            int *unread_out,
                            int *total_out,
                            GError **error)
{
  struct mailimap_mailbox_data_status *result = NULL;
  int rc = mailimap_status (self->imap, mailbox, att_list, &result);
  if (!imap_rc_ok (rc))
    {
      if (is_transport_fatal (rc))
        {
          set_imap_error (error, rc, "STATUS");
          drop_connection_locked (self);
          return FALSE;
        }
      g_warning ("IMAP STATUS failed for \"%s\" (libetpan rc=%d); "
                 "leaving unread/total at 0",
                 mailbox, rc);
      return TRUE;
    }
  status_extract_counts (result, unread_out, total_out);
  mailimap_mailbox_data_status_free (result);
  return TRUE;
}

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

  /* Second pass: ask the server for UNSEEN/MESSAGES per selectable
   * folder. LIST itself doesn't carry counts in IMAP; STATUS does
   * (RFC 3501 §6.3.10), non-destructively, one round-trip per
   * mailbox. The att_list is reused across folders. */
  struct mailimap_status_att_list *att_list = mailimap_status_att_list_new_empty ();
  mailimap_status_att_list_add (att_list, MAILIMAP_STATUS_ATT_MESSAGES);
  mailimap_status_att_list_add (att_list, MAILIMAP_STATUS_ATT_UNSEEN);
  for (guint i = 0; i < out->len; i++)
    {
      MailFolder *f = g_ptr_array_index (out, i);
      if (!fetch_folder_status_locked (self, f->id, att_list,
                                       &f->unread_count, &f->total_count,
                                       error))
        {
          mailimap_status_att_list_free (att_list);
          g_ptr_array_unref (out);
          return NULL;
        }
    }
  mailimap_status_att_list_free (att_list);
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

/* --- worker: fetch_messages_raw (batched UID FETCH) ---------- */

/* For one folder-group: SELECT, verify UIDVALIDITY, UID FETCH the
 * whole UID set with BODY.PEEK[], and store the resulting body
 * GBytes into @out_bodies at the slot indices recorded in
 * @slot_indices (parallel to @uids). Soft misses (UIDs the server
 * forgot) leave the slot as NULL. */
static gboolean
worker_fetch_messages_in_folder (MailBackendIMAP *self,
                                 const char *folder_name,
                                 guint32 expected_uidvalidity,
                                 const guint32 *uids,
                                 const gsize *slot_indices,
                                 gsize n_uids,
                                 GPtrArray *out_bodies,
                                 GError **error)
{
  if (n_uids == 0)
    return TRUE;

  if (!select_mailbox_locked (self, folder_name, error))
    return FALSE;
  if (self->imap->imap_selection_info == NULL || self->imap->imap_selection_info->sel_uidvalidity != expected_uidvalidity)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "UIDVALIDITY changed on the server; re-list required");
      return FALSE;
    }

  struct mailimap_set *set = mailimap_set_new_empty ();
  for (gsize i = 0; i < n_uids; i++)
    mailimap_set_add_single (set, uids[i]);
  struct mailimap_section *section = mailimap_section_new (NULL); /* whole message */
  struct mailimap_fetch_att *att = mailimap_fetch_att_new_body_peek_section (section);
  struct mailimap_fetch_type *ft = mailimap_fetch_type_new_fetch_att (att);

  clist *result = NULL;
  int rc = mailimap_uid_fetch (self->imap, set, ft, &result);
  mailimap_set_free (set);
  mailimap_fetch_type_free (ft);
  if (!imap_rc_ok (rc))
    {
      set_imap_error (error, rc, "UID FETCH (BODY.PEEK[]) batched");
      if (is_transport_fatal (rc))
        drop_connection_locked (self);
      return FALSE;
    }

  /* Index responses by UID — the server is allowed to interleave
   * order, and our @slot_indices map UID positions to output slots. */
  g_autoptr (GHashTable) by_uid = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                         NULL, (GDestroyNotify) g_bytes_unref);
  for (clistiter *it = clist_begin (result); it != NULL; it = clist_next (it))
    {
      struct mailimap_msg_att *att_msg = clist_content (it);
      if (att_msg == NULL || att_msg->att_list == NULL)
        continue;
      guint32 uid = 0;
      GBytes *body = NULL;
      for (clistiter *j = clist_begin (att_msg->att_list); j != NULL; j = clist_next (j))
        {
          struct mailimap_msg_att_item *item = clist_content (j);
          if (item == NULL || item->att_type != MAILIMAP_MSG_ATT_ITEM_STATIC)
            continue;
          struct mailimap_msg_att_static *s = item->att_data.att_static;
          if (s == NULL)
            continue;
          if (s->att_type == MAILIMAP_MSG_ATT_UID)
            uid = s->att_data.att_uid;
          else if (s->att_type == MAILIMAP_MSG_ATT_BODY_SECTION && s->att_data.att_body_section->sec_body_part != NULL)
            body = g_bytes_new (s->att_data.att_body_section->sec_body_part,
                                s->att_data.att_body_section->sec_length);
        }
      if (uid != 0 && body != NULL)
        g_hash_table_insert (by_uid, GUINT_TO_POINTER (uid), body);
      else if (body != NULL)
        g_bytes_unref (body);
    }
  mailimap_fetch_list_free (result);

  /* Place each fetched body into its output slot. Misses stay NULL —
   * sync's apply_fetch_result treats NULL as a soft skip. */
  for (gsize i = 0; i < n_uids; i++)
    {
      GBytes *body = g_hash_table_lookup (by_uid, GUINT_TO_POINTER (uids[i]));
      if (body != NULL)
        {
          g_ptr_array_index (out_bodies, slot_indices[i]) = g_bytes_ref (body);
        }
    }
  return TRUE;
}

static GPtrArray *
worker_fetch_messages_raw (MailBackendIMAP *self,
                           const char *const *ids,
                           gsize n_ids,
                           GError **error)
{
  GPtrArray *bodies = g_ptr_array_new_full (n_ids, (GDestroyNotify) g_bytes_unref);
  g_ptr_array_set_size (bodies, n_ids); /* NULL slots until filled */

  /* Group ids by folder. PHASE_LISTS appends fetches folder-by-
   * folder, so most batches share a folder; cross-folder splits
   * cost at most one extra SELECT inside the loop. We process the
   * groups in order of first appearance. */
  g_autoptr (GHashTable) groups = g_hash_table_new_full (
      g_str_hash, g_str_equal,
      g_free, /* folder name copy owns the key */
      NULL    /* values are GArrays owned by parallel storage below */
  );
  GPtrArray *group_folders = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *group_uid_arrays = g_ptr_array_new_with_free_func ((GDestroyNotify) g_array_unref);
  GPtrArray *group_slot_arrays = g_ptr_array_new_with_free_func ((GDestroyNotify) g_array_unref);
  GArray *group_uidvalidities = g_array_new (FALSE, FALSE, sizeof (guint32));

  gboolean ok = TRUE;
  for (gsize i = 0; i < n_ids; i++)
    {
      guint32 uidvalidity = 0, uid = 0;
      const char *folder_name = NULL;
      if (!mail_imap_id_decode (ids[i], &uidvalidity, &uid, &folder_name))
        {
          /* Malformed id: leave the slot NULL — apply_fetch_result
           * surfaces it as a g_warning. The rest of the batch
           * proceeds. */
          g_warning ("mail-backend-imap: malformed message id '%s' in batch", ids[i]);
          continue;
        }

      gpointer existing_idx = NULL;
      GArray *uid_arr;
      GArray *slot_arr;
      if (g_hash_table_lookup_extended (groups, folder_name, NULL, &existing_idx))
        {
          gsize idx = GPOINTER_TO_SIZE (existing_idx) - 1; /* 0 means "absent" */
          uid_arr = g_ptr_array_index (group_uid_arrays, idx);
          slot_arr = g_ptr_array_index (group_slot_arrays, idx);
        }
      else
        {
          char *key_copy = g_strdup (folder_name);
          g_ptr_array_add (group_folders, key_copy);
          uid_arr = g_array_new (FALSE, FALSE, sizeof (guint32));
          slot_arr = g_array_new (FALSE, FALSE, sizeof (gsize));
          g_ptr_array_add (group_uid_arrays, uid_arr);
          g_ptr_array_add (group_slot_arrays, slot_arr);
          g_array_append_val (group_uidvalidities, uidvalidity);
          g_hash_table_insert (groups, g_strdup (folder_name),
                               GSIZE_TO_POINTER (group_folders->len));
        }
      g_array_append_val (uid_arr, uid);
      g_array_append_val (slot_arr, i);
    }

  for (guint g = 0; g < group_folders->len && ok; g++)
    {
      const char *folder = g_ptr_array_index (group_folders, g);
      GArray *uid_arr = g_ptr_array_index (group_uid_arrays, g);
      GArray *slot_arr = g_ptr_array_index (group_slot_arrays, g);
      guint32 vu = g_array_index (group_uidvalidities, guint32, g);
      ok = worker_fetch_messages_in_folder (self, folder, vu,
                                            (const guint32 *) uid_arr->data,
                                            (const gsize *) slot_arr->data,
                                            uid_arr->len, bodies, error);
    }

  g_ptr_array_unref (group_folders);
  g_ptr_array_unref (group_uid_arrays);
  g_ptr_array_unref (group_slot_arrays);
  g_array_unref (group_uidvalidities);

  if (!ok)
    {
      g_ptr_array_unref (bodies);
      return NULL;
    }
  return bodies;
}

/* --- async glue: thread + result ------------------------------ */

typedef enum
{
  IMAP_OP_LIST_FOLDERS,
  IMAP_OP_LIST_MESSAGES,
  IMAP_OP_FETCH_RAW,
  IMAP_OP_FETCH_BATCH,
} ImapOp;

typedef struct
{
  ImapOp op;
  /* list_messages */
  char *folder_id;
  int top_n;
  /* fetch_message_raw */
  guint32 fetch_uidvalidity;
  guint32 fetch_uid;
  char *fetch_folder_name;
  /* fetch_messages_raw (batched) */
  char **batch_ids; /* g_strdup'd; NULL-terminated array of n_batch ids */
  gsize n_batch;
} ImapJob;

static void
imap_job_free (gpointer p)
{
  ImapJob *j = p;
  if (j == NULL)
    return;
  g_free (j->folder_id);
  g_free (j->fetch_folder_name);
  if (j->batch_ids != NULL)
    g_strfreev (j->batch_ids);
  g_free (j);
}

/* Forward decl — the batch worker is defined below the per-message
 * helpers, but worker_thread dispatches into it. */
static GPtrArray *worker_fetch_messages_raw (MailBackendIMAP *self,
                                             const char *const *ids,
                                             gsize n_ids,
                                             GError **error);

/* Run an op under the lock, with a one-shot retry if the failure
 * looks like a stale token: invalidate, drop the connection (so the
 * next pass re-authenticates), and re-run. Returns whatever the op
 * returned (typed via the union below). */
static gboolean
run_with_auth_retry (MailBackendIMAP *self,
                     ImapJob *job,
                     GCancellable *cancellable,
                     GError **error,
                     gpointer *out_result /* out, op-specific */)
{
  for (int attempt = 0; attempt < 2; attempt++)
    {
      g_clear_error (error);

      const char *token = acquire_token_locked (self, cancellable, error);
      if (token == NULL)
        return FALSE;

      if (!ensure_connected_locked (self, token, error))
        {
          /* Auth or transport failure on connect/AUTHENTICATE.
           * ensure_connected_locked has already dropped the
           * connection on its own; we just decide whether to retry.
           * Auth: refresh the token. Transport: token is fine, the
           * next attempt's ensure_connected_locked rebuilds the
           * socket. */
          if (attempt == 0)
            {
              ImapRetryAction action = imap_retry_action_for_error (*error);
              if (action == IMAP_RETRY_AUTH)
                {
                  invalidate_token_locked (self);
                  continue;
                }
              if (action == IMAP_RETRY_TRANSPORT)
                continue;
            }
          return FALSE;
        }

      gpointer result = NULL;
      switch (job->op)
        {
        case IMAP_OP_LIST_FOLDERS:
          result = worker_list_folders (self, error);
          break;
        case IMAP_OP_LIST_MESSAGES:
          result = worker_list_messages (self, job->folder_id, job->top_n, error);
          break;
        case IMAP_OP_FETCH_RAW:
          result = worker_fetch_message_raw (self, job->fetch_uidvalidity,
                                             job->fetch_uid,
                                             job->fetch_folder_name, error);
          break;
        case IMAP_OP_FETCH_BATCH:
          result = worker_fetch_messages_raw (self,
                                              (const char *const *) job->batch_ids,
                                              job->n_batch, error);
          break;
        }

      if (result != NULL)
        {
          *out_result = result;
          return TRUE;
        }
      if (attempt == 0)
        {
          ImapRetryAction action = imap_retry_action_for_error (*error);
          if (action == IMAP_RETRY_AUTH)
            {
              invalidate_token_locked (self);
              drop_connection_locked (self);
              continue;
            }
          if (action == IMAP_RETRY_TRANSPORT)
            {
              /* Worker already dropped the connection on a
               * transport-fatal rc; call drop_connection_locked
               * again defensively (it's idempotent) in case a
               * future op type forgets. Token is still valid. */
              drop_connection_locked (self);
              continue;
            }
        }
      return FALSE;
    }
  return FALSE;
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
  gpointer result = NULL;

  g_mutex_lock (&self->imap_lock);
  gboolean ok = run_with_auth_retry (self, job, cancellable, &error, &result);
  g_mutex_unlock (&self->imap_lock);

  if (!ok)
    {
      g_task_return_error (task, error);
      return;
    }

  GDestroyNotify free_fn = NULL;
  switch (job->op)
    {
    case IMAP_OP_LIST_FOLDERS:
    case IMAP_OP_LIST_MESSAGES:
    case IMAP_OP_FETCH_BATCH:
      free_fn = (GDestroyNotify) g_ptr_array_unref;
      break;
    case IMAP_OP_FETCH_RAW:
      free_fn = (GDestroyNotify) g_bytes_unref;
      break;
    }
  g_task_return_pointer (task, result, free_fn);
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
  g_task_run_in_thread (task, worker_thread);
  g_object_unref (task);
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
  g_task_run_in_thread (task, worker_thread);
  g_object_unref (task);
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
  g_task_run_in_thread (task, worker_thread);
  g_object_unref (task);
}

static GBytes *
mb_imap_fetch_message_raw_finish (MailBackend *base,
                                  GAsyncResult *result,
                                  GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* --- vtable: fetch_messages_raw (batched) -------------------- */

static void
mb_imap_fetch_messages_raw_async (MailBackend *base,
                                  const char *const *message_ids,
                                  gsize n_ids,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  MailBackendIMAP *self = (MailBackendIMAP *) base;
  /* Same arena rule as the per-message fetch: the UI still holds
   * MailMessageMeta pointers from the preceding list_messages, so
   * we must not reset the arena here. */
  g_byte_array_set_size (self->base.response_buf, 0);

  ImapJob *job = g_new0 (ImapJob, 1);
  job->op = IMAP_OP_FETCH_BATCH;
  job->batch_ids = g_new0 (char *, n_ids + 1);
  for (gsize i = 0; i < n_ids; i++)
    job->batch_ids[i] = g_strdup (message_ids[i]);
  job->n_batch = n_ids;
  GTask *task = new_imap_task (self, job, cancellable, callback, user_data);
  g_task_run_in_thread (task, worker_thread);
  g_object_unref (task);
}

static GPtrArray *
mb_imap_fetch_messages_raw_finish (MailBackend *base,
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
  .fetch_messages_raw_async = mb_imap_fetch_messages_raw_async,
  .fetch_messages_raw_finish = mb_imap_fetch_messages_raw_finish,
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
