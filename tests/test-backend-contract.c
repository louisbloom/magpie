/* tests/test-backend-contract.c - Lifetime-contract regression tests.
 *
 * The MailBackend contract states:
 *   - pointers returned by list_folders_finish are valid until the next
 *     list_folders call on the same backend;
 *   - pointers returned by list_messages_finish are valid until the next
 *     list_messages call on the same backend;
 *   - fetch_message_raw_async/finish MUST NOT invalidate either, since
 *     the sidebar holds folder pointers and the message-list pane holds
 *     message-meta pointers across fetches.
 *
 * Regression context: the initial mb_msgraph_fetch_message_raw_async
 * called a shared reset_state() helper that wiped the arena, dangling
 * every message-list row pointer. The next time the row factory ran,
 * GtkLabel rendered garbage and the app eventually segfaulted with
 * "Invalid UTF-8 encoded text in name" markup-parse warnings.
 *
 * These tests pin the contract using the fake backend. Real backends
 * (msgraph, imap) are trusted to follow the same pattern; the comment
 * on reset_buffers / reset_arena_and_buffers documents the rule.
 */

#include "mail-backend-fake.h"

#include "../src/mail-arena.h"
#include "../src/mail-backend.h"

#include <gtk/gtk.h>
#include <string.h>

static void
pump_main_loop (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

typedef struct
{
  GAsyncResult *result; /* ref'd */
} ResultCapture;

static void
capture_result (GObject *source,
                GAsyncResult *result,
                gpointer user_data)
{
  ResultCapture *cap = user_data;
  cap->result = g_object_ref (result);
}

/* Synchronously run list_messages_async via the fake. */
static GPtrArray *
sync_list_messages (MailBackend *backend,
                    const char *folder_id,
                    int top_n)
{
  ResultCapture cap = { 0 };
  mail_backend_list_messages_async (backend, folder_id, top_n,
                                    NULL, capture_result, &cap);
  pump_main_loop ();
  g_assert_nonnull (cap.result);
  GError *error = NULL;
  GPtrArray *out = mail_backend_list_messages_finish (backend, cap.result, &error);
  g_assert_no_error (error);
  g_object_unref (cap.result);
  return out;
}

static GPtrArray *
sync_list_folders (MailBackend *backend)
{
  ResultCapture cap = { 0 };
  mail_backend_list_folders_async (backend, NULL, capture_result, &cap);
  pump_main_loop ();
  g_assert_nonnull (cap.result);
  GError *error = NULL;
  GPtrArray *out = mail_backend_list_folders_finish (backend, cap.result, &error);
  g_assert_no_error (error);
  g_object_unref (cap.result);
  return out;
}

static GBytes *
sync_fetch_message_raw (MailBackend *backend,
                        const char *message_id)
{
  ResultCapture cap = { 0 };
  mail_backend_fetch_message_raw_async (backend, message_id, NULL, capture_result, &cap);
  pump_main_loop ();
  g_assert_nonnull (cap.result);
  GError *error = NULL;
  GBytes *body = mail_backend_fetch_message_raw_finish (backend, cap.result, &error);
  g_assert_no_error (error);
  g_object_unref (cap.result);
  return body;
}

/* --- The tests ------------------------------------------------- */

static void
test_fetch_preserves_messages (void)
{
  MailBackend *fake = mail_backend_fake_new ();
  FakeMessageSpec msgs[] = {
    { "m1", "Hello", "alice@example.com", 1700000000, FALSE, "Subject: Hello\r\n\r\nBody1" },
    { "m2", "Second", "bob@example.com", 1700000100, TRUE, "Subject: Second\r\n\r\nBody2" },
    { "m3", "Third", "carol@example.com", 1700000200, FALSE, "Subject: Third\r\n\r\nBody3" },
  };
  mail_backend_fake_set_messages (fake, "inbox", msgs, G_N_ELEMENTS (msgs));

  GPtrArray *messages = sync_list_messages (fake, "inbox", 10);
  g_assert_cmpuint (messages->len, ==, 3);

  /* Snapshot the pointers and the strings at them so we can compare
   * after the fetch. The contract says these strings outlive the fetch. */
  const MailMessageMeta *m0 = g_ptr_array_index (messages, 0);
  const MailMessageMeta *m1 = g_ptr_array_index (messages, 1);
  const MailMessageMeta *m2 = g_ptr_array_index (messages, 2);

  g_autofree char *m0_id_snap = g_strdup (m0->id);
  g_autofree char *m0_subject_snap = g_strdup (m0->subject);
  g_autofree char *m1_subject_snap = g_strdup (m1->subject);
  g_autofree char *m2_subject_snap = g_strdup (m2->subject);

  /* fetch_message_raw must NOT invalidate the message-list pointers. */
  g_autoptr (GBytes) body = sync_fetch_message_raw (fake, m0->id);
  g_assert_nonnull (body);

  g_assert_cmpstr (m0->id, ==, m0_id_snap);
  g_assert_cmpstr (m0->subject, ==, m0_subject_snap);
  g_assert_cmpstr (m1->subject, ==, m1_subject_snap);
  g_assert_cmpstr (m2->subject, ==, m2_subject_snap);

  /* And the returned bytes match the seeded raw body. */
  gsize len = 0;
  const char *data = g_bytes_get_data (body, &len);
  g_autofree char *text = g_strndup (data, len);
  g_assert_cmpstr (text, ==, "Subject: Hello\r\n\r\nBody1");

  g_ptr_array_unref (messages);
  mail_backend_destroy (fake);
}

static void
test_fetch_preserves_folders (void)
{
  MailBackend *fake = mail_backend_fake_new ();
  FakeFolderSpec folders[] = {
    { "inbox", "Inbox", NULL, 5, 10 },
    { "drafts", "Drafts", NULL, 0, 3 },
  };
  FakeMessageSpec msgs[] = {
    { "m1", "Only", "x@y.z", 0, FALSE, "raw" },
  };
  mail_backend_fake_set_folders (fake, folders, G_N_ELEMENTS (folders));
  mail_backend_fake_set_messages (fake, "inbox", msgs, G_N_ELEMENTS (msgs));

  GPtrArray *flist = sync_list_folders (fake);
  g_assert_cmpuint (flist->len, ==, 2);

  const MailFolder *f0 = g_ptr_array_index (flist, 0);
  const MailFolder *f1 = g_ptr_array_index (flist, 1);

  g_autofree char *f0_id_snap = g_strdup (f0->id);
  g_autofree char *f0_name_snap = g_strdup (f0->display_name);
  g_autofree char *f1_name_snap = g_strdup (f1->display_name);

  /* fetch_message_raw must not invalidate folder pointers either. */
  g_autoptr (GBytes) body = sync_fetch_message_raw (fake, "m1");
  g_assert_nonnull (body);

  g_assert_cmpstr (f0->id, ==, f0_id_snap);
  g_assert_cmpstr (f0->display_name, ==, f0_name_snap);
  g_assert_cmpstr (f1->display_name, ==, f1_name_snap);

  g_ptr_array_unref (flist);
  mail_backend_destroy (fake);
}

static void
test_fetch_does_not_change_arena_usage (void)
{
  /* The strongest assertion: fetch_message_raw must not touch the
   * backend's arena AT ALL. Even arena_reset (which only bumps used=0
   * without overwriting bytes) is a contract violation, because the
   * NEXT arena allocation will reuse those bytes and dangle every
   * pointer the message-list pane still holds. */
  MailBackend *fake = mail_backend_fake_new ();
  FakeMessageSpec msgs[] = {
    { "m1", "First", "x@y.z", 0, FALSE, "Body 1" },
    { "m2", "Second", "x@y.z", 0, FALSE, "Body 2" },
  };
  mail_backend_fake_set_messages (fake, "inbox", msgs, G_N_ELEMENTS (msgs));

  GPtrArray *messages = sync_list_messages (fake, "inbox", 10);
  g_assert_cmpuint (messages->len, ==, 2);

  size_t used_before = mail_arena_used (&fake->fetch_arena);
  g_assert_cmpuint (used_before, >, 0); /* sanity: list_messages put data in the arena */

  g_autoptr (GBytes) body = sync_fetch_message_raw (fake, "m1");
  g_assert_nonnull (body);

  size_t used_after = mail_arena_used (&fake->fetch_arena);
  g_assert_cmpuint (used_after, ==, used_before);

  g_ptr_array_unref (messages);
  mail_backend_destroy (fake);
}

static void
test_list_messages_invalidates_prior_list_messages (void)
{
  /* The contract DOES allow list_messages to invalidate prior
   * list_messages results — callers must clear their UI state before
   * reissuing. Document this with a positive test: after a second
   * list_messages, the returned arrays are independent. */
  MailBackend *fake = mail_backend_fake_new ();

  FakeMessageSpec a[] = { { "a1", "A1", "u", 0, FALSE, "raw" } };
  FakeMessageSpec b[] = { { "b1", "B1", "u", 0, FALSE, "raw" } };
  mail_backend_fake_set_messages (fake, "fa", a, 1);
  mail_backend_fake_set_messages (fake, "fb", b, 1);

  GPtrArray *first = sync_list_messages (fake, "fa", 10);
  g_assert_cmpuint (first->len, ==, 1);
  const MailMessageMeta *am0 = g_ptr_array_index (first, 0);
  g_assert_cmpstr (am0->subject, ==, "A1");

  /* Caller is expected to drop references to the previous array's
   * contents before this point. We don't read am0 again. */
  GPtrArray *second = sync_list_messages (fake, "fb", 10);
  g_assert_cmpuint (second->len, ==, 1);
  const MailMessageMeta *bm0 = g_ptr_array_index (second, 0);
  g_assert_cmpstr (bm0->subject, ==, "B1");

  g_ptr_array_unref (first);
  g_ptr_array_unref (second);
  mail_backend_destroy (fake);
}

int
main (int argc,
      char *argv[])
{
  gtk_test_init (&argc, &argv, NULL);

  g_test_add_func ("/backend-contract/fetch-preserves-messages",
                   test_fetch_preserves_messages);
  g_test_add_func ("/backend-contract/fetch-preserves-folders",
                   test_fetch_preserves_folders);
  g_test_add_func ("/backend-contract/fetch-does-not-touch-arena",
                   test_fetch_does_not_change_arena_usage);
  g_test_add_func ("/backend-contract/list-messages-invalidates-prior",
                   test_list_messages_invalidates_prior_list_messages);
  return g_test_run ();
}
