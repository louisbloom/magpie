/* mail-store.h - Per-account on-disk store: sqlite index + Maildir tree.
 *
 * Layout (rooted at ~/Mail/<identity>/):
 *   state.db                  sqlite index of folders, messages, sync state
 *   <dir_name>/cur/new/tmp/   one directory per folder, Maildir-spec layout
 *
 * Lifetime: a MailStore owns its sqlite connection and prepared
 * statements for the process lifetime of the account. Strings returned
 * by reader helpers go into the caller-supplied MailArena and follow
 * that arena's lifetime contract — see [[feedback-memory-reuse]].
 */

#pragma once

#include "mail-arena.h"
#include "mail-backend.h" /* MailFolder, MailMessageMeta */

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _MailStore MailStore;

/* Open (creating if needed) the store at @account_root for @identity.
 * The directory is created with 0700. Returns NULL on failure. */
MailStore *mail_store_open (const char *account_root,
                            const char *identity,
                            GError **error);

void mail_store_close (MailStore *self);

const char *mail_store_root (MailStore *self);

/* --- folders -------------------------------------------------- */

/* Insert-or-update a folder by remote_id. On first insert, picks a
 * sanitized dir_name (collision-suffixed with ~N if needed), creates
 * <root>/<dir_name>/{cur,new,tmp}/, and writes the row. On update,
 * mutates display_name / unread / total but keeps the existing
 * dir_name. *out_dir_name (if non-NULL) receives a borrowed pointer
 * valid until the next upsert on this store. */
gboolean mail_store_upsert_folder (MailStore *self,
                                   const char *remote_id,
                                   const char *display_name,
                                   const char *parent_remote_id,
                                   int unread,
                                   int total,
                                   const char **out_dir_name,
                                   GError **error);

/* Snapshot of every folder, ordered by display_name. MailFolder
 * fields are arena-allocated. */
GPtrArray *mail_store_list_folders (MailStore *self,
                                    MailArena *arena,
                                    GError **error);

/* Returns a GHashTable mapping g_strdup'd remote_id -> "1" (set).
 * Caller must g_hash_table_unref. Used by sync to compute deletions. */
GHashTable *mail_store_folder_remote_ids (MailStore *self,
                                          GError **error);

/* Drop a folder row (cascades messages) and rm -r its directory. */
gboolean mail_store_delete_folder (MailStore *self,
                                   const char *remote_id,
                                   GError **error);

/* --- messages ------------------------------------------------- */

/* Insert-or-update a message row keyed by remote_id. Does not touch
 * the raw file; caller does that via mail_store_write_raw first. */
gboolean mail_store_upsert_message (MailStore *self,
                                    const char *folder_remote_id,
                                    const char *remote_id,
                                    const char *filename,
                                    const char *subject,
                                    const char *from_addr,
                                    gint64 received_unix,
                                    gboolean unread,
                                    const char *flags,
                                    GError **error);

/* Top-@top_n messages for a folder ordered by received_unix DESC.
 * MailMessageMeta fields are arena-allocated. */
GPtrArray *mail_store_list_messages (MailStore *self,
                                     const char *folder_remote_id,
                                     int top_n,
                                     MailArena *arena,
                                     GError **error);

/* For diffing during sync: every remote_id known for the folder. */
GHashTable *mail_store_message_remote_ids (MailStore *self,
                                           const char *folder_remote_id,
                                           GError **error);

/* Look up the on-disk location of a message. *out_dir_name and
 * *out_filename point into arena. Returns FALSE without setting error
 * if the message id is unknown. */
gboolean mail_store_message_location (MailStore *self,
                                      const char *remote_id,
                                      MailArena *arena,
                                      const char **out_dir_name,
                                      const char **out_filename,
                                      GError **error);

/* Drop the row and unlink the file. */
gboolean mail_store_delete_message (MailStore *self,
                                    const char *remote_id,
                                    GError **error);

/* --- raw RFC822 file IO -------------------------------------- */

/* Write @bytes to <root>/<dir_name>/tmp/<name>, fsync, rename into
 * cur/ as a Maildir-spec filename. @seen sets the :2,S flag suffix.
 * The chosen leaf filename is g_strdup'd into *out_filename. */
gboolean mail_store_write_raw (MailStore *self,
                               const char *dir_name,
                               GBytes *bytes,
                               gboolean seen,
                               char **out_filename,
                               GError **error);

/* Read the file at <root>/<dir_name>/cur/<filename>. */
GBytes *mail_store_read_raw (MailStore *self,
                             const char *dir_name,
                             const char *filename,
                             GError **error);

G_END_DECLS
