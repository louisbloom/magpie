/* mail-application.h - AdwApplication subclass for GNOME Mail. */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define MAIL_TYPE_APPLICATION (mail_application_get_type ())
G_DECLARE_FINAL_TYPE (MailApplication, mail_application, MAIL, APPLICATION, AdwApplication)

MailApplication *mail_application_new (void);

G_END_DECLS
