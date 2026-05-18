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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           3, 42, &dir_name, &error));
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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           3, 42, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (mail_store_upsert_folder (f->store, "rid-Sent", "Sent", NULL,
                                           0, 99, NULL, &error));
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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           999, 999, NULL, &error));
  g_assert_no_error (error);

  /* Two unread, one read. Local truth: unread=2, total=3. */
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-u1", NULL, "f1",
                                            "S1", "a@x", 1700000000, TRUE, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-u2", NULL, "f2",
                                            "S2", "a@x", 1700000001, TRUE, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-r1", NULL, "f3",
                                            "S3", "a@x", 1700000002, FALSE, NULL, &error));
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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL,
                                           3, 42, &dir1, &error));
  g_assert_no_error (error);
  g_autofree char *dir1_copy = g_strdup (dir1);
  /* Update counts: dir_name should stay. */
  const char *dir2 = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX (renamed)", NULL,
                                           5, 50, &dir2, &error));
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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-1", "INBOX", NULL, 0, 0, &d1, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (d1, ==, "INBOX");
  g_assert_true (mail_store_upsert_folder (f->store, "rid-2", "INBOX", NULL, 0, 0, &d2, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (d2, ==, "INBOX~2");
  g_assert_true (mail_store_upsert_folder (f->store, "rid-3", "INBOX", NULL, 0, 0, &d3, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (d3, ==, "INBOX~3");
}

static void
test_slash_in_display_name_sanitized (Fixture *f,
                                      gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  const char *dir_name = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-X", "foo/bar", NULL, 0, 0,
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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, 0, 0,
                                           NULL, &error));
  g_assert_no_error (error);

  const char *payload = "From: a@example.com\r\nSubject: hi\r\n\r\nbody\r\n";
  g_autoptr (GBytes) in = g_bytes_new_static (payload, strlen (payload));
  g_autofree char *filename = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", in, TRUE, &filename, &error));
  g_assert_no_error (error);
  g_assert_nonnull (filename);
  /* File should be in cur/ with :2,S suffix because seen=TRUE. */
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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, 0, 0,
                                           NULL, &error));
  g_assert_no_error (error);

  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", NULL, "file-1",
                                            "Hello", "alice@example.com",
                                            1700000000, TRUE, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-2", NULL, "file-2",
                                            "Hi", "bob@example.com",
                                            1700000050, FALSE, NULL, &error));
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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, 0, 0,
                                           NULL, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", NULL, "filename.eml",
                                            NULL, NULL, 0, FALSE, NULL, &error));
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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, 0, 0,
                                           NULL, &error));
  /* Write a real file and link a message to it. */
  const char *payload = "hi";
  g_autoptr (GBytes) in = g_bytes_new_static (payload, strlen (payload));
  g_autofree char *filename = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", in, FALSE, &filename, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", NULL, filename,
                                            NULL, NULL, 0, FALSE, NULL, &error));
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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, 0, 0,
                                           NULL, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", NULL, "f", NULL,
                                            NULL, 0, FALSE, NULL, &error));
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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, 0, 0,
                                           NULL, &error));
  g_assert_true (mail_store_upsert_folder (f->store, "rid-LABEL", "Travel", NULL, 0, 0,
                                           NULL, &error));

  const char *payload = "From: a@b\r\n\r\nshared body\r\n";
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, strlen (payload));
  g_autofree char *inbox_name = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", bytes, FALSE,
                                       &inbox_name, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1",
                                            "<mid@example.com>", inbox_name,
                                            "shared", "a@b", 100, FALSE, NULL, &error));
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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, 0, 0,
                                           NULL, &error));
  g_assert_true (mail_store_upsert_folder (f->store, "rid-LABEL", "Travel", NULL, 0, 0,
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
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, 0, 0,
                                           NULL, &error));
  g_assert_no_error (error);

  const char *payload = "From: a@example.com\r\nSubject: hi\r\n\r\nbody\r\n";
  g_autoptr (GBytes) in = g_bytes_new_static (payload, strlen (payload));
  g_autofree char *write_name = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", in, FALSE, &write_name, &error));
  g_assert_no_error (error);
  /* seen=FALSE → bare basename, no :2,S. */
  g_assert_false (g_str_has_suffix (write_name, ":2,S"));

  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", NULL, write_name,
                                            "hi", "a@example.com", 1700000000, TRUE,
                                            NULL, &error));
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

static void
test_schema_migration_v1_to_v2 (Fixture *f,
                                gconstpointer data)
{
  /* Simulate an existing v1 store by reaching into the sqlite db and
   * dropping the content_key column + bumping user_version back to 1.
   * Reopening the store must run the migration and produce a working
   * content_key column. */
  g_autoptr (GError) error = NULL;
  mail_store_close (f->store);
  f->store = NULL;

  g_autofree char *dbpath = g_build_filename (f->root, "state.db", NULL);
  sqlite3 *db = NULL;
  g_assert_cmpint (sqlite3_open (dbpath, &db), ==, SQLITE_OK);
  char *err = NULL;
  /* Recreate the v1 messages schema (no content_key) and reset
   * user_version. SQLite ALTER TABLE can't drop a column on older
   * versions, so DROP + recreate is easier here. */
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
      "PRAGMA user_version = 1;";
  g_assert_cmpint (sqlite3_exec (db, rewind, NULL, NULL, &err), ==, SQLITE_OK);
  sqlite3_close (db);

  /* Reopen — schema setup should detect user_version=1 and migrate. */
  f->store = mail_store_open (f->root, "test@example.com", &error);
  g_assert_no_error (error);
  g_assert_nonnull (f->store);

  /* After migration, content_key works end-to-end. */
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX", NULL, 0, 0,
                                           NULL, &error));
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1",
                                            "<post-migration@x>", "f",
                                            NULL, NULL, 0, FALSE, NULL, &error));
  g_assert_no_error (error);
  const char *dir = NULL, *file = NULL;
  g_assert_true (mail_store_locate_body_by_content_key (f->store, "<post-migration@x>",
                                                        &f->arena, &dir, &file, &error));
  g_assert_cmpstr (dir, ==, "INBOX");
  g_assert_cmpstr (file, ==, "f");

  /* PRAGMA user_version is 2 now. */
  g_autofree char *dbpath2 = g_build_filename (f->root, "state.db", NULL);
  sqlite3 *probe = NULL;
  g_assert_cmpint (sqlite3_open (dbpath2, &probe), ==, SQLITE_OK);
  sqlite3_stmt *st = NULL;
  g_assert_cmpint (sqlite3_prepare_v2 (probe, "PRAGMA user_version;", -1, &st, NULL),
                   ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_step (st), ==, SQLITE_ROW);
  g_assert_cmpint (sqlite3_column_int (st, 0), ==, 2);
  sqlite3_finalize (st);
  sqlite3_close (probe);
}

static void
test_folder_remote_ids_set (Fixture *f,
                            gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "rid-A", "A", NULL, 0, 0, NULL, &error));
  g_assert_true (mail_store_upsert_folder (f->store, "rid-B", "B", NULL, 0, 0, NULL, &error));
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
  ADD ("schema-migration-v1-to-v2", test_schema_migration_v1_to_v2);
  ADD ("application-id-stamped", test_application_id_is_magpie);
  ADD ("set-message-unread-renames", test_set_message_unread_renames_and_updates);

  g_test_add_func ("/mail-store/maildir-basename-add-flag", test_maildir_basename_add_flag);
  g_test_add_func ("/mail-store/maildir-basename-remove-flag", test_maildir_basename_remove_flag);

#undef ADD

  return g_test_run ();
}
