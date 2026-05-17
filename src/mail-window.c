/* mail-window.c - AdwApplicationWindow with OverlaySplitView + NavigationView. */

#include "config.h"

#include "mail-message-list.h"
#include "mail-message-view.h"
#include "mail-sidebar.h"
#include "mail-sync-progress-page.h"
#include "mail-window.h"

struct _MailWindow
{
  AdwApplicationWindow parent_instance;

  /* template children — borrowed pointers, owned by the template. */
  AdwOverlaySplitView *split_view;
  AdwNavigationView *nav_view;
  GtkToggleButton *sidebar_toggle;
  MailSidebar *sidebar;
  MailMessageList *message_list;
  MailMessageView *message_view;
  AdwNavigationPage *message_list_page;
  AdwWindowTitle *message_list_title;
  AdwNavigationPage *message_view_page;
  GtkToggleButton *plain_toggle;

  /* Added programmatically. */
  MailSyncProgressPage *progress_page;

  /* Sync orchestration. */
  MailAccount *current_account;      /* borrowed; the account currently driving the right pane */
  GCancellable *current_pass_cancel; /* ref'd; set while a sync pass is in flight */
  MailAccount *current_pass_account; /* borrowed; account the in-flight pass belongs to */
};

G_DEFINE_FINAL_TYPE (MailWindow, mail_window, ADW_TYPE_APPLICATION_WINDOW)

/* Forward decls. */
static void recompute_nav_stack (MailWindow *self);
static void on_account_added (MailSidebar *sidebar, MailAccount *acct, gpointer user_data);
static void on_refresh_requested (MailSidebar *sidebar, MailAccount *acct, gpointer user_data);
static void on_sync_running_notify (GObject *src, GParamSpec *pspec, gpointer user_data);

/* --- nav stack shape --------------------------------------------- */

static gboolean
account_is_syncing (MailAccount *acct)
{
  return acct != NULL && acct->sync != NULL && mail_sync_is_running (acct->sync);
}

static void
recompute_nav_stack (MailWindow *self)
{
  if (account_is_syncing (self->current_account))
    {
      const char *tags[] = { "sync-progress" };
      adw_navigation_view_replace_with_tags (self->nav_view, tags, 1);
    }
  else
    {
      const char *tags[] = { "message-list" };
      adw_navigation_view_replace_with_tags (self->nav_view, tags, 1);
    }
}

/* --- sidebar handlers -------------------------------------------- */

static void
on_folder_selected (MailSidebar *sidebar,
                    gpointer backend_ptr,
                    const char *folder_id,
                    gpointer account_ptr,
                    const char *folder_display_name,
                    gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  MailBackend *backend = backend_ptr;
  MailAccount *acct = account_ptr;
  if (backend == NULL || folder_id == NULL)
    return;
  self->current_account = acct;
  /* Reflect the selected folder + account in the content header. The
   * AdwWindowTitle gives us a two-line layout: folder on top,
   * account identity (email) below. The AdwNavigationPage::title is
   * kept in sync as the single-line folder name — that's what gets
   * read for accessibility and the back-button label when the split
   * view collapses to a navigation stack. */
  const char *folder_title = (folder_display_name != NULL && folder_display_name[0] != '\0')
                                 ? folder_display_name
                                 : "Mail";
  const char *account_subtitle = (acct != NULL && acct->identity != NULL) ? acct->identity : "";
  adw_navigation_page_set_title (self->message_list_page, folder_title);
  adw_window_title_set_title (self->message_list_title, folder_title);
  adw_window_title_set_subtitle (self->message_list_title, account_subtitle);
  recompute_nav_stack (self);
  /* Only load if we're showing the list — otherwise the load would
   * fire into the message-list widget while the user can't see it. */
  if (!account_is_syncing (acct))
    mail_message_list_load (self->message_list, backend, folder_id, 50);
}

static void
on_message_activated (MailMessageList *list,
                      gpointer backend_ptr,
                      const char *message_id,
                      const char *subject,
                      gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  MailBackend *backend = (MailBackend *) backend_ptr;
  if (backend == NULL || message_id == NULL)
    return;
  /* If the user pulled refresh while a message was open elsewhere, the
   * progress page is shown — don't allow drilling into a message there. */
  if (account_is_syncing (self->current_account))
    return;
  adw_navigation_page_set_title (self->message_view_page,
                                 (subject != NULL && subject[0] != '\0') ? subject : "Message");
  adw_navigation_view_push_by_tag (self->nav_view, "message-view");
  mail_message_view_load (self->message_view, backend, message_id);
}

static void
on_account_added (MailSidebar *sidebar,
                  MailAccount *acct,
                  gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  if (acct == NULL || acct->sync == NULL)
    return;
  /* Use connect_object so the handler is auto-dropped if either object
   * is finalized first; avoids dangling on shutdown order. */
  g_signal_connect_object (acct->sync, "notify::running",
                           G_CALLBACK (on_sync_running_notify),
                           self, 0);
}

