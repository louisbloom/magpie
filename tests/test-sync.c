/* tests/test-sync.c - MailSync against mail-backend-fake as remote source. */

#include "mail-backend-fake.h"

#include "../src/mail-arena.h"
#include "../src/mail-backend.h"
#include "../src/mail-store.h"
#include "../src/mail-sync.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

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
  /* The four new bodies (3 inbox + 1 sent) arrive across at most two
   * batched calls — one per folder; the batch size is way above 4.
   * Test the body-count side of the contract: zero per-message
   * fallback calls + at least one batched call. */
  guint fallback_after_first = mail_backend_fake_fetch_raw_calls (remote);
  guint batched_after_first = mail_backend_fake_fetch_messages_raw_calls (remote);
  g_assert_cmpuint (fallback_after_first, ==, 0);
  g_assert_cmpuint (batched_after_first, >=, 1);

  /* Re-run with the same fake; nothing new on the remote. */
  run_to_completion (sync, remote, local, NULL);
  g_assert_cmpuint (mail_backend_fake_fetch_raw_calls (remote), ==, fallback_after_first);
  g_assert_cmpuint (mail_backend_fake_fetch_messages_raw_calls (remote), ==, batched_after_first);

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

/* Walk the messages table directly to confirm content_key was
 * persisted; the public API only exposes locate-by-content-key,
 * which is what production code uses. We piggyback on
 * mail_store_locate_body_by_content_key here. */
static void
assert_inodes_equal (const char *root,
                     const char *dir_a,
                     const char *file_a,
                     const char *dir_b,
                     const char *file_b)
{
  g_autofree char *pa = g_build_filename (root, dir_a, "cur", file_a, NULL);
  g_autofree char *pb = g_build_filename (root, dir_b, "cur", file_b, NULL);
  struct stat sa, sb;
  g_assert_cmpint (stat (pa, &sa), ==, 0);
  g_assert_cmpint (stat (pb, &sb), ==, 0);
  g_assert_cmpint (sa.st_ino, ==, sb.st_ino);
}

static void
test_dedup_by_content_key (void)
{
  /* Two folders ("INBOX" and "All Mail") each list the same Gmail
   * message — same content_key, different per-folder remote_ids. The
   * sync engine must:
   *   - Fetch the body once (exactly one batched call carrying one id).
   *   - Upsert two message rows (one per folder).
   *   - Hardlink the bodies so both Maildir entries share an inode. */
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("mail-sync-dedup-XXXXXX", &error);
  g_assert_no_error (error);
  MailStore *local = mail_store_open (root, "u@example.com", &error);
  g_assert_no_error (error);

  MailBackend *remote = mail_backend_fake_new ();
  FakeFolderSpec folders[] = {
    { "f-inbox", "Inbox", NULL, 0, 1 },
    { "f-all", "All Mail", NULL, 0, 1 },
  };
  FakeMessageSpec inbox[] = {
    { "inbox-uid-1", "Greetings", "alice@example.com",
      1700000000, FALSE, "Shared body bytes", "<gmail-mid-42@example.com>" },
  };
  FakeMessageSpec all_mail[] = {
    { "all-uid-7", "Greetings", "alice@example.com",
      1700000000, FALSE, "Shared body bytes", "<gmail-mid-42@example.com>" },
  };
  mail_backend_fake_set_folders (remote, folders, G_N_ELEMENTS (folders));
  mail_backend_fake_set_messages (remote, "f-inbox", inbox, G_N_ELEMENTS (inbox));
  mail_backend_fake_set_messages (remote, "f-all", all_mail, G_N_ELEMENTS (all_mail));

  MailSync *sync = mail_sync_new ();
  run_to_completion (sync, remote, local, NULL);

  /* Only one network fetch happened. */
  g_assert_cmpuint (mail_backend_fake_fetch_raw_calls (remote), ==, 0);
  g_assert_cmpuint (mail_backend_fake_fetch_messages_raw_calls (remote), ==, 1);

  /* Both folders have their message row. */
  GHashTable *ibo = mail_store_message_remote_ids (local, "f-inbox", &error);
  g_assert_cmpuint (g_hash_table_size (ibo), ==, 1);
  g_assert_true (g_hash_table_contains (ibo, "inbox-uid-1"));
  g_hash_table_unref (ibo);
  GHashTable *aml = mail_store_message_remote_ids (local, "f-all", &error);
  g_assert_cmpuint (g_hash_table_size (aml), ==, 1);
  g_assert_true (g_hash_table_contains (aml, "all-uid-7"));
  g_hash_table_unref (aml);

  /* Bodies share an inode (hardlink). */
  MailArena tmp;
  mail_arena_init (&tmp, 256);
  const char *dir_a = NULL, *file_a = NULL;
  const char *dir_b = NULL, *file_b = NULL;
  g_assert_true (mail_store_message_location (local, "inbox-uid-1", &tmp,
                                              &dir_a, &file_a, &error));
  g_assert_true (mail_store_message_location (local, "all-uid-7", &tmp,
                                              &dir_b, &file_b, &error));
  assert_inodes_equal (root, dir_a, file_a, dir_b, file_b);
  mail_arena_destroy (&tmp);

  g_object_unref (sync);
  mail_backend_destroy (remote);
  mail_store_close (local);
  rm_rf (root);
}

