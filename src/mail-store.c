/* mail-store.c - sqlite + Maildir helpers for the local store.
 *
 * Schema is created on first open and tagged via PRAGMA user_version
 * so a future migration can be added incrementally. The stable_id used
 * as primary key in folders/messages is SHA-1(identity ":" remote_id)
 * truncated to 16 hex chars — short enough to be cheap in indexes,
 * long enough that the per-account collision probability is negligible.
 */

#include "config.h"

#include "mail-store.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAIL_STORE_SCHEMA_VERSION 3

/* Stamp the sqlite file header so `file(1)` and forensic tooling can
 * identify a stray state.db as Magpie's. Encoded as the fourcc 'Mgpi'
 * (M=0x4D, g=0x67, p=0x70, i=0x69 -> 0x4D677069). */
#define MAIL_STORE_APPLICATION_ID 0x4D677069

struct _MailStore
{
  char *root;     /* ~/Mail/<identity> */
  char *identity; /* used to derive stable_ids */
  char *hostname; /* cached for maildir filenames */
  sqlite3 *db;

  /* Hot prepared statements. */
  sqlite3_stmt *st_folder_select_by_remote;
  sqlite3_stmt *st_folder_dir_in_use;
  sqlite3_stmt *st_folder_insert;
  sqlite3_stmt *st_folder_update;
  sqlite3_stmt *st_folder_list;
  sqlite3_stmt *st_folder_remote_ids;
  sqlite3_stmt *st_folder_dir_name;
  sqlite3_stmt *st_folder_delete;

  sqlite3_stmt *st_message_upsert;
  sqlite3_stmt *st_message_list;
  sqlite3_stmt *st_message_remote_ids;
  sqlite3_stmt *st_message_location;
  sqlite3_stmt *st_message_location_by_content_key;
  sqlite3_stmt *st_message_delete;
  sqlite3_stmt *st_message_set_unread;
  sqlite3_stmt *st_folder_counts;

  /* Scratch buffer used by upsert_folder to hand back a borrowed
   * pointer to the chosen dir_name (valid until the next upsert). */
  char *last_dir_name;

  /* Monotonic counter for maildir filename uniqueness within a usec. */
  guint64 filename_counter;

  /* Change-notification subscribers. Fan-out is synchronous on the
   * caller's thread (always the GLib default main context in practice). */
  GArray *listeners; /* of StoreListener */
  guint next_listener_id;
};

typedef struct
{
  guint id;
  MailStoreChangeCb cb;
  gpointer user_data;
  GDestroyNotify notify;
} StoreListener;

/* --- internal helpers --------------------------------------------- */

static void
set_sqlite_error (GError **error,
                  sqlite3 *db,
                  const char *what)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s: %s", what, sqlite3_errmsg (db));
}

static gboolean
exec_sql (sqlite3 *db,
          const char *sql,
          GError **error)
{
  char *err = NULL;
  if (sqlite3_exec (db, sql, NULL, NULL, &err) != SQLITE_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "sqlite: %s", err);
      sqlite3_free (err);
      return FALSE;
    }
  return TRUE;
}

static sqlite3_stmt *
prepare (sqlite3 *db,
         const char *sql,
         GError **error)
{
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) != SQLITE_OK)
    {
      set_sqlite_error (error, db, "prepare");
      return NULL;
    }
  return st;
}

/* SHA-1(identity ":" remote_id) truncated to 16 hex chars. */
static char *
stable_id (const char *identity,
           const char *remote_id)
{
  GChecksum *c = g_checksum_new (G_CHECKSUM_SHA1);
  g_checksum_update (c, (const guchar *) identity, -1);
  g_checksum_update (c, (const guchar *) ":", 1);
  g_checksum_update (c, (const guchar *) remote_id, -1);
  const char *hex = g_checksum_get_string (c);
  char *out = g_strndup (hex, 16);
  g_checksum_free (c);
  return out;
}

static gboolean
mkdir_p_0700 (const char *path,
              GError **error)
{
  if (g_mkdir_with_parents (path, 0700) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "mkdir %s: %s", path, g_strerror (errno));
      return FALSE;
    }
  return TRUE;
}

static gboolean
create_maildir_subdirs (const char *folder_path,
                        GError **error)
{
  const char *subs[] = { "cur", "new", "tmp" };
  for (gsize i = 0; i < G_N_ELEMENTS (subs); i++)
    {
      char *sub = g_build_filename (folder_path, subs[i], NULL);
      gboolean ok = mkdir_p_0700 (sub, error);
      g_free (sub);
      if (!ok)
        return FALSE;
    }
  return TRUE;
}

/* Sanitize a folder display name for use as a directory name:
 *  - replace '/' and NUL with '_'
 *  - empty -> "_"
 * Maildir++-style nesting via '.' is handled by the caller already
 * baking parent prefixes into display_name; for v1 we don't compute
 * that hierarchy and just use the display_name verbatim. */
static char *
sanitize_dir_name (const char *display_name)
{
  if (display_name == NULL || *display_name == '\0')
    return g_strdup ("_");
  GString *s = g_string_new (NULL);
  for (const char *p = display_name; *p; p++)
    {
      if (*p == '/')
        g_string_append_c (s, '_');
      else
        g_string_append_c (s, *p);
    }
  return g_string_free (s, FALSE);
}

/* --- dir_name collision resolution ------------------------------- */

