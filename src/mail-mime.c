/* mail-mime.c - MIME helpers (GMime 3.0).
 *
 * The viewer holds the raw RFC822 bytes and asks this module for the
 * decoded text/plain body. Returning NULL means "no plain alternative"
 * — the viewer hides its toggle in that case.
 *
 * Selection rule (RFC 2046 §5.1.4):
 *   1. If the part tree contains a multipart/alternative whose direct
 *      children include a text/plain, prefer that one. The alternative
 *      group is by definition "the same content in different formats",
 *      and the plain version is the canonical fallback for that group.
 *   2. Otherwise, fall back to the first text/plain part anywhere in
 *      the tree (e.g. multipart/mixed { text/plain, attachments }).
 *
 * The two-pass walk means a top-level disclaimer that happens to be
 * text/plain doesn't beat the actual body inside a nested alternative
 * — which is the production case for nearly every promo email.
 *
 * GMime initialisation is process-wide; the g_once guard makes the
 * extractor safe to call from any number of tests without re-init.
 */

#include "config.h"

#include "mail-mime.h"

#include <gmime/gmime.h>

static void
ensure_gmime_init (void)
{
  static gsize once = 0;
  if (g_once_init_enter (&once))
    {
      g_mime_init ();
      g_once_init_leave (&once, 1);
    }
}

static gboolean
is_text_plain (GMimeObject *obj)
{
  if (obj == NULL)
    return FALSE;
  GMimeContentType *ct = g_mime_object_get_content_type (obj);
  return ct != NULL && g_mime_content_type_is_type (ct, "text", "plain");
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