static void
test_dedup_across_passes (void)
{
  /* On a fresh store: pass 1 syncs INBOX with one message. Pass 2
   * adds the same message (same content_key) under "All Mail". The
   * second pass must not fetch the body again — it should locate the
   * existing one by content_key and hardlink it. */
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("mail-sync-dedup-passes-XXXXXX", &error);
  g_assert_no_error (error);
  MailStore *local = mail_store_open (root, "u@example.com", &error);
  g_assert_no_error (error);

  MailBackend *remote = mail_backend_fake_new ();
  FakeFolderSpec one_folder[] = { { "f-inbox", "Inbox", NULL, 0, 1 } };
  FakeMessageSpec inbox[] = {
    { "inbox-uid-1", "Greetings", "alice@example.com",
      1700000000, FALSE, "Shared body bytes", "<gmail-mid-42@example.com>" },
  };
  mail_backend_fake_set_folders (remote, one_folder, G_N_ELEMENTS (one_folder));
  mail_backend_fake_set_messages (remote, "f-inbox", inbox, G_N_ELEMENTS (inbox));

  MailSync *sync = mail_sync_new ();
  run_to_completion (sync, remote, local, NULL);
  g_assert_cmpuint (mail_backend_fake_fetch_raw_calls (remote), ==, 0);
  g_assert_cmpuint (mail_backend_fake_fetch_messages_raw_calls (remote), ==, 1);

  /* Pass 2 — "All Mail" appears with the same content_key. */
  FakeFolderSpec two_folders[] = {
    { "f-inbox", "Inbox", NULL, 0, 1 },
    { "f-all", "All Mail", NULL, 0, 1 },
  };
  FakeMessageSpec all_mail[] = {
    { "all-uid-7", "Greetings", "alice@example.com",
      1700000000, FALSE, "Shared body bytes", "<gmail-mid-42@example.com>" },
  };
  mail_backend_fake_set_folders (remote, two_folders, G_N_ELEMENTS (two_folders));
  mail_backend_fake_set_messages (remote, "f-all", all_mail, G_N_ELEMENTS (all_mail));

  run_to_completion (sync, remote, local, NULL);

  /* Still exactly 1 body fetch from the remote across both passes. */
  g_assert_cmpuint (mail_backend_fake_fetch_raw_calls (remote), ==, 0);
  g_assert_cmpuint (mail_backend_fake_fetch_messages_raw_calls (remote), ==, 1);

  /* And All Mail's message row exists locally. */
  GHashTable *aml = mail_store_message_remote_ids (local, "f-all", &error);
  g_assert_cmpuint (g_hash_table_size (aml), ==, 1);
  g_assert_true (g_hash_table_contains (aml, "all-uid-7"));
  g_hash_table_unref (aml);

  g_object_unref (sync);
  mail_backend_destroy (remote);
  mail_store_close (local);
  rm_rf (root);
}

/* --- batched fetch -------------------------------------------- */

