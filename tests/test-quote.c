/* tests/test-quote.c - mail-quote regression tests.
 *
 * Covers the Gnus-style reply quoting rules we follow:
 *
 *   message-yank-prefix       = "> "  (fresh quote)
 *   message-yank-cited-prefix = ">"   (already cited)
 *   message-yank-empty-prefix = ">"   (blank line)
 *
 * Plus signature stripping (RFC 3676 §4.3) and idempotent "Re: "
 * subject prefixing (RFC 5322 §3.6.5).
 */

#include "../src/mail-quote.h"

#include <glib.h>
#include <string.h>

static void
test_prefix_basic (void)
{
  g_autofree gchar *out = mail_quote_text_plain ("hello\nworld\n");
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==, "> hello\n> world\n");
}

static void
test_prefix_empty_line (void)
{
  /* Blank lines get ">" with no trailing space — message-yank-empty-prefix. */
  g_autofree gchar *out = mail_quote_text_plain ("a\n\nb\n");
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==, "> a\n>\n> b\n");
}

static void
test_prefix_already_cited (void)
{
  /* Already-cited lines start with '>'; we add a single '>' (no space)
   * so the output looks like ">>old text" / ">> old text". */
  g_autofree gchar *out = mail_quote_text_plain ("> old\n>> deeper\nfresh\n");
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==, ">> old\n>>> deeper\n> fresh\n");
}

static void
test_strip_signature (void)
{
  /* The "-- \n" (dash-dash-SPACE-NL) marker and everything after is
   * dropped before quoting. */
  g_autofree gchar *out = mail_quote_text_plain ("body line\n-- \nSig Name\nhttps://example\n");
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==, "> body line\n");
}

static void
test_strip_signature_no_match_when_no_space (void)
{
  /* "--\n" without the trailing space is NOT a sig delimiter. */
  g_autofree gchar *out = mail_quote_text_plain ("body\n--\nstill body\n");
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==, "> body\n> --\n> still body\n");
}

static void
test_normalises_crlf (void)
{
  g_autofree gchar *out = mail_quote_text_plain ("a\r\nb\r\n");
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==, "> a\n> b\n");
}

static void
test_no_trailing_newline (void)
{
  /* Caller may pass a body without a terminating newline. Each line is
   * still quoted; we don't invent a final newline. */
  g_autofree gchar *out = mail_quote_text_plain ("one\ntwo");
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==, "> one\n> two");
}

static void
test_empty_input (void)
{
  g_autofree gchar *out = mail_quote_text_plain ("");
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==, "");
}

static void
test_null_input (void)
{
  g_autofree gchar *out = mail_quote_text_plain (NULL);
  g_assert_null (out);
}

static void
test_attribution_with_name (void)
{
  g_autofree gchar *out = mail_quote_attribution ("Alice Example", "alice@example.com");
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==, "Alice Example <alice@example.com> writes:");
}

static void
test_attribution_addr_only (void)
{
  g_autofree gchar *out = mail_quote_attribution (NULL, "alice@example.com");
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==, "alice@example.com writes:");
}

static void
test_attribution_empty_name (void)
{
  g_autofree gchar *out = mail_quote_attribution ("", "alice@example.com");
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==, "alice@example.com writes:");
}

static void
test_attribution_no_addr (void)
{
  g_autofree gchar *out = mail_quote_attribution ("Alice", NULL);
  g_assert_null (out);
}

static void
test_build_reply_body (void)
{
  g_autofree gchar *out = mail_quote_build_reply_body (
      "Hi Bob,\n\nCan you take a look?\n",
      "Alice", "alice@example.com");
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==,
                   "Alice <alice@example.com> writes:\n"
                   "\n"
                   "> Hi Bob,\n"
                   ">\n"
                   "> Can you take a look?\n");
}

static void
test_subject_reply_adds_re (void)
{
  g_autofree gchar *out = mail_quote_subject_reply ("Hello");
  g_assert_cmpstr (out, ==, "Re: Hello");
}

static void
test_subject_reply_idempotent (void)
{
  /* "Re: Re: foo" must not happen. */
  g_autofree gchar *out = mail_quote_subject_reply ("Re: Hello");
  g_assert_cmpstr (out, ==, "Re: Hello");
}

static void
test_subject_reply_case_insensitive (void)
{
  /* "RE:" / "re:" both count. */
  g_autofree gchar *out1 = mail_quote_subject_reply ("RE: yo");
  g_autofree gchar *out2 = mail_quote_subject_reply ("re: yo");
  g_assert_cmpstr (out1, ==, "RE: yo");
  g_assert_cmpstr (out2, ==, "re: yo");
}

static void
test_subject_reply_null (void)
{
  g_autofree gchar *out = mail_quote_subject_reply (NULL);
  g_assert_cmpstr (out, ==, "Re: ");
}

static void
test_subject_reply_empty (void)
{
  g_autofree gchar *out = mail_quote_subject_reply ("");
  g_assert_cmpstr (out, ==, "Re: ");
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/quote/prefix-basic", test_prefix_basic);
  g_test_add_func ("/quote/prefix-empty-line", test_prefix_empty_line);
  g_test_add_func ("/quote/prefix-already-cited", test_prefix_already_cited);
  g_test_add_func ("/quote/strip-signature", test_strip_signature);
  g_test_add_func ("/quote/strip-signature-no-match-when-no-space",
                   test_strip_signature_no_match_when_no_space);
  g_test_add_func ("/quote/normalises-crlf", test_normalises_crlf);
  g_test_add_func ("/quote/no-trailing-newline", test_no_trailing_newline);
  g_test_add_func ("/quote/empty-input", test_empty_input);
  g_test_add_func ("/quote/null-input", test_null_input);

  g_test_add_func ("/quote/attribution-with-name", test_attribution_with_name);
  g_test_add_func ("/quote/attribution-addr-only", test_attribution_addr_only);
  g_test_add_func ("/quote/attribution-empty-name", test_attribution_empty_name);
  g_test_add_func ("/quote/attribution-no-addr", test_attribution_no_addr);

  g_test_add_func ("/quote/build-reply-body", test_build_reply_body);

  g_test_add_func ("/quote/subject-reply-adds-re", test_subject_reply_adds_re);
  g_test_add_func ("/quote/subject-reply-idempotent", test_subject_reply_idempotent);
  g_test_add_func ("/quote/subject-reply-case-insensitive", test_subject_reply_case_insensitive);
  g_test_add_func ("/quote/subject-reply-null", test_subject_reply_null);
  g_test_add_func ("/quote/subject-reply-empty", test_subject_reply_empty);

  return g_test_run ();
}
