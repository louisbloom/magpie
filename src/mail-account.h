/* mail-account.h - Pair a GOA account with its local store + sync engine.
 *
 * Each account owns three pieces of data plumbing:
 *   - store          : on-disk Maildir + sqlite index at ~/Mail/<identity>/
 *   - store_backend  : MailBackend the UI reads from (always non-NULL for
 *                      real accounts, and for tests that plug in a fake)
 *   - remote_backend : MailBackend that talks to the provider; consumed
 *                      by sync only. NULL if the provider isn't supported
 *                      (or for test accounts that don't sync).
 *   - sync           : MailSync that reconciles remote -> store on user
 *                      refresh. NULL when remote_backend is NULL.
 *
 * UI code MUST go through store_backend. Sync code uses remote_backend.
 * String accessors return borrowed pointers that live for the account's
 * lifetime.
 */

#pragma once

#include "mail-backend.h"
#include "mail-store.h"
#include "mail-sync.h"

#include <gio/gio.h>

#ifndef GOA_API_IS_SUBJECT_TO_CHANGE
#define GOA_API_IS_SUBJECT_TO_CHANGE
#endif
#include <goa/goa.h>

G_BEGIN_DECLS

typedef struct
{
  GoaObject *goa_object; /* ref'd; NULL for test accounts */
  GoaAccount *account;   /* borrowed from goa_object; NULL for test accounts */
  GoaMail *mail;         /* borrowed from goa_object; NULL for test accounts */

  MailStore *store;            /* owned; NULL for test accounts */
  MailBackend *store_backend;  /* owned; UI reads through this */
  MailBackend *remote_backend; /* owned; NULL if not supported / test */
  MailSync *sync;              /* owned; NULL when no remote_backend */

  /* Cached for hot access. For GOA accounts these point into the parent
   * GoaAccount/GoaMail. For test accounts they point into the owned-
   * strings block below. */
  const char *identity;
  const char *provider_type;
  const char *provider_name;
  const char *provider_icon; /* themed icon spec; may be NULL */

  /* Owned-by-MailAccount string backing for test accounts; NULL on
   * GOA-backed accounts. mail_account_free g_free's these. */
  char *test_identity;
  char *test_provider_type;
  char *test_provider_name;
} MailAccount;

/* Returns NULL if the GOA object does not expose a Mail interface, is
 * mail-disabled, or the per-account local store cannot be created. */
MailAccount *mail_account_new_from_goa (GoaObject *goa_object);

/* Test-only constructor. @store_backend stands in for the UI-facing
 * backend; mail_account_free destroys it. No MailStore or MailSync is
 * created. @identity and @provider_name are duplicated. */
MailAccount *mail_account_new_for_test (MailBackend *store_backend,
                                        const char *identity,
                                        const char *provider_name);

void mail_account_free (MailAccount *acct);

G_END_DECLS