static void
test_fetch_uses_batched_when_available (void)
{
  /* Seed 100 messages in INBOX. The fake's vtable supplies a
   * batched fetch path; sync should land all bodies via that path
   * and never call the per-message path. With MAIL_SYNC_FETCH_BATCH
   * = 50, the expected batched-call count is 2. */
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("mail-sync-batch-XXXXXX", &error);
  g_assert_no_error (error);
  MailStore *local = mail_store_open (root, "u@example.com", &error);
  g_assert_no_error (error);

  MailBackend *remote = mail_backend_fake_new ();
  FakeFolderSpec folders[] = { { "f-inbox", "Inbox", NULL, 0, 100 } };
  mail_backend_fake_set_folders (remote, folders, G_N_ELEMENTS (folders));

  enum
  {
    N = 100
  };
  FakeMessageSpec msgs[N];
  char *id_storage[N];
  char *body_storage[N];
  for (guint i = 0; i < N; i++)
    {
      id_storage[i] = g_strdup_printf ("m%03u", i);
      body_storage[i] = g_strdup_printf ("Body %u\r\n", i);
      msgs[i] = (FakeMessageSpec){
        .id = id_storage[i],
        .subject = "S",
        .from = "a@b",
        .received_unix = 1700000000 + i,
        .unread = FALSE,
        .raw_rfc822 = body_storage[i],
        .content_key = NULL,
      };
    }
  mail_backend_fake_set_messages (remote, "f-inbox", msgs, N);

  MailSync *sync = mail_sync_new ();
  run_to_completion (sync, remote, local, NULL);

  g_assert_cmpuint (mail_backend_fake_fetch_raw_calls (remote), ==, 0);
  g_assert_cmpuint (mail_backend_fake_fetch_messages_raw_calls (remote), ==, 2);

  GHashTable *ids = mail_store_message_remote_ids (local, "f-inbox", &error);
  g_assert_cmpuint (g_hash_table_size (ids), ==, N);
  g_hash_table_unref (ids);

  for (guint i = 0; i < N; i++)
    {
      g_free (id_storage[i]);
      g_free (body_storage[i]);
    }
  g_object_unref (sync);
  mail_backend_destroy (remote);
  mail_store_close (local);
  rm_rf (root);
}

/* --- fallback when no batched vt ------------------------------ */

/* A thin wrapper that forwards every vtable slot to an inner
 * MailBackend *except* fetch_messages_raw_async/finish, which it
 * deliberately leaves NULL. Used to verify the public trampoline
 * in mail-backend.c falls back to looping the per-message path —
 * the route mail-backend-msgraph uses today. */
typedef struct
{
  MailBackend base;
  MailBackend *inner;
} FetchOnlyBackend;

static void
fb_list_folders_async (MailBackend *b, GCancellable *c, GAsyncReadyCallback cb, gpointer u)
{
  mail_backend_list_folders_async (((FetchOnlyBackend *) b)->inner, c, cb, u);
}
static GPtrArray *
fb_list_folders_finish (MailBackend *b, GAsyncResult *r, GError **e)
{
  return mail_backend_list_folders_finish (((FetchOnlyBackend *) b)->inner, r, e);
}
static void
fb_list_messages_async (MailBackend *b, const char *fid, int n, GCancellable *c, GAsyncReadyCallback cb, gpointer u)
{
  mail_backend_list_messages_async (((FetchOnlyBackend *) b)->inner, fid, n, c, cb, u);
}
static GPtrArray *
fb_list_messages_finish (MailBackend *b, GAsyncResult *r, GError **e)
{
  return mail_backend_list_messages_finish (((FetchOnlyBackend *) b)->inner, r, e);
}
static void
fb_fetch_raw_async (MailBackend *b, const char *id, GCancellable *c, GAsyncReadyCallback cb, gpointer u)
{
  mail_backend_fetch_message_raw_async (((FetchOnlyBackend *) b)->inner, id, c, cb, u);
}
static GBytes *
fb_fetch_raw_finish (MailBackend *b, GAsyncResult *r, GError **e)
{
  return mail_backend_fetch_message_raw_finish (((FetchOnlyBackend *) b)->inner, r, e);
}
static void
fb_destroy (MailBackend *b)
{
  FetchOnlyBackend *self = (FetchOnlyBackend *) b;
  /* inner is freed by the test, not us. */
  if (self->base.response_buf != NULL)
    g_byte_array_unref (self->base.response_buf);
  if (self->base.path_buf != NULL)
    g_string_free (self->base.path_buf, TRUE);
  mail_arena_destroy (&self->base.fetch_arena);
  g_free (self);
}
static const MailBackendVTable fetch_only_vt = {
  .list_folders_async = fb_list_folders_async,
  .list_folders_finish = fb_list_folders_finish,
  .list_messages_async = fb_list_messages_async,
  .list_messages_finish = fb_list_messages_finish,
  .fetch_message_raw_async = fb_fetch_raw_async,
  .fetch_message_raw_finish = fb_fetch_raw_finish,
  /* fetch_messages_raw_{async,finish} deliberately NULL */
  .destroy = fb_destroy,
};

