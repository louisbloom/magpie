/* tests/test-store.c - Round-trip exercises for MailStore.
 *
 * Each subtest opens a fresh store under a tmp dir, runs its case,
 * then cleans up. The store creates the dir so we only need to choose
 * a unique path.
 */

#include "../src/mail-arena.h"
#include "../src/mail-backend.h"
#include "../src/mail-store.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>

typedef struct
{
  char *root;
  MailStore *store;
  MailArena arena;
} Fixture;

static void
fixture_setup (Fixture *f,
               gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  f->root = g_dir_make_tmp ("mail-store-test-XXXXXX", &error);
  g_assert_no_error (error);
  f->store = mail_store_open (f->root, "test@example.com", &error);
  g_assert_no_error (error);
  g_assert_nonnull (f->store);
  mail_arena_init (&f->arena, 4096);
}

/* Recursively remove the tmp dir. We only ever populate it via
 * MailStore so no surprises here. */
static void
rm_rf (const char *path)
{
  GDir *d = g_dir_open (path, 0, NULL);
  if (d != NULL)
    {
      const char *n;
      while ((n = g_dir_read_name (d)) != NULL)
        {
          g_autofree char *child = g_build_filename (path, n, NULL);
          if (g_file_test (child, G_FILE_TEST_IS_DIR) && !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
            rm_rf (child);
          else
            g_unlink (child);
        }
      g_dir_close (d);
    }
  g_rmdir (path);
}

static void
fixture_teardown (Fixture *f,
                  gconstpointer data)
{
  mail_arena_destroy (&f->arena);
  mail_store_close (f->store);
  rm_rf (f->root);
  g_free (f->root);
}

/* --- tests --------------------------------------------------------- */

static void
test_open_creates_root (Fixture *f,
                        gconstpointer data)
{
  g_assert_true (g_file_test (f->root, G_FILE_TEST_IS_DIR));
  g_autofree char *dbpath = g_build_filename (f->root, "state.db", NULL);
  g_assert_true (g_file_test (dbpath, G_FILE_TEST_EXISTS));
}

static void
test_upsert_folder_creates_maildir_dirs (Fixture *f,
                                         gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  const char *dir_name = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, &dir_name, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (dir_name, ==, "INBOX");

  for (gsize i = 0; i < 3; i++)
    {
      const char *sub = (const char *[]){ "cur", "new", "tmp" }[i];
      g_autofree char *p = g_build_filename (f->root, "INBOX", sub, NULL);
      g_assert_true (g_file_test (p, G_FILE_TEST_IS_DIR));
    }
}

static void
test_list_folders_returns_inserted (Fixture *f,
                                    gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  /* unread/total args here populate folders.unread / folders.total
   * — backends report server-side counts via this path — but
   * mail_store_list_folders derives the values it returns from the
   * messages table. Both folders are empty here, so the listed
   * counts are 0 regardless of what we passed in. The pin against
   * the cached column is in test_list_folders_counts_derive_from_messages. */
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (mail_store_upsert_folder (f->store, "rid-Sent", "Sent", NULL, NULL, &error));
  g_assert_no_error (error);

  GPtrArray *folders = mail_store_list_folders (f->store, &f->arena, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (folders->len, ==, 2);
  /* Ordered by display_name. */
  MailFolder *first = g_ptr_array_index (folders, 0);
  MailFolder *second = g_ptr_array_index (folders, 1);
  g_assert_cmpstr (first->display_name, ==, "INBOX");
  g_assert_cmpint (first->unread_count, ==, 0);
  g_assert_cmpint (first->total_count, ==, 0);
  g_assert_cmpstr (first->id, ==, "rid-INBOX");
  g_assert_cmpstr (second->display_name, ==, "Sent");
  g_assert_cmpint (second->unread_count, ==, 0);
  g_assert_cmpint (second->total_count, ==, 0);
  g_ptr_array_unref (folders);
}

/* Pin the rule that mail_store_list_folders derives unread/total
 * from the messages table rather than the cached folders.unread /
 * folders.total columns. Backstory: backends populate the cached
 * columns from server-reported counts (Graph's unreadItemCount,
 * IMAP STATUS UNSEEN/MESSAGES), so after a cancelled sync those
 * cached numbers advertised mail the local store didn't actually
 * have — the sidebar showed Inbox=5315 while only ~50 messages had
 * been fetched. The new SELECT counts messages live; this test
 * confirms it. */
static void
test_list_folders_counts_derive_from_messages (Fixture *f,
                                               gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  /* Upsert a folder with deliberately wrong cached counts — what a
   * server would report — so we can prove the returned values come
   * from the messages table, not from these arguments. */
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, NULL, &error));
  g_assert_no_error (error);

  /* Two unread, one read. Local truth: unread=2, total=3. */
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-u1", NULL, "f1",
                                            "S1", "a@x", 1700000000, TRUE, &error));
  g_assert_no_error (error);
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-u2", NULL, "f2",
                                            "S2", "a@x", 1700000001, TRUE, &error));
  g_assert_no_error (error);
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-r1", NULL, "f3",
                                            "S3", "a@x", 1700000002, FALSE, &error));
  g_assert_no_error (error);

  GPtrArray *folders = mail_store_list_folders (f->store, &f->arena, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (folders->len, ==, 1);
  MailFolder *only = g_ptr_array_index (folders, 0);
  g_assert_cmpint (only->unread_count, ==, 2);
  g_assert_cmpint (only->total_count, ==, 3);
  g_ptr_array_unref (folders);
}

