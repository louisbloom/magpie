/* main.c - entry point for GNOME Mail. */

#include "config.h"

#include "mail-application.h"

#include <adwaita.h>

int
main (int argc,
      char *argv[])
{
  g_autoptr (MailApplication) app = mail_application_new ();
  return g_application_run (G_APPLICATION (app), argc, argv);
}
