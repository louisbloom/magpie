/* mail-imap-retry.c - Classify an IMAP error into a retry action.
 *
 * set_imap_error() in mail-backend-imap.c folds libetpan return
 * codes into a small set of GIOErrorEnum values:
 *
 *   MAILIMAP_ERROR_LOGIN, BAD_STATE        -> PERMISSION_DENIED
 *   MAILIMAP_ERROR_STREAM, PARSE, REFUSED  -> BROKEN_PIPE
 *   anything else                          -> FAILED
 *
 * We retry the first two flavours once; anything else propagates.
 */

#include "config.h"

#include "mail-imap-retry.h"

#include <gio/gio.h>

ImapRetryAction
imap_retry_action_for_error (GError *error)
{
  if (error == NULL)
    return IMAP_RETRY_NONE;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
    return IMAP_RETRY_AUTH;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE))
    return IMAP_RETRY_TRANSPORT;
  return IMAP_RETRY_NONE;
}
