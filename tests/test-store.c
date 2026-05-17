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
#include <string.h>

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
  g_assert_cmpint (first->unread_count, ==, 3);
  g_assert_cmpint (first->total_count, ==, 42);
  g_assert_cmpstr (first->id, ==, "rid-INBOX");
  g_assert_cmpstr (second->display_name, ==, "Sent");
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
  g_assert_cmpint (only->unread_count, ==, 5);
  g_assert_cmpint (only->total_count, ==, 50);
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

  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", "file-1",
                                            "Hello", "alice@example.com",
                                            1700000000, TRUE, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-2", "file-2",
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
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", "filename.eml",
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
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", filename,
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
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", "msg-1", "f", NULL,
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
  ADD ("upsert-folder-update-keeps-dir", test_upsert_folder_update_does_not_rename_dir);
  ADD ("collision-suffix", test_collision_suffixes_with_tilde);
  ADD ("slash-sanitized", test_slash_in_display_name_sanitized);
  ADD ("raw-roundtrip", test_write_then_read_raw_roundtrip);
  ADD ("message-upsert-list", test_message_upsert_and_list);
  ADD ("message-location", test_message_location_lookup);
  ADD ("message-delete-unlinks", test_message_delete_unlinks_file);
  ADD ("folder-delete-cascades", test_folder_delete_drops_dir_and_cascades);
  ADD ("folder-remote-ids-set", test_folder_remote_ids_set);

#undef ADD

  return g_test_run ();
}
