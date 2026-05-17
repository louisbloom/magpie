/* mail-mime.c - MIME helpers (GMime 3.0).
 *
 * Two public entry points serve the message viewer:
 *
 *   mail_mime_pick_best — drives the default "Rendered" mode. Picks
 *     the richest displayable part per RFC 2046 §5.1.4: inside a
 *     multipart/alternative, the *last* recognisable text/html or
 *     text/plain wins (senders order alternatives worst-to-best, so
 *     "last recognisable" is the canonical "best representation");
 *     bare text parts are returned as-is; everything else yields
 *     UNSUPPORTED with the encountered media type in *detail_out.
 *     Non-MIME bodies fall through GMime's RFC 2045 §5.2 default
 *     (text/plain us-ascii) and arrive here as PLAIN.
 *
 *   mail_mime_extract_text_plain — drives the "Plain" toggle. Forces
 *     the text/plain alternative regardless of what would be the best
 *     render, so the user can always escape to the plain version when
 *     one exists. Selection rule mirrors the original two-pass walker:
 *       1. Direct text/plain child of a multipart/alternative wins
 *          (the canonical fallback for that group);
 *       2. otherwise, the first text/plain part anywhere in the tree
 *          (e.g. multipart/mixed { text/plain, attachments }).
 *     A top-level disclaimer text/plain doesn't beat the body inside
 *     a nested alternative — that's the production case for nearly
 *     every promo email.
 *
 * GMime initialisation is process-wide; the g_once guard makes both
 * extractors safe to call from any number of tests without re-init.
 */

#include "config.h"

#include "mail-mime.h"

#include <gmime/gmime.h>

void
mail_mime_ensure_init (void)
{
  static gsize once = 0;
  if (g_once_init_enter (&once))
    {
      g_mime_init ();
      g_once_init_leave (&once, 1);
    }
}

/* Keep the internal name working for the existing call sites below
 * without churning every reference. */
#define ensure_gmime_init mail_mime_ensure_init

static gboolean
is_text_plain (GMimeObject *obj)
{
  if (obj == NULL)
    return FALSE;
  GMimeContentType *ct = g_mime_object_get_content_type (obj);
  return ct != NULL && g_mime_content_type_is_type (ct, "text", "plain");
}

static gboolean
is_text_html (GMimeObject *obj)
{
  if (obj == NULL)
    return FALSE;
  GMimeContentType *ct = g_mime_object_get_content_type (obj);
  return ct != NULL && g_mime_content_type_is_type (ct, "text", "html");
}

static gboolean
is_multipart_alternative (GMimeObject *obj)
{
  if (obj == NULL || !GMIME_IS_MULTIPART (obj))
    return FALSE;
  GMimeContentType *ct = g_mime_object_get_content_type (obj);
  return ct != NULL && g_mime_content_type_is_type (ct, "multipart", "alternative");
}

/* Pass 1: pick the text/plain child of any multipart/alternative.
 * Recurses into non-alternative multiparts so nested alternatives are
 * reachable, but inside an alternative we only look one level deep —
 * the standard treats those children as the alternatives themselves. */
static GMimeObject *
find_plain_in_alternative (GMimeObject *obj)
{
  if (obj == NULL)
    return NULL;

  if (GMIME_IS_MULTIPART (obj))
    {
      GMimeMultipart *mp = (GMimeMultipart *) obj;
      int n = g_mime_multipart_get_count (mp);

      if (is_multipart_alternative (obj))
        {
          for (int i = 0; i < n; i++)
            {
              GMimeObject *child = g_mime_multipart_get_part (mp, i);
              if (is_text_plain (child))
                return child;
            }
          /* No plain at this alternative's top level; an alternative
           * may legitimately contain a nested multipart whose own
           * alternative carries the plain. Recurse. */
        }

      for (int i = 0; i < n; i++)
        {
          GMimeObject *hit = find_plain_in_alternative (g_mime_multipart_get_part (mp, i));
          if (hit != NULL)
            return hit;
        }
      return NULL;
    }

  if (GMIME_IS_MESSAGE_PART (obj))
    {
      GMimeMessage *inner = g_mime_message_part_get_message ((GMimeMessagePart *) obj);
      if (inner != NULL)
        return find_plain_in_alternative (g_mime_message_get_mime_part (inner));
    }

  return NULL;
}

