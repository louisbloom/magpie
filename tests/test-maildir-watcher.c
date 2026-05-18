/* tests/test-maildir-watcher.c - GFileMonitor-based reconciler tests.
 *
 * Drives the real MailMaildirWatcher against a real MailStore in a
 * tmp dir. External mutations are simulated by g_rename'ing files
 * inside cur/; the test then pumps the main loop until the
 * watcher's debounce timer fires (with a generous timeout) and
 * asserts on the events that surfaced via the store's listener
 * registry.
 *
 * The trigger-for-test seam runs the reconcile path synchronously,
 * which is used by the debounce-coalesces test to verify that
 * multiple events within the debounce window collapse into a single
 * reconcile pass.
 */

#include "../src/mail-arena.h"
#include "../src/mail-backend.h"
#include "../src/mail-maildir-watcher.h"
#include "../src/mail-store.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* --- recorder shared with test-store-listener style ---------------- */

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
} Recorder;

static void
recorder_init (Recorder *r)
{
  r->changes = g_array_new (FALSE, FALSE, sizeof (RecordedChange));
}

static void
recorder_destroy (Recorder *r)
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
  Recorder *r = user_data;
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

static guint
recorder_count_kind (Recorder *r,
                     MailStoreChangeKind kind)
{
  guint n = 0;
  for (guint i = 0; i < r->changes->len; i++)
    if (g_array_index (r->changes, RecordedChange, i).kind == kind)
      n++;
  return n;
}

/* --- fixture ------------------------------------------------------- */

typedef struct
{
  char *root;
  MailStore *store;
  MailMaildirWatcher *watcher;
  MailArena arena;
  /* The message remote_id seeded in cur/, plus the basename we wrote
   * (so the test can g_rename it to add :2,S without re-resolving). */
  char *msg_id;
  char *msg_basename;
} Fixture;

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
fixture_setup (Fixture *f,
               gconstpointer data)
{
  (void) data;
  g_autoptr (GError) error = NULL;
  f->root = g_dir_make_tmp ("mail-maildir-watcher-XXXXXX", &error);
  g_assert_no_error (error);
  f->store = mail_store_open (f->root, "test@example.com", &error);
  g_assert_no_error (error);
  mail_arena_init (&f->arena, 4096);

  /* Seed: one folder with one unread message. */
  g_assert_true (mail_store_upsert_folder (f->store, "rid-INBOX", "INBOX",
                                           NULL, NULL, &error));
  const char *payload = "Subject: hi\r\n\r\nbody\r\n";
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, strlen (payload));
  char *name = NULL;
  g_assert_true (mail_store_write_raw (f->store, "INBOX", bytes, FALSE, &name, &error));
  g_assert_no_error (error);
  f->msg_basename = name; /* g_strdup'd inside mail_store_write_raw */
  f->msg_id = g_strdup ("m1");
  g_assert_true (mail_store_upsert_message (f->store, "rid-INBOX", f->msg_id, NULL,
                                            f->msg_basename, "hi", "a@x", 1700000000,
                                            TRUE, &error));

  f->watcher = mail_maildir_watcher_new (f->store);
}

static void
fixture_teardown (Fixture *f,
                  gconstpointer data)
{
  (void) data;
  mail_maildir_watcher_free (f->watcher);
  mail_arena_destroy (&f->arena);
  mail_store_close (f->store);
  rm_rf (f->root);
  g_free (f->root);
  g_free (f->msg_id);
  g_free (f->msg_basename);
}

/* Build cur/old_basename → cur/<basename + :2,S> rename. Returns the
 * new basename; caller frees. */
static char *
rename_add_s (Fixture *f,
              const char *basename)
{
  g_autofree char *new_name = _mail_store_maildir_basename_add_flag_for_test (basename, 'S');
  g_autofree char *old_path = g_build_filename (f->root, "INBOX", "cur", basename, NULL);
  g_autofree char *new_path = g_build_filename (f->root, "INBOX", "cur", new_name, NULL);
  g_assert_cmpint (g_rename (old_path, new_path), ==, 0);
  return g_strdup (new_name);
}

/* Pump the main loop until @cond becomes true or @timeout_ms elapses. */
static gboolean
pump_until (gboolean (*cond) (gpointer), gpointer data, guint timeout_ms)
{
  gint64 deadline = g_get_monotonic_time () + timeout_ms * 1000;
  while (!cond (data))
    {
      if (g_get_monotonic_time () > deadline)
        return FALSE;
      g_main_context_iteration (NULL, FALSE);
      g_usleep (1000); /* tiny yield so the kernel can deliver events */
    }
  return TRUE;
}

static gboolean
has_message_flags (gpointer data)
{
  Recorder *r = data;
  return recorder_count_kind (r, MAIL_STORE_CHANGE_MESSAGE_FLAGS) > 0;
}

/* --- tests --------------------------------------------------------- */