static gboolean
dir_name_in_use (MailStore *self,
                 const char *dir_name,
                 const char *exclude_stable_id,
                 GError **error)
{
  sqlite3_stmt *st = self->st_folder_dir_in_use;
  sqlite3_reset (st);
  sqlite3_bind_text (st, 1, dir_name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (st, 2, exclude_stable_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step (st);
  if (rc == SQLITE_ROW)
    return TRUE;
  if (rc != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "dir_name_in_use");
      return TRUE;
    }
  return FALSE;
}

static char *
choose_dir_name (MailStore *self,
                 const char *base,
                 const char *exclude_stable_id,
                 GError **error)
{
  char *candidate = g_strdup (base);
  guint suffix = 2;
  while (dir_name_in_use (self, candidate, exclude_stable_id, error))
    {
      if (error != NULL && *error != NULL)
        {
          g_free (candidate);
          return NULL;
        }
      g_free (candidate);
      candidate = g_strdup_printf ("%s~%u", base, suffix++);
    }
  return candidate;
}

/* --- schema setup ------------------------------------------------ */

static gboolean
ensure_schema (MailStore *self,
               GError **error)
{
  if (!exec_sql (self->db, "PRAGMA foreign_keys = ON;", error))
    return FALSE;
  if (!exec_sql (self->db, "PRAGMA journal_mode = WAL;", error))
    return FALSE;

  /* Tag the file header with MAIL_STORE_APPLICATION_ID. Idempotent at
   * the sqlite level (same value re-written is a no-op page write). */
  char *app_pragma = g_strdup_printf ("PRAGMA application_id = %u;",
                                      MAIL_STORE_APPLICATION_ID);
  gboolean app_ok = exec_sql (self->db, app_pragma, error);
  g_free (app_pragma);
  if (!app_ok)
    return FALSE;

  /* Probe user_version first. 0 = brand new; 1 = pre-content_key
   * store that needs an ALTER TABLE; 2 = current version, no schema
   * work needed below. */
  sqlite3_stmt *probe = NULL;
  if (sqlite3_prepare_v2 (self->db, "PRAGMA user_version;", -1, &probe, NULL) != SQLITE_OK)
    {
      set_sqlite_error (error, self->db, "prepare user_version probe");
      return FALSE;
    }
  int existing_version = 0;
  if (sqlite3_step (probe) == SQLITE_ROW)
    existing_version = sqlite3_column_int (probe, 0);
  sqlite3_finalize (probe);

  if (existing_version == MAIL_STORE_SCHEMA_VERSION)
    return TRUE;

  /* Base tables — content_key is part of the messages schema for
   * fresh stores. For an existing v1 store the CREATE TABLE IF NOT
   * EXISTS is a no-op, and we'll add the column via ALTER below.
   *
   * folders.unread/total are kept in the schema for backward
   * compatibility with v2 stores (sqlite < 3.35 lacks DROP COLUMN
   * and we require ≥ 3.24). They are not bound on insert/update or
   * read on select — mail_store_list_folders recomputes both via a
   * live COUNT against the messages table. The DEFAULT 0 from the
   * CREATE matches the legacy contents. messages.flags is dead weight
   * for the same reasons. */
  const char *base_ddl =
      "CREATE TABLE IF NOT EXISTS folders ("
      "  stable_id        TEXT PRIMARY KEY,"
      "  remote_id        TEXT NOT NULL UNIQUE,"
      "  display_name     TEXT NOT NULL,"
      "  dir_name         TEXT NOT NULL UNIQUE,"
      "  parent_remote_id TEXT,"
      "  unread           INTEGER NOT NULL DEFAULT 0,"
      "  total            INTEGER NOT NULL DEFAULT 0"
      ");"
      "CREATE TABLE IF NOT EXISTS messages ("
      "  stable_id        TEXT PRIMARY KEY,"
      "  folder_stable_id TEXT NOT NULL REFERENCES folders(stable_id) ON DELETE CASCADE,"
      "  remote_id        TEXT NOT NULL UNIQUE,"
      "  filename         TEXT NOT NULL,"
      "  subject          TEXT,"
      "  from_addr        TEXT,"
      "  received_unix    INTEGER NOT NULL DEFAULT 0,"
      "  unread           INTEGER NOT NULL DEFAULT 0,"
      "  flags            TEXT,"
      "  content_key      TEXT"
      ");"
      "CREATE INDEX IF NOT EXISTS messages_folder_received"
      "  ON messages (folder_stable_id, received_unix DESC);";
  if (!exec_sql (self->db, base_ddl, error))
    return FALSE;

  if (existing_version == 1)
    {
      /* v1 store: messages table exists without content_key. */
      if (!exec_sql (self->db, "ALTER TABLE messages ADD COLUMN content_key TEXT;", error))
        return FALSE;
    }

  /* The sync_state table (defined as a placeholder for future
   * delta-token sync in v1/v2 but never read or written by any code
   * path) is dropped in v3. Idempotent: a no-op for fresh stores. */
  if (!exec_sql (self->db, "DROP TABLE IF EXISTS sync_state;", error))
    return FALSE;

  /* content_key index, created here (after the column definitely
   * exists) so the path is identical for fresh stores (v=0) and
   * migrated stores (v=1). */
  if (!exec_sql (self->db,
                 "CREATE INDEX IF NOT EXISTS messages_content_key"
                 "  ON messages (content_key) WHERE content_key IS NOT NULL;",
                 error))
    return FALSE;

  char *pragma = g_strdup_printf ("PRAGMA user_version = %d;",
                                  MAIL_STORE_SCHEMA_VERSION);
  gboolean ok = exec_sql (self->db, pragma, error);
  g_free (pragma);
  return ok;
}

static gboolean
prepare_statements (MailStore *self,
                    GError **error)
{
  struct
  {
    sqlite3_stmt **out;
    const char *sql;
  } table[] = {
    { &self->st_folder_select_by_remote,
      "SELECT stable_id, dir_name FROM folders WHERE remote_id = ?;" },
    { &self->st_folder_dir_in_use,
      "SELECT 1 FROM folders WHERE dir_name = ? AND stable_id != ? LIMIT 1;" },
    { &self->st_folder_insert,
      /* unread/total left at the schema DEFAULT 0; the read path
       * recomputes them live via COUNT against messages. */
      "INSERT INTO folders"
      " (stable_id, remote_id, display_name, dir_name, parent_remote_id)"
      " VALUES (?, ?, ?, ?, ?);" },
    { &self->st_folder_update,
      "UPDATE folders SET display_name = ?, parent_remote_id = ?"
      " WHERE stable_id = ?;" },
    { &self->st_folder_list,
      /* unread and total are derived live from the messages table
       * rather than read from folders.unread / folders.total.
       * Backends populate those cached columns from server-reported
       * counts (Graph's unreadItemCount / IMAP STATUS), so after a
       * cancelled sync they advertise messages the local store
       * doesn't actually have. The COUNT-from-messages form keeps
       * the sidebar in sync with what the message list view can
       * actually show. LEFT JOIN preserves empty folders as 0/0;
       * COUNT(CASE ...) only counts unread rows; the
       * messages_folder_received index covers the per-folder scan. */
      "SELECT f.remote_id, f.display_name, f.parent_remote_id,"
      "       COUNT(CASE WHEN m.unread = 1 THEN 1 END) AS unread,"
      "       COUNT(m.stable_id)                       AS total"
      " FROM folders f"
      " LEFT JOIN messages m ON m.folder_stable_id = f.stable_id"
      " GROUP BY f.stable_id"
      " ORDER BY f.display_name;" },
    { &self->st_folder_remote_ids,
      "SELECT remote_id FROM folders;" },
    { &self->st_folder_dir_name,
      "SELECT dir_name FROM folders WHERE remote_id = ?;" },
    { &self->st_folder_delete,
      "DELETE FROM folders WHERE remote_id = ?;" },
    { &self->st_message_upsert,
      /* flags column left at NULL (schema DEFAULT). The Maildir info
       * suffix in `filename` is the source of truth for per-message
       * flags; sqlite carries only the unread bit for cheap COUNTs. */
      "INSERT INTO messages"
      " (stable_id, folder_stable_id, remote_id, filename, subject, from_addr, received_unix, unread, content_key)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
      " ON CONFLICT(stable_id) DO UPDATE SET"
      "   folder_stable_id = excluded.folder_stable_id,"
      "   filename = excluded.filename,"
      "   subject = excluded.subject,"
      "   from_addr = excluded.from_addr,"
      "   received_unix = excluded.received_unix,"
      "   unread = excluded.unread,"
      "   content_key = COALESCE(excluded.content_key, content_key);" },
    { &self->st_message_list,
      "SELECT m.remote_id, m.subject, m.from_addr, m.received_unix, m.unread"
      " FROM messages m"
      " JOIN folders f ON f.stable_id = m.folder_stable_id"
      " WHERE f.remote_id = ?"
      " ORDER BY m.received_unix DESC"
      " LIMIT ?;" },
    { &self->st_message_remote_ids,
      "SELECT m.remote_id"
      " FROM messages m"
      " JOIN folders f ON f.stable_id = m.folder_stable_id"
      " WHERE f.remote_id = ?;" },
    { &self->st_message_location,
      /* col 2 (folder remote_id) is read by mail_store_set_message_unread
       * so it can name the affected folder in the FOLDER_COUNTS event;
       * mail_store_message_location only reads cols 0/1. */
      "SELECT f.dir_name, m.filename, f.remote_id"
      " FROM messages m"
      " JOIN folders f ON f.stable_id = m.folder_stable_id"
      " WHERE m.remote_id = ?;" },
    { &self->st_message_location_by_content_key,
      "SELECT f.dir_name, m.filename"
      " FROM messages m"
      " JOIN folders f ON f.stable_id = m.folder_stable_id"
      " WHERE m.content_key = ?"
      " LIMIT 1;" },
    { &self->st_message_delete,
      "SELECT f.dir_name, m.filename FROM messages m"
      " JOIN folders f ON f.stable_id = m.folder_stable_id"
      " WHERE m.remote_id = ?;" },
    { &self->st_message_set_unread,
      "UPDATE messages SET filename = ?, unread = ? WHERE remote_id = ?;" },
    { &self->st_folder_counts,
      /* Per-folder absolute counts. Same shape as st_folder_list but
       * for a single folder; the messages_folder_received index
       * covers the per-folder LEFT JOIN. */
      "SELECT COUNT(CASE WHEN m.unread = 1 THEN 1 END),"
      "       COUNT(m.stable_id)"
      " FROM folders f"
      " LEFT JOIN messages m ON m.folder_stable_id = f.stable_id"
      " WHERE f.remote_id = ?"
      " GROUP BY f.stable_id;" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (table); i++)
    {
      *table[i].out = prepare (self->db, table[i].sql, error);
      if (*table[i].out == NULL)
        return FALSE;
    }
  return TRUE;
}

/* --- open / close ------------------------------------------------ */

MailStore *
mail_store_open (const char *account_root,
                 const char *identity,
                 GError **error)
{
  g_return_val_if_fail (account_root != NULL, NULL);
  g_return_val_if_fail (identity != NULL, NULL);

  if (!mkdir_p_0700 (account_root, error))
    return NULL;

  MailStore *self = g_new0 (MailStore, 1);
  self->root = g_strdup (account_root);
  self->identity = g_strdup (identity);
  self->listeners = g_array_new (FALSE, FALSE, sizeof (StoreListener));
  char hbuf[256];
  if (gethostname (hbuf, sizeof hbuf) != 0)
    g_strlcpy (hbuf, "localhost", sizeof hbuf);
  hbuf[sizeof hbuf - 1] = '\0';
  /* Maildir filenames may not contain '/' or ':'. */
  for (char *p = hbuf; *p; p++)
    if (*p == '/' || *p == ':')
      *p = '_';
  self->hostname = g_strdup (hbuf);

  char *dbpath = g_build_filename (account_root, "state.db", NULL);
  int rc = sqlite3_open (dbpath, &self->db);
  g_free (dbpath);
  if (rc != SQLITE_OK)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "sqlite open: %s",
                   self->db != NULL ? sqlite3_errmsg (self->db) : "out of memory");
      mail_store_close (self);
      return NULL;
    }

  if (!ensure_schema (self, error) || !prepare_statements (self, error))
    {
      mail_store_close (self);
      return NULL;
    }
  return self;
}

