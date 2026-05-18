/* tests/test-imap-retry.c - Pin the retry policy used by
 *                           mail-backend-imap.c's run_with_auth_retry.
 *
 * Backstory: the IMAP backend only retried on G_IO_ERROR_PERMISSION_DENIED
 * for a long time, so a transient TCP hangup mid-LIST (libetpan rc=4 ->
 * G_IO_ERROR_BROKEN_PIPE) propagated as a hard sync failure on Gmail
 * with the message "sync failed: IMAP LIST failed (libetpan rc=4)".
 * Now classified as IMAP_RETRY_TRANSPORT — one re-attempt before
 * giving up, with the socket rebuilt by ensure_connected_locked.
 */

#include "../src/mail-imap-retry.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_null_is_no_retry (void)
{
  g_assert_cmpint (imap_retry_action_for_error (NULL), ==, IMAP_RETRY_NONE);
}

static void
test_permission_denied_is_auth_retry (void)
{
  g_autoptr (GError) e = g_error_new (G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                      "IMAP XOAUTH2 failed (libetpan rc=29)");
  g_assert_cmpint (imap_retry_action_for_error (e), ==, IMAP_RETRY_AUTH);
}

static void
test_broken_pipe_is_transport_retry (void)
{
  /* This is the exact mapping set_imap_error () applies for
   * MAILIMAP_ERROR_STREAM, the rc=4 the user saw on Gmail. */
  g_autoptr (GError) e = g_error_new (G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                                      "IMAP LIST failed (libetpan rc=4)");
  g_assert_cmpint (imap_retry_action_for_error (e), ==, IMAP_RETRY_TRANSPORT);
}

static void
test_failed_is_no_retry (void)
{
  /* Generic FAILED: anything libetpan emits that isn't stream/parse/
   * refused or login/bad-state. Treat as final — retrying won't help. */
  g_autoptr (GError) e = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                      "IMAP SELECT failed (libetpan rc=12)");
  g_assert_cmpint (imap_retry_action_for_error (e), ==, IMAP_RETRY_NONE);
}

static void
test_cancelled_is_no_retry (void)
{
  /* If GOA's token acquisition propagates a cancellation, don't dress
   * it up as a retryable failure — let the caller see the cancel. */
  g_autoptr (GError) e = g_error_new (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                      "operation cancelled");
  g_assert_cmpint (imap_retry_action_for_error (e), ==, IMAP_RETRY_NONE);
}

static void
test_invalid_data_is_no_retry (void)
{
  /* worker_fetch_messages_in_folder raises INVALID_DATA on a
   * UIDVALIDITY change; retrying the same batch won't help — the
   * sync engine has to re-list. */
  g_autoptr (GError) e = g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                      "UIDVALIDITY changed");
  g_assert_cmpint (imap_retry_action_for_error (e), ==, IMAP_RETRY_NONE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/imap-retry/null", test_null_is_no_retry);
  g_test_add_func ("/imap-retry/permission-denied", test_permission_denied_is_auth_retry);
  g_test_add_func ("/imap-retry/broken-pipe", test_broken_pipe_is_transport_retry);
  g_test_add_func ("/imap-retry/failed", test_failed_is_no_retry);
  g_test_add_func ("/imap-retry/cancelled", test_cancelled_is_no_retry);
  g_test_add_func ("/imap-retry/invalid-data", test_invalid_data_is_no_retry);
  return g_test_run ();
}
