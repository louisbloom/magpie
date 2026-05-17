/* mail-account.c - Pair a GOA account with its local store and sync. */

#include "config.h"

#include "mail-account.h"
#include "mail-backend-imap.h"
#include "mail-backend-msgraph.h"
#include "mail-backend-store.h"

static gboolean
provider_is_msgraph (const char *provider_type)
{
  return provider_type != NULL && (g_str_equal (provider_type, "ms_graph") || g_str_equal (provider_type, "windows_live") || g_str_equal (provider_type, "exchange"));
}

/* The local store lives at ~/Mail/<identity>/ (mutt convention). The
 * identity is used verbatim — it's the email address GOA gave us. */
static char *
account_store_root (const char *identity)
{
  const char *home = g_get_home_dir ();
  return g_build_filename (home, "Mail", identity, NULL);
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

  const char *identity = goa_account_get_identity (account);
  const char *provider_type = goa_account_get_provider_type (account);

  /* Open (creating if needed) the per-account local store. */
  g_autofree char *root = account_store_root (identity);
  g_autoptr (GError) error = NULL;
  MailStore *store = mail_store_open (root, identity, &error);
  if (store == NULL)
    {
      g_warning ("mail_account: cannot open store at %s: %s",
                 root, error != NULL ? error->message : "(no error)");
      return NULL;
    }

  /* Instantiate the remote backend if we know how to talk to this
   * provider. NULL is fine — the account just won't sync. */
  MailBackend *remote = NULL;
  if (provider_is_msgraph (provider_type))
    remote = mail_backend_msgraph_new (goa_object);
  else if (goa_mail_get_imap_supported (mail))
    remote = mail_backend_imap_new (goa_object);

  MailAccount *self = g_new0 (MailAccount, 1);
  self->goa_object = g_object_ref (goa_object);
  self->account = account;
  self->mail = mail;
  self->store = store;
  self->store_backend = mail_backend_store_new (store);
  self->remote_backend = remote;
  self->sync = remote != NULL ? mail_sync_new () : NULL;
  self->identity = identity;
  self->provider_type = provider_type;
  self->provider_name = goa_account_get_provider_name (account);
  self->provider_icon = goa_account_get_provider_icon (account);
  return self;
}

MailAccount *
mail_account_new_for_test (MailBackend *store_backend,
                           const char *identity,
                           const char *provider_name)
{
  MailAccount *self = g_new0 (MailAccount, 1);
  self->store_backend = store_backend;
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
  g_clear_object (&acct->sync);
  mail_backend_destroy (acct->store_backend);
  mail_backend_destroy (acct->remote_backend);
  if (acct->store != NULL)
    mail_store_close (acct->store);
  if (acct->goa_object != NULL)
    g_object_unref (acct->goa_object);
  g_free (acct->test_identity);
  g_free (acct->test_provider_type);
  g_free (acct->test_provider_name);
  g_free (acct);
}