static void
test_initial_reconcile_on_watch (Fixture *f,
                                 gconstpointer data)
{
  (void) data;
  /* Rename the file in cur/ to add :2,S before the watcher arms — this
   * is the "magpie was closed while mutt marked messages read" case. */
  g_autofree char *new_name = rename_add_s (f, f->msg_basename);

  Recorder rec;
  recorder_init (&rec);
  guint id = mail_store_add_listener (f->store, recorder_cb, &rec, NULL);

  g_autoptr (GError) error = NULL;
  g_assert_true (mail_maildir_watcher_watch_folder (f->watcher, "rid-INBOX", &error));
  g_assert_no_error (error);

  /* watch_folder runs reconcile synchronously, so the events are
   * already in the recorder by the time we return. */
  g_assert_cmpuint (recorder_count_kind (&rec, MAIL_STORE_CHANGE_MESSAGE_FLAGS), ==, 1);
  g_assert_cmpuint (recorder_count_kind (&rec, MAIL_STORE_CHANGE_FOLDER_COUNTS), ==, 1);

  /* sqlite filename now matches disk; m1.unread is FALSE. */
  GPtrArray *msgs = mail_store_list_messages (f->store, "rid-INBOX", 10, &f->arena, &error);
  g_assert_cmpuint (msgs->len, ==, 1);
  MailMessageMeta *m = g_ptr_array_index (msgs, 0);
  g_assert_false (m->unread);
  g_ptr_array_unref (msgs);

  mail_store_remove_listener (f->store, id);
  recorder_destroy (&rec);
}

static void
test_external_rename_fires_events (Fixture *f,
                                   gconstpointer data)
{
  (void) data;
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_maildir_watcher_watch_folder (f->watcher, "rid-INBOX", &error));
  /* The initial reconcile ran with no drift, so no events queued. */

  Recorder rec;
  recorder_init (&rec);
  guint id = mail_store_add_listener (f->store, recorder_cb, &rec, NULL);

  /* External rename in cur/ — simulates mutt mark-read. */
  g_autofree char *new_name = rename_add_s (f, f->msg_basename);

  /* GFileMonitor delivers events asynchronously; the watcher then
   * schedules a 120 ms debounce. Wait up to 2 s for the event chain
   * to complete. */
  g_assert_true (pump_until (has_message_flags, &rec, 2000));

  g_assert_cmpuint (recorder_count_kind (&rec, MAIL_STORE_CHANGE_MESSAGE_FLAGS), >=, 1);
  g_assert_cmpuint (recorder_count_kind (&rec, MAIL_STORE_CHANGE_FOLDER_COUNTS), >=, 1);

  /* Verify the payload of the first FLAGS event. */
  for (guint i = 0; i < rec.changes->len; i++)
    {
      RecordedChange *rc = &g_array_index (rec.changes, RecordedChange, i);
      if (rc->kind == MAIL_STORE_CHANGE_MESSAGE_FLAGS)
        {
          g_assert_cmpstr (rc->folder_id, ==, "rid-INBOX");
          g_assert_cmpstr (rc->message_id, ==, "m1");
          g_assert_false (rc->unread);
          break;
        }
    }

  mail_store_remove_listener (f->store, id);
  recorder_destroy (&rec);
}

static void
test_self_rename_emits_no_drift (Fixture *f,
                                 gconstpointer data)
{
  (void) data;
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_maildir_watcher_watch_folder (f->watcher, "rid-INBOX", &error));

  /* Local mark-read: the store renames and emits FLAGS + COUNTS as
   * part of set_message_unread. Then the watcher will (after debounce)
   * run reconcile, which should find no drift (disk and sqlite both
   * already encode :2,S) and emit *nothing*. */
  Recorder rec;
  recorder_init (&rec);
  guint id = mail_store_add_listener (f->store, recorder_cb, &rec, NULL);

  g_assert_true (mail_store_set_message_unread (f->store, "m1", FALSE, &error));
  g_assert_no_error (error);
  /* Two events from set_message_unread itself: FLAGS + COUNTS. */
  g_assert_cmpuint (rec.changes->len, ==, 2);

  /* Trigger the reconcile path directly (mirrors what the debounce
   * timer would do, sidestepping the 120 ms wall-clock wait). The
   * test seam returns TRUE when the folder is watched. */
  g_assert_true (mail_maildir_watcher_trigger_for_test (f->watcher, "rid-INBOX"));

  /* No drift → reconcile emitted nothing. Still just the two events
   * from set_message_unread. */
  g_assert_cmpuint (rec.changes->len, ==, 2);

  mail_store_remove_listener (f->store, id);
  recorder_destroy (&rec);
}

static void
test_unknown_folder_returns_false (Fixture *f,
                                   gconstpointer data)
{
  (void) data;
  g_autoptr (GError) error = NULL;
  g_assert_false (mail_maildir_watcher_watch_folder (f->watcher, "no-such-folder", &error));
  g_assert_no_error (error);
}

static void
test_unwatch_is_idempotent (Fixture *f,
                            gconstpointer data)
{
  (void) data;
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_maildir_watcher_watch_folder (f->watcher, "rid-INBOX", &error));
  mail_maildir_watcher_unwatch_folder (f->watcher, "rid-INBOX");
  /* Second unwatch is a no-op. */
  mail_maildir_watcher_unwatch_folder (f->watcher, "rid-INBOX");
  /* trigger_for_test now returns FALSE because the folder is no
   * longer watched. */
  g_assert_false (mail_maildir_watcher_trigger_for_test (f->watcher, "rid-INBOX"));
}

int
main (int argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

#define ADD(name, fn) \
  g_test_add ("/maildir-watcher/" name, Fixture, NULL, fixture_setup, fn, fixture_teardown)

  ADD ("initial-reconcile-on-watch", test_initial_reconcile_on_watch);
  ADD ("external-rename-fires-events", test_external_rename_fires_events);
  ADD ("self-rename-emits-no-drift", test_self_rename_emits_no_drift);
  ADD ("unknown-folder-returns-false", test_unknown_folder_returns_false);
  ADD ("unwatch-is-idempotent", test_unwatch_is_idempotent);

#undef ADD

  return g_test_run ();
}
