/* mail-imap-retry.h - Classify a GError from an IMAP operation into
 *                     a retry action for run_with_auth_retry().
 *
 * Pulled into its own translation unit so the policy is testable
 * without spinning up libetpan / GOA. mail-backend-imap.c is the
 * only production caller.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  /* Don't retry; propagate the error to the caller. */
  IMAP_RETRY_NONE,
  /* Stale-token failure: invalidate the cached OAuth token, drop the
   * connection (so the next attempt re-AUTHENTICATEs), and retry. */
  IMAP_RETRY_AUTH,
  /* Transport-level failure (stream broken, parse desync, connection
   * refused mid-op): drop the connection only — the token is still
   * good — and retry. */
  IMAP_RETRY_TRANSPORT,
} ImapRetryAction;

ImapRetryAction imap_retry_action_for_error (GError *error);

G_END_DECLS
