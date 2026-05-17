/* tests/test-mime.c - mail-mime extractor regression tests.
 *
 * Each case builds a small RFC822 byte buffer inline (literal string)
 * and asserts what mail_mime_extract_text_plain returns. These are the
 * shapes the message viewer must handle when offering its raw / plain
 * toggle.
 *
 * Regression context: modern multipart mail (multipart/alternative,
 * quoted-printable, base64, non-UTF-8 charsets) renders as gibberish
 * in the raw view. The viewer relies on this extractor to populate the
 * "plain" alternative; if extraction silently drops content the toggle
 * becomes either invisible (regression A) or shows the wrong text
 * (regression B). Either is a bug; these cases pin both.
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
  return g_test_run ();
}
