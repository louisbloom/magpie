/* mail-imap-id.c - composite-id codec, no libetpan dep. */

#include "config.h"

#include "mail-imap-id.h"

#include <string.h>

#define SOH '\x01'

gchar *
mail_imap_id_encode (guint32 uidvalidity,
                     guint32 uid,
                     const char *folder_name)
{
  if (folder_name == NULL)
    folder_name = "";
  return g_strdup_printf ("%u%c%u%c%s",
                          uidvalidity, SOH, uid, SOH, folder_name);
}

static gboolean
parse_u32 (const char *start, const char *end, guint32 *out)
{
  if (start == end)
    return FALSE;
  guint64 v = 0;
  for (const char *p = start; p < end; p++)
    {
      if (*p < '0' || *p > '9')
        return FALSE;
      v = v * 10 + (guint64) (*p - '0');
      if (v > G_MAXUINT32)
        return FALSE;
    }
  *out = (guint32) v;
  return TRUE;
}

gboolean
mail_imap_id_decode (const char *id,
                     guint32 *uidvalidity_out,
                     guint32 *uid_out,
                     const char **folder_name_out)
{
  if (id == NULL)
    return FALSE;

  const char *first = strchr (id, SOH);
  if (first == NULL || first == id)
    return FALSE;
  const char *second = strchr (first + 1, SOH);
  if (second == NULL || second == first + 1)
    return FALSE;
  if (second[1] == '\0')
    return FALSE; /* empty folder name */

  guint32 vu = 0, uid = 0;
  if (!parse_u32 (id, first, &vu))
    return FALSE;
  if (!parse_u32 (first + 1, second, &uid))
    return FALSE;

  *uidvalidity_out = vu;
  *uid_out = uid;
  *folder_name_out = second + 1;
  return TRUE;
}
