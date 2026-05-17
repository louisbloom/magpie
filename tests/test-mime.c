/* tests/test-mime.c - mail-mime extractor regression tests.
 *
 * Two suites in one binary:
 *
 *  /mime/...      - mail_mime_extract_text_plain: forces the
 *                   text/plain alternative for the Plain toggle.
 *  /mime/pick/... - mail_mime_pick_best: drives the default render
 *                   path, picking the richest displayable part per
 *                   RFC 2046 §5.1.4 (last recognisable in
 *                   multipart/alternative wins; HTML naturally beats
 *                   plain when both are present in the standard order).
 *
 * Each case builds a small RFC822 byte buffer inline and asserts the
 * extractor's output. Regression context: modern multipart mail
 * (multipart/alternative, quoted-printable, base64, non-UTF-8 charsets)
 * renders as gibberish in the raw view; the viewer relies on these two
 * entry points to populate the rendered and plain modes respectively.
 */

#include "../src/mail-mime.h"

#include <glib.h>
#include <string.h>

static void
test_plain_only (void)
{
  static const char raw[] =
      "From: a@x\r\n"
      "Subject: t\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "hello plain world\r\n";
  g_autofree gchar *out = mail_mime_extract_text_plain ((const guint8 *) raw, sizeof raw - 1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "hello plain world") != NULL);
}

static void
test_quoted_printable (void)
{
  static const char raw[] =
      "From: a@x\r\n"
      "Subject: t\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "Content-Transfer-Encoding: quoted-printable\r\n"
      "\r\n"
      "caf=C3=A9 break\r\n";
  g_autofree gchar *out = mail_mime_extract_text_plain ((const guint8 *) raw, sizeof raw - 1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "café break") != NULL);
}

static void
test_base64 (void)
{
  /* base64("Hello\n") == "SGVsbG8K" */
  static const char raw[] =
      "From: a@x\r\n"
      "Subject: t\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "Content-Transfer-Encoding: base64\r\n"
      "\r\n"
      "SGVsbG8K\r\n";
  g_autofree gchar *out = mail_mime_extract_text_plain ((const guint8 *) raw, sizeof raw - 1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "Hello") != NULL);
}

static void
test_multipart_alternative (void)
{
  static const char raw[] =
      "From: a@x\r\n"
      "Subject: t\r\n"
      "MIME-Version: 1.0\r\n"
      "Content-Type: multipart/alternative; boundary=\"BOUND\"\r\n"
      "\r\n"
      "--BOUND\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "plain version\r\n"
      "--BOUND\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n"
      "<p>HTML version</p>\r\n"
      "--BOUND--\r\n";
  g_autofree gchar *out = mail_mime_extract_text_plain ((const guint8 *) raw, sizeof raw - 1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "plain version") != NULL);
  g_assert_true (strstr (out, "HTML") == NULL);
}

static void
test_prefers_alternative_plain_over_top_level (void)
{
  /* Regression for the MIME selection rule (RFC 2046 §5.1.4): when the
   * tree has a text/plain at top level *and* a multipart/alternative
   * whose own text/plain is the real body, the alternative wins. The
   * naive depth-first "first plain" walker returned the disclaimer
   * here and stopped — wrong. */
  static const char raw[] =
      "From: a@x\r\n"
      "Subject: t\r\n"
      "MIME-Version: 1.0\r\n"
      "Content-Type: multipart/mixed; boundary=\"A\"\r\n"
      "\r\n"
      "--A\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "DISCLAIMER text\r\n"
      "--A\r\n"
      "Content-Type: multipart/alternative; boundary=\"B\"\r\n"
      "\r\n"
      "--B\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "real plain body\r\n"
      "--B\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n"
      "<p>html</p>\r\n"
      "--B--\r\n"
      "--A--\r\n";
  g_autofree gchar *out = mail_mime_extract_text_plain ((const guint8 *) raw, sizeof raw - 1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "real plain body") != NULL);
  g_assert_true (strstr (out, "DISCLAIMER") == NULL);
}

static void
test_falls_back_to_top_level_plain (void)
{
  /* No multipart/alternative anywhere; the top-level text/plain in a
   * multipart/mixed (typical for "body + attachment") is the body. */
  static const char raw[] =
      "From: a@x\r\n"
      "Subject: t\r\n"
      "MIME-Version: 1.0\r\n"
      "Content-Type: multipart/mixed; boundary=\"X\"\r\n"
      "\r\n"
      "--X\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "the only body here\r\n"
      "--X\r\n"
      "Content-Type: application/octet-stream\r\n"
      "Content-Disposition: attachment; filename=\"a.bin\"\r\n"
      "\r\n"
      "binarydata\r\n"
      "--X--\r\n";
  g_autofree gchar *out = mail_mime_extract_text_plain ((const guint8 *) raw, sizeof raw - 1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "the only body here") != NULL);
}

