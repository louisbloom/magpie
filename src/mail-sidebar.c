/* mail-sidebar.c - Account/folder sidebar driven by GoaClient.
 *
 * Layout: a GtkListBox in .navigation-sidebar style, bound to a
 * GListStore of MailSidebarItem GObjects via gtk_list_box_bind_model.
 * Item rows are AdwActionRow widgets.
 *
 * On first realize we create a GoaClient asynchronously. Once the
 * client is ready, we enumerate accounts, instantiate a MailAccount
 * per supported provider, append an account row, and kick off
 * list_folders_async on the backend; folder rows are appended in the
 * completion callback.
 *
 * Strings stored on MailSidebarItem are g_strdup'd: folder identifiers
 * in particular need to outlive the backend arena's next reset (e.g.,
 * when the user clicks a folder and we run list_messages). This is a
 * one-time-per-row malloc cost on the cold sidebar path.
 */

#include "config.h"

#include "mail-sidebar.h"

#include <adwaita.h>
#include <goa/goa.h>

typedef enum
{
  MAIL_SIDEBAR_ITEM_ACCOUNT,
  MAIL_SIDEBAR_ITEM_FOLDER,
} MailSidebarItemKind;

#define MAIL_TYPE_SIDEBAR_ITEM (mail_sidebar_item_get_type ())
G_DECLARE_FINAL_TYPE (MailSidebarItem, mail_sidebar_item, MAIL, SIDEBAR_ITEM, GObject)

struct _MailSidebarItem
{
  GObject parent_instance;

  MailSidebarItemKind kind;
  MailAccount *account; /* borrowed */
  char *title;          /* g_strdup */
  char *subtitle;       /* g_strdup; may be NULL */
  char *folder_id;      /* g_strdup; NULL for accounts */
  int unread;
};

G_DEFINE_FINAL_TYPE (MailSidebarItem, mail_sidebar_item, G_TYPE_OBJECT)

static void
mail_sidebar_item_finalize (GObject *object)
{
  MailSidebarItem *self = MAIL_SIDEBAR_ITEM (object);
  g_free (self->title);
  g_free (self->subtitle);
  g_free (self->folder_id);
  G_OBJECT_CLASS (mail_sidebar_item_parent_class)->finalize (object);
}

static void
mail_sidebar_item_class_init (MailSidebarItemClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = mail_sidebar_item_finalize;
}

static void
mail_sidebar_item_init (MailSidebarItem *self)
{
}

static MailSidebarItem *
mail_sidebar_item_new_account (MailAccount *acct)
{
  MailSidebarItem *self = g_object_new (MAIL_TYPE_SIDEBAR_ITEM, NULL);
  self->kind = MAIL_SIDEBAR_ITEM_ACCOUNT;
  self->account = acct;
  self->title = g_strdup (acct->identity != NULL ? acct->identity : "(unknown)");
  self->subtitle = g_strdup (acct->provider_name != NULL ? acct->provider_name : "");
  return self;
}

static MailSidebarItem *
mail_sidebar_item_new_folder (MailAccount *acct,
                              const char *folder_id,
                              const char *display_name,
                              int unread,
                              int total)
{
  MailSidebarItem *self = g_object_new (MAIL_TYPE_SIDEBAR_ITEM, NULL);
  self->kind = MAIL_SIDEBAR_ITEM_FOLDER;
  self->account = acct;
  self->title = g_strdup (display_name != NULL ? display_name : "(folder)");
  self->folder_id = g_strdup (folder_id);
  self->unread = unread;
  if (total > 0)
    self->subtitle = g_strdup_printf ("%d", total);
  return self;
}

/* --------------------------------------------------------------- */

struct _MailSidebar
{
  GtkWidget parent_instance;

  GtkListBox *list_box;
  GListStore *store;     /* of MailSidebarItem */
  GoaClient *goa_client; /* may be NULL while loading */
  GPtrArray *accounts;   /* of MailAccount* (owned) */
  GCancellable *cancellable;
  gboolean goa_started; /* GoaClient creation kicked off on first realize */
};

enum
{
  SIGNAL_FOLDER_SELECTED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (MailSidebar, mail_sidebar, GTK_TYPE_WIDGET)

static GtkWidget *
build_row_widget (gpointer item,
                  gpointer user_data)
{
  MailSidebarItem *it = MAIL_SIDEBAR_ITEM (item);
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());

