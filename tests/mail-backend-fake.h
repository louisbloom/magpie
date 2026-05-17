/* tests/mail-backend-fake.h - In-memory MailBackend for tests.
 *
 * Seed folders, messages, and raw RFC822 bodies before exercising any
 * code that uses the backend. Async completion is dispatched via
 * g_idle_add, so tests pump the main context the same way the real
 * app does (`while (g_main_context_iteration (NULL, FALSE)) {}`).
 *
 * Lifetime: pointers returned by the *_finish trampolines live in the
 * backend's MailArena and are valid only until the next *_async call
 * on the same backend — matching the production contract.
 */

#pragma once

#include "../src/mail-backend.h"

G_BEGIN_DECLS

typedef struct
{
  const char *id;
  const char *display_name;
  const char *parent_id; /* may be NULL */
  int unread;
  int total;
} FakeFolderSpec;

typedef struct
{
  const char *id;
  const char *subject;
  const char *from;
  gint64 received_unix;
  gboolean unread;
  const char *raw_rfc822;  /* body returned by fetch_message_raw */
  const char *content_key; /* may be NULL; surfaces on MailMessageMeta.content_key */
} FakeMessageSpec;

/* Construct an empty fake backend. */
MailBackend *mail_backend_fake_new (void);

/* Replace the seeded folder list. The fake deep-copies @folders. */
void mail_backend_fake_set_folders (MailBackend *backend,
                                    const FakeFolderSpec *folders,
                                    gsize n);

/* Replace the seeded message list for @folder_id. Deep-copied. */
void mail_backend_fake_set_messages (MailBackend *backend,
                                     const char *folder_id,
                                     const FakeMessageSpec *msgs,
                                     gsize n);

/* --- Observation ------------------------------------------------ */

guint mail_backend_fake_list_folders_calls (MailBackend *backend);
guint mail_backend_fake_list_messages_calls (MailBackend *backend);
guint mail_backend_fake_fetch_raw_calls (MailBackend *backend);

/* Pointers borrowed; valid until the backend is destroyed. */
const char *mail_backend_fake_last_folder_id (MailBackend *backend);
const char *mail_backend_fake_last_message_id (MailBackend *backend);

G_END_DECLS