/* Pass 2: first text/plain anywhere in the tree. */
static GMimeObject *
find_any_plain (GMimeObject *obj)
{
  if (obj == NULL)
    return NULL;

  if (GMIME_IS_MULTIPART (obj))
    {
      GMimeMultipart *mp = (GMimeMultipart *) obj;
      int n = g_mime_multipart_get_count (mp);
      for (int i = 0; i < n; i++)
        {
          GMimeObject *hit = find_any_plain (g_mime_multipart_get_part (mp, i));
          if (hit != NULL)
            return hit;
        }
      return NULL;
    }

  if (GMIME_IS_MESSAGE_PART (obj))
    {
      GMimeMessage *inner = g_mime_message_part_get_message ((GMimeMessagePart *) obj);
      if (inner != NULL)
        return find_any_plain (g_mime_message_get_mime_part (inner));
      return NULL;
    }

  return is_text_plain (obj) ? obj : NULL;
}

/* find_displayable: walks the MIME tree to pick the best renderable
 * part for the default view.
 *
 *   - In a multipart/alternative, scan children in reverse and take
 *     the first text/html or text/plain encountered. RFC 2046 §5.1.4
 *     mandates that alternatives are listed worst-to-best, so the
 *     last recognisable type is what we should display. HTML naturally
 *     beats plain in the overwhelmingly common case (plain first,
 *     HTML second) without us having to encode a preference.
 *   - In any other multipart, walk children in order and return the
 *     first hit. This handles multipart/mixed { multipart/alternative,
 *     attachment } and multipart/related { html, image }.
 *   - Recurse through message/rfc822 wrappers (forwarded mail).
 *   - Bare text/html and text/plain are returned as-is.
 *
 * On hit, *kind_out is set to HTML or PLAIN and the part is returned.
 * On miss, returns NULL with *kind_out unchanged.
 */
static GMimeObject *
find_displayable (GMimeObject *obj, MailMimeKind *kind_out)
{
  if (obj == NULL)
    return NULL;

  if (GMIME_IS_MULTIPART (obj))
    {
      GMimeMultipart *mp = (GMimeMultipart *) obj;
      int n = g_mime_multipart_get_count (mp);

      if (is_multipart_alternative (obj))
        {
          for (int i = n - 1; i >= 0; i--)
            {
              GMimeObject *child = g_mime_multipart_get_part (mp, i);
              if (is_text_html (child))
                {
                  *kind_out = MAIL_MIME_KIND_HTML;
                  return child;
                }
              if (is_text_plain (child))
                {
                  *kind_out = MAIL_MIME_KIND_PLAIN;
                  return child;
                }
            }
          /* A nested multipart could carry the alternatives instead;
           * fall through to a forward recursive scan. */
        }

      for (int i = 0; i < n; i++)
        {
          GMimeObject *hit = find_displayable (g_mime_multipart_get_part (mp, i), kind_out);
          if (hit != NULL)
            return hit;
        }
      return NULL;
    }

  if (GMIME_IS_MESSAGE_PART (obj))
    {
      GMimeMessage *inner = g_mime_message_part_get_message ((GMimeMessagePart *) obj);
      if (inner != NULL)
        return find_displayable (g_mime_message_get_mime_part (inner), kind_out);
      return NULL;
    }

  if (is_text_html (obj))
    {
      *kind_out = MAIL_MIME_KIND_HTML;
      return obj;
    }
  if (is_text_plain (obj))
    {
      *kind_out = MAIL_MIME_KIND_PLAIN;
      return obj;
    }

  return NULL;
}

/* Decode a text part to a NUL-terminated UTF-8 string. GMime usually
 * returns UTF-8 already; we validate defensively so a malformed source
 * never escapes into a GtkTextBuffer / WebKitWebView. */
