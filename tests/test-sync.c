/* tests/test-sync.c - MailSync against mail-backend-fake as remote source. */

#include "mail-backend-fake.h"

#include "../src/mail-arena.h"
#include "../src/mail-backend.h"
#include "../src/mail-store.h"
#include "../src/mail-sync.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static void
pump (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

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

typedef struct
{
  gboolean done;
  gboolean ok;
  GError *error;
} CompletionState;

static void
on_done (GObject *src,
         GAsyncResult *res,
         gpointer user_data)
{
  CompletionState *s = user_data;
  s->ok = mail_sync_run_finish (MAIL_SYNC (src), res, &s->error);
  s->done = TRUE;
}

static void
on_progress_notify (GObject *src,
                    GParamSpec *pspec,
                    gpointer user_data)
{
  GArray *log = user_data;
  double v = mail_sync_get_progress (MAIL_SYNC (src));
  g_array_append_val (log, v);
}

static MailBackend *
build_seeded_remote (void)
{
  MailBackend *remote = mail_backend_fake_new ();
  FakeFolderSpec folders[] = {
    { "f-inbox", "Inbox", NULL, 2, 3 },
    { "f-sent", "Sent", NULL, 0, 1 },
  };
  FakeMessageSpec inbox[] = {
    { "i1", "Hello", "alice@example.com", 1700000000, FALSE, "Body 1" },
    { "i2", "Hi", "bob@example.com", 1700000100, TRUE, "Body 2" },
    { "i3", "Hey", "carol@example.com", 1700000200, TRUE, "Body 3" },
  };
  FakeMessageSpec sent[] = {
    { "s1", "Re: Hello", "me@example.com", 1700000050, FALSE, "Body S1" },
  };
  mail_backend_fake_set_folders (remote, folders, G_N_ELEMENTS (folders));
  mail_backend_fake_set_messages (remote, "f-inbox", inbox, G_N_ELEMENTS (inbox));
  mail_backend_fake_set_messages (remote, "f-sent", sent, G_N_ELEMENTS (sent));
  return remote;
}

static void
run_to_completion (MailSync *sync,
                   MailBackend *remote,
                   MailStore *local,
                   GArray *progress_log)
{
  CompletionState s = { 0 };
  gulong sig = 0;
  if (progress_log != NULL)
    sig = g_signal_connect (sync, "notify::progress",
                            G_CALLBACK (on_progress_notify), progress_log);
  mail_sync_run_async (sync, remote, local, NULL, on_done, &s);
  while (!s.done)
    pump ();
  if (sig != 0)
    g_signal_handler_disconnect (sync, sig);
  g_assert_no_error (s.error);
  g_assert_true (s.ok);
}

/* --- tests -------------------------------------------------------- */

static void
test_first_pass_populates_store (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("mail-sync-XXXXXX", &error);
  g_assert_no_error (error);
  MailStore *local = mail_store_open (root, "u@example.com", &error);
  g_assert_no_error (error);
  MailBackend *remote = build_seeded_remote ();
  MailSync *sync = mail_sync_new ();

  GArray *progress = g_array_new (FALSE, FALSE, sizeof (double));
  run_to_completion (sync, remote, local, progress);

  /* Folders ended up in the store. */
  GHashTable *fids = mail_store_folder_remote_ids (local, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (g_hash_table_size (fids), ==, 2);
  g_assert_true (g_hash_table_contains (fids, "f-inbox"));
  g_assert_true (g_hash_table_contains (fids, "f-sent"));
  g_hash_table_unref (fids);

  /* Inbox has 3 messages, Sent has 1. */
  GHashTable *imids = mail_store_message_remote_ids (local, "f-inbox", &error);
  g_assert_no_error (error);
  g_assert_cmpuint (g_hash_table_size (imids), ==, 3);
  g_hash_table_unref (imids);
  GHashTable *smids = mail_store_message_remote_ids (local, "f-sent", &error);
  g_assert_cmpuint (g_hash_table_size (smids), ==, 1);
  g_hash_table_unref (smids);

  /* Raw bytes round-trip via the store. */
  MailArena tmp;
  mail_arena_init (&tmp, 256);
  const char *dir = NULL, *file = NULL;
  g_assert_true (mail_store_message_location (local, "i1", &tmp, &dir, &file, &error));
  g_autoptr (GBytes) body = mail_store_read_raw (local, dir, file, &error);
  g_assert_no_error (error);
  gsize len = 0;
  const char *bytes = g_bytes_get_data (body, &len);
  g_assert_cmpmem (bytes, len, "Body 1", strlen ("Body 1"));
  mail_arena_destroy (&tmp);

  /* Progress: monotonic non-decreasing, ending at exactly 1.0. */
  g_assert_cmpuint (progress->len, >, 0);
  double last = 0.0;
  for (guint i = 0; i < progress->len; i++)
    {
      double v = g_array_index (progress, double, i);
      g_assert_cmpfloat (v, >=, last);
      last = v;
    }
  g_assert_cmpfloat (last, ==, 1.0);
  g_array_unref (progress);

  /* Not running anymore. */
  g_assert_false (mail_sync_is_running (sync));

  g_object_unref (sync);
  mail_backend_destroy (remote);
  mail_store_close (local);
  rm_rf (root);
}

static void
test_second_pass_is_noop (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("mail-sync-XXXXXX", &error);
  MailStore *local = mail_store_open (root, "u@example.com", &error);
  MailBackend *remote = build_seeded_remote ();
  MailSync *sync = mail_sync_new ();

  run_to_completion (sync, remote, local, NULL);
  guint fetch_calls_after_first = mail_backend_fake_fetch_raw_calls (remote);
  g_assert_cmpuint (fetch_calls_after_first, ==, 4); /* 3 inbox + 1 sent */

  /* Re-run with the same fake; nothing new on the remote. */
  run_to_completion (sync, remote, local, NULL);
  guint fetch_calls_after_second = mail_backend_fake_fetch_raw_calls (remote);
  g_assert_cmpuint (fetch_calls_after_second, ==, 4); /* no new fetches */

  g_object_unref (sync);
  mail_backend_destroy (remote);
  mail_store_close (local);
  rm_rf (root);
}

static void
test_removed_messages_get_deleted_locally (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("mail-sync-XXXXXX", &error);
  MailStore *local = mail_store_open (root, "u@example.com", &error);
  MailBackend *remote = build_seeded_remote ();
  MailSync *sync = mail_sync_new ();

  run_to_completion (sync, remote, local, NULL);

  /* Remove i2 from the remote and re-sync. */
  FakeMessageSpec shrunken[] = {
    { "i1", "Hello", "alice@example.com", 1700000000, FALSE, "Body 1" },
    { "i3", "Hey", "carol@example.com", 1700000200, TRUE, "Body 3" },
  };
  mail_backend_fake_set_messages (remote, "f-inbox", shrunken, G_N_ELEMENTS (shrunken));

  run_to_completion (sync, remote, local, NULL);

  GHashTable *ids = mail_store_message_remote_ids (local, "f-inbox", &error);
  g_assert_cmpuint (g_hash_table_size (ids), ==, 2);
  g_assert_false (g_hash_table_contains (ids, "i2"));
  g_assert_true (g_hash_table_contains (ids, "i1"));
  g_assert_true (g_hash_table_contains (ids, "i3"));
  g_hash_table_unref (ids);

  g_object_unref (sync);
  mail_backend_destroy (remote);
  mail_store_close (local);
  rm_rf (root);
}

static void
test_removed_folder_gets_deleted_locally (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("mail-sync-XXXXXX", &error);
  MailStore *local = mail_store_open (root, "u@example.com", &error);
  MailBackend *remote = build_seeded_remote ();
  MailSync *sync = mail_sync_new ();

  run_to_completion (sync, remote, local, NULL);

  /* Remote now reports only Inbox. */
  FakeFolderSpec just_inbox[] = {
    { "f-inbox", "Inbox", NULL, 2, 3 },
  };
  mail_backend_fake_set_folders (remote, just_inbox, 1);

  run_to_completion (sync, remote, local, NULL);

  GHashTable *fids = mail_store_folder_remote_ids (local, &error);
  g_assert_cmpuint (g_hash_table_size (fids), ==, 1);
  g_assert_true (g_hash_table_contains (fids, "f-inbox"));
  g_hash_table_unref (fids);
  /* Sent's directory is gone too. */
  g_autofree char *sent_path = g_build_filename (root, "Sent", NULL);
  g_assert_false (g_file_test (sent_path, G_FILE_TEST_EXISTS));

  g_object_unref (sync);
  mail_backend_destroy (remote);
  mail_store_close (local);
  rm_rf (root);
}

static void
test_empty_remote_still_succeeds (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("mail-sync-XXXXXX", &error);
  MailStore *local = mail_store_open (root, "u@example.com", &error);
  MailBackend *remote = mail_backend_fake_new ();
  MailSync *sync = mail_sync_new ();

  GArray *progress = g_array_new (FALSE, FALSE, sizeof (double));
  run_to_completion (sync, remote, local, progress);
  /* Should still hit 1.0. */
  g_assert_cmpfloat (g_array_index (progress, double, progress->len - 1), ==, 1.0);
  g_array_unref (progress);

  g_object_unref (sync);
  mail_backend_destroy (remote);
  mail_store_close (local);
  rm_rf (root);
}

static void
test_large_folder_syncs_all_messages (void)
{
  /* Regression: a hard-coded MAIL_SYNC_TOP_N = 200 cap used to limit
   * how many messages sync wrote per folder per pass, so real inboxes
   * with thousands of messages were permanently missing tail history.
   * The fix removes the constant and passes G_MAXINT to the backend's
   * list_messages_async, relying on backend pagination to fetch
   * everything. Seed 1000 messages and assert all 1000 land. */
  enum
  {
    N = 1000
  };
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("mail-sync-XXXXXX", &error);
  g_assert_no_error (error);
  MailStore *local = mail_store_open (root, "u@example.com", &error);
  g_assert_no_error (error);

  MailBackend *remote = mail_backend_fake_new ();
  FakeFolderSpec folders[] = {
    { "f-big", "Big", NULL, 0, N },
  };
  mail_backend_fake_set_folders (remote, folders, G_N_ELEMENTS (folders));

  GArray *specs = g_array_new (FALSE, FALSE, sizeof (FakeMessageSpec));
  GPtrArray *strings = g_ptr_array_new_with_free_func (g_free);
  for (int i = 0; i < N; i++)
    {
      char *id = g_strdup_printf ("m%04d", i);
      char *subj = g_strdup_printf ("Subject %04d", i);
      char *body = g_strdup_printf ("Body %04d", i);
      g_ptr_array_add (strings, id);
      g_ptr_array_add (strings, subj);
      g_ptr_array_add (strings, body);
      FakeMessageSpec spec = {
        .id = id,
        .subject = subj,
        .from = "a@b.c",
        .received_unix = 1700000000 + i,
        .unread = FALSE,
        .raw_rfc822 = body,
      };
      g_array_append_val (specs, spec);
    }
  mail_backend_fake_set_messages (remote, "f-big",
                                  (FakeMessageSpec *) specs->data, specs->len);

  MailSync *sync = mail_sync_new ();
  run_to_completion (sync, remote, local, NULL);

  GHashTable *mids = mail_store_message_remote_ids (local, "f-big", &error);
  g_assert_no_error (error);
  g_assert_cmpuint (g_hash_table_size (mids), ==, N);
  g_hash_table_unref (mids);

  g_object_unref (sync);
  mail_backend_destroy (remote);
  mail_store_close (local);
  rm_rf (root);
  g_array_unref (specs);
  g_ptr_array_unref (strings);
}

static void
test_cancel_sets_terminal_status (void)
{
  /* Regression: clicking Cancel mid-sync used to leave the page
   * stuck on the last in-flight status string ("Downloading messages
   * (2098 / 2926)…") because finish_pass didn't update :status on
   * error. The fix overrides :status to "Canceled." (or "Sync
   * failed." for non-cancel errors) before set_running(FALSE) so
   * observers see a terminal string. */
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("mail-sync-XXXXXX", &error);
  MailStore *local = mail_store_open (root, "u@example.com", &error);
  MailBackend *remote = build_seeded_remote ();
  MailSync *sync = mail_sync_new ();

  CompletionState s = { 0 };
  GCancellable *cancel = g_cancellable_new ();
  mail_sync_run_async (sync, remote, local, cancel, on_done, &s);

  /* Cancel immediately — the pass will unwind through finish_pass(error). */
  g_cancellable_cancel (cancel);
  while (!s.done)
    pump ();

  g_assert_true (g_error_matches (s.error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
  g_assert_false (s.ok);
  g_assert_cmpstr (mail_sync_get_status (sync), ==, "Canceled.");

  g_clear_error (&s.error);
  g_object_unref (cancel);
  g_object_unref (sync);
  mail_backend_destroy (remote);
  mail_store_close (local);
  rm_rf (root);
}

int
main (int argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/mail-sync/first-pass", test_first_pass_populates_store);
  g_test_add_func ("/mail-sync/second-pass-noop", test_second_pass_is_noop);
  g_test_add_func ("/mail-sync/removed-messages-deleted", test_removed_messages_get_deleted_locally);
  g_test_add_func ("/mail-sync/removed-folder-deleted", test_removed_folder_gets_deleted_locally);
  g_test_add_func ("/mail-sync/empty-remote", test_empty_remote_still_succeeds);
  g_test_add_func ("/mail-sync/large-folder-syncs-all", test_large_folder_syncs_all_messages);
  g_test_add_func ("/mail-sync/cancel-sets-terminal-status", test_cancel_sets_terminal_status);
  return g_test_run ();
}
