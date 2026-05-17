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

#include "mail-refresh-button.h"

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
  SIGNAL_ACCOUNT_ADDED,
  SIGNAL_REFRESH_REQUESTED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (MailSidebar, mail_sidebar, GTK_TYPE_WIDGET)

static void
mail_sidebar_emit_refresh_for_button (MailSidebar *self,
                                      GtkWidget *button)
{
  MailAccount *acct = g_object_get_data (G_OBJECT (button), "mail-sidebar-account");
  if (acct == NULL)
    return;
  g_signal_emit (self, signals[SIGNAL_REFRESH_REQUESTED], 0, acct);
}

/* Account rows: hand-rolled GtkListBoxRow with the same horizontal
 * margins (start=12, end=6) as the folder rows below, so the provider
 * icon lines up with the folder icons and the refresh button lines up
 * with the unread-count badges. AdwActionRow added its own internal
 * prefix/suffix padding which pushed the icons out of alignment and
 * stole the space the email needed to render in full. */
static GtkWidget *
build_account_row (MailSidebarItem *it,
                   MailSidebar *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 6);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 6);

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
  gtk_widget_set_valign (image, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), image);

  /* Title + subtitle vbox. No ellipsize and no single-line cap — the
   * user wants the full identity visible; if the sidebar is dragged
   * narrow enough to overflow, the title wraps rather than truncates. */
  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (vbox, TRUE);
  gtk_widget_set_valign (vbox, GTK_ALIGN_CENTER);

  GtkWidget *title = gtk_label_new (it->title);
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_label_set_use_markup (GTK_LABEL (title), FALSE);
  gtk_label_set_wrap (GTK_LABEL (title), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (title), PANGO_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (title, "heading");
  gtk_box_append (GTK_BOX (vbox), title);

  if (it->subtitle != NULL && it->subtitle[0] != '\0')
    {
      GtkWidget *subtitle = gtk_label_new (it->subtitle);
      gtk_label_set_xalign (GTK_LABEL (subtitle), 0.0);
      gtk_label_set_use_markup (GTK_LABEL (subtitle), FALSE);
      gtk_widget_add_css_class (subtitle, "caption");
      gtk_widget_add_css_class (subtitle, "dim-label");
      gtk_box_append (GTK_BOX (vbox), subtitle);
    }
  gtk_box_append (GTK_BOX (box), vbox);

  GtkWidget *refresh = mail_refresh_button_new (it->account != NULL ? it->account->sync : NULL);
  g_object_set_data (G_OBJECT (refresh), "mail-sidebar-account", it->account);
  g_signal_connect_swapped (refresh, "clicked",
                            G_CALLBACK (mail_sidebar_emit_refresh_for_button), self);
  gtk_widget_set_valign (refresh, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), refresh);

  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  return row;
}

/* Folder rows: dense single-line GtkListBoxRow (~28-32 px tall) —
 * folder icon + name + optional unread badge. The total-message count
 * lives in the row tooltip rather than a subtitle. */
static GtkWidget *
build_folder_row (MailSidebarItem *it)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top (box, 4);
  gtk_widget_set_margin_bottom (box, 4);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 6);

  GtkWidget *image = gtk_image_new_from_icon_name ("folder-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (image), 16);
  gtk_box_append (GTK_BOX (box), image);

  GtkWidget *label = gtk_label_new (it->title);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_use_markup (GTK_LABEL (label), FALSE);
  gtk_widget_set_hexpand (label, TRUE);
  if (it->unread > 0)
    gtk_widget_add_css_class (label, "heading");
  gtk_box_append (GTK_BOX (box), label);

  if (it->unread > 0)
    {
      char buf[16];
      g_snprintf (buf, sizeof buf, "%d", it->unread);
      GtkWidget *badge = gtk_label_new (buf);
      gtk_widget_add_css_class (badge, "numeric");
      gtk_widget_add_css_class (badge, "caption");
      gtk_widget_add_css_class (badge, "dim-label");
      gtk_box_append (GTK_BOX (box), badge);
    }

  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);

  if (it->subtitle != NULL && it->subtitle[0] != '\0')
    {
      g_autofree char *tip = g_strdup_printf ("%s messages", it->subtitle);
      gtk_widget_set_tooltip_text (row, tip);
    }

  return row;
}

static GtkWidget *
build_row_widget (gpointer item,
                  gpointer user_data)
{
  MailSidebarItem *it = MAIL_SIDEBAR_ITEM (item);
  MailSidebar *self = user_data;
  if (it->kind == MAIL_SIDEBAR_ITEM_ACCOUNT)
    return build_account_row (it, self);
  return build_folder_row (it);
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
                   it->account->store_backend, it->folder_id, it->account,
                   it->title);
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

  GPtrArray *folders = mail_backend_list_folders_finish (acct->store_backend, result, &error);
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

  if (acct->store_backend != NULL)
    {
      LoadFoldersCtx *ctx = g_new (LoadFoldersCtx, 1);
      ctx->self = g_object_ref (self);
      ctx->acct = acct;
      mail_backend_list_folders_async (acct->store_backend, self->cancellable,
                                       on_list_folders_done, ctx);
    }

  g_signal_emit (self, signals[SIGNAL_ACCOUNT_ADDED], 0, acct);
}

/* Reload folders for an account from its store_backend; called after a
 * sync completes so freshly-synced folders appear without a relaunch. */
void
mail_sidebar_reload_folders (MailSidebar *self,
                             MailAccount *acct)
{
  g_return_if_fail (MAIL_IS_SIDEBAR (self));
  g_return_if_fail (acct != NULL);
  if (acct->store_backend == NULL)
    return;

  /* Remove existing folder rows for this account. They sit between the
   * account row and the next account row (or the end). */
  guint anchor = find_account_index (self, acct);
  if (anchor == G_MAXUINT)
    return;
  guint i = anchor + 1;
  while (i < g_list_model_get_n_items (G_LIST_MODEL (self->store)))
    {
      g_autoptr (MailSidebarItem) it = g_list_model_get_item (G_LIST_MODEL (self->store), i);
      if (it->kind != MAIL_SIDEBAR_ITEM_FOLDER || it->account != acct)
        break;
      g_list_store_remove (self->store, i);
    }

  LoadFoldersCtx *ctx = g_new (LoadFoldersCtx, 1);
  ctx->self = g_object_ref (self);
  ctx->acct = acct;
  mail_backend_list_folders_async (acct->store_backend, self->cancellable,
                                   on_list_folders_done, ctx);
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
                                                  4,
                                                  G_TYPE_POINTER,
                                                  G_TYPE_STRING,
                                                  G_TYPE_POINTER,
                                                  G_TYPE_STRING);
  signals[SIGNAL_ACCOUNT_ADDED] = g_signal_new ("account-added",
                                                G_TYPE_FROM_CLASS (klass),
                                                G_SIGNAL_RUN_LAST,
                                                0, NULL, NULL, NULL,
                                                G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[SIGNAL_REFRESH_REQUESTED] = g_signal_new ("refresh-requested",
                                                    G_TYPE_FROM_CLASS (klass),
                                                    G_SIGNAL_RUN_LAST,
                                                    0, NULL, NULL, NULL,
                                                    G_TYPE_NONE, 1, G_TYPE_POINTER);
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
