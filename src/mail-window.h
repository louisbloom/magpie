/* mail-window.h - The main AdwApplicationWindow. */

#pragma once

#include "mail-application.h"

#include <adwaita.h>

G_BEGIN_DECLS

#define MAIL_TYPE_WINDOW (mail_window_get_type ())
G_DECLARE_FINAL_TYPE (MailWindow, mail_window, MAIL, WINDOW, AdwApplicationWindow)

MailWindow *mail_window_new (MailApplication *app);

G_END_DECLS
