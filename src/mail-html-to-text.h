/* mail-html-to-text.h - HTML → plain-text converter for reply quoting.
 *
 * This is NOT a renderer. It's the fallback the compose dialog uses
 * when an incoming message has no text/plain alternative: we need
 * *something* readable to put behind "> " in the reply body.
 *
 * Backed by libxml2's HTML parser in error-recovering mode. JavaScript
 * and styles are dropped, block elements become newlines, links keep
 * their href if it adds information, and <blockquote> content is
 * pre-prefixed with "> " so the existing quote-builder stacks on top
 * of it correctly.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Convert an HTML document fragment to plain text.
 *
 * len: length in bytes, or -1 if `html` is NUL-terminated.
 *
 * Returns a newly-allocated UTF-8 string (caller g_frees), or NULL if
 * `html` is NULL or libxml2 fails entirely. An empty document yields
 * the empty string. */
gchar *mail_html_to_text (const char *html, gssize len);

G_END_DECLS
