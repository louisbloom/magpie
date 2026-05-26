/* mail-outbox.c - mbox-format outbound debug spool. */

#include "config.h"

#include "mail-outbox.h"

#include "mail-mime.h"

#include <gio/gio.h>
#include <gmime/gmime.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAIL_OUTBOX_ERROR (mail_outbox_error_quark ())

static GQuark
mail_outbox_error_quark (void)
{
  return g_quark_from_static_string ("mail-outbox-error");
}

static char *
outbox_path_for (MailAccount *account)
{
  /* Read $HOME live rather than g_get_home_dir() — the latter caches on
   * first call, which is fine in production but trips per-test
   * temp-dir fixtures. mutt itself honours $HOME. */
  const char *home = g_getenv ("HOME");
  if (home == NULL || home[0] == '\0')
    home = g_get_home_dir ();
  return g_build_filename (home, "Mail", account->identity, "Outbox.mbox", NULL);
}

/* RFC 5322 §3.3 / asctime() format used in mbox "From " envelopes:
 * "Tue May 19 20:34:51 2026". */
static char *
mbox_envelope_date (void)
{
  time_t now = time (NULL);
  struct tm tm;
  if (gmtime_r (&now, &tm) == NULL)
    return g_strdup ("Thu Jan  1 00:00:00 1970");
  static const char *const wday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  return g_strdup_printf ("%s %s %2d %02d:%02d:%02d %d",
                          wday[tm.tm_wday], mon[tm.tm_mon], tm.tm_mday,
                          tm.tm_hour, tm.tm_min, tm.tm_sec, 1900 + tm.tm_year);
}

/* mboxrd ">From " escaping: a body line consisting of zero or more
 * '>' followed by "From " gets one extra '>' prepended. Returns a
 * newly-allocated GString — caller owns. */
static GString *
mboxrd_escape (const char *body)
{
  GString *out = g_string_new (NULL);
  const char *p = body;
  while (*p != '\0')
    {
      /* Check ">>...From " at line start. */
      const char *q = p;
      while (*q == '>')
        q++;
      if (g_str_has_prefix (q, "From "))
        g_string_append_c (out, '>');

      const char *eol = strchr (p, '\n');
      if (eol == NULL)
        {
          g_string_append (out, p);
          break;
        }
      g_string_append_len (out, p, (gssize) (eol - p + 1));
      p = eol + 1;
    }
  return out;
}

static char *
build_references (MailMimeReplySource *src)
{
  /* Concatenate the original References (if any) + the original
   * Message-Id wrapped in <>. */
  GString *out = g_string_new (NULL);
  if (src->references != NULL)
    {
      g_string_append (out, src->references);
      g_string_append_c (out, ' ');
    }
  if (src->message_id != NULL)
    g_string_append_printf (out, "<%s>", src->message_id);
  if (out->len == 0)
    return g_string_free (out, TRUE);
  return g_string_free (out, FALSE);
}

static GMimeMessage *
build_message (MailAccount *account,
               const char *to,
               const char *subject,
               const char *body,
               MailMimeReplySource *in_reply_to)
{
  mail_mime_ensure_init ();

  GMimeMessage *msg = g_mime_message_new (TRUE);

  /* From: <account identity>. We don't have a separate display name. */
  {
    InternetAddressList *list = g_mime_message_get_from (msg);
    InternetAddress *ia = internet_address_mailbox_new (NULL, account->identity);
    internet_address_list_add (list, ia);
    g_object_unref (ia);
  }

  if (to != NULL && to[0] != '\0')
    {
      InternetAddressList *list = g_mime_message_get_to (msg);
      InternetAddress *ia = internet_address_mailbox_new (NULL, to);
      internet_address_list_add (list, ia);
      g_object_unref (ia);
    }

  g_mime_message_set_subject (msg, subject != NULL ? subject : "", "UTF-8");

  /* Date: now, local timezone. */
  {
    GDateTime *now = g_date_time_new_now_local ();
    g_mime_message_set_date (msg, now);
    g_date_time_unref (now);
  }

  /* Message-Id: gmime-generated. */
  {
    char hostname[256];
    if (gethostname (hostname, sizeof hostname) != 0)
      g_strlcpy (hostname, "spool.local", sizeof hostname);
    hostname[sizeof hostname - 1] = '\0';
    g_autofree char *mid = g_mime_utils_generate_message_id (hostname);
    g_mime_message_set_message_id (msg, mid);
  }

  if (in_reply_to != NULL && in_reply_to->message_id != NULL)
    {
      GMimeObject *obj = (GMimeObject *) msg;
      g_autofree char *irt = g_strdup_printf ("<%s>", in_reply_to->message_id);
      g_mime_object_set_header (obj, "In-Reply-To", irt, "UTF-8");
      g_autofree char *refs = build_references (in_reply_to);
      if (refs != NULL && refs[0] != '\0')
        g_mime_object_set_header (obj, "References", refs, "UTF-8");
    }

  /* Body: text/plain, UTF-8. GMime picks the transfer-encoding (8bit
   * for clean UTF-8, quoted-printable for 8-bit-with-long-lines). */
  GMimeTextPart *part = g_mime_text_part_new_with_subtype ("plain");
  g_mime_text_part_set_charset (part, "utf-8");
  g_mime_text_part_set_text (part, body != NULL ? body : "");
  g_mime_message_set_mime_part (msg, (GMimeObject *) part);
  g_object_unref (part);

  return msg;
}