static void
test_html_only (void)
{
  static const char raw[] =
      "From: a@x\r\n"
      "Subject: t\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n"
      "<p>only html</p>\r\n";
  g_autofree gchar *out = mail_mime_extract_text_plain ((const guint8 *) raw, sizeof raw - 1);
  /* No text/plain alternative; the toggle should stay hidden, so the
   * extractor must return NULL rather than the HTML body. */
  g_assert_null (out);
}

static void
test_charset_iso_8859_1 (void)
{
  /* 0xe9 is "é" in ISO-8859-1. */
  static const char raw[] =
      "From: a@x\r\n"
      "Subject: t\r\n"
      "Content-Type: text/plain; charset=iso-8859-1\r\n"
      "\r\n"
      "caf\xe9 break\r\n";
  g_autofree gchar *out = mail_mime_extract_text_plain ((const guint8 *) raw, sizeof raw - 1);
  g_assert_nonnull (out);
  g_assert_true (strstr (out, "café") != NULL);
}

static void
test_empty_input (void)
{
  g_autofree gchar *out = mail_mime_extract_text_plain (NULL, 0);
  g_assert_null (out);
}

static void
test_garbage (void)
{
  static const guint8 garbage[] = { 0x00, 0xff, 0x42, 0x13, 0x37, 0xaa, 0x55 };
  g_autofree gchar *out = mail_mime_extract_text_plain (garbage, sizeof garbage);
  /* GMime may parse this as "headerless body with no content-type",
   * which by RFC defaults to text/plain — so either NULL or a string
   * is acceptable. The contract we need is "no crash"; if we got a
   * string, it must be valid UTF-8. */
  if (out != NULL)
    g_assert_true (g_utf8_validate (out, -1, NULL));
}

/* --- mail_mime_pick_best ---------------------------------------- */

static void
test_pick_alternative_html_wins (void)
{
  /* Plain first, HTML second — the standard ordering. RFC 2046 §5.1.4
   * says the last recognisable type is the best representation; HTML
   * wins here. */
  static const char raw[] =
      "From: a@x\r\n"
      "MIME-Version: 1.0\r\n"
      "Content-Type: multipart/alternative; boundary=\"B\"\r\n"
      "\r\n"
      "--B\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "plain version\r\n"
      "--B\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n"
      "<p>HTML version</p>\r\n"
      "--B--\r\n";
  g_autofree gchar *content = NULL;
  g_autofree gchar *detail = NULL;
  MailMimeKind k = mail_mime_pick_best ((const guint8 *) raw, sizeof raw - 1,
                                        &content, &detail);
  g_assert_cmpint (k, ==, MAIL_MIME_KIND_HTML);
  g_assert_nonnull (content);
  g_assert_true (strstr (content, "<p>HTML version</p>") != NULL);
  g_assert_null (detail);
}

static void
test_pick_alternative_plain_only (void)
{
  static const char raw[] =
      "From: a@x\r\n"
      "MIME-Version: 1.0\r\n"
      "Content-Type: multipart/alternative; boundary=\"B\"\r\n"
      "\r\n"
      "--B\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "just the plain body\r\n"
      "--B--\r\n";
  g_autofree gchar *content = NULL;
  g_autofree gchar *detail = NULL;
  MailMimeKind k = mail_mime_pick_best ((const guint8 *) raw, sizeof raw - 1,
                                        &content, &detail);
  g_assert_cmpint (k, ==, MAIL_MIME_KIND_PLAIN);
  g_assert_nonnull (content);
  g_assert_true (strstr (content, "just the plain body") != NULL);
}

static void
test_pick_top_level_html (void)
{
  static const char raw[] =
      "From: a@x\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n"
      "<h1>hi</h1>\r\n";
  g_autofree gchar *content = NULL;
  g_autofree gchar *detail = NULL;
  MailMimeKind k = mail_mime_pick_best ((const guint8 *) raw, sizeof raw - 1,
                                        &content, &detail);
  g_assert_cmpint (k, ==, MAIL_MIME_KIND_HTML);
  g_assert_nonnull (content);
  g_assert_true (strstr (content, "<h1>hi</h1>") != NULL);
}

static void
test_pick_top_level_plain (void)
{
  static const char raw[] =
      "From: a@x\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "hi there\r\n";
  g_autofree gchar *content = NULL;
  g_autofree gchar *detail = NULL;
  MailMimeKind k = mail_mime_pick_best ((const guint8 *) raw, sizeof raw - 1,
                                        &content, &detail);
  g_assert_cmpint (k, ==, MAIL_MIME_KIND_PLAIN);
  g_assert_nonnull (content);
  g_assert_true (strstr (content, "hi there") != NULL);
}

