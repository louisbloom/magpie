/* mail-mime.h - MIME helpers for the message viewer.
 *
 * Thin GMime 3.0 wrapper. Two entry points: pick the best displayable
 * part of an RFC822 message (default render path), and force-extract
 * the text/plain alternative (Plain toggle). Both shield the rest of
 * the codebase from GMime types.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Kind of body returned by mail_mime_pick_best. */
typedef enum
{
  MAIL_MIME_KIND_HTML,        /* content is HTML markup, UTF-8 */
  MAIL_MIME_KIND_PLAIN,       /* content is plain text, UTF-8 */
  MAIL_MIME_KIND_UNSUPPORTED, /* no displayable part found */
} MailMimeKind;

/* Pick the best displayable part of an RFC822 message.
 *
 * Selection rule (RFC 2046 §5.1.4 + RFC 2045 §5.2):
 *   - Inside a multipart/alternative, return the *last* child whose
 *     content type is text/html or text/plain (the spec mandates that
 *     senders order alternatives worst-to-best, so the last
 *     recognisable type is the best representation).
 *   - In any other multipart container, recurse into children and
 *     return the first hit.
 *   - A bare text/html or text/plain top-level part is returned as-is.
 *   - Absent Content-Type is text/plain by default (RFC 2045 §5.2);
 *     GMime materialises that for us.
 *   - Anything else yields MAIL_MIME_KIND_UNSUPPORTED, with
 *     *detail_out set to the encountered top-level media type (e.g.
 *     "application/pdf") for the caller to display to the user.
 *
 * On HTML / PLAIN: *content_out is set to a newly-allocated NUL-
 * terminated UTF-8 string; *detail_out is set to NULL.
 * On UNSUPPORTED: *content_out is NULL; *detail_out is set if a
 * concrete top-level type was recovered, NULL when parsing failed.
 *
 * Caller frees both *content_out and *detail_out with g_free().
 * content_out and detail_out must both be non-NULL.
 */
MailMimeKind mail_mime_pick_best (const guint8 *raw, gsize len, gchar **content_out, gchar **detail_out);

/* Extract the decoded text/plain body of an RFC822 message.
 *
 * Returns a newly-allocated, NUL-terminated UTF-8 string the caller
 * must free with g_free(), or NULL if no text/plain part is available
 * or the message fails to parse.
 *
 * Handles charset conversion and transfer-encoding (quoted-printable,
 * base64) via GMime. For multipart/alternative messages the text/plain
 * part is preferred over text/html — this is the "force plain" path
 * the Plain toggle binds to, independent of mail_mime_pick_best.
 */
gchar *mail_mime_extract_text_plain (const guint8 *raw, gsize len);

G_END_DECLS