static gchar *
decode_text_utf8 (GMimeObject *part)
{
  if (part == NULL || !GMIME_IS_TEXT_PART (part))
    return NULL;
  gchar *text = g_mime_text_part_get_text (GMIME_TEXT_PART (part));
  if (text == NULL)
    return NULL;
  if (g_utf8_validate (text, -1, NULL))
    return text;
  gchar *valid = g_utf8_make_valid (text, -1);
  g_free (text);
  return valid;
}

MailMimeKind
mail_mime_pick_best (const guint8 *raw, gsize len, gchar **content_out, gchar **detail_out)
{
  g_return_val_if_fail (content_out != NULL, MAIL_MIME_KIND_UNSUPPORTED);
  g_return_val_if_fail (detail_out != NULL, MAIL_MIME_KIND_UNSUPPORTED);

  *content_out = NULL;
  *detail_out = NULL;

  if (raw == NULL || len == 0)
    return MAIL_MIME_KIND_UNSUPPORTED;

  ensure_gmime_init ();

  GMimeStream *stream = g_mime_stream_mem_new_with_buffer ((const char *) raw, len);
  if (stream == NULL)
    return MAIL_MIME_KIND_UNSUPPORTED;

  GMimeParser *parser = g_mime_parser_new_with_stream (stream);
  g_object_unref (stream);
  if (parser == NULL)
    return MAIL_MIME_KIND_UNSUPPORTED;

  GMimeMessage *msg = g_mime_parser_construct_message (parser, NULL);
  g_object_unref (parser);
  if (msg == NULL)
    return MAIL_MIME_KIND_UNSUPPORTED;

  GMimeObject *root = g_mime_message_get_mime_part (msg);
  MailMimeKind kind = MAIL_MIME_KIND_UNSUPPORTED;
  GMimeObject *part = find_displayable (root, &kind);

  MailMimeKind result;
  if (part != NULL)
    {
      gchar *text = decode_text_utf8 (part);
      if (text != NULL)
        {
          *content_out = text;
          result = kind;
        }
      else
        {
          /* Part was nominally displayable but produced no usable
           * text — fall through to unsupported with the type as detail
           * so the user sees a concrete reason. */
          GMimeContentType *ct = g_mime_object_get_content_type (part);
          if (ct != NULL)
            *detail_out = g_mime_content_type_get_mime_type (ct);
          result = MAIL_MIME_KIND_UNSUPPORTED;
        }
    }
  else
    {
      if (root != NULL)
        {
          GMimeContentType *ct = g_mime_object_get_content_type (root);
          if (ct != NULL)
            *detail_out = g_mime_content_type_get_mime_type (ct);
        }
      result = MAIL_MIME_KIND_UNSUPPORTED;
    }

  g_object_unref (msg);
  return result;
}

gchar *
mail_mime_extract_text_plain (const guint8 *raw, gsize len)
{
  if (raw == NULL || len == 0)
    return NULL;

  ensure_gmime_init ();

  GMimeStream *stream = g_mime_stream_mem_new_with_buffer ((const char *) raw, len);
  if (stream == NULL)
    return NULL;

  GMimeParser *parser = g_mime_parser_new_with_stream (stream);
  g_object_unref (stream);
  if (parser == NULL)
    return NULL;

  GMimeMessage *msg = g_mime_parser_construct_message (parser, NULL);
  g_object_unref (parser);
  if (msg == NULL)
    return NULL;

  gchar *result = NULL;
  GMimeObject *root = g_mime_message_get_mime_part (msg);
  GMimeObject *part = find_plain_in_alternative (root);
  if (part == NULL)
    part = find_any_plain (root);
  if (part != NULL && GMIME_IS_TEXT_PART (part))
    {
      gchar *text = g_mime_text_part_get_text (GMIME_TEXT_PART (part));
      if (text != NULL)
        {
          /* GMime returns UTF-8 already; validate defensively so a
           * malformed source never reaches gtk_text_buffer_set_text. */
          if (g_utf8_validate (text, -1, NULL))
            {
              result = text;
            }
          else
            {
              result = g_utf8_make_valid (text, -1);
              g_free (text);
            }
        }
    }

  g_object_unref (msg);
  return result;
}