static MailBackend *
fetch_only_backend_new (MailBackend *inner)
{
  FetchOnlyBackend *self = g_new0 (FetchOnlyBackend, 1);
  self->base.vt = &fetch_only_vt;
  mail_arena_init (&self->base.fetch_arena, 1024);
  self->base.response_buf = g_byte_array_new ();
  self->base.path_buf = g_string_sized_new (64);
  self->inner = inner;
  return (MailBackend *) self;
}

static void
test_fallback_when_no_batched_vt (void)
{
  /* Sync against a backend missing the batched slot. The public
   * trampoline must serialise N per-message calls and assemble the
   * parallel array correctly — same end-state in the store. */
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("mail-sync-fallback-XXXXXX", &error);
  g_assert_no_error (error);
  MailStore *local = mail_store_open (root, "u@example.com", &error);

  MailBackend *fake = mail_backend_fake_new ();
  FakeFolderSpec folders[] = { { "f-inbox", "Inbox", NULL, 0, 10 } };
  FakeMessageSpec msgs[10];
  char *ids[10];
  char *bodies[10];
  for (guint i = 0; i < 10; i++)
    {
      ids[i] = g_strdup_printf ("x%u", i);
      bodies[i] = g_strdup_printf ("body %u", i);
      msgs[i] = (FakeMessageSpec){ .id = ids[i], .subject = "s", .from = "a@b", .received_unix = 1700000000 + i, .unread = FALSE, .raw_rfc822 = bodies[i] };
    }
  mail_backend_fake_set_folders (fake, folders, G_N_ELEMENTS (folders));
  mail_backend_fake_set_messages (fake, "f-inbox", msgs, 10);

  MailBackend *wrapped = fetch_only_backend_new (fake);
  MailSync *sync = mail_sync_new ();
  run_to_completion (sync, wrapped, local, NULL);

  g_assert_cmpuint (mail_backend_fake_fetch_raw_calls (fake), ==, 10);
  g_assert_cmpuint (mail_backend_fake_fetch_messages_raw_calls (fake), ==, 0);

  GHashTable *store_ids = mail_store_message_remote_ids (local, "f-inbox", &error);
  g_assert_cmpuint (g_hash_table_size (store_ids), ==, 10);
  g_hash_table_unref (store_ids);

  for (guint i = 0; i < 10; i++)
    {
      g_free (ids[i]);
      g_free (bodies[i]);
    }
  g_object_unref (sync);
  mail_backend_destroy (wrapped);
  mail_backend_destroy (fake);
  mail_store_close (local);
  rm_rf (root);
}

/* Pin the user-visible badge count pipeline end-to-end: a backend
 * reports messages with unread flags, sync persists them to the
 * local store, and mail_store_list_folders surfaces counts derived
 * from the actual stored messages — not from what the backend's
 * folder spec advertised. This means an interrupted sync correctly
 * displays a smaller badge: only what's actually been fetched
 * shows, instead of the server's (cached) UNSEEN count promising
 * mail that isn't here yet. The pre-sync folder counts in the
 * FakeFolderSpec below are deliberately wildly wrong (999 / 999)
 * to make sure they can't leak through. */
