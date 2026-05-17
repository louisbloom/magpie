/* mail-backend-store.h - MailBackend that reads from a local MailStore.
 *
 * This is what the UI is wired to: every sidebar/folder/message read
 * goes through here, never out to the network. Sync between this
 * store and the provider happens elsewhere (see mail-sync.h).
 *
 * The backend borrows its MailStore — it does not own it. Lifetime
 * follows MailAccount, which owns both.
 */

#pragma once

#include "mail-backend.h"
#include "mail-store.h"

G_BEGIN_DECLS

MailBackend *mail_backend_store_new (MailStore *store);

G_END_DECLS
