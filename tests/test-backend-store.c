/* tests/test-backend-store.c - Lifetime contract against MailBackendStore.
 *
 * Mirrors tests/test-backend-contract.c (which pins the contract
 * against the fake) but the backend under test is the real
 * MailBackendStore reading from a populated on-disk MailStore.
 *
 * The three guards from the original contract are non-negotiable for
 * any backend the UI binds to:
 *   1. fetch_message_raw must not invalidate message-list pointers
 *   2. fetch_message_raw must not invalidate folder pointers
 *   3. fetch_message_raw must not change the arena's used byte count
 */

#include "../src/mail-arena.h"
#include "../src/mail-backend-store.h"
#include "../src/mail-backend.h"
#include "../src/mail-store.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct
{
  GAsyncResult *result;
} ResultCapture;

static void
capture_result (GObject *src,
                GAsyncResult *result,
                gpointer user_data)
{
  ResultCapture *cap = user_data;
  cap->result = g_object_ref (result);
}

static void
pump_main_loop (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static GPtrArray *
sync_list_folders (MailBackend *b)
{
  ResultCapture cap = { 0 };
  mail_backend_list_folders_async (b, NULL, capture_result, &cap);
  pump_main_loop ();
  GError *error = NULL;
  GPtrArray *out = mail_backend_list_folders_finish (b, cap.result, &error);
  g_assert_no_error (error);
  g_object_unref (cap.result);
  return out;
}

static GPtrArray *
sync_list_messages (MailBackend *b,
                    const char *folder_id,
                    int n)
{
  ResultCapture cap = { 0 };
  mail_backend_list_messages_async (b, folder_id, n, NULL, capture_result, &cap);
  pump_main_loop ();
  GError *error = NULL;
  GPtrArray *out = mail_backend_list_messages_finish (b, cap.result, &error);
  g_assert_no_error (error);
  g_object_unref (cap.result);
  return out;
}

static GBytes *
sync_fetch_raw (MailBackend *b,
                const char *message_id)
{
  ResultCapture cap = { 0 };
  mail_backend_fetch_message_raw_async (b, message_id, NULL, capture_result, &cap);
  pump_main_loop ();
  GError *error = NULL;
  GBytes *out = mail_backend_fetch_message_raw_finish (b, cap.result, &error);
  g_assert_no_error (error);
  g_object_unref (cap.result);
  return out;
}

/* --- fixture: tmp store seeded with a couple folders + messages -- */

typedef struct
{
  char *root;
  MailStore *store;
  MailBackend *backend;
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
seed_store (Fixture *f)
{
  g_autoptr (GError) error = NULL;
  g_assert_true (mail_store_upsert_folder (f->store, "inbox", "Inbox", NULL, 5, 10, NULL, &error));
  g_assert_true (mail_store_upsert_folder (f->store, "drafts", "Drafts", NULL, 0, 3, NULL, &error));
  g_assert_no_error (error);

  struct
  {
    const char *id;
    const char *subject;
    const char *from;
    gint64 t;
    gboolean unread;
    const char *raw;
  } seed[] = {
    { "m1", "Hello", "alice@example.com", 1700000000, FALSE, "Subject: Hello\r\n\r\nBody1" },
    { "m2", "Second", "bob@example.com", 1700000100, TRUE, "Subject: Second\r\n\r\nBody2" },
    { "m3", "Third", "carol@example.com", 1700000200, FALSE, "Subject: Third\r\n\r\nBody3" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (seed); i++)
    {
      g_autoptr (GBytes) bytes = g_bytes_new_static (seed[i].raw, strlen (seed[i].raw));
      g_autofree char *filename = NULL;
      g_assert_true (mail_store_write_raw (f->store, "Inbox", bytes, !seed[i].unread,
                                           &filename, &error));
      g_assert_true (mail_store_upsert_message (f->store, "inbox", seed[i].id, filename,
                                                seed[i].subject, seed[i].from,
                                                seed[i].t, seed[i].unread, NULL, &error));
      g_assert_no_error (error);
    }
}

static void
fixture_setup (Fixture *f,
               gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  f->root = g_dir_make_tmp ("mail-backend-store-XXXXXX", &error);
  g_assert_no_error (error);
  f->store = mail_store_open (f->root, "test@example.com", &error);
  g_assert_no_error (error);
  seed_store (f);
  f->backend = mail_backend_store_new (f->store);
}

static void
fixture_teardown (Fixture *f,
                  gconstpointer data)
{
  mail_backend_destroy (f->backend);
  mail_store_close (f->store);
  rm_rf (f->root);
  g_free (f->root);
}

/* --- the three regression guards --------------------------------- */

static void
test_fetch_preserves_messages (Fixture *f,
                               gconstpointer data)
{
  GPtrArray *msgs = sync_list_messages (f->backend, "inbox", 10);
  g_assert_cmpuint (msgs->len, ==, 3);
  const MailMessageMeta *m0 = g_ptr_array_index (msgs, 0);
  const MailMessageMeta *m1 = g_ptr_array_index (msgs, 1);
  const MailMessageMeta *m2 = g_ptr_array_index (msgs, 2);
  g_autofree char *m0_id = g_strdup (m0->id);
  g_autofree char *m0_subj = g_strdup (m0->subject);
  g_autofree char *m1_subj = g_strdup (m1->subject);
  g_autofree char *m2_subj = g_strdup (m2->subject);

  g_autoptr (GBytes) body = sync_fetch_raw (f->backend, m0->id);
  g_assert_nonnull (body);

  g_assert_cmpstr (m0->id, ==, m0_id);
  g_assert_cmpstr (m0->subject, ==, m0_subj);
  g_assert_cmpstr (m1->subject, ==, m1_subj);
  g_assert_cmpstr (m2->subject, ==, m2_subj);

  g_ptr_array_unref (msgs);
}

static void
test_fetch_preserves_folders (Fixture *f,
                              gconstpointer data)
{
  GPtrArray *folders = sync_list_folders (f->backend);
  g_assert_cmpuint (folders->len, ==, 2);
  const MailFolder *f0 = g_ptr_array_index (folders, 0);
  const MailFolder *f1 = g_ptr_array_index (folders, 1);
  g_autofree char *f0_name = g_strdup (f0->display_name);
  g_autofree char *f1_name = g_strdup (f1->display_name);

  g_autoptr (GBytes) body = sync_fetch_raw (f->backend, "m1");
  g_assert_nonnull (body);

  g_assert_cmpstr (f0->display_name, ==, f0_name);
  g_assert_cmpstr (f1->display_name, ==, f1_name);

  g_ptr_array_unref (folders);
}

static void
test_fetch_does_not_change_arena (Fixture *f,
                                  gconstpointer data)
{
  GPtrArray *msgs = sync_list_messages (f->backend, "inbox", 10);
  g_assert_cmpuint (msgs->len, ==, 3);

  size_t used_before = mail_arena_used (&f->backend->fetch_arena);
  g_assert_cmpuint (used_before, >, 0);

  g_autoptr (GBytes) body = sync_fetch_raw (f->backend, "m1");
  g_assert_nonnull (body);

  size_t used_after = mail_arena_used (&f->backend->fetch_arena);
  g_assert_cmpuint (used_after, ==, used_before);

  g_ptr_array_unref (msgs);
}

static void
test_fetch_returns_seeded_bytes (Fixture *f,
                                 gconstpointer data)
{
  GPtrArray *msgs = sync_list_messages (f->backend, "inbox", 10);
  const MailMessageMeta *m0 = g_ptr_array_index (msgs, 0);

  g_autoptr (GBytes) body = sync_fetch_raw (f->backend, m0->id);
  g_assert_nonnull (body);
  gsize len = 0;
  const char *bytes = g_bytes_get_data (body, &len);
  /* m0 is the newest (received_unix DESC), so it's m3. */
  g_assert_cmpmem (bytes, len, "Subject: Third\r\n\r\nBody3", strlen ("Subject: Third\r\n\r\nBody3"));

  g_ptr_array_unref (msgs);
}

int
main (int argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

#define ADD(name, fn) \
  g_test_add ("/backend-store/" name, Fixture, NULL, fixture_setup, fn, fixture_teardown)

  ADD ("fetch-preserves-messages", test_fetch_preserves_messages);
  ADD ("fetch-preserves-folders", test_fetch_preserves_folders);
  ADD ("fetch-does-not-change-arena", test_fetch_does_not_change_arena);
  ADD ("fetch-returns-seeded-bytes", test_fetch_returns_seeded_bytes);

#undef ADD

  return g_test_run ();
}
