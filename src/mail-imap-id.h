/* mail-imap-id.h - Encode (UIDVALIDITY, UID, mailbox-name) as the
 *                  MailMessageMeta.id used by the IMAP backend.
 *
 * IMAP UIDs are per-folder. The sync engine round-trips id strings
 * across arena resets between list_messages and fetch_message_raw,
 * so the id has to carry enough information to find the right
 * mailbox and verify UIDVALIDITY at fetch time. Format:
 *
 *   <uidvalidity>\x01<uid>\x01<folder_name>
 *
 * SOH (\x01) is safe as a delimiter because RFC 3501 mailbox names
 * use modified UTF-7 / printable bytes — no control chars.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Format an id string. Caller frees with g_free(). */
gchar *mail_imap_id_encode (guint32 uidvalidity,
                            guint32 uid,
                            const char *folder_name);

/* Parse an id string in place.
 *
 * On success, *folder_name_out points into @id (no allocation); the
 * caller must keep @id alive for the lifetime of *folder_name_out.
 * On any malformed input (missing delimiter, non-numeric component,
 * empty folder name) returns FALSE and leaves the out-params
 * untouched.
 */
gboolean mail_imap_id_decode (const char *id,
                              guint32 *uidvalidity_out,
                              guint32 *uid_out,
                              const char **folder_name_out);

G_END_DECLS
