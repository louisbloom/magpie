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
 * the raw file; caller does that via mail_store_write_raw first.
 * @content_key may be NULL; when non-NULL it lets future syncs find
 * an existing body (any folder) and hardlink instead of re-fetching.
 * On update, a non-NULL content_key overwrites the stored one; NULL
 * preserves the existing value (so backends that learn the key only
 * later don't clobber it on a re-pass). */
gboolean mail_store_upsert_message (MailStore *self,
                                    const char *folder_remote_id,
                                    const char *remote_id,
                                    const char *content_key,
                                    const char *filename,
                                    const char *subject,
                                    const char *from_addr,
                                    gint64 received_unix,
                                    gboolean unread,
                                    const char *flags,
                                    GError **error);

/* Find any existing message row whose content_key matches and return
 * its on-disk location. The sync engine uses this to detect "I
 * already have this body somewhere" and hardlink rather than refetch.
 * Returns FALSE without setting *error when no match exists. */
gboolean mail_store_locate_body_by_content_key (MailStore *self,
                                                const char *content_key,
                                                MailArena *arena,
                                                const char **out_dir_name,
                                                const char **out_filename,
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

/* Toggle the Maildir `S` (seen) flag for @remote_id. Per the project
 * rule, the on-disk file is the source of truth: this renames the
 * file in cur/ to add or remove `S` from its `:2,FLAGS` info suffix,
 * then updates messages.filename and messages.unread to track. On
 * rename failure (typically ENOENT from a parallel mutator), sqlite
 * is left untouched and the reconciler will catch up to disk on the
 * next sync pass. Unknown @remote_id is a no-op success — the local
 * index may not have caught up to remote state yet. */
gboolean mail_store_set_message_unread (MailStore *self,
                                        const char *remote_id,
                                        gboolean unread,
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

/* Test-only: parse and reshape a Maildir basename's `:2,FLAGS` info
 * suffix. Inserts/removes @flag in the spec-mandated alphabetical
 * order (DFPRST). Returned string is g_malloc'd. Production code uses
 * the static helpers inside mail-store.c. */
char *_mail_store_maildir_basename_add_flag_for_test (const char *basename,
                                                      char flag);
char *_mail_store_maildir_basename_remove_flag_for_test (const char *basename,
                                                         char flag);

/* Hardlink an existing body file into another folder. Used by the
 * sync engine when dedupping cross-folder duplicates: one fetch,
 * many hardlinked Maildir entries sharing the underlying inode. On
 * EXDEV (filesystems differ across folders) falls back to a byte
 * copy. The new leaf name is generated fresh and returned via
 * *out_filename. */
gboolean mail_store_link_raw (MailStore *self,
                              const char *source_dir,
                              const char *source_filename,
                              const char *target_dir,
                              gboolean seen,
                              char **out_filename,
                              GError **error);

G_END_DECLS
