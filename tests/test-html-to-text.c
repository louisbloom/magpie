/* tests/test-html-to-text.c - mail-html-to-text regression tests.
 *
 * The converter is the fallback for HTML-only messages we want to
 * quote in a reply. It's not a renderer — it tries to produce a
 * plain-text approximation that survives a `> ` prefix without
 * looking absurd.
 */

#include "../src/mail-html-to-text.h"

#include <glib.h>
#include <string.h>

static void
test_strips_tags (void)
{
  g_autofree gchar *out = mail_html_to_text ("<p>hello world</p>", -1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "hello world") != NULL);
  g_assert_true (strstr (out, "<p>") == NULL);
}

static void
test_p_and_br_become_newlines (void)
{
  g_autofree gchar *out = mail_html_to_text ("<p>one</p><p>two</p>", -1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "one") != NULL);
  g_assert_true (strstr (out, "two") != NULL);
  /* "one" must be on its own line, separated from "two". */
  const char *one = strstr (out, "one");
  const char *two = strstr (out, "two");
  g_assert_true (one != NULL && two != NULL && one < two);
  /* At least one newline between them. */
  gboolean found_nl = FALSE;
  for (const char *p = one; p < two; p++)
    if (*p == '\n')
      {
        found_nl = TRUE;
        break;
      }
  g_assert_true (found_nl);
}

static void
test_br (void)
{
  g_autofree gchar *out = mail_html_to_text ("alpha<br>beta", -1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "alpha\nbeta") != NULL);
}

static void
test_script_style_dropped (void)
{
  g_autofree gchar *out = mail_html_to_text (
      "<html><head><style>p{color:red}</style></head>"
      "<body><script>alert(1)</script><p>visible</p></body></html>",
      -1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "visible") != NULL);
  g_assert_true (strstr (out, "alert") == NULL);
  g_assert_true (strstr (out, "color:red") == NULL);
}

static void
test_blockquote_prefixed (void)
{
  /* Nested <blockquote> should yield "> "-prefixed lines so the
   * quote-builder can stack on top of them sensibly. */
  g_autofree gchar *out = mail_html_to_text (
      "<p>top</p><blockquote><p>cited</p></blockquote>", -1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "top") != NULL);
  g_assert_true (strstr (out, "> cited") != NULL);
}

static void
test_entities_decoded (void)
{
  g_autofree gchar *out = mail_html_to_text ("<p>a &amp; b &lt;c&gt;</p>", -1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "a & b <c>") != NULL);
}

static void
test_malformed_recovers (void)
{
  /* HTML_PARSE_RECOVER: missing close tags, stray angle brackets,
   * unknown elements — must not crash, must return SOMETHING. */
  g_autofree gchar *out = mail_html_to_text (
      "<p>open<div><span>nested<p>second", -1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "open") != NULL);
  g_assert_true (strstr (out, "nested") != NULL);
  g_assert_true (strstr (out, "second") != NULL);
}

static void
test_collapses_whitespace (void)
{
  /* Browser-style: collapse runs of inline whitespace to a single space. */
  g_autofree gchar *out = mail_html_to_text (
      "<p>  hello    world  </p>", -1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "hello world") != NULL);
  g_assert_true (strstr (out, "hello  world") == NULL);
}

static void
test_collapses_blank_lines (void)
{
  /* Many empty blocks shouldn't produce huge runs of blank lines. */
  g_autofree gchar *out = mail_html_to_text (
      "<p>a</p><p></p><p></p><p></p><p>b</p>", -1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "\n\n\n") == NULL);
}

static void
test_null_input (void)
{
  g_autofree gchar *out = mail_html_to_text (NULL, 0);
  g_assert_null (out);
}

static void
test_empty_input (void)
{
  g_autofree gchar *out = mail_html_to_text ("", 0);
  g_assert_nonnull (out);
  g_assert_cmpstr (out, ==, "");
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/html-to-text/strips-tags", test_strips_tags);
  g_test_add_func ("/html-to-text/p-and-br-become-newlines", test_p_and_br_become_newlines);
  g_test_add_func ("/html-to-text/br", test_br);
  g_test_add_func ("/html-to-text/script-style-dropped", test_script_style_dropped);
  g_test_add_func ("/html-to-text/blockquote-prefixed", test_blockquote_prefixed);
  g_test_add_func ("/html-to-text/entities-decoded", test_entities_decoded);
  g_test_add_func ("/html-to-text/malformed-recovers", test_malformed_recovers);
  g_test_add_func ("/html-to-text/collapses-whitespace", test_collapses_whitespace);
  g_test_add_func ("/html-to-text/collapses-blank-lines", test_collapses_blank_lines);
  g_test_add_func ("/html-to-text/null-input", test_null_input);
  g_test_add_func ("/html-to-text/empty-input", test_empty_input);

  return g_test_run ();
}