  /* See mail-message-list.c: account identities and folder names are
   * literal text, never Pango markup. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), it->title);
  if (it->subtitle != NULL && it->subtitle[0] != '\0')
    adw_action_row_set_subtitle (row, it->subtitle);

  if (it->kind == MAIL_SIDEBAR_ITEM_ACCOUNT)
    {
      gtk_widget_add_css_class (GTK_WIDGET (row), "heading");
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
      gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);

      GtkWidget *image = NULL;
      if (it->account != NULL && it->account->provider_icon != NULL)
        {
          g_autoptr (GIcon) gicon = g_icon_new_for_string (it->account->provider_icon, NULL);
          if (gicon != NULL)
            image = gtk_image_new_from_gicon (gicon);
        }
      if (image == NULL)
        image = gtk_image_new_from_icon_name ("mail-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
      adw_action_row_add_prefix (row, image);
    }
  else
    {
      /* AdwActionRow defaults to activatable=FALSE (no activatable-widget
       * is set), which suppresses GtkListBox::row-activated. Folder rows
       * must opt in explicitly so single-click navigation works. */
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
      gtk_widget_set_margin_start (GTK_WIDGET (row), 12);
      GtkWidget *image = gtk_image_new_from_icon_name ("folder-symbolic");
      adw_action_row_add_prefix (row, image);

      if (it->unread > 0)
        {
          char buf[16];
          g_snprintf (buf, sizeof buf, "%d", it->unread);
          GtkWidget *badge = gtk_label_new (buf);
          gtk_widget_add_css_class (badge, "numeric");
          gtk_widget_add_css_class (badge, "caption");
          adw_action_row_add_suffix (row, badge);
        }
    }

  return GTK_WIDGET (row);
}

static void
on_row_activated (GtkListBox *list_box,
                  GtkListBoxRow *row,
                  gpointer user_data)
{
  MailSidebar *self = MAIL_SIDEBAR (user_data);
  guint index = gtk_list_box_row_get_index (row);
  g_autoptr (GObject) item = g_list_model_get_item (G_LIST_MODEL (self->store), index);
  if (item == NULL)
    return;
  MailSidebarItem *it = MAIL_SIDEBAR_ITEM (item);
  if (it->kind == MAIL_SIDEBAR_ITEM_FOLDER && it->folder_id != NULL)
    g_signal_emit (self, signals[SIGNAL_FOLDER_SELECTED], 0,
                   it->account->backend, it->folder_id);
}

static guint
find_account_index (MailSidebar *self,
                    MailAccount *acct)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  for (guint i = 0; i < n; i++)
    {
      g_autoptr (MailSidebarItem) it = g_list_model_get_item (G_LIST_MODEL (self->store), i);
      if (it->kind == MAIL_SIDEBAR_ITEM_ACCOUNT && it->account == acct)
        return i;
    }
  return G_MAXUINT;
}

typedef struct
{
  MailSidebar *self; /* ref'd */
  MailAccount *acct; /* borrowed */
} LoadFoldersCtx;

static void
on_list_folders_done (GObject *source,
                      GAsyncResult *result,
                      gpointer user_data)
{
  LoadFoldersCtx *ctx = user_data;
  MailSidebar *self = ctx->self;
  MailAccount *acct = ctx->acct;
  g_autoptr (GError) error = NULL;

  GPtrArray *folders = mail_backend_list_folders_finish (acct->backend, result, &error);
  if (folders == NULL)
    {
      if (error == NULL || !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("list_folders failed: %s", error != NULL ? error->message : "(no error)");
      g_object_unref (self);
      g_free (ctx);
      return;
    }

  guint anchor = find_account_index (self, acct);
  if (anchor == G_MAXUINT)
    {
      g_object_unref (self);
      g_free (ctx);
      return;
    }
  g_debug ("loaded %u folders for %s", folders->len, acct->identity);
  guint insert_at = anchor + 1;
  for (guint i = 0; i < folders->len; i++)
    {
      const MailFolder *f = g_ptr_array_index (folders, i);
      MailSidebarItem *it = mail_sidebar_item_new_folder (acct, f->id, f->display_name, f->unread_count, f->total_count);
      g_list_store_insert (self->store, insert_at++, it);
      g_object_unref (it);
    }

  g_object_unref (self);
  g_free (ctx);
}

static void
sidebar_add_account (MailSidebar *self,
                     MailAccount *acct)
{
  g_ptr_array_add (self->accounts, acct);

  MailSidebarItem *row = mail_sidebar_item_new_account (acct);
  g_list_store_append (self->store, row);
  g_object_unref (row);

  if (acct->backend != NULL)
    {
      LoadFoldersCtx *ctx = g_new (LoadFoldersCtx, 1);
      ctx->self = g_object_ref (self);
      ctx->acct = acct;
      mail_backend_list_folders_async (acct->backend, self->cancellable,
                                       on_list_folders_done, ctx);
    }
}

static void
on_goa_client_ready (GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
  MailSidebar *self = MAIL_SIDEBAR (user_data);
  g_autoptr (GError) error = NULL;

  GoaClient *client = goa_client_new_finish (result, &error);
  if (client == NULL)
    {
      g_warning ("GoaClient creation failed: %s", error != NULL ? error->message : "(no error)");
      g_object_unref (self);
      return;
    }
  self->goa_client = client;

  GList *objects = goa_client_get_accounts (client);
  for (GList *l = objects; l != NULL; l = l->next)
    {
      MailAccount *acct = mail_account_new_from_goa (GOA_OBJECT (l->data));
      if (acct != NULL)
        sidebar_add_account (self, acct);
    }
  g_list_free_full (objects, g_object_unref);

  if (g_list_model_get_n_items (G_LIST_MODEL (self->store)) == 0)
    {
      /* No usable accounts found. The placeholder remains. */
    }

  g_object_unref (self);
}

static void
mail_sidebar_dispose (GObject *object)
{
  MailSidebar *self = MAIL_SIDEBAR (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  if (self->accounts != NULL)
    {
      for (guint i = 0; i < self->accounts->len; i++)
        mail_account_free (g_ptr_array_index (self->accounts, i));
      g_ptr_array_set_size (self->accounts, 0);
    }

  g_clear_object (&self->goa_client);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->store);

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))) != NULL)
    gtk_widget_unparent (child);

  G_OBJECT_CLASS (mail_sidebar_parent_class)->dispose (object);
}

