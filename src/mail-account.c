/* mail-account.c - Pair a GOA account with its concrete MailBackend. */

#include "config.h"

#include "mail-account.h"
#include "mail-backend-imap.h"
#include "mail-backend-msgraph.h"

#include <string.h>

static gboolean
provider_is_msgraph (const char *provider_type)
{
  return provider_type != NULL && (g_str_equal (provider_type, "ms_graph") || g_str_equal (provider_type, "windows_live") || g_str_equal (provider_type, "exchange"));
}

MailAccount *
mail_account_new_from_goa (GoaObject *goa_object)
{
  g_return_val_if_fail (GOA_IS_OBJECT (goa_object), NULL);

  GoaAccount *account = goa_object_peek_account (goa_object);
  if (account == NULL)
    return NULL;
  if (goa_account_get_mail_disabled (account))
    return NULL;

  GoaMail *mail = goa_object_peek_mail (goa_object);
  if (mail == NULL)
    return NULL;

  const char *provider_type = goa_account_get_provider_type (account);
  MailBackend *backend = NULL;
  if (provider_is_msgraph (provider_type))
    backend = mail_backend_msgraph_new (goa_object);
  else if (goa_mail_get_imap_supported (mail))
    backend = mail_backend_imap_new (goa_object);

  MailAccount *self = g_new0 (MailAccount, 1);
  self->goa_object = g_object_ref (goa_object);
  self->account = account;
  self->mail = mail;
  self->backend = backend;
  self->identity = goa_account_get_identity (account);
  self->provider_type = provider_type;
  self->provider_name = goa_account_get_provider_name (account);
  self->provider_icon = goa_account_get_provider_icon (account);
  return self;
}

MailAccount *
mail_account_new_for_test (MailBackend *backend,
                           const char *identity,
                           const char *provider_name)
{
  MailAccount *self = g_new0 (MailAccount, 1);
  self->backend = backend;
  self->test_identity = g_strdup (identity != NULL ? identity : "test@example.com");
  self->test_provider_type = g_strdup ("test");
  self->test_provider_name = g_strdup (provider_name != NULL ? provider_name : "Test");
  self->identity = self->test_identity;
  self->provider_type = self->test_provider_type;
  self->provider_name = self->test_provider_name;
  return self;
}

void
mail_account_free (MailAccount *acct)
{
  if (acct == NULL)
    return;
  mail_backend_destroy (acct->backend);
  if (acct->goa_object != NULL)
    g_object_unref (acct->goa_object);
  g_free (acct->test_identity);
  g_free (acct->test_provider_type);
  g_free (acct->test_provider_name);
  g_free (acct);
}
