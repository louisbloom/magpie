/* mail-account.h - Wraps a GOA account with its chosen MailBackend.
 *
 * Owns: a ref on the GoaObject, and the MailBackend it instantiated for
 * that account. All string accessors return borrowed pointers that
 * live for the account's lifetime.
 */

#pragma once

#include "mail-backend.h"

#include <gio/gio.h>

/* libgoa is "unstable" but stable enough; acknowledge in-header so any
 * consumer (including test binaries that skip config.h) compiles. */
#ifndef GOA_API_IS_SUBJECT_TO_CHANGE
#define GOA_API_IS_SUBJECT_TO_CHANGE
#endif
#include <goa/goa.h>

G_BEGIN_DECLS

typedef struct
{
  GoaObject *goa_object; /* ref'd; NULL for test accounts */
  MailBackend *backend;  /* owned; may be NULL if we don't support the provider */
  GoaAccount *account;   /* borrowed from goa_object; NULL for test accounts */
  GoaMail *mail;         /* borrowed from goa_object; NULL for test accounts */
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
 * mail-disabled, or no suitable backend can be instantiated. */
MailAccount *mail_account_new_from_goa (GoaObject *goa_object);

/* Test-only constructor. Takes ownership of @backend (mail_account_free
 * destroys it). @identity and @provider_name are duplicated. */
MailAccount *mail_account_new_for_test (MailBackend *backend,
                                        const char *identity,
                                        const char *provider_name);

void mail_account_free (MailAccount *acct);

G_END_DECLS
