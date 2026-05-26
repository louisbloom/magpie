/* mail-application.c - AdwApplication subclass for Spool. */

#include "config.h"

#include "mail-about.h"
#include "mail-application.h"
#include "mail-window.h"

struct _MailApplication
{
  AdwApplication parent_instance;
};

G_DEFINE_FINAL_TYPE (MailApplication, mail_application, ADW_TYPE_APPLICATION)

static void
mail_application_activate (GApplication *app)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (window == NULL)
    window = GTK_WINDOW (mail_window_new (MAIL_APPLICATION (app)));
  gtk_window_present (window);
}

static void
on_about_activate (GSimpleAction *action,
                   GVariant *parameter,
                   gpointer user_data)
{
  GtkApplication *app = GTK_APPLICATION (user_data);
  mail_about_present (gtk_application_get_active_window (app));
}

static const GActionEntry app_entries[] = {
  { .name = "about", .activate = on_about_activate },
};

static void
mail_application_class_init (MailApplicationClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);
  app_class->activate = mail_application_activate;
}

static void
mail_application_init (MailApplication *self)
{
  g_action_map_add_action_entries (G_ACTION_MAP (self), app_entries,
                                   G_N_ELEMENTS (app_entries), self);

  const char *about_accels[] = { "<Ctrl><Shift>a", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (self), "app.about",
                                         about_accels);
}

MailApplication *
mail_application_new (void)
{
  return g_object_new (MAIL_TYPE_APPLICATION,
                       "application-id", SPOOL_APP_ID,
                       "resource-base-path", "/org/gnome/Spool",
                       "flags", G_APPLICATION_DEFAULT_FLAGS,
                       NULL);
}