static void
mail_sidebar_finalize (GObject *object)
{
  MailSidebar *self = MAIL_SIDEBAR (object);
  if (self->accounts != NULL)
    g_ptr_array_unref (self->accounts);
  G_OBJECT_CLASS (mail_sidebar_parent_class)->finalize (object);
}

static void
mail_sidebar_class_init (MailSidebarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = mail_sidebar_dispose;
  object_class->finalize = mail_sidebar_finalize;

  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);

  signals[SIGNAL_FOLDER_SELECTED] = g_signal_new ("folder-selected",
                                                  G_TYPE_FROM_CLASS (klass),
                                                  G_SIGNAL_RUN_LAST,
                                                  0,
                                                  NULL, NULL, NULL,
                                                  G_TYPE_NONE,
                                                  2,
                                                  G_TYPE_POINTER,
                                                  G_TYPE_STRING);
}

static void
on_sidebar_realize (GtkWidget *widget,
                    gpointer user_data)
{
  MailSidebar *self = MAIL_SIDEBAR (widget);
  if (self->goa_started)
    return;
  self->goa_started = TRUE;
  goa_client_new (self->cancellable, on_goa_client_ready, g_object_ref (self));
}

static void
mail_sidebar_init (MailSidebar *self)
{
  self->store = g_list_store_new (MAIL_TYPE_SIDEBAR_ITEM);
  self->accounts = g_ptr_array_new ();
  self->cancellable = g_cancellable_new ();

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_parent (scroller, GTK_WIDGET (self));

  self->list_box = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_widget_add_css_class (GTK_WIDGET (self->list_box), "navigation-sidebar");
  gtk_list_box_bind_model (self->list_box, G_LIST_MODEL (self->store),
                           build_row_widget, self, NULL);
  g_signal_connect (self->list_box, "row-activated",
                    G_CALLBACK (on_row_activated), self);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), GTK_WIDGET (self->list_box));

  /* Defer GoaClient creation until the widget is realized (i.e., shown
   * in a window). This keeps tests that never realize the widget from
   * picking up the real user's GOA accounts. */
  g_signal_connect (self, "realize", G_CALLBACK (on_sidebar_realize), NULL);
}

GtkWidget *
mail_sidebar_new (void)
{
  return g_object_new (MAIL_TYPE_SIDEBAR, NULL);
}

/* ------------------------------------------------------------------
 * Test-only hooks. Wired into tests/test-sidebar.c via the same
 * symbols declared in mail-sidebar.h. They reuse the production
 * sidebar_add_account path, so coverage matches the runtime control
 * flow exactly.
 * ------------------------------------------------------------------ */

void
mail_sidebar_add_test_account (MailSidebar *self,
                               MailAccount *acct)
{
  g_return_if_fail (MAIL_IS_SIDEBAR (self));
  g_return_if_fail (acct != NULL);
  sidebar_add_account (self, acct);
}

GtkListBox *
_mail_sidebar_get_list_box_for_test (MailSidebar *self)
{
  g_return_val_if_fail (MAIL_IS_SIDEBAR (self), NULL);
  return self->list_box;
}