void
mail_store_close (MailStore *self)
{
  if (self == NULL)
    return;
  sqlite3_stmt **all[] = {
    &self->st_folder_select_by_remote,
    &self->st_folder_dir_in_use,
    &self->st_folder_insert,
    &self->st_folder_update,
    &self->st_folder_list,
    &self->st_folder_remote_ids,
    &self->st_folder_dir_name,
    &self->st_folder_delete,
    &self->st_message_upsert,
    &self->st_message_list,
    &self->st_message_remote_ids,
    &self->st_message_location,
    &self->st_message_location_by_content_key,
    &self->st_message_delete,
    &self->st_message_set_unread,
    &self->st_folder_counts,
  };
  for (gsize i = 0; i < G_N_ELEMENTS (all); i++)
    if (*all[i] != NULL)
      sqlite3_finalize (*all[i]);
  if (self->db != NULL)
    sqlite3_close (self->db);
  if (self->listeners != NULL)
    {
      for (guint i = 0; i < self->listeners->len; i++)
        {
          StoreListener *l = &g_array_index (self->listeners, StoreListener, i);
          if (l->notify != NULL)
            l->notify (l->user_data);
        }
      g_array_free (self->listeners, TRUE);
    }
  g_free (self->root);
  g_free (self->identity);
  g_free (self->hostname);
  g_free (self->last_dir_name);
  g_free (self);
}

/* --- change-notification registry --------------------------------- */

guint
mail_store_add_listener (MailStore *self,
                         MailStoreChangeCb cb,
                         gpointer user_data,
                         GDestroyNotify notify)
{
  g_return_val_if_fail (self != NULL, 0);
  g_return_val_if_fail (cb != NULL, 0);

  /* Skip 0 so the caller can use it as a sentinel for "no subscription". */
  if (self->next_listener_id == 0)
    self->next_listener_id = 1;
  StoreListener l = {
    .id = self->next_listener_id++,
    .cb = cb,
    .user_data = user_data,
    .notify = notify,
  };
  g_array_append_val (self->listeners, l);
  return l.id;
}

void
mail_store_remove_listener (MailStore *self,
                            guint id)
{
  g_return_if_fail (self != NULL);
  if (id == 0)
    return;
  for (guint i = 0; i < self->listeners->len; i++)
    {
      StoreListener *l = &g_array_index (self->listeners, StoreListener, i);
      if (l->id == id)
        {
          GDestroyNotify notify = l->notify;
          gpointer user_data = l->user_data;
          g_array_remove_index (self->listeners, i);
          if (notify != NULL)
            notify (user_data);
          return;
        }
    }
}

