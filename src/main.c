/* main.c - entry point for GNOME Mail. */

#include "config.h"

#include "mail-application.h"

#include <adwaita.h>
#include <string.h>

#ifdef DEBUG
/* Regression guard for "GtkGizmo without a current allocation" snapshot
 * warnings. This class of warning fires from a live display's frame
 * clock and is not reproducible under gtk_test_init's offscreen
 * backend, so a unit test can't catch it. Instead we call
 * G_BREAKPOINT() from inside GLib's log routing — gdb sees a stack
 * rooted in the snapshot pass, which is enough to localise the
 * misbehaving widget tree.
 *
 * Other warnings continue to print normally via g_log_writer_default.
 * To use:
 *   gdb ./src/mail
 *   (gdb) run
 *   ... reproduce the click / scroll / resize that triggers it ...
 *   (gdb) bt full
 *
 * Only installed when configured with --enable-debug. */
static GLogWriterOutput
debug_break_on_gizmo_warning (GLogLevelFlags level,
                              const GLogField *fields,
                              gsize n_fields,
                              gpointer user_data)
{
  GLogWriterOutput out = g_log_writer_default (level, fields, n_fields, user_data);

  if (level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL))
    {
      for (gsize i = 0; i < n_fields; i++)
        {
          if (g_strcmp0 (fields[i].key, "MESSAGE") == 0 && fields[i].value != NULL)
            {
              const char *msg = (const char *) fields[i].value;
              if (strstr (msg, "snapshot") != NULL && strstr (msg, "GtkGizmo") != NULL && strstr (msg, "allocation") != NULL)
                {
                  G_BREAKPOINT ();
                }
            }
        }
    }
  return out;
}
#endif

int
main (int argc,
      char *argv[])
{
#ifdef DEBUG
  g_log_set_writer_func (debug_break_on_gizmo_warning, NULL, NULL);
#endif
  g_autoptr (MailApplication) app = mail_application_new ();
  return g_application_run (G_APPLICATION (app), argc, argv);
}
