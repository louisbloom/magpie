/* mail-backend-msgraph.h - Microsoft Graph backend (libsoup + json-glib). */

#pragma once

#include "mail-backend.h"

G_BEGIN_DECLS

typedef struct _GoaObject GoaObject;

MailBackend *mail_backend_msgraph_new (GoaObject *goa_object);

G_END_DECLS
