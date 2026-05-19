/* mail-outbox.h - Append a composed message to a debug mbox file.
 *
 * Until SMTP is wired up, "Send" in the compose dialog just appends
 * the composed RFC 5322 message to <account_root>/Outbox.mbox. The
 * file is mbox(rd) so mutt and less can read it.
 *
 * Locking: each append takes flock(LOCK_EX) on the file so concurrent
 * appends (and reads from a running mutt) don't tear.
 */

#pragma once

#include "mail-account.h"
#include "mail-mime.h"

#include <glib.h>

G_BEGIN_DECLS

/* Build an RFC 5322 message from the given fields and append it to
 * ~/Mail/<account-identity>/Outbox.mbox.
 *
 *   account     : sender account; @identity is used for From.
 *   to          : single recipient mailbox addr (RFC 5322 mailbox syntax OK).
 *   subject     : Subject header value (may be empty / NULL).
 *   body        : UTF-8 plain text body.
 *   in_reply_to : if non-NULL, the message's In-Reply-To and References
 *                 are derived from this. May be NULL for fresh mail.
 *
 * Returns TRUE on success. On failure, *error is set if non-NULL. */
gboolean mail_outbox_append (MailAccount *account,
                             const char *to,
                             const char *subject,
                             const char *body,
                             MailMimeReplySource *in_reply_to,
                             GError **error);

G_END_DECLS