static void
emit_change (MailStore *self,
             const MailStoreChange *change)
{
  /* Snapshot the listener list so a callback that removes itself or
   * another listener doesn't break our iteration. */
  guint n = self->listeners->len;
  if (n == 0)
    return;
  StoreListener *snap = g_newa (StoreListener, n);
  memcpy (snap, self->listeners->data, n * sizeof (StoreListener));
  for (guint i = 0; i < n; i++)
    snap[i].cb (change, snap[i].user_data);
}

/* Query the per-folder live unread/total and emit a FOLDER_COUNTS
 * event. Skipped early if no subscribers are registered, so the
 * indexed COUNT does not run on the read path when nobody is
 * listening (relevant for the test fixture and headless backends). */
static void
emit_folder_counts (MailStore *self,
                    const char *folder_remote_id)
{
  if (self->listeners->len == 0)
    return;
  sqlite3_stmt *st = self->st_folder_counts;
  sqlite3_reset (st);
  sqlite3_bind_text (st, 1, folder_remote_id, -1, SQLITE_TRANSIENT);
  int unread = 0;
  int total = 0;
  if (sqlite3_step (st) == SQLITE_ROW)
    {
      unread = sqlite3_column_int (st, 0);
      total = sqlite3_column_int (st, 1);
    }
  MailStoreChange c = {
    .kind = MAIL_STORE_CHANGE_FOLDER_COUNTS,
    .folder_id = folder_remote_id,
    .message_id = NULL,
    .unread = FALSE,
    .folder_unread = unread,
    .folder_total = total,
  };
  emit_change (self, &c);
}

const char *
mail_store_root (MailStore *self)
{
  return self->root;
}

/* --- folders ------------------------------------------------------ */

gboolean
mail_store_upsert_folder (MailStore *self,
                          const char *remote_id,
                          const char *display_name,
                          const char *parent_remote_id,
                          const char **out_dir_name,
                          GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (remote_id != NULL, FALSE);
  g_return_val_if_fail (display_name != NULL, FALSE);

  g_autofree char *sid = stable_id (self->identity, remote_id);

  /* Does it already exist? */
  sqlite3_reset (self->st_folder_select_by_remote);
  sqlite3_bind_text (self->st_folder_select_by_remote, 1, remote_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step (self->st_folder_select_by_remote);
  g_autofree char *dir_name = NULL;
  gboolean updating = FALSE;
  if (rc == SQLITE_ROW)
    {
      dir_name = g_strdup ((const char *) sqlite3_column_text (self->st_folder_select_by_remote, 1));
      updating = TRUE;
    }
  else if (rc != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "folder select");
      return FALSE;
    }

  if (!updating)
    {
      g_autofree char *base = sanitize_dir_name (display_name);
      dir_name = choose_dir_name (self, base, sid, error);
      if (dir_name == NULL)
        return FALSE;

      sqlite3_stmt *st = self->st_folder_insert;
      sqlite3_reset (st);
      sqlite3_bind_text (st, 1, sid, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text (st, 2, remote_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text (st, 3, display_name, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text (st, 4, dir_name, -1, SQLITE_TRANSIENT);
      if (parent_remote_id != NULL)
        sqlite3_bind_text (st, 5, parent_remote_id, -1, SQLITE_TRANSIENT);
      else
        sqlite3_bind_null (st, 5);
      if (sqlite3_step (st) != SQLITE_DONE)
        {
          set_sqlite_error (error, self->db, "folder insert");
          return FALSE;
        }

      g_autofree char *folder_path = g_build_filename (self->root, dir_name, NULL);
      if (!create_maildir_subdirs (folder_path, error))
        return FALSE;
    }
  else
    {
      sqlite3_stmt *st = self->st_folder_update;
      sqlite3_reset (st);
      sqlite3_bind_text (st, 1, display_name, -1, SQLITE_TRANSIENT);
      if (parent_remote_id != NULL)
        sqlite3_bind_text (st, 2, parent_remote_id, -1, SQLITE_TRANSIENT);
      else
        sqlite3_bind_null (st, 2);
      sqlite3_bind_text (st, 3, sid, -1, SQLITE_TRANSIENT);
      if (sqlite3_step (st) != SQLITE_DONE)
        {
          set_sqlite_error (error, self->db, "folder update");
          return FALSE;
        }
    }

  g_free (self->last_dir_name);
  self->last_dir_name = g_steal_pointer (&dir_name);
  if (out_dir_name != NULL)
    *out_dir_name = self->last_dir_name;
  return TRUE;
}

GPtrArray *
mail_store_list_folders (MailStore *self,
                         MailArena *arena,
                         GError **error)
{
  g_return_val_if_fail (self != NULL && arena != NULL, NULL);

  GPtrArray *out = g_ptr_array_new ();
  sqlite3_stmt *st = self->st_folder_list;
  sqlite3_reset (st);
  int rc;
  while ((rc = sqlite3_step (st)) == SQLITE_ROW)
    {
      MailFolder *f = mail_arena_alloc (arena, sizeof *f, _Alignof (MailFolder));
      f->id = mail_arena_strdup (arena, (const char *) sqlite3_column_text (st, 0));
      f->display_name = mail_arena_strdup (arena, (const char *) sqlite3_column_text (st, 1));
      const unsigned char *parent = sqlite3_column_text (st, 2);
      f->parent_id = parent != NULL ? mail_arena_strdup (arena, (const char *) parent) : NULL;
      f->unread_count = sqlite3_column_int (st, 3);
      f->total_count = sqlite3_column_int (st, 4);
      g_ptr_array_add (out, f);
    }
  if (rc != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "folder list");
      g_ptr_array_unref (out);
      return NULL;
    }
  return out;
}

GHashTable *
mail_store_folder_remote_ids (MailStore *self,
                              GError **error)
{
  GHashTable *out = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  sqlite3_stmt *st = self->st_folder_remote_ids;
  sqlite3_reset (st);
  int rc;
  while ((rc = sqlite3_step (st)) == SQLITE_ROW)
    g_hash_table_add (out, g_strdup ((const char *) sqlite3_column_text (st, 0)));
  if (rc != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "folder remote_ids");
      g_hash_table_unref (out);
      return NULL;
    }
  return out;
}

/* Recursive rm -r. Errors are reported but iteration continues so the
 * DB row is still dropped. */
static gboolean
rmtree (const char *path,
        GError **error)
{
  GError *dir_err = NULL;
  GDir *d = g_dir_open (path, 0, &dir_err);
  if (d == NULL)
    {
      if (g_error_matches (dir_err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_clear_error (&dir_err);
          return TRUE;
        }
      g_propagate_error (error, dir_err);
      return FALSE;
    }
  const char *name;
  gboolean ok = TRUE;
  while ((name = g_dir_read_name (d)) != NULL)
    {
      g_autofree char *child = g_build_filename (path, name, NULL);
      if (g_file_test (child, G_FILE_TEST_IS_DIR) && !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
        ok = rmtree (child, error) && ok;
      else if (g_unlink (child) != 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "unlink %s: %s", child, g_strerror (errno));
          ok = FALSE;
        }
    }
  g_dir_close (d);
  if (g_rmdir (path) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "rmdir %s: %s", path, g_strerror (errno));
      ok = FALSE;
    }
  return ok;
}

char *
mail_store_folder_dir_name (MailStore *self,
                            const char *folder_remote_id)
{
  g_return_val_if_fail (self != NULL && folder_remote_id != NULL, NULL);
  sqlite3_stmt *q = self->st_folder_dir_name;
  sqlite3_reset (q);
  sqlite3_bind_text (q, 1, folder_remote_id, -1, SQLITE_TRANSIENT);
  char *dir_name = NULL;
  if (sqlite3_step (q) == SQLITE_ROW)
    dir_name = g_strdup ((const char *) sqlite3_column_text (q, 0));
  return dir_name;
}

gboolean
mail_store_delete_folder (MailStore *self,
                          const char *remote_id,
                          GError **error)
{
  sqlite3_stmt *q = self->st_folder_dir_name;
  sqlite3_reset (q);
  sqlite3_bind_text (q, 1, remote_id, -1, SQLITE_TRANSIENT);
  g_autofree char *dir_name = NULL;
  int rc = sqlite3_step (q);
  if (rc == SQLITE_ROW)
    dir_name = g_strdup ((const char *) sqlite3_column_text (q, 0));
  else if (rc != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "folder dir_name");
      return FALSE;
    }

  sqlite3_stmt *d = self->st_folder_delete;
  sqlite3_reset (d);
  sqlite3_bind_text (d, 1, remote_id, -1, SQLITE_TRANSIENT);
  if (sqlite3_step (d) != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "folder delete");
      return FALSE;
    }

  if (dir_name != NULL)
    {
      g_autofree char *path = g_build_filename (self->root, dir_name, NULL);
      GError *rm_err = NULL;
      if (!rmtree (path, &rm_err))
        {
          /* Row is already gone; surface as a warning, not a hard error. */
          g_warning ("mail_store_delete_folder: rm %s: %s",
                     path, rm_err != NULL ? rm_err->message : "(no error)");
          g_clear_error (&rm_err);
        }
    }
  return TRUE;
}