static void
on_sync_done (GObject *src,
              GAsyncResult *res,
              gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  GError *error = NULL;
  gboolean ok = mail_sync_run_finish (MAIL_SYNC (src), res, &error);

  MailAccount *acct = self->current_pass_account;
  g_clear_object (&self->current_pass_cancel);
  self->current_pass_account = NULL;

  if (!ok && error != NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("sync failed: %s", error->message);
  g_clear_error (&error);

  /* Re-list folders for the synced account so freshly-arrived folders/
   * messages appear immediately. */
  if (acct != NULL)
    mail_sidebar_reload_folders (self->sidebar, acct);

  /* notify::running -> FALSE will have fired before us; the stack swap
   * happens in that handler, so nothing else to do here. */
}

static void
on_refresh_requested (MailSidebar *sidebar,
                      MailAccount *acct,
                      gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  if (acct == NULL || acct->sync == NULL || acct->remote_backend == NULL || acct->store == NULL)
    return;
  if (mail_sync_is_running (acct->sync))
    return;

  /* Set the progress page state to point at this sync. The page is
   * the same widget instance every time; we just rebind its state. */
  g_clear_object (&self->current_pass_cancel);
  self->current_pass_cancel = g_cancellable_new ();
  self->current_pass_account = acct;
  mail_sync_progress_page_set_state (self->progress_page,
                                     acct->sync,
                                     self->current_pass_cancel,
                                     acct->identity);

  mail_sync_run_async (acct->sync, acct->remote_backend, acct->store,
                       self->current_pass_cancel,
                       on_sync_done, self);
}

static void
on_sync_running_notify (GObject *src,
                        GParamSpec *pspec,
                        gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  /* Find which account this sync belongs to among the sidebar's. We
   * don't strictly need to know — we only act if it's the current one. */
  if (self->current_account != NULL && self->current_account->sync == MAIL_SYNC (src))
    recompute_nav_stack (self);
}

/* --- toggle plumbing --------------------------------------------- */

static void
on_plain_toggle_active_changed (GObject *object,
                                GParamSpec *pspec,
                                gpointer user_data)
{
  GtkToggleButton *btn = GTK_TOGGLE_BUTTON (object);
  gboolean active = gtk_toggle_button_get_active (btn);
  gtk_button_set_icon_name (GTK_BUTTON (btn),
                            active ? "text-x-generic-symbolic"
                                   : "view-paged-symbolic");
}

/* --- GObject ----------------------------------------------------- */

static void
mail_window_dispose (GObject *object)
{
  MailWindow *self = MAIL_WINDOW (object);
  if (self->current_pass_cancel != NULL)
    g_cancellable_cancel (self->current_pass_cancel);
  g_clear_object (&self->current_pass_cancel);
  G_OBJECT_CLASS (mail_window_parent_class)->dispose (object);
}

static void
mail_window_class_init (MailWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = mail_window_dispose;

  /* Register custom widget types so the template parser can find them. */
  g_type_ensure (MAIL_TYPE_SIDEBAR);
  g_type_ensure (MAIL_TYPE_MESSAGE_LIST);
  g_type_ensure (MAIL_TYPE_MESSAGE_VIEW);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/Mail/ui/window.ui");
  gtk_widget_class_bind_template_child (widget_class, MailWindow, split_view);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, nav_view);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, sidebar_toggle);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, sidebar);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_list);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_view);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_list_page);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_list_title);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_view_page);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, plain_toggle);
}

static void
mail_window_init (MailWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  g_object_bind_property (self->split_view, "show-sidebar",
                          self->sidebar_toggle, "active",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  g_object_bind_property (self->message_view, "has-plain-part",
                          self->plain_toggle, "sensitive",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->message_view, "show-plain",
                          self->plain_toggle, "active",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  g_signal_connect (self->plain_toggle, "notify::active",
                    G_CALLBACK (on_plain_toggle_active_changed), self);

  /* Add the sync-progress page to the nav view so it can be swapped
   * in via adw_navigation_view_replace_with_tags. It is never shown
   * as the initial page. */
  self->progress_page = MAIL_SYNC_PROGRESS_PAGE (mail_sync_progress_page_new ());
  adw_navigation_view_add (self->nav_view, ADW_NAVIGATION_PAGE (self->progress_page));

  g_signal_connect (self->sidebar, "folder-selected",
                    G_CALLBACK (on_folder_selected), self);
  g_signal_connect (self->message_list, "message-activated",
                    G_CALLBACK (on_message_activated), self);
  g_signal_connect (self->sidebar, "account-added",
                    G_CALLBACK (on_account_added), self);
  g_signal_connect (self->sidebar, "refresh-requested",
                    G_CALLBACK (on_refresh_requested), self);
}

MailWindow *
mail_window_new (MailApplication *app)
{
  return g_object_new (MAIL_TYPE_WINDOW,
                       "application", app,
                       NULL);
}
