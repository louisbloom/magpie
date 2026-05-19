/* mail-quote.c - Reply quoting helpers (Gnus-style). */

#include "config.h"

#include "mail-quote.h"

#include <string.h>

/* Detect the RFC 3676 §4.3 signature delimiter at the start of `line`.
 * The whole line (no trailing whitespace beyond the mandated space)
 * must be exactly "-- ". We accept "-- \n" and a bare "-- " at EOF;
 * "--\n" without the trailing space is NOT a delimiter (common typo
 * we explicitly don't honour, to match mutt/gnus). */
static gboolean
is_sig_delim (const char *line, gsize len)
{
  return len == 3 && line[0] == '-' && line[1] == '-' && line[2] == ' ';
}

gchar *
mail_quote_text_plain (const char *body)
{
  if (body == NULL)
    return NULL;

  /* Walk the body line by line, normalising "\r\n" to "\n" and
   * stopping at a sig delimiter. */
  GString *out = g_string_new (NULL);
  const char *p = body;
  while (*p != '\0')
    {
      const char *eol = strchr (p, '\n');
      const char *line_end = eol != NULL ? eol : p + strlen (p);

      gsize len = (gsize) (line_end - p);
      gsize content_len = len;
      if (content_len > 0 && p[content_len - 1] == '\r')
        content_len--;

      if (is_sig_delim (p, content_len))
        break;

      if (content_len == 0)
        g_string_append_c (out, '>');
      else if (p[0] == '>')
        {
          g_string_append_c (out, '>');
          g_string_append_len (out, p, (gssize) content_len);
        }
      else
        {
          g_string_append (out, "> ");
          g_string_append_len (out, p, (gssize) content_len);
        }

      if (eol != NULL)
        {
          g_string_append_c (out, '\n');
          p = eol + 1;
        }
      else
        {
          break;
        }
    }

  return g_string_free (out, FALSE);
}

gchar *
mail_quote_attribution (const char *from_name, const char *from_addr)
{
  if (from_addr == NULL || from_addr[0] == '\0')
    return NULL;

  if (from_name == NULL || from_name[0] == '\0')
    return g_strdup_printf ("%s writes:", from_addr);

  return g_strdup_printf ("%s <%s> writes:", from_name, from_addr);
}

gchar *
mail_quote_build_reply_body (const char *original_plain,
                             const char *from_name,
                             const char *from_addr)
{
  g_autofree gchar *attr = mail_quote_attribution (from_name, from_addr);
  g_autofree gchar *quoted = mail_quote_text_plain (original_plain != NULL ? original_plain : "");

  if (attr == NULL)
    return g_steal_pointer (&quoted);

  return g_strdup_printf ("%s\n\n%s", attr, quoted != NULL ? quoted : "");
}

static gboolean
has_re_prefix (const char *subject)
{
  if (subject == NULL)
    return FALSE;
  return (subject[0] == 'R' || subject[0] == 'r') && (subject[1] == 'E' || subject[1] == 'e') && subject[2] == ':';
}

gchar *
mail_quote_subject_reply (const char *original_subject)
{
  if (original_subject == NULL)
    return g_strdup ("Re: ");
  if (has_re_prefix (original_subject))
    return g_strdup (original_subject);
  return g_strdup_printf ("Re: %s", original_subject);
}
