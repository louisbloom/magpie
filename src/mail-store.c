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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAIL_STORE_SCHEMA_VERSION 1

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
  sqlite3_stmt *st_message_delete;

  /* Scratch buffer used by upsert_folder to hand back a borrowed
   * pointer to the chosen dir_name (valid until the next upsert). */
  char *last_dir_name;

  /* Monotonic counter for maildir filename uniqueness within a usec. */
  guint64 filename_counter;
};

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

  const char *ddl =
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
      "  flags            TEXT"
      ");"
      "CREATE INDEX IF NOT EXISTS messages_folder_received"
      "  ON messages (folder_stable_id, received_unix DESC);"
      "CREATE TABLE IF NOT EXISTS sync_state ("
      "  folder_stable_id TEXT PRIMARY KEY REFERENCES folders(stable_id) ON DELETE CASCADE,"
      "  delta_token      TEXT,"
      "  uidvalidity      INTEGER,"
      "  uidnext          INTEGER,"
      "  last_synced_unix INTEGER"
      ");";
  if (!exec_sql (self->db, ddl, error))
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
      "INSERT INTO folders"
      " (stable_id, remote_id, display_name, dir_name, parent_remote_id, unread, total)"
      " VALUES (?, ?, ?, ?, ?, ?, ?);" },
    { &self->st_folder_update,
      "UPDATE folders SET display_name = ?, parent_remote_id = ?, unread = ?, total = ?"
      " WHERE stable_id = ?;" },
    { &self->st_folder_list,
      "SELECT remote_id, display_name, parent_remote_id, unread, total"
      " FROM folders ORDER BY display_name;" },
    { &self->st_folder_remote_ids,
      "SELECT remote_id FROM folders;" },
    { &self->st_folder_dir_name,
      "SELECT dir_name FROM folders WHERE remote_id = ?;" },
    { &self->st_folder_delete,
      "DELETE FROM folders WHERE remote_id = ?;" },
    { &self->st_message_upsert,
      "INSERT INTO messages"
      " (stable_id, folder_stable_id, remote_id, filename, subject, from_addr, received_unix, unread, flags)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
      " ON CONFLICT(stable_id) DO UPDATE SET"
      "   folder_stable_id = excluded.folder_stable_id,"
      "   filename = excluded.filename,"
      "   subject = excluded.subject,"
      "   from_addr = excluded.from_addr,"
      "   received_unix = excluded.received_unix,"
      "   unread = excluded.unread,"
      "   flags = excluded.flags;" },
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
      "SELECT f.dir_name, m.filename"
      " FROM messages m"
      " JOIN folders f ON f.stable_id = m.folder_stable_id"
      " WHERE m.remote_id = ?;" },
    { &self->st_message_delete,
      "SELECT f.dir_name, m.filename FROM messages m"
      " JOIN folders f ON f.stable_id = m.folder_stable_id"
      " WHERE m.remote_id = ?;" },
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
    &self->st_message_delete,
  };
  for (gsize i = 0; i < G_N_ELEMENTS (all); i++)
    if (*all[i] != NULL)
      sqlite3_finalize (*all[i]);
  if (self->db != NULL)
    sqlite3_close (self->db);
  g_free (self->root);
  g_free (self->identity);
  g_free (self->hostname);
  g_free (self->last_dir_name);
  g_free (self);
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
                          int unread,
                          int total,
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
      sqlite3_bind_int (st, 6, unread);
      sqlite3_bind_int (st, 7, total);
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
      sqlite3_bind_int (st, 3, unread);
      sqlite3_bind_int (st, 4, total);
      sqlite3_bind_text (st, 5, sid, -1, SQLITE_TRANSIENT);
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
                           const char *filename,
                           const char *subject,
                           const char *from_addr,
                           gint64 received_unix,
                           gboolean unread,
                           const char *flags,
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
  if (flags != NULL)
    sqlite3_bind_text (st, 9, flags, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null (st, 9);
  if (sqlite3_step (st) != SQLITE_DONE)
    {
      set_sqlite_error (error, self->db, "message upsert");
      return FALSE;
    }
  return TRUE;
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

/* --- raw IO ------------------------------------------------------- */

static char *
maildir_filename (MailStore *self,
                  gboolean seen)
{
  GDateTime *now = g_date_time_new_now_utc ();
  gint64 secs = g_date_time_to_unix (now);
  gint usec = g_date_time_get_microsecond (now);
  g_date_time_unref (now);
  guint64 counter = ++self->filename_counter;
  return g_strdup_printf ("%" G_GINT64_FORMAT ".M%dP%dQ%" G_GUINT64_FORMAT ".%s%s",
                          secs, usec, (int) getpid (), counter,
                          self->hostname, seen ? ":2,S" : "");
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