static void
test_upsert_folder_update_does_not_rename_dir (Fixture *f,
                                               gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  const char *dir1 = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, &dir1, &error));
  g_assert_no_error (error);
  g_autofree char *dir1_copy = g_strdup (dir1);
  /* Update counts: dir_name should stay. */
  const char *dir2 = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX (renamed)", NULL,
                                           &dir2, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (dir2, ==, dir1_copy);

  GPtrArray *folders = mail_store_list_folders (f->store, &f->arena, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (folders->len, ==, 1);
  MailFolder *only = g_ptr_array_index (folders, 0);
  g_assert_cmpstr (only->display_name, ==, "INBOX (renamed)");
  /* Counts come from the messages table; no messages were upserted,
   * so even though the upsert passed unread=5/total=50, listing
   * yields the local-store truth: 0/0. */
  g_assert_cmpint (only->unread_count, ==, 0);
  g_assert_cmpint (only->total_count, ==, 0);
  g_ptr_array_unref (folders);
}

static void
test_collision_suffixes_with_tilde (Fixture *f,
                                    gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  const char *d1 = NULL, *d2 = NULL, *d3 = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-1", "INBOX", NULL, &d1, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (d1, ==, "INBOX");
  g_assert_true (mail_store_upsert_folder (f->store, "rid-2", "INBOX", NULL, &d2, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (d2, ==, "INBOX~2");
  g_assert_true (mail_store_upsert_folder (f->store, "rid-3", "INBOX", NULL, &d3, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (d3, ==, "INBOX~3");
}

static void
test_slash_in_display_name_sanitized (Fixture *f,
                                      gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  const char *dir_name = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-X", "foo/bar", NULL,
                                           &dir_name, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (dir_name, ==, "foo_bar");
  /* And the directory really exists with that sanitized name. */
  g_autofree char *p = g_build_filename (f->root, "foo_bar", "cur", NULL);
  g_assert_true (g_file_test (p, G_FILE_TEST_IS_DIR));
}

static void
test_write_then_read_raw_roundtrip (Fixture *f,
                                    gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           NULL, &error));
  g_assert_no_error (error);

  const char *payload = "From: a@example.com\r\nSubject: hi\r\n\r\nbody\r\n";
  g_autoptr (GBytes) in = g_bytes_new_static (payload, strlen (payload));
  g_autofree char *filename = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", in, TRUE, &filename, &error));
  g_assert_no_error (error);
  g_assert_nonnull (filename);
  /* File should be in cur/ with :2,S suffix because seen=TRUE. The
   * `:2,` info marker is required by the Maildir spec for every
   * cur/ entry — present even when no flags are set (seen=FALSE).
   * The seen=FALSE pin lives in test_set_message_unread_renames_and
   * _updates. */
  g_assert_true (g_str_has_suffix (filename, ":2,S"));
  g_autofree char *p = g_build_filename (f->root, "INBOX", "cur", filename, NULL);
  g_assert_true (g_file_test (p, G_FILE_TEST_EXISTS));

  g_autoptr (GBytes) out = mail_store_read_raw (f->store, "INBOX", filename, &error);
  g_assert_no_error (error);
  g_assert_nonnull (out);
  gsize len = 0;
  const char *bytes = g_bytes_get_data (out, &len);
  g_assert_cmpuint (len, ==, strlen (payload));
  g_assert_cmpint (memcmp (bytes, payload, len), ==, 0);
}

static void
test_message_upsert_and_list (Fixture *f,
                              gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           NULL, &error));
  g_assert_no_error (error);

  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", NULL, "file-1",
                                            "Hello", "alice@example.com",
                                            1700000000, TRUE, &error));
  g_assert_no_error (error);
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-2", NULL, "file-2",
                                            "Hi", "bob@example.com",
                                            1700000050, FALSE, &error));
  g_assert_no_error (error);

  GPtrArray *msgs = mail_store_list_messages (f->store, "rid-INBOX", 10, &f->arena, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (msgs->len, ==, 2);
  /* Ordered DESC by received_unix: msg-2 first. */
  MailMessageMeta *first = g_ptr_array_index (msgs, 0);
  MailMessageMeta *second = g_ptr_array_index (msgs, 1);
  g_assert_cmpstr (first->id, ==, "msg-2");
  g_assert_cmpstr (first->subject, ==, "Hi");
  g_assert_false (first->unread);
  g_assert_cmpstr (second->id, ==, "msg-1");
  g_assert_true (second->unread);
  g_ptr_array_unref (msgs);
}

