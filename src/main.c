/* main.c - entry point for GNOME Mail. */

#include "config.h"

#include "mail-application.h"

#include <adwaita.h>
#include <string.h>

#ifdef DEBUG
#include <gtk/gtk.h>

/* Regression guard for "GtkGizmo without a current allocation" snapshot
 * warnings. The class fires from the live display's frame clock and
 * isn't reproducible under the offscreen backend, so a unit test
 * can't catch it. Strategy:
 *   1. Parse the widget pointer out of the warning text.
 *   2. Walk up the parent chain and dump (type, css name) of each.
 *      That immediately tells us which subtree owns the bad child.
 *   3. Call G_BREAKPOINT() so a gdb-attached run can drill further.
 *
 * Only installed when configured with --enable-debug. */
static void
dump_widget_chain (gpointer addr)
{
  GtkWidget *w = addr;
  if (!GTK_IS_WIDGET (w))
    {
      g_printerr ("  (not a GtkWidget at %p)\n", addr);
      return;
    }
  int depth = 0;
  for (GtkWidget *p = w; p != NULL; p = gtk_widget_get_parent (p), depth++)
    {
      int aw = gtk_widget_get_width (p);
      int ah = gtk_widget_get_height (p);
      g_printerr ("  [%2d] %p  type=%-32s  css=%-20s  alloc=%dx%d  visible=%d  mapped=%d\n",
                  depth, p,
                  G_OBJECT_TYPE_NAME (p),
                  gtk_widget_get_css_name (p),
                  aw, ah,
                  gtk_widget_get_visible (p),
                  gtk_widget_get_mapped (p));
    }
}

/* Recursively dump natural-height requests so we can spot which child
 * is asking for the excess. for_width passed as the actual allocated
 * width to mimic the real layout pass. */
static void
dump_natural_heights (GtkWidget *w,
                      int for_width,
                      int depth)
{
  if (w == NULL)
    return;
  int min_h = 0, nat_h = 0;
  gtk_widget_measure (w, GTK_ORIENTATION_VERTICAL, for_width,
                      &min_h, &nat_h, NULL, NULL);
  g_printerr ("  %*s%s %p  nat_h=%d min_h=%d  visible=%d  mapped=%d\n",
              depth * 2, "",
              G_OBJECT_TYPE_NAME (w), w,
              nat_h, min_h,
              gtk_widget_get_visible (w),
              gtk_widget_get_mapped (w));
  /* Only descend if this widget actually contributes a large natural. */
  if (nat_h < 700)
    return;
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
    dump_natural_heights (c, for_width, depth + 1);
}

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
          if (g_strcmp0 (fields[i].key, "MESSAGE") != 0 || fields[i].value == NULL)
            continue;
          const char *msg = (const char *) fields[i].value;
          gboolean is_gizmo = strstr (msg, "snapshot") != NULL && strstr (msg, "GtkGizmo") != NULL && strstr (msg, "allocation") != NULL;
          gboolean is_overlay_height = strstr (msg, "AdwOverlaySplitView") != NULL && strstr (msg, "exceeds") != NULL;
          if (!is_gizmo && !is_overlay_height)
            continue;

          /* Parse out the widget pointer ("...Type 0x...."). */
          const char *p = strstr (msg, "0x");
          gpointer addr = NULL;
          if (p != NULL)
            sscanf (p, "%p", &addr);

          if (is_gizmo && addr != NULL)
            {
              g_printerr ("--- offending widget chain (root last) ---\n");
              dump_widget_chain (addr);
              g_printerr ("------------------------------------------\n");
              G_BREAKPOINT ();
            }
          else if (is_overlay_height && addr != NULL && GTK_IS_WIDGET (addr))
            {
              g_printerr ("--- height-request culprits (nat_h >= 700) ---\n");
              dump_natural_heights (GTK_WIDGET (addr),
                                    gtk_widget_get_width (GTK_WIDGET (addr)),
                                    0);
              g_printerr ("-----------------------------------------------\n");
              /* Don't trap on the height warning — let the run continue
               * until the snapshot warning hits and we can capture both. */
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