static void
test_folder_unread_and_total_persist_through_sync (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *root = g_dir_make_tmp ("mail-sync-XXXXXX", &error);
  g_assert_no_error (error);
  MailStore *local = mail_store_open (root, "u@example.com", &error);
  g_assert_no_error (error);

  MailBackend *remote = mail_backend_fake_new ();
  /* The FakeFolderSpec unread/total are bogus on purpose. */
  const FakeFolderSpec folders[] = {
    { "f-inbox", "Inbox", NULL, 999, 999 },
    { "f-sent", "Sent", NULL, 999, 999 },
    { "f-junk", "Junk", NULL, 999, 999 },
  };
  mail_backend_fake_set_folders (remote, folders, G_N_ELEMENTS (folders));

  /* Inbox: 2 unread + 3 read = 5 total. */
  const FakeMessageSpec inbox_msgs[] = {
    { "i1", "s", "a@b", 1700000000, TRUE, "body i1" },
    { "i2", "s", "a@b", 1700000001, TRUE, "body i2" },
    { "i3", "s", "a@b", 1700000002, FALSE, "body i3" },
    { "i4", "s", "a@b", 1700000003, FALSE, "body i4" },
    { "i5", "s", "a@b", 1700000004, FALSE, "body i5" },
  };
  /* Sent: 0 unread + 3 read = 3 total. */
  const FakeMessageSpec sent_msgs[] = {
    { "s1", "s", "a@b", 1700000000, FALSE, "body s1" },
    { "s2", "s", "a@b", 1700000001, FALSE, "body s2" },
    { "s3", "s", "a@b", 1700000002, FALSE, "body s3" },
  };
  /* Junk: 4 unread + 0 read = 4 total. */
  const FakeMessageSpec junk_msgs[] = {
    { "j1", "s", "a@b", 1700000000, TRUE, "body j1" },
    { "j2", "s", "a@b", 1700000001, TRUE, "body j2" },
    { "j3", "s", "a@b", 1700000002, TRUE, "body j3" },
    { "j4", "s", "a@b", 1700000003, TRUE, "body j4" },
  };
  mail_backend_fake_set_messages (remote, "f-inbox", inbox_msgs, G_N_ELEMENTS (inbox_msgs));
  mail_backend_fake_set_messages (remote, "f-sent", sent_msgs, G_N_ELEMENTS (sent_msgs));
  mail_backend_fake_set_messages (remote, "f-junk", junk_msgs, G_N_ELEMENTS (junk_msgs));

  MailSync *sync = mail_sync_new ();
  run_to_completion (sync, remote, local, NULL);

  MailArena arena;
  mail_arena_init (&arena, 4096);
  GPtrArray *rows = mail_store_list_folders (local, &arena, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (rows->len, ==, G_N_ELEMENTS (folders));

  struct
  {
    const char *id;
    int unread;
    int total;
  } expected[] = {
    { "f-inbox", 2, 5 },
    { "f-sent", 0, 3 },
    { "f-junk", 4, 4 },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (expected); i++)
    {
      gboolean found = FALSE;
      for (guint j = 0; j < rows->len; j++)
        {
          MailFolder *got = g_ptr_array_index (rows, j);
          if (g_strcmp0 (got->id, expected[i].id) != 0)
            continue;
          g_assert_cmpint (got->unread_count, ==, expected[i].unread);
          g_assert_cmpint (got->total_count, ==, expected[i].total);
          found = TRUE;
          break;
        }
      g_assert_true (found);
    }

  g_ptr_array_unref (rows);
  mail_arena_destroy (&arena);
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
  g_test_add_func ("/mail-sync/dedup-by-content-key", test_dedup_by_content_key);
  g_test_add_func ("/mail-sync/dedup-across-passes", test_dedup_across_passes);
  g_test_add_func ("/mail-sync/fetch-uses-batched", test_fetch_uses_batched_when_available);
  g_test_add_func ("/mail-sync/fallback-when-no-batched-vt", test_fallback_when_no_batched_vt);
  g_test_add_func ("/mail-sync/folder-counts-persist", test_folder_unread_and_total_persist_through_sync);
  return g_test_run ();
}
