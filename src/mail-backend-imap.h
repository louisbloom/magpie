/* mail-backend-imap.h - IMAP backend (libetpan). */

#pragma once

#include "mail-backend.h"

G_BEGIN_DECLS

typedef struct _GoaObject GoaObject;

MailBackend *mail_backend_imap_new (GoaObject *goa_object);

G_END_DECLS