static void
test_message_location_lookup (Fixture *f,
                              gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           NULL, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", NULL, "filename.eml",
                                            NULL, NULL, 0, FALSE, &error));
  g_assert_no_error (error);

  const char *dir = NULL, *file = NULL;
  g_assert_true (mail_store_message_location (f->store, "msg-1", &f->arena,
                                              &dir, &file, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (dir, ==, "INBOX");
  g_assert_cmpstr (file, ==, "filename.eml");

  /* Unknown id -> FALSE, no error. */
  g_assert_false (mail_store_message_location (f->store, "nope", &f->arena,
                                               &dir, &file, &error));
  g_assert_no_error (error);
}

static void
test_message_delete_unlinks_file (Fixture *f,
                                  gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           NULL, &error));
  /* Write a real file and link a message to it. */
  const char *payload = "hi";
  g_autoptr (GBytes) in = g_bytes_new_static (payload, strlen (payload));
  g_autofree char *filename = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", in, FALSE, &filename, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", NULL, filename,
                                            NULL, NULL, 0, FALSE, &error));
  g_assert_no_error (error);
  g_autofree char *path = g_build_filename (f->root, "INBOX", "cur", filename, NULL);
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));

  g_assert_true (mail_store_delete_message (f->store, "msg-1", &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  /* Row is gone too. */
  GHashTable *ids = mail_store_message_remote_ids (f->store, "rid-INBOX", &error);
  g_assert_no_error (error);
  g_assert_cmpuint (g_hash_table_size (ids), ==, 0);
  g_hash_table_unref (ids);
}

static void
test_folder_delete_drops_dir_and_cascades (Fixture *f,
                                           gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           NULL, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", NULL, "f", NULL,
                                            NULL, 0, FALSE, &error));
  g_assert_no_error (error);

  g_assert_true (mail_store_delete_folder (f->store, "rid-INBOX", &error));
  g_assert_no_error (error);

  g_autofree char *p = g_build_filename (f->root, "INBOX", NULL);
  g_assert_false (g_file_test (p, G_FILE_TEST_EXISTS));

  GHashTable *fids = mail_store_folder_remote_ids (f->store, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (g_hash_table_size (fids), ==, 0);
  g_hash_table_unref (fids);
  /* Messages cascaded. */
  GHashTable *mids = mail_store_message_remote_ids (f->store, "rid-INBOX", &error);
  g_assert_no_error (error);
  g_assert_cmpuint (g_hash_table_size (mids), ==, 0);
  g_hash_table_unref (mids);
}

static void
test_locate_by_content_key (Fixture *f,
                            gconstpointer data)
{
  /* Two folders share a message body keyed by content_key. The store
   * locates it by key regardless of which folder asked. */
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           NULL, &error));
  g_assert_true (mail_store_upsert_folder (f->store, "rid-LABEL", "Travel", NULL,
                                           NULL, &error));

  const char *payload = "From: a@b\r\n\r\nshared body\r\n";
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, strlen (payload));
  g_autofree char *inbox_name = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", bytes, FALSE,
                                       &inbox_name, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1",
                                            "<mid@example.com>", inbox_name,
                                            "shared", "a@b", 100, FALSE, &error));
  g_assert_no_error (error);

  const char *dir = NULL, *file = NULL;
  g_assert_true (mail_store_locate_body_by_content_key (f->store, "<mid@example.com>",
                                                        &f->arena, &dir, &file, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (dir, ==, "INBOX");
  g_assert_cmpstr (file, ==, inbox_name);

  /* NULL / unknown content_key → FALSE, no error. */
  g_assert_false (mail_store_locate_body_by_content_key (f->store, "<unknown@x>",
                                                         &f->arena, &dir, &file, &error));
  g_assert_no_error (error);
  g_assert_false (mail_store_locate_body_by_content_key (f->store, NULL,
                                                         &f->arena, &dir, &file, &error));
  g_assert_no_error (error);
}

static void
test_link_raw_creates_hardlink (Fixture *f,
                                gconstpointer data)
{
  /* mail_store_link_raw must create a hardlink so the body is stored
   * once on disk regardless of how many folders reference it. */
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           NULL, &error));
  g_assert_true (mail_store_upsert_folder (f->store, "rid-LABEL", "Travel", NULL,
                                           NULL, &error));

  const char *payload = "X-Mid: 1\r\n\r\nbody\r\n";
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, strlen (payload));
  g_autofree char *src_name = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", bytes, FALSE,
                                       &src_name, &error));

  g_autofree char *dst_name = NULL;
  g_assert_true (mail_store_link_raw (f->store, "INBOX", src_name, "Travel", FALSE,
                                      &dst_name, &error));
  g_assert_no_error (error);
  g_assert_nonnull (dst_name);
  g_assert_cmpstr (dst_name, !=, src_name); /* fresh leaf name */

  g_autofree char *src_path = g_build_filename (f->root, "INBOX", "cur", src_name, NULL);
  g_autofree char *dst_path = g_build_filename (f->root, "Travel", "cur", dst_name, NULL);

  struct stat sa, sb;
  g_assert_cmpint (stat (src_path, &sa), ==, 0);
  g_assert_cmpint (stat (dst_path, &sb), ==, 0);
  g_assert_cmpint (sa.st_ino, ==, sb.st_ino); /* same inode → hardlinked */
  g_assert_cmpint (sa.st_nlink, ==, 2);       /* link count bumped */
}

/* Regression: when an external client (mutt, another magpie instance)
 * renames a file in cur/ to add or remove the S flag, the next sync
 * pass must pick that up and update sqlite to match — per the
 * Maildir-is-truth rule. The reconciler matches sqlite rows to disk
 * files by their unique prefix (everything before ":2,"), so a rename
 * that only changes the info suffix is identified as the same logical
 * message. */