static void
test_pick_no_content_type (void)
{
  /* RFC 2045 §5.2: absence of Content-Type is "text/plain; us-ascii".
   * GMime materialises that default, so the picker reports PLAIN. */
  static const char raw[] =
      "From: a@x\r\n"
      "Subject: no content type\r\n"
      "\r\n"
      "body of a non-MIME message\r\n";
  g_autofree gchar *content = NULL;
  g_autofree gchar *detail = NULL;
  MailMimeKind k = mail_mime_pick_best ((const guint8 *) raw, sizeof raw - 1,
                                        &content, &detail);
  g_assert_cmpint (k, ==, MAIL_MIME_KIND_PLAIN);
  g_assert_nonnull (content);
  g_assert_true (strstr (content, "body of a non-MIME message") != NULL);
}

static void
test_pick_unsupported_pdf (void)
{
  static const char raw[] =
      "From: a@x\r\n"
      "Content-Type: application/pdf\r\n"
      "Content-Transfer-Encoding: base64\r\n"
      "\r\n"
      "SGVsbG8K\r\n";
  g_autofree gchar *content = NULL;
  g_autofree gchar *detail = NULL;
  MailMimeKind k = mail_mime_pick_best ((const guint8 *) raw, sizeof raw - 1,
                                        &content, &detail);
  g_assert_cmpint (k, ==, MAIL_MIME_KIND_UNSUPPORTED);
  g_assert_null (content);
  g_assert_nonnull (detail);
  g_assert_cmpstr (detail, ==, "application/pdf");
}

static void
test_pick_nested_mixed_alternative (void)
{
  /* multipart/mixed { multipart/alternative { plain, html },
   *                   application/pdf attachment } — the picker recurses
   * into the first child, finds the alternative, and returns its HTML. */
  static const char raw[] =
      "From: a@x\r\n"
      "MIME-Version: 1.0\r\n"
      "Content-Type: multipart/mixed; boundary=\"A\"\r\n"
      "\r\n"
      "--A\r\n"
      "Content-Type: multipart/alternative; boundary=\"B\"\r\n"
      "\r\n"
      "--B\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "\r\n"
      "plain\r\n"
      "--B\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "\r\n"
      "<p>HTML body</p>\r\n"
      "--B--\r\n"
      "--A\r\n"
      "Content-Type: application/pdf\r\n"
      "Content-Disposition: attachment; filename=\"x.pdf\"\r\n"
      "Content-Transfer-Encoding: base64\r\n"
      "\r\n"
      "SGVsbG8K\r\n"
      "--A--\r\n";
  g_autofree gchar *content = NULL;
  g_autofree gchar *detail = NULL;
  MailMimeKind k = mail_mime_pick_best ((const guint8 *) raw, sizeof raw - 1,
                                        &content, &detail);
  g_assert_cmpint (k, ==, MAIL_MIME_KIND_HTML);
  g_assert_nonnull (content);
  g_assert_true (strstr (content, "<p>HTML body</p>") != NULL);
}

static void
test_pick_empty_input (void)
{
  g_autofree gchar *content = NULL;
  g_autofree gchar *detail = NULL;
  MailMimeKind k = mail_mime_pick_best (NULL, 0, &content, &detail);
  g_assert_cmpint (k, ==, MAIL_MIME_KIND_UNSUPPORTED);
  g_assert_null (content);
  g_assert_null (detail);
}

int
main (int argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/mime/plain-only", test_plain_only);
  g_test_add_func ("/mime/quoted-printable", test_quoted_printable);
  g_test_add_func ("/mime/base64", test_base64);
  g_test_add_func ("/mime/multipart-alternative", test_multipart_alternative);
  g_test_add_func ("/mime/prefers-alternative-plain-over-top-level",
                   test_prefers_alternative_plain_over_top_level);
  g_test_add_func ("/mime/falls-back-to-top-level-plain",
                   test_falls_back_to_top_level_plain);
  g_test_add_func ("/mime/html-only", test_html_only);
  g_test_add_func ("/mime/charset-iso-8859-1", test_charset_iso_8859_1);
  g_test_add_func ("/mime/empty-input", test_empty_input);
  g_test_add_func ("/mime/garbage", test_garbage);

  g_test_add_func ("/mime/pick/alternative-html-wins",
                   test_pick_alternative_html_wins);
  g_test_add_func ("/mime/pick/alternative-plain-only",
                   test_pick_alternative_plain_only);
  g_test_add_func ("/mime/pick/top-level-html", test_pick_top_level_html);
  g_test_add_func ("/mime/pick/top-level-plain", test_pick_top_level_plain);
  g_test_add_func ("/mime/pick/no-content-type", test_pick_no_content_type);
  g_test_add_func ("/mime/pick/unsupported-pdf", test_pick_unsupported_pdf);
  g_test_add_func ("/mime/pick/nested-mixed-alternative",
                   test_pick_nested_mixed_alternative);
  g_test_add_func ("/mime/pick/empty-input", test_pick_empty_input);

  return g_test_run ();
}
