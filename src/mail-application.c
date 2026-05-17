/* mail-application.c - AdwApplication subclass for GNOME Mail. */

#include "config.h"

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
mail_application_class_init (MailApplicationClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);
  app_class->activate = mail_application_activate;
}

static void
mail_application_init (MailApplication *self)
{
}

MailApplication *
mail_application_new (void)
{
  return g_object_new (MAIL_TYPE_APPLICATION,
                       "application-id", GNOME_MAIL_APP_ID,
                       "resource-base-path", "/org/gnome/Mail",
                       "flags", G_APPLICATION_DEFAULT_FLAGS,
                       NULL);
}
