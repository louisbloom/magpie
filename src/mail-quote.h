/* mail-quote.h - Reply quoting helpers (Gnus-style).
 *
 * Pure functions: no GTK, no GMime. The mail-mime parsers extract the
 * fields we need (sender name/addr, original body); these helpers turn
 * them into the body of a reply that follows the same conventions as
 * Emacs message-mode:
 *
 *   message-yank-prefix       = "> "
 *   message-yank-cited-prefix = ">"
 *   message-yank-empty-prefix = ">"
 *
 * Signature stripping per RFC 3676 §4.3 — the first line equal to
 * "-- " (dash, dash, space) plus everything after it is removed before
 * quoting.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Quote a plain-text body following the Gnus rules above.
 *
 * Behaviour:
 *   - line endings normalised to "\n";
 *   - signature ("-- \n…") stripped;
 *   - fresh lines prefixed "> ";
 *   - lines that already start with '>' prefixed ">" (no space);
 *   - blank lines prefixed ">" (no trailing whitespace).
 *
 * Returns a newly-allocated NUL-terminated UTF-8 string; caller frees
 * with g_free(). Returns NULL only if `body` is NULL. */
gchar *mail_quote_text_plain (const char *body);

/* Build a Gnus-style attribution line:
 *
 *   "<Name> <addr> writes:"   if name is non-empty
 *   "<addr> writes:"          otherwise
 *
 * Returns a newly-allocated string. Caller frees with g_free().
 * Returns NULL if `addr` is NULL or empty. */
gchar *mail_quote_attribution (const char *from_name, const char *from_addr);

/* Compose attribution + blank line + quoted body. Convenience that
 * combines mail_quote_attribution and mail_quote_text_plain. */
gchar *mail_quote_build_reply_body (const char *original_plain,
                                    const char *from_name,
                                    const char *from_addr);

/* Idempotent "Re: " prefix on a subject (RFC 5322 §3.6.5).
 *
 * Already-prefixed subjects ("Re: foo", "RE: foo", "re: foo") pass
 * through unchanged. NULL or empty input yields "Re: ". Caller frees
 * with g_free(). */
gchar *mail_quote_subject_reply (const char *original_subject);

G_END_DECLS