/* --- messages ----------------------------------------------------- */

gboolean
mail_store_upsert_message (MailStore *self,
                           const char *folder_remote_id,
                           const char *remote_id,
                           const char *content_key,
                           const char *filename,
                           const char *subject,
                           const char *from_addr,
                           gint64 received_unix,
                           gboolean unread,
                           GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (folder_remote_id != NULL, FALSE);
  g_return_val_if_fail (remote_id != NULL, FALSE);
  g_return_val_if_fail (filename != NULL, FALSE);

  g_autofree char *folder_sid = stable_id (self->identity, folder_remote_id);
  g_autofree char *msg_sid = stable_id (self->identity, remote_id);

  sqlite3_stmt *st = self->st_message_upsert;
  sqlite3_reset (st);
  sqlite3_bind_text (st, 1, msg_sid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (st, 2, folder_sid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (st, 3, remote_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (st, 4, filename, -1, SQLITE_TRANSIENT);
  if (subject != NULL)
    sqlite3_bind_text (st, 5, subject, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null (st, 5);
  if (from_addr != NULL)
    sqlite3_bind_text (st, 6, from_addr, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null (st, 6);
  sqlite3_bind_int64 (st, 7, received_unix);
  sqlite3_bind_int (st, 8, unread ? 1 : 0);
  if (content_key != NULL)
    sqlite3_bind_text (st, 9, content_key, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null (st, 9);
  if (sqlite3_step (st) != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "message upsert");
      return FALSE;
    }
  return TRUE;
}

gboolean
mail_store_locate_body_by_content_key (MailStore *self,
                                       const char *content_key,
                                       MailArena *arena,
                                       const char **out_dir_name,
                                       const char **out_filename,
                                       GError **error)
{
  g_return_val_if_fail (self != NULL && arena != NULL, FALSE);
  if (content_key == NULL || content_key[0] == '\0')
    return FALSE;

  sqlite3_stmt *st = self->st_message_location_by_content_key;
  sqlite3_reset (st);
  sqlite3_bind_text (st, 1, content_key, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step (st);
  if (rc == SQLITE_ROW)
    {
      if (out_dir_name != NULL)
        *out_dir_name = mail_arena_strdup (arena, (const char *) sqlite3_column_text (st, 0));
      if (out_filename != NULL)
        *out_filename = mail_arena_strdup (arena, (const char *) sqlite3_column_text (st, 1));
      return TRUE;
    }
  if (rc != SQLITE_DONE)
    set_sqlite_error (error, self->db, "message locate by content_key");
  return FALSE;
}

GPtrArray *
mail_store_list_messages (MailStore *self,
                          const char *folder_remote_id,
                          int top_n,
                          MailArena *arena,
                          GError **error)
{
  g_return_val_if_fail (self != NULL && arena != NULL, NULL);
  GPtrArray *out = g_ptr_array_new ();
  sqlite3_stmt *st = self->st_message_list;
  sqlite3_reset (st);
  sqlite3_bind_text (st, 1, folder_remote_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int (st, 2, top_n);
  int rc;
  while ((rc = sqlite3_step (st)) == SQLITE_ROW)
    {
      MailMessageMeta *m = mail_arena_alloc (arena, sizeof *m, _Alignof (MailMessageMeta));
      m->id = mail_arena_strdup (arena, (const char *) sqlite3_column_text (st, 0));
      const unsigned char *subj = sqlite3_column_text (st, 1);
      m->subject = subj != NULL ? mail_arena_strdup (arena, (const char *) subj) : mail_arena_strdup (arena, "");
      const unsigned char *from = sqlite3_column_text (st, 2);
      m->from = from != NULL ? mail_arena_strdup (arena, (const char *) from) : mail_arena_strdup (arena, "");
      m->received_unix = sqlite3_column_int64 (st, 3);
      m->unread = sqlite3_column_int (st, 4) != 0;
      g_ptr_array_add (out, m);
    }
  if (rc != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "message list");
      g_ptr_array_unref (out);
      return NULL;
    }
  return out;
}

GHashTable *
mail_store_message_remote_ids (MailStore *self,
                               const char *folder_remote_id,
                               GError **error)
{
  GHashTable *out = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  sqlite3_stmt *st = self->st_message_remote_ids;
  sqlite3_reset (st);
  sqlite3_bind_text (st, 1, folder_remote_id, -1, SQLITE_TRANSIENT);
  int rc;
  while ((rc = sqlite3_step (st)) == SQLITE_ROW)
    g_hash_table_add (out, g_strdup ((const char *) sqlite3_column_text (st, 0)));
  if (rc != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "message remote_ids");
      g_hash_table_unref (out);
      return NULL;
    }
  return out;
}

gboolean
mail_store_message_location (MailStore *self,
                             const char *remote_id,
                             MailArena *arena,
                             const char **out_dir_name,
                             const char **out_filename,
                             GError **error)
{
  sqlite3_stmt *st = self->st_message_location;
  sqlite3_reset (st);
  sqlite3_bind_text (st, 1, remote_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step (st);
  if (rc == SQLITE_ROW)
    {
      if (out_dir_name != NULL)
        *out_dir_name = mail_arena_strdup (arena, (const char *) sqlite3_column_text (st, 0));
      if (out_filename != NULL)
        *out_filename = mail_arena_strdup (arena, (const char *) sqlite3_column_text (st, 1));
      return TRUE;
    }
  if (rc != SQLITE_DONE)
    set_sqlite_error (error, self->db, "message location");
  return FALSE;
}

gboolean
mail_store_delete_message (MailStore *self,
                           const char *remote_id,
                           GError **error)
{
  /* Look up file location before deleting the row. */
  sqlite3_stmt *q = self->st_message_delete;
  sqlite3_reset (q);
  sqlite3_bind_text (q, 1, remote_id, -1, SQLITE_TRANSIENT);
  g_autofree char *dir_name = NULL;
  g_autofree char *filename = NULL;
  int rc = sqlite3_step (q);
  if (rc == SQLITE_ROW)
    {
      dir_name = g_strdup ((const char *) sqlite3_column_text (q, 0));
      filename = g_strdup ((const char *) sqlite3_column_text (q, 1));
    }
  else if (rc != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "message location for delete");
      return FALSE;
    }

  if (dir_name != NULL && filename != NULL)
    {
      g_autofree char *path = g_build_filename (self->root, dir_name, "cur", filename, NULL);
      if (g_unlink (path) != 0 && errno != ENOENT)
        g_warning ("mail_store_delete_message: unlink %s: %s",
                   path, g_strerror (errno));
    }

  g_autofree char *sql = g_strdup ("DELETE FROM messages WHERE remote_id = ?;");
  sqlite3_stmt *del = NULL;
  if (sqlite3_prepare_v2 (self->db, sql, -1, &del, NULL) != SQLITE_OK)
    {
      set_sqlite_error (error, self->db, "prepare message delete");
      return FALSE;
    }
  sqlite3_bind_text (del, 1, remote_id, -1, SQLITE_TRANSIENT);
  gboolean ok = sqlite3_step (del) == SQLITE_DONE;
  if (!ok)
    set_sqlite_error (error, self->db, "message delete");
  sqlite3_finalize (del);
  return ok;
}

static char *maildir_basename_add_flag (const char *basename, char flag);
static char *maildir_basename_remove_flag (const char *basename, char flag);
static char *maildir_basename_unique_prefix (const char *basename);

gboolean
mail_store_set_message_unread (MailStore *self,
                               const char *remote_id,
                               gboolean unread,
                               GError **error)
{
  g_return_val_if_fail (self != NULL && remote_id != NULL, FALSE);

  /* Locate the on-disk file via the existing message-location join.
   * Inline the statement use (rather than calling the public API)
   * because the public version requires a MailArena and we just need
   * private copies for the rename + UPDATE. The third column carries
   * the folder's remote_id, used to name the FOLDER_COUNTS event when
   * subscribers are listening — same query, no extra round-trip. */
  sqlite3_stmt *loc = self->st_message_location;
  sqlite3_reset (loc);
  sqlite3_bind_text (loc, 1, remote_id, -1, SQLITE_TRANSIENT);
  g_autofree char *dir_name = NULL;
  g_autofree char *filename = NULL;
  g_autofree char *folder_remote_id = NULL;
  int rc = sqlite3_step (loc);
  if (rc == SQLITE_ROW)
    {
      dir_name = g_strdup ((const char *) sqlite3_column_text (loc, 0));
      filename = g_strdup ((const char *) sqlite3_column_text (loc, 1));
      folder_remote_id = g_strdup ((const char *) sqlite3_column_text (loc, 2));
    }
  else if (rc != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "message location for set-unread");
      return FALSE;
    }
  /* Unknown message id is not an error: the local index may not have
   * caught up yet (e.g. the row was opened in a race with sync). The
   * caller still got TRUE so its UI bookkeeping completes. No event
   * is emitted in this branch — there's nothing locally to invalidate. */
  if (dir_name == NULL || filename == NULL)
    return TRUE;

  g_autofree char *new_filename = unread
                                      ? maildir_basename_remove_flag (filename, 'S')
                                      : maildir_basename_add_flag (filename, 'S');
  if (g_strcmp0 (filename, new_filename) == 0)
    return TRUE; /* on-disk basename and sqlite already reflect the desired state */

  g_autofree char *old_path = g_build_filename (self->root, dir_name, "cur", filename, NULL);
  g_autofree char *new_path = g_build_filename (self->root, dir_name, "cur", new_filename, NULL);

  /* Atomic same-filesystem rename. If this fails (typically ENOENT
   * from a parallel mutator that already renamed the file) we leave
   * sqlite alone so the reconciler can catch up to disk truth. */
  if (g_rename (old_path, new_path) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "rename %s -> %s: %s",
                   old_path, new_path, g_strerror (errno));
      return FALSE;
    }

  sqlite3_stmt *st = self->st_message_set_unread;
  sqlite3_reset (st);
  sqlite3_bind_text (st, 1, new_filename, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int (st, 2, unread ? 1 : 0);
  sqlite3_bind_text (st, 3, remote_id, -1, SQLITE_TRANSIENT);
  if (sqlite3_step (st) != SQLITE_DONE)
    {
      /* Disk renamed, sqlite didn't catch up. Surface the error so
       * the caller can log; the next reconciler pass restores
       * agreement (disk is the source of truth). */
      set_sqlite_error (error, self->db, "message set unread");
      return FALSE;
    }

  /* Disk + sqlite agree on the new state. Notify subscribers in the
   * same call frame so the message-list re-bind and sidebar badge
   * update before this function returns. */
  MailStoreChange flags = {
    .kind = MAIL_STORE_CHANGE_MESSAGE_FLAGS,
    .folder_id = folder_remote_id,
    .message_id = remote_id,
    .unread = unread,
    .folder_unread = 0,
    .folder_total = 0,
  };
  emit_change (self, &flags);
  emit_folder_counts (self, folder_remote_id);
  return TRUE;
}

gboolean
mail_store_reconcile_folder_from_disk (MailStore *self,
                                       const char *folder_remote_id,
                                       GError **error)
{
  g_return_val_if_fail (self != NULL && folder_remote_id != NULL, FALSE);

  /* Resolve the folder's dir_name. Reuse the existing per-folder
   * dir lookup the upsert path also uses. */
  sqlite3_stmt *qdir = self->st_folder_dir_name;
  sqlite3_reset (qdir);
  sqlite3_bind_text (qdir, 1, folder_remote_id, -1, SQLITE_TRANSIENT);
  g_autofree char *dir_name = NULL;
  int rc = sqlite3_step (qdir);
  if (rc == SQLITE_ROW)
    dir_name = g_strdup ((const char *) sqlite3_column_text (qdir, 0));
  else if (rc != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "folder dir_name for reconcile");
      return FALSE;
    }
  if (dir_name == NULL)
    return TRUE; /* unknown folder; nothing to reconcile */

  /* Build unique-prefix -> on-disk basename map by scanning cur/. */
  g_autofree char *cur_path = g_build_filename (self->root, dir_name, "cur", NULL);
  g_autoptr (GError) dir_err = NULL;
  g_autoptr (GDir) dir = g_dir_open (cur_path, 0, &dir_err);
  if (dir == NULL)
    {
      /* The folder dir might not exist yet on a freshly upserted
       * folder; treat that as "nothing to reconcile". */
      if (g_error_matches (dir_err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        return TRUE;
      g_propagate_error (error, g_steal_pointer (&dir_err));
      return FALSE;
    }
  g_autoptr (GHashTable) disk = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                       g_free, g_free);
  const char *leaf;
  while ((leaf = g_dir_read_name (dir)) != NULL)
    {
      char *prefix = maildir_basename_unique_prefix (leaf);
      /* In the unlikely case two entries share a unique-prefix, keep
       * the first — we can't pick a winner, and either choice trips
       * the same reconcile path when sqlite next disagrees. */
      if (!g_hash_table_contains (disk, prefix))
        g_hash_table_insert (disk, prefix, g_strdup (leaf));
      else
        g_free (prefix);
    }

  /* Walk sqlite rows for this folder, updating any whose on-disk
   * basename has drifted. The query is small — one row per message in
   * the folder — and the loop body's UPDATE is the only mutation. */
  sqlite3_stmt *q = NULL;
  const char *sql = "SELECT m.remote_id, m.filename"
                    " FROM messages m"
                    " JOIN folders f ON f.stable_id = m.folder_stable_id"
                    " WHERE f.remote_id = ?;";
  if (sqlite3_prepare_v2 (self->db, sql, -1, &q, NULL) != SQLITE_OK)
    {
      set_sqlite_error (error, self->db, "prepare reconcile select");
      return FALSE;
    }
  sqlite3_bind_text (q, 1, folder_remote_id, -1, SQLITE_TRANSIENT);

  gboolean ok = TRUE;
  gboolean any_drift = FALSE;
  while ((rc = sqlite3_step (q)) == SQLITE_ROW)
    {
      const char *remote_id = (const char *) sqlite3_column_text (q, 0);
      const char *db_filename = (const char *) sqlite3_column_text (q, 1);

      g_autofree char *prefix = maildir_basename_unique_prefix (db_filename);
      const char *disk_filename = g_hash_table_lookup (disk, prefix);
      if (disk_filename == NULL)
        {
          /* File missing from disk. Don't drop the row — that's a
           * separate policy decision (might be mid-sync, might be a
           * mutt-side delete). Log and move on. */
          g_warning ("mail-store: message %s missing from %s (sqlite says %s)",
                     remote_id, cur_path, db_filename);
          continue;
        }
      if (g_strcmp0 (disk_filename, db_filename) == 0)
        continue;

      /* Disk and sqlite disagree on the info suffix. Trust disk. */
      const char *info = strstr (disk_filename, ":2,");
      gboolean disk_unread = !(info != NULL && strchr (info + 3, 'S') != NULL);
      /* Copy the remote_id before the next sqlite3_step invalidates the
       * sqlite-owned text pointer; the emit below references it. */
      g_autofree char *remote_id_copy = g_strdup (remote_id);
      sqlite3_stmt *upd = self->st_message_set_unread;
      sqlite3_reset (upd);
      sqlite3_bind_text (upd, 1, disk_filename, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int (upd, 2, disk_unread ? 1 : 0);
      sqlite3_bind_text (upd, 3, remote_id_copy, -1, SQLITE_TRANSIENT);
      if (sqlite3_step (upd) != SQLITE_DONE)
        {
          set_sqlite_error (error, self->db, "reconcile update");
          ok = FALSE;
          break;
        }
      any_drift = TRUE;
      /* Per-row MESSAGE_FLAGS emit so the message-list rebinds the
       * affected row when the user happens to be viewing this folder.
       * folder_remote_id is the caller-supplied stable string; it
       * outlives the emit. */
      MailStoreChange flags = {
        .kind = MAIL_STORE_CHANGE_MESSAGE_FLAGS,
        .folder_id = folder_remote_id,
        .message_id = remote_id_copy,
        .unread = disk_unread,
        .folder_unread = 0,
        .folder_total = 0,
      };
      emit_change (self, &flags);
    }
  if (ok && rc != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "reconcile select");
      ok = FALSE;
    }
  sqlite3_finalize (q);
  /* One aggregate FOLDER_COUNTS at the end keeps the sidebar refresh
   * cost O(1) per reconcile pass even when many rows drifted (mutt
   * marking 30 messages at once). Skipped if nothing changed so a
   * watcher event triggered by our own write doesn't ping subscribers. */
  if (ok && any_drift)
    emit_folder_counts (self, folder_remote_id);
  return ok;
}

/* --- raw IO ------------------------------------------------------- */

/* Maildir info-suffix flag manipulation. Per the spec, a "cur" file
 * is named <unique>[:2,FLAGS] where FLAGS are ASCII letters in
 * alphabetical order: D F P R S T (draft, flagged, passed, replied,
 * seen, trashed). We treat three input shapes:
 *   1. no ":2," marker         -> append ":2,<flag>"
 *   2. ":2," with empty info   -> append the flag
 *   3. ":2,<flags>"            -> insert the flag at its sort position
 * If the flag is already set, returns a duplicate of @basename.
 *
 * Returned string is g_malloc'd; caller frees. */
static char *
maildir_basename_add_flag (const char *basename,
                           char flag)
{
  g_return_val_if_fail (basename != NULL, NULL);
  g_return_val_if_fail (g_ascii_isalpha (flag), NULL);

  const char *marker = strstr (basename, ":2,");
  if (marker == NULL)
    return g_strdup_printf ("%s:2,%c", basename, flag);

  const char *flags = marker + 3; /* points at first flag char, or '\0' */
  if (strchr (flags, flag) != NULL)
    return g_strdup (basename); /* already set */

  /* Build a new flags string with @flag inserted in alphabetical order. */
  gsize n_flags = strlen (flags);
  g_autofree char *new_flags = g_malloc0 (n_flags + 2);
  gsize i = 0, j = 0;
  gboolean placed = FALSE;
  while (i < n_flags)
    {
      if (!placed && flags[i] > flag)
        {
          new_flags[j++] = flag;
          placed = TRUE;
        }
      new_flags[j++] = flags[i++];
    }
  if (!placed)
    new_flags[j++] = flag;
  new_flags[j] = '\0';

  gsize prefix_len = (gsize) (marker - basename); /* up to and excluding ":2," */
  return g_strdup_printf ("%.*s:2,%s", (int) prefix_len, basename, new_flags);
}

/* Inverse: returns a basename with @flag removed. No-op (returns a
 * duplicate) if @flag is not present or there is no ":2," marker.
 * Preserves the empty ":2," marker when removing the last flag, so the
 * file remains a well-formed Maildir cur/ entry. */
static char *
maildir_basename_remove_flag (const char *basename,
                              char flag)
{
  g_return_val_if_fail (basename != NULL, NULL);
  g_return_val_if_fail (g_ascii_isalpha (flag), NULL);

  const char *marker = strstr (basename, ":2,");
  if (marker == NULL)
    return g_strdup (basename);

  const char *flags = marker + 3;
  if (strchr (flags, flag) == NULL)
    return g_strdup (basename);

  gsize n_flags = strlen (flags);
  g_autofree char *new_flags = g_malloc0 (n_flags + 1);
  gsize j = 0;
  for (gsize i = 0; i < n_flags; i++)
    if (flags[i] != flag)
      new_flags[j++] = flags[i];
  new_flags[j] = '\0';

  gsize prefix_len = (gsize) (marker - basename);
  return g_strdup_printf ("%.*s:2,%s", (int) prefix_len, basename, new_flags);
}

/* Stable identifier for a Maildir entry: the portion of the basename
 * before ":2,". Per the spec, only the info suffix changes when flags
 * are added or removed, so two basenames with the same unique prefix
 * refer to the same logical message regardless of which client last
 * wrote it. */
static char *
maildir_basename_unique_prefix (const char *basename)
{
  g_return_val_if_fail (basename != NULL, NULL);
  const char *marker = strstr (basename, ":2,");
  if (marker == NULL)
    return g_strdup (basename);
  return g_strndup (basename, (gsize) (marker - basename));
}

char *
_mail_store_maildir_basename_unique_prefix_for_test (const char *basename)
{
  return maildir_basename_unique_prefix (basename);
}

char *
_mail_store_maildir_basename_add_flag_for_test (const char *basename,
                                                char flag)
{
  return maildir_basename_add_flag (basename, flag);
}

char *
_mail_store_maildir_basename_remove_flag_for_test (const char *basename,
                                                   char flag)
{
  return maildir_basename_remove_flag (basename, flag);
}

static char *
maildir_filename (MailStore *self,
                  gboolean seen)
{
  GDateTime *now = g_date_time_new_now_utc ();
  gint64 secs = g_date_time_to_unix (now);
  gint usec = g_date_time_get_microsecond (now);
  g_date_time_unref (now);
  guint64 counter = ++self->filename_counter;
  /* Per the Maildir spec, every entry in cur/ has the `:2,FLAGS`
   * info suffix; for unread mail FLAGS is empty (just `:2,`). Older
   * Magpie deliveries omitted the marker entirely for unread; mutt
   * tolerated it but stricter Maildir readers (notmuch, some MUAs)
   * don't, so we now always emit the marker. The reconciler matches
   * by the unique prefix before `:2,`, so existing pre-marker rows
   * keep matching their on-disk files until the next mark-read. */
  return g_strdup_printf ("%" G_GINT64_FORMAT ".M%dP%dQ%" G_GUINT64_FORMAT ".%s:2,%s",
                          secs, usec, (int) getpid (), counter,
                          self->hostname, seen ? "S" : "");
}

gboolean
mail_store_write_raw (MailStore *self,
                      const char *dir_name,
                      GBytes *bytes,
                      gboolean seen,
                      char **out_filename,
                      GError **error)
{
  g_return_val_if_fail (self != NULL && dir_name != NULL && bytes != NULL, FALSE);

  g_autofree char *name = maildir_filename (self, seen);
  g_autofree char *tmp_path = g_build_filename (self->root, dir_name, "tmp", name, NULL);
  g_autofree char *cur_path = g_build_filename (self->root, dir_name, "cur", name, NULL);

  gsize len = 0;
  const char *data = g_bytes_get_data (bytes, &len);

  int fd = open (tmp_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "open %s: %s", tmp_path, g_strerror (errno));
      return FALSE;
    }
  gssize n_written = 0;
  while ((gsize) n_written < len)
    {
      gssize w = write (fd, data + n_written, len - n_written);
      if (w < 0)
        {
          if (errno == EINTR)
            continue;
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "write %s: %s", tmp_path, g_strerror (errno));
          close (fd);
          g_unlink (tmp_path);
          return FALSE;
        }
      n_written += w;
    }
  if (fsync (fd) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "fsync %s: %s", tmp_path, g_strerror (errno));
      close (fd);
      g_unlink (tmp_path);
      return FALSE;
    }
  close (fd);
  if (g_rename (tmp_path, cur_path) != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "rename %s -> %s: %s", tmp_path, cur_path, g_strerror (errno));
      g_unlink (tmp_path);
      return FALSE;
    }
  if (out_filename != NULL)
    *out_filename = g_steal_pointer (&name);
  return TRUE;
}