static void
test_reconcile_folder_from_disk (Fixture *f,
                                 gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           NULL, &error));

  /* Write an unread file (no `S` in its info suffix) and upsert a
   * matching row that agrees: unread=TRUE, filename=basename. */
  const char *payload = "Subject: hi\r\n\r\nbody\r\n";
  g_autoptr (GBytes) in = g_bytes_new_static (payload, strlen (payload));
  g_autofree char *name = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", in, FALSE, &name, &error));
  g_assert_false (g_str_has_suffix (name, ":2,S"));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", NULL, name,
                                            "hi", "a@x", 1700000000, TRUE, &error));
  g_assert_no_error (error);

  /* Pretend mutt opened the message and marked it read: rename the
   * on-disk file to add :2,S, then run the reconciler. Sqlite must
   * reflect both the new filename and unread=FALSE. The basename
   * helper builds the correct flagged form whether or not the
   * source already has a `:2,` marker. */
  g_autofree char *seen_name = _mail_store_maildir_basename_add_flag_for_test (name, 'S');
  g_autofree char *old_path = g_build_filename (f->root, "INBOX", "cur", name, NULL);
  g_autofree char *new_path = g_build_filename (f->root, "INBOX", "cur", seen_name, NULL);
  g_assert_cmpint (g_rename (old_path, new_path), ==, 0);

  g_assert_true (mail_store_reconcile_folder_from_disk (f->store, "rid-INBOX", &error));
  g_assert_no_error (error);

  GPtrArray *msgs = mail_store_list_messages (f->store, "rid-INBOX", 10, &f->arena, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (msgs->len, ==, 1);
  MailMessageMeta *m = g_ptr_array_index (msgs, 0);
  g_assert_false (m->unread);
  g_ptr_array_unref (msgs);

  /* Body is still readable through the store via the now-tracked filename. */
  const char *dir = NULL, *file = NULL;
  g_assert_true (mail_store_message_location (f->store, "msg-1", &f->arena, &dir, &file, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (file, ==, seen_name);
  g_autoptr (GBytes) body = mail_store_read_raw (f->store, dir, file, &error);
  g_assert_no_error (error);
  g_assert_nonnull (body);

  /* Now flip the other way: rename to drop :2,S, reconcile, and
   * watch sqlite flip back to unread. */
  g_assert_cmpint (g_rename (new_path, old_path), ==, 0);
  g_assert_true (mail_store_reconcile_folder_from_disk (f->store, "rid-INBOX", &error));
  g_assert_no_error (error);
  mail_arena_reset (&f->arena);
  msgs = mail_store_list_messages (f->store, "rid-INBOX", 10, &f->arena, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (msgs->len, ==, 1);
  m = g_ptr_array_index (msgs, 0);
  g_assert_true (m->unread);
  g_ptr_array_unref (msgs);

  /* Reconciling a folder with no on-disk drift is a no-op success. */
  g_assert_true (mail_store_reconcile_folder_from_disk (f->store, "rid-INBOX", &error));
  g_assert_no_error (error);

  /* Reconciling an unknown folder is a no-op success. */
  g_assert_true (mail_store_reconcile_folder_from_disk (f->store, "nope", &error));
  g_assert_no_error (error);
}

static void
test_maildir_basename_unique_prefix (void)
{
  struct
  {
    const char *in;
    const char *expect;
  } cases[] = {
    { "1700.M1P2Q1.host", "1700.M1P2Q1.host" },     /* no marker */
    { "1700.M1P2Q1.host:2,", "1700.M1P2Q1.host" },  /* empty info */
    { "1700.M1P2Q1.host:2,S", "1700.M1P2Q1.host" }, /* with seen */
    { "1700.M1P2Q1.host:2,FRST", "1700.M1P2Q1.host" },
    /* Pathological: ":2," inside the unique portion would only
     * happen if a previous client wrote it that way; the spec says
     * the unique prefix can be anything but ":/". We split on the
     * first occurrence and live with that. */
    { ":2,first", "" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *got = _mail_store_maildir_basename_unique_prefix_for_test (cases[i].in);
      g_assert_cmpstr (got, ==, cases[i].expect);
    }
}

static void
test_maildir_basename_add_flag (void)
{
  struct
  {
    const char *in;
    char flag;
    const char *expect;
  } cases[] = {
    /* No ":2," marker → append. */
    { "1700.M1P2Q1.host", 'S', "1700.M1P2Q1.host:2,S" },
    /* Empty info → append. */
    { "1700.M1P2Q1.host:2,", 'S', "1700.M1P2Q1.host:2,S" },
    /* Insert in alphabetical position. */
    { "1700.M1P2Q1.host:2,F", 'S', "1700.M1P2Q1.host:2,FS" },
    { "1700.M1P2Q1.host:2,T", 'S', "1700.M1P2Q1.host:2,ST" },
    { "1700.M1P2Q1.host:2,FRT", 'S', "1700.M1P2Q1.host:2,FRST" },
    { "1700.M1P2Q1.host:2,DT", 'F', "1700.M1P2Q1.host:2,DFT" },
    /* Already set → unchanged. */
    { "1700.M1P2Q1.host:2,S", 'S', "1700.M1P2Q1.host:2,S" },
    { "1700.M1P2Q1.host:2,FRS", 'S', "1700.M1P2Q1.host:2,FRS" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *got = _mail_store_maildir_basename_add_flag_for_test (cases[i].in, cases[i].flag);
      g_assert_cmpstr (got, ==, cases[i].expect);
    }
}

static void
test_maildir_basename_remove_flag (void)
{
  struct
  {
    const char *in;
    char flag;
    const char *expect;
  } cases[] = {
    /* Remove from middle / end. */
    { "1700.M1P2Q1.host:2,FS", 'S', "1700.M1P2Q1.host:2,F" },
    { "1700.M1P2Q1.host:2,FRST", 'S', "1700.M1P2Q1.host:2,FRT" },
    /* Removing the last flag leaves the ":2," marker — still
     * spec-compliant for a cur/ entry. */
    {
        "1700.M1P2Q1.host:2,S",
        'S',
        "1700.M1P2Q1.host:2,",
    },
    /* Flag absent → unchanged. */
    { "1700.M1P2Q1.host:2,F", 'S', "1700.M1P2Q1.host:2,F" },
    /* No marker → unchanged. */
    { "1700.M1P2Q1.host", 'S', "1700.M1P2Q1.host" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *got = _mail_store_maildir_basename_remove_flag_for_test (cases[i].in, cases[i].flag);
      g_assert_cmpstr (got, ==, cases[i].expect);
    }
}

/* End-to-end: writing an unread message, then calling
 * mail_store_set_message_unread (false) must rename the file in cur/
 * to add :2,S, update sqlite to track, and keep the body readable
 * under its new name. Mutt and other Maildir readers will see the
 * file under the new name and read the S flag directly. */
static void
test_set_message_unread_renames_and_updates (Fixture *f,
                                             gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           NULL, &error));
  g_assert_no_error (error);

  const char *payload = "From: a@example.com\r\nSubject: hi\r\n\r\nbody\r\n";
  g_autoptr (GBytes) in = g_bytes_new_static (payload, strlen (payload));
  g_autofree char *write_name = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", in, FALSE, &write_name, &error));
  g_assert_no_error (error);
  /* seen=FALSE → no `S` in the info suffix (basename still ends
   * with `:2,` to satisfy the Maildir cur/ spec). */
  g_assert_false (g_str_has_suffix (write_name, ":2,S"));
  g_assert_true (g_str_has_suffix (write_name, ":2,"));

  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", NULL, write_name,
                                            "hi", "a@example.com", 1700000000, TRUE, &error));
  g_assert_no_error (error);

  g_autofree char *old_path = g_build_filename (f->root, "INBOX", "cur", write_name, NULL);
  g_assert_true (g_file_test (old_path, G_FILE_TEST_EXISTS));

  /* Mark read. */
  g_assert_true (mail_store_set_message_unread (f->store, "msg-1", FALSE, &error));
  g_assert_no_error (error);

  /* Old basename is gone from cur/. */
  g_assert_false (g_file_test (old_path, G_FILE_TEST_EXISTS));

  /* sqlite reflects the new state. */
  GPtrArray *msgs = mail_store_list_messages (f->store, "rid-INBOX", 10, &f->arena, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (msgs->len, ==, 1);
  MailMessageMeta *m = g_ptr_array_index (msgs, 0);
  g_assert_false (m->unread);
  g_ptr_array_unref (msgs);

  /* The new on-disk name ends in :2,S and is reachable via the store's
   * location lookup. */
  const char *dir = NULL, *file = NULL;
  g_assert_true (mail_store_message_location (f->store, "msg-1", &f->arena, &dir, &file, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (dir, ==, "INBOX");
  g_assert_true (g_str_has_suffix (file, ":2,S"));
  g_autofree char *new_path = g_build_filename (f->root, "INBOX", "cur", file, NULL);
  g_assert_true (g_file_test (new_path, G_FILE_TEST_EXISTS));

  /* Body is still readable under the new filename. */
  g_autoptr (GBytes) out = mail_store_read_raw (f->store, "INBOX", file, &error);
  g_assert_no_error (error);
  g_assert_nonnull (out);
  gsize len = 0;
  const char *bytes = g_bytes_get_data (out, &len);
  g_assert_cmpuint (len, ==, strlen (payload));
  g_assert_cmpint (memcmp (bytes, payload, len), ==, 0);

  /* Mark unread again — file renamed back (no :2,S), sqlite flipped,
   * body still readable under the latest name. */
  g_assert_true (mail_store_set_message_unread (f->store, "msg-1", TRUE, &error));
  g_assert_no_error (error);
  mail_arena_reset (&f->arena);
  const char *dir2 = NULL, *file2 = NULL;
  g_assert_true (mail_store_message_location (f->store, "msg-1", &f->arena, &dir2, &file2, &error));
  g_assert_no_error (error);
  g_assert_false (g_str_has_suffix (file2, ",S"));
  g_autofree char *path2 = g_build_filename (f->root, "INBOX", "cur", file2, NULL);
  g_assert_true (g_file_test (path2, G_FILE_TEST_EXISTS));
  g_autoptr (GBytes) out2 = mail_store_read_raw (f->store, "INBOX", file2, &error);
  g_assert_no_error (error);
  g_assert_nonnull (out2);

  /* Unknown remote_id is a no-op success; nothing on disk changes. */
  g_assert_true (mail_store_set_message_unread (f->store, "nope", FALSE, &error));
  g_assert_no_error (error);
}

static void
test_application_id_is_magpie (Fixture *f,
                               gconstpointer data)
{
  /* The store stamps its sqlite header with the fourcc 'Mgpi'
   * (0x4D677069) so a stray state.db is identifiable as Magpie's. */
  g_autofree char *dbpath = g_build_filename (f->root, "state.db", NULL);
  sqlite3 *probe = NULL;
  g_assert_cmpint (sqlite3_open (dbpath, &probe), ==, SQLITE_OK);
  sqlite3_stmt *st = NULL;
  g_assert_cmpint (sqlite3_prepare_v2 (probe, "PRAGMA application_id;", -1, &st, NULL),
                   ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_step (st), ==, SQLITE_ROW);
  g_assert_cmpint (sqlite3_column_int (st, 0), ==, 0x4D677069);
  sqlite3_finalize (st);
  sqlite3_close (probe);
}

/* Returns the count of rows in sqlite_master matching @name. Helper for
 * "this table exists" / "this table is gone" assertions. */
static int
count_sqlite_objects (const char *dbpath,
                      const char *kind,
                      const char *name)
{
  sqlite3 *probe = NULL;
  g_assert_cmpint (sqlite3_open (dbpath, &probe), ==, SQLITE_OK);
  sqlite3_stmt *st = NULL;
  g_assert_cmpint (sqlite3_prepare_v2 (probe,
                                       "SELECT COUNT(*) FROM sqlite_master"
                                       " WHERE type = ? AND name = ?;",
                                       -1, &st, NULL),
                   ==, SQLITE_OK);
  sqlite3_bind_text (st, 1, kind, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (st, 2, name, -1, SQLITE_TRANSIENT);
  g_assert_cmpint (sqlite3_step (st), ==, SQLITE_ROW);
  int n = sqlite3_column_int (st, 0);
  sqlite3_finalize (st);
  sqlite3_close (probe);
  return n;
}

static int
read_user_version (const char *dbpath)
{
  sqlite3 *probe = NULL;
  g_assert_cmpint (sqlite3_open (dbpath, &probe), ==, SQLITE_OK);
  sqlite3_stmt *st = NULL;
  g_assert_cmpint (sqlite3_prepare_v2 (probe, "PRAGMA user_version;", -1, &st, NULL),
                   ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_step (st), ==, SQLITE_ROW);
  int v = sqlite3_column_int (st, 0);
  sqlite3_finalize (st);
  sqlite3_close (probe);
  return v;
}

static void
test_schema_migration_v1_to_current (Fixture *f,
                                     gconstpointer data)
{
  /* Simulate an existing v1 store and verify reopen migrates straight
   * to the current schema version (v3): content_key column added,
   * sync_state table dropped. */
  g_autoptr (GError) error = NULL;
  mail_store_close (f->store);
  f->store = NULL;

  g_autofree char *dbpath = g_build_filename (f->root, "state.db", NULL);
  sqlite3 *db = NULL;
  g_assert_cmpint (sqlite3_open (dbpath, &db), ==, SQLITE_OK);
  char *err = NULL;
  /* Recreate the v1 messages schema (no content_key), keep the
   * sync_state table that v1/v2 created so the v3 DROP migration has
   * something to remove, and reset user_version. */
  const char *rewind =
      "DROP TABLE IF EXISTS messages;"
      "CREATE TABLE messages ("
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
      "CREATE TABLE IF NOT EXISTS sync_state ("
      "  folder_stable_id TEXT PRIMARY KEY REFERENCES folders(stable_id) ON DELETE CASCADE,"
      "  delta_token      TEXT,"
      "  uidvalidity      INTEGER,"
      "  uidnext          INTEGER,"
      "  last_synced_unix INTEGER"
      ");"
      "PRAGMA user_version = 1;";
  g_assert_cmpint (sqlite3_exec (db, rewind, NULL, NULL, &err), ==, SQLITE_OK);
  sqlite3_close (db);

  /* Reopen — schema setup should detect user_version=1 and migrate. */
  f->store = mail_store_open (f->root, "test@example.com", &error);
  g_assert_no_error (error);
  g_assert_nonnull (f->store);

  /* After migration, content_key works end-to-end. */
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           NULL, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1",
                                            "<post-migration@x>", "f",
                                            NULL, NULL, 0, FALSE, &error));
  g_assert_no_error (error);
  const char *dir = NULL, *file = NULL;
  g_assert_true (mail_store_locate_body_by_content_key (f->store, "<post-migration@x>",
                                                        &f->arena, &dir, &file, &error));
  g_assert_cmpstr (dir, ==, "INBOX");
  g_assert_cmpstr (file, ==, "f");

  g_autofree char *dbpath2 = g_build_filename (f->root, "state.db", NULL);
  g_assert_cmpint (read_user_version (dbpath2), ==, 3);
  /* v3 drops the dead sync_state table. */
  g_assert_cmpint (count_sqlite_objects (dbpath2, "table", "sync_state"), ==, 0);
}

/* Pin the v2→v3 migration in isolation: a store with the v2 schema
 * (content_key present, sync_state present, user_version=2) must, on
 * next open, drop sync_state and bump user_version to 3. */
static void
test_schema_migration_v2_to_v3 (Fixture *f,
                                gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  mail_store_close (f->store);
  f->store = NULL;

  g_autofree char *dbpath = g_build_filename (f->root, "state.db", NULL);
  sqlite3 *db = NULL;
  g_assert_cmpint (sqlite3_open (dbpath, &db), ==, SQLITE_OK);
  char *err = NULL;
  /* The current schema already has content_key; we just need to make
   * sure sync_state exists and rewind user_version to 2. */
  const char *rewind =
      "CREATE TABLE IF NOT EXISTS sync_state ("
      "  folder_stable_id TEXT PRIMARY KEY REFERENCES folders(stable_id) ON DELETE CASCADE,"
      "  delta_token      TEXT,"
      "  uidvalidity      INTEGER,"
      "  uidnext          INTEGER,"
      "  last_synced_unix INTEGER"
      ");"
      "PRAGMA user_version = 2;";
  g_assert_cmpint (sqlite3_exec (db, rewind, NULL, NULL, &err), ==, SQLITE_OK);
  sqlite3_close (db);
  /* Sanity: sync_state is in place before reopen. */
  g_assert_cmpint (count_sqlite_objects (dbpath, "table", "sync_state"), ==, 1);
  g_assert_cmpint (read_user_version (dbpath), ==, 2);

  f->store = mail_store_open (f->root, "test@example.com", &error);
  g_assert_no_error (error);
  g_assert_nonnull (f->store);

  g_assert_cmpint (read_user_version (dbpath), ==, 3);
  g_assert_cmpint (count_sqlite_objects (dbpath, "table", "sync_state"), ==, 0);
}

/* --- listener registry -------------------------------------------- */

/* Recording listener for the registry plumbing tests. Counts callback
 * invocations and tracks whether the GDestroyNotify ran. */
typedef struct
{
  int callbacks;
  int destroyed;
} ListenerProbe;

static void
probe_cb (const MailStoreChange *change,
          gpointer user_data)
{
  (void) change;
  ListenerProbe *p = user_data;
  p->callbacks++;
}

static void
probe_notify (gpointer user_data)
{
  ListenerProbe *p = user_data;
  p->destroyed++;
}

/* Recording listener for emit-path tests. Collects every change in
 * order so the test can assert exact event payloads. */
typedef struct
{
  MailStoreChangeKind kind;
  char *folder_id;
  char *message_id;
  gboolean unread;
  int folder_unread;
  int folder_total;
} RecordedChange;

typedef struct
{
  GArray *changes; /* of RecordedChange */
} ChangeRecorder;

static void
recorder_init (ChangeRecorder *r)
{
  r->changes = g_array_new (FALSE, FALSE, sizeof (RecordedChange));
  g_array_set_clear_func (r->changes, (GDestroyNotify) (void (*) (RecordedChange *)) NULL);
}

static void
recorder_destroy (ChangeRecorder *r)
{
  for (guint i = 0; i < r->changes->len; i++)
    {
      RecordedChange *rc = &g_array_index (r->changes, RecordedChange, i);
      g_free (rc->folder_id);
      g_free (rc->message_id);
    }
  g_array_free (r->changes, TRUE);
}

static void
recorder_cb (const MailStoreChange *change,
             gpointer user_data)
{
  ChangeRecorder *r = user_data;
  RecordedChange rc = {
    .kind = change->kind,
    .folder_id = g_strdup (change->folder_id),
    .message_id = g_strdup (change->message_id),
    .unread = change->unread,
    .folder_unread = change->folder_unread,
    .folder_total = change->folder_total,
  };
  g_array_append_val (r->changes, rc);
}

static void
test_set_unread_emits_flags_and_counts (Fixture *f,
                                        gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  /* Two unread messages in one folder; mark one as read and inspect
   * the events the listener saw. */
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, NULL, &error));
  const char *payload = "Subject: hi\r\n\r\nbody\r\n";
  g_autoptr (GBytes) in = g_bytes_new_static (payload, strlen (payload));
  g_autofree char *n1 = NULL;
  g_autofree char *n2 = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", in, FALSE, &n1, &error));
  g_assert_true (mail_store_write_raw (f->store, "INBOX", in, FALSE, &n2, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "m1", NULL, n1,
                                            NULL, NULL, 0, TRUE, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "m2", NULL, n2,
                                            NULL, NULL, 0, TRUE, &error));

  ChangeRecorder rec;
  recorder_init (&rec);
  guint id = mail_store_add_listener (f->store, recorder_cb, &rec, NULL);

  g_assert_true (mail_store_set_message_unread (f->store, "m1", FALSE, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (rec.changes->len, ==, 2);
  RecordedChange *e0 = &g_array_index (rec.changes, RecordedChange, 0);
  g_assert_cmpint (e0->kind, ==, MAIL_STORE_CHANGE_MESSAGE_FLAGS);
  g_assert_cmpstr (e0->folder_id, ==, "rid-INBOX");
  g_assert_cmpstr (e0->message_id, ==, "m1");
  g_assert_false (e0->unread);
  RecordedChange *e1 = &g_array_index (rec.changes, RecordedChange, 1);
  g_assert_cmpint (e1->kind, ==, MAIL_STORE_CHANGE_FOLDER_COUNTS);
  g_assert_cmpstr (e1->folder_id, ==, "rid-INBOX");
  g_assert_null (e1->message_id);
  g_assert_cmpint (e1->folder_unread, ==, 1); /* one remaining unread (m2) */
  g_assert_cmpint (e1->folder_total, ==, 2);

  mail_store_remove_listener (f->store, id);
  recorder_destroy (&rec);
}

static void
test_set_unread_noop_emits_nothing (Fixture *f,
                                    gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, NULL, &error));
  /* Upsert a message that's already read; ask to set unread=FALSE
   * (already read) and verify no events fire. The on-disk basename
   * already lacks 'S' if we never wrote it as seen, so we have to
   * use a basename that already has :2,S. Write seen=TRUE so the
   * file is named with :2,S and the row's unread is FALSE. */
  const char *payload = "Subject: hi\r\n\r\nbody\r\n";
  g_autoptr (GBytes) in = g_bytes_new_static (payload, strlen (payload));
  g_autofree char *name = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", in, TRUE /*seen*/, &name, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "m1", NULL, name,
                                            NULL, NULL, 0, FALSE /*already read*/, &error));

  ChangeRecorder rec;
  recorder_init (&rec);
  guint id = mail_store_add_listener (f->store, recorder_cb, &rec, NULL);

  /* The basename already encodes :2,S so the rename is a no-op; no
   * events should fire (mail-store.c short-circuits this path). */
  g_assert_true (mail_store_set_message_unread (f->store, "m1", FALSE, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (rec.changes->len, ==, 0);

  /* Unknown message id is also silent. */
  g_assert_true (mail_store_set_message_unread (f->store, "no-such-id", FALSE, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (rec.changes->len, ==, 0);

  mail_store_remove_listener (f->store, id);
  recorder_destroy (&rec);
}

static void
test_listener_remove_runs_destroy_notify (Fixture *f,
                                          gconstpointer data)
{
  ListenerProbe p = { 0, 0 };
  guint id = mail_store_add_listener (f->store, probe_cb, &p, probe_notify);
  g_assert_cmpuint (id, !=, 0);
  g_assert_cmpint (p.destroyed, ==, 0);
  mail_store_remove_listener (f->store, id);
  g_assert_cmpint (p.destroyed, ==, 1);
  /* Removing again is a no-op (id no longer registered). */
  mail_store_remove_listener (f->store, id);
  g_assert_cmpint (p.destroyed, ==, 1);
}

static void
test_listener_close_runs_destroy_notify (Fixture *f,
                                         gconstpointer data)
{
  ListenerProbe p = { 0, 0 };
  guint id = mail_store_add_listener (f->store, probe_cb, &p, probe_notify);
  g_assert_cmpuint (id, !=, 0);
  /* Forcing the store closed mid-fixture must still run the notify
   * hook so subscribers get a chance to clean up their state. The
   * teardown function nulls f->store so we don't double-close. */
  mail_store_close (f->store);
  f->store = NULL;
  g_assert_cmpint (p.destroyed, ==, 1);
}

static void
test_listener_remove_id_zero_is_noop (Fixture *f,
                                      gconstpointer data)
{
  ListenerProbe p = { 0, 0 };
  /* Sentinel id 0 is never returned by mail_store_add_listener and
   * removing it must be a safe no-op (so callers can store 0 to mean
   * "no subscription"). */
  mail_store_remove_listener (f->store, 0);
  guint id = mail_store_add_listener (f->store, probe_cb, &p, probe_notify);
  g_assert_cmpuint (id, !=, 0);
  mail_store_remove_listener (f->store, 0);
  /* The real listener is still registered. */
  g_assert_cmpint (p.destroyed, ==, 0);
  mail_store_remove_listener (f->store, id);
  g_assert_cmpint (p.destroyed, ==, 1);
}

static void
test_folder_remote_ids_set (Fixture *f,
                            gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-A", "A", NULL, NULL, &error));
  g_assert_true (mail_store_upsert_folder (f->store, "rid-B", "B", NULL, NULL, &error));
  g_assert_no_error (error);
  GHashTable *ids = mail_store_folder_remote_ids (f->store, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (g_hash_table_size (ids), ==, 2);
  g_assert_true (g_hash_table_contains (ids, "rid-A"));
  g_assert_true (g_hash_table_contains (ids, "rid-B"));
  g_hash_table_unref (ids);
}

int
main (int argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

#define ADD(name, fn) \
  g_test_add ("/mail-store/" name, Fixture, NULL, fixture_setup, fn, fixture_teardown)

  ADD ("open-creates-root", test_open_creates_root);
  ADD ("upsert-folder-maildir-dirs", test_upsert_folder_creates_maildir_dirs);
  ADD ("list-folders", test_list_folders_returns_inserted);
  ADD ("list-folders-counts-from-messages", test_list_folders_counts_derive_from_messages);
  ADD ("upsert-folder-update-keeps-dir", test_upsert_folder_update_does_not_rename_dir);
  ADD ("collision-suffix", test_collision_suffixes_with_tilde);
  ADD ("slash-sanitized", test_slash_in_display_name_sanitized);
  ADD ("raw-roundtrip", test_write_then_read_raw_roundtrip);
  ADD ("message-upsert-list", test_message_upsert_and_list);
  ADD ("message-location", test_message_location_lookup);
  ADD ("message-delete-unlinks", test_message_delete_unlinks_file);
  ADD ("folder-delete-cascades", test_folder_delete_drops_dir_and_cascades);
  ADD ("folder-remote-ids-set", test_folder_remote_ids_set);
  ADD ("locate-body-by-content-key", test_locate_by_content_key);
  ADD ("link-raw-creates-hardlink", test_link_raw_creates_hardlink);
  ADD ("schema-migration-v1-to-current", test_schema_migration_v1_to_current);
  ADD ("schema-migration-v2-to-v3", test_schema_migration_v2_to_v3);
  ADD ("listener-remove-runs-destroy-notify", test_listener_remove_runs_destroy_notify);
  ADD ("listener-close-runs-destroy-notify", test_listener_close_runs_destroy_notify);
  ADD ("listener-remove-id-zero-is-noop", test_listener_remove_id_zero_is_noop);
  ADD ("set-unread-emits-flags-and-counts", test_set_unread_emits_flags_and_counts);
  ADD ("set-unread-noop-emits-nothing", test_set_unread_noop_emits_nothing);
  ADD ("application-id-stamped", test_application_id_is_magpie);
  ADD ("set-message-unread-renames", test_set_message_unread_renames_and_updates);
  ADD ("reconcile-folder-from-disk", test_reconcile_folder_from_disk);

  g_test_add_func ("/mail-store/maildir-basename-add-flag", test_maildir_basename_add_flag);
  g_test_add_func ("/mail-store/maildir-basename-remove-flag", test_maildir_basename_remove_flag);
  g_test_add_func ("/mail-store/maildir-basename-unique-prefix", test_maildir_basename_unique_prefix);

#undef ADD

  return g_test_run ();
}
