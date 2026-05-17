/* mail-mime.h - MIME helpers for the message viewer.
 *
 * Thin GMime 3.0 wrapper. The only public entry point extracts the
 * decoded text/plain body of an RFC822 message; the viewer uses it to
 * offer a raw / plain toggle without dragging GMime into the rest of
 * the codebase.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Extract the decoded text/plain body of an RFC822 message.
 *
 * Returns a newly-allocated, NUL-terminated UTF-8 string the caller
 * must free with g_free(), or NULL if no text/plain part is available
 * or the message fails to parse.
 *
 * Handles charset conversion and transfer-encoding (quoted-printable,
 * base64) via GMime. For multipart/alternative messages the text/plain
 * part is preferred over text/html.
 */
gchar *mail_mime_extract_text_plain (const guint8 *raw, gsize len);

G_END_DECLS