GBytes *
mail_store_read_raw (MailStore *self,
                     const char *dir_name,
                     const char *filename,
                     GError **error)
{
  g_autofree char *path = g_build_filename (self->root, dir_name, "cur", filename, NULL);
  char *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &contents, &len, error))
    return NULL;
  return g_bytes_new_take (contents, len);
}

gboolean
mail_store_link_raw (MailStore *self,
                     const char *source_dir,
                     const char *source_filename,
                     const char *target_dir,
                     gboolean seen,
                     char **out_filename,
                     GError **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (source_dir != NULL && source_filename != NULL, FALSE);
  g_return_val_if_fail (target_dir != NULL, FALSE);

  g_autofree char *src = g_build_filename (self->root, source_dir, "cur", source_filename, NULL);
  g_autofree char *name = maildir_filename (self, seen);
  g_autofree char *dst = g_build_filename (self->root, target_dir, "cur", name, NULL);

  if (link (src, dst) == 0)
    {
      if (out_filename != NULL)
        *out_filename = g_steal_pointer (&name);
      return TRUE;
    }

  /* link() can fail for cross-FS (EXDEV) or if the source is missing
   * (ENOENT). For EXDEV we fall back to a byte copy so a Maildir
   * spanning two filesystems still works. Anything else is fatal. */
  int saved_errno = errno;
  if (saved_errno != EXDEV)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "link %s -> %s: %s", src, dst, g_strerror (saved_errno));
      return FALSE;
    }

  g_autoptr (GBytes) bytes = mail_store_read_raw (self, source_dir, source_filename, error);
  if (bytes == NULL)
    return FALSE;
  return mail_store_write_raw (self, target_dir, bytes, seen, out_filename, error);
}