static gboolean
serialize_message (GMimeMessage *msg, GString *out, GError **error)
{
  GMimeStream *mem = g_mime_stream_mem_new ();
  if (mem == NULL)
    {
      g_set_error_literal (error, MAIL_OUTBOX_ERROR, 0, "failed to allocate mime stream");
      return FALSE;
    }
  ssize_t n = g_mime_object_write_to_stream ((GMimeObject *) msg, NULL, mem);
  if (n < 0)
    {
      g_object_unref (mem);
      g_set_error_literal (error, MAIL_OUTBOX_ERROR, 0, "failed to serialize message");
      return FALSE;
    }
  GByteArray *buf = g_mime_stream_mem_get_byte_array (GMIME_STREAM_MEM (mem));
  g_string_append_len (out, (const char *) buf->data, (gssize) buf->len);
  g_object_unref (mem);
  return TRUE;
}

gboolean
mail_outbox_append (MailAccount *account,
                    const char *to,
                    const char *subject,
                    const char *body,
                    MailMimeReplySource *in_reply_to,
                    GError **error)
{
  g_return_val_if_fail (account != NULL, FALSE);
  g_return_val_if_fail (account->identity != NULL, FALSE);

  g_autofree char *path = outbox_path_for (account);
  g_autofree char *parent = g_path_get_dirname (path);
  if (g_mkdir_with_parents (parent, 0700) != 0 && errno != EEXIST)
    {
      g_set_error (error, MAIL_OUTBOX_ERROR, 0, "create %s: %s", parent, g_strerror (errno));
      return FALSE;
    }

  GMimeMessage *msg = build_message (account, to, subject, body, in_reply_to);

  g_autoptr (GString) serialized = g_string_new (NULL);
  if (!serialize_message (msg, serialized, error))
    {
      g_object_unref (msg);
      return FALSE;
    }
  g_object_unref (msg);

  /* mboxrd escape the body of the serialized message. We escape the
   * whole stream — header lines never start with "From ", so this is
   * safe and simpler than splitting at the headers/body boundary. */
  g_autoptr (GString) escaped = mboxrd_escape (serialized->str);

  g_autofree char *envelope_date = mbox_envelope_date ();
  g_autofree char *envelope = g_strdup_printf ("From spool@localhost %s\n", envelope_date);

  int fd = open (path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      g_set_error (error, MAIL_OUTBOX_ERROR, 0, "open %s: %s", path, g_strerror (errno));
      return FALSE;
    }

  if (flock (fd, LOCK_EX) != 0)
    {
      int saved = errno;
      close (fd);
      g_set_error (error, MAIL_OUTBOX_ERROR, 0, "flock %s: %s", path, g_strerror (saved));
      return FALSE;
    }

  gboolean ok = TRUE;
  if (write (fd, envelope, strlen (envelope)) < 0)
    ok = FALSE;
  if (ok && write (fd, escaped->str, escaped->len) < 0)
    ok = FALSE;
  /* Ensure trailing blank line between mbox entries. */
  if (ok && (escaped->len == 0 || escaped->str[escaped->len - 1] != '\n'))
    {
      if (write (fd, "\n", 1) < 0)
        ok = FALSE;
    }
  if (ok && write (fd, "\n", 1) < 0)
    ok = FALSE;

  if (!ok)
    g_set_error (error, MAIL_OUTBOX_ERROR, 0, "write %s: %s", path, g_strerror (errno));

  fsync (fd);
  flock (fd, LOCK_UN);
  close (fd);
  return ok;
}
