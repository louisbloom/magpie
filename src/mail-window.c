/* mail-window.c - AdwApplicationWindow with OverlaySplitView + NavigationView. */

#include "config.h"

#include "mail-account-page.h"
#include "mail-message-list.h"
#include "mail-message-view.h"
#include "mail-sidebar.h"
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
  AdwToggleGroup *view_mode_group;

  /* Added programmatically. */
  MailAccountPage *account_page;

  /* Sync orchestration. */
  MailAccount *current_account;      /* borrowed; the account currently driving the right pane */
  char *current_folder_id;           /* g_strdup'd; folder displayed in message-list (or NULL) */
  gboolean account_mode;             /* TRUE when the sidebar selection is an account row */
  GCancellable *current_pass_cancel; /* ref'd; set while a sync pass is in flight */
  MailAccount *current_pass_account; /* borrowed; account the in-flight pass belongs to */
};

G_DEFINE_FINAL_TYPE (MailWindow, mail_window, ADW_TYPE_APPLICATION_WINDOW)

/* Forward decls. */
static void recompute_nav_stack (MailWindow *self);
static void on_account_added (MailSidebar *sidebar, MailAccount *acct, gpointer user_data);
static void on_account_selected (MailSidebar *sidebar, MailAccount *acct, gpointer user_data);
static void on_sync_requested (MailAccountPage *page, gpointer user_data);
static void on_sync_running_notify (GObject *src, GParamSpec *pspec, gpointer user_data);
static void schedule_sidebar_width_update (MailWindow *self);

/* Hard floor (libadwaita's default min) and a cap so a runaway folder
 * name can't eat the content pane. The HIG defaults documented in
 * libadwaita's AdwOverlaySplitView reference are fraction=0.25,
 * min=180sp, max=280sp; we override at runtime so the sidebar is a
 * content-driven fixed pixel size that doesn't grow with window width
 * and doesn't shrink with it either. */
#define SIDEBAR_MIN_FLOOR_PX 240
#define SIDEBAR_MAX_CAP_PX 480

/* --- nav stack shape --------------------------------------------- */

static gboolean
account_is_syncing (MailAccount *acct)
{
  return acct != NULL && acct->sync != NULL && mail_sync_is_running (acct->sync);
}

static void
recompute_nav_stack (MailWindow *self)
{
  /* The account page is shown when the sidebar selection is an account
   * row, OR when the current folder's account is in the middle of a
   * sync pass (auto-swap from message-list to account page so the user
   * sees progress instead of stale list data during the pass). */
  if (self->account_mode || account_is_syncing (self->current_account))
    {
      const char *tags[] = { "account" };
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
  /* Re-activating the already-selected folder (e.g. clicking Inbox
   * while a message is open) means "go back to the list view".
   * recompute_nav_stack pops any pushed message viewer; skip the
   * reload — the list already shows this folder, and refetching
   * would just churn the backend and lose the scroll position. */
  if (self->current_account == acct && !self->account_mode && g_strcmp0 (self->current_folder_id, folder_id) == 0)
    {
      recompute_nav_stack (self);
      return;
    }
  self->current_account = acct;
  g_free (self->current_folder_id);
  self->current_folder_id = g_strdup (folder_id);
  self->account_mode = FALSE;
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
    mail_message_list_load (self->message_list, backend, folder_id);
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
on_account_selected (MailSidebar *sidebar,
                     MailAccount *acct,
                     gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  if (acct == NULL)
    return;
  self->current_account = acct;
  g_clear_pointer (&self->current_folder_id, g_free);
  self->account_mode = TRUE;
  /* Bind the account page: pass acct->sync if a pass is in flight for
   * this account, else NULL for the idle render. */
  MailSync *sync_for_page = account_is_syncing (acct) ? acct->sync : NULL;
  GCancellable *cancel_for_page = (sync_for_page != NULL && self->current_pass_account == acct)
                                      ? self->current_pass_cancel
                                      : NULL;
  mail_account_page_set_state (self->account_page, sync_for_page, cancel_for_page,
                               acct->identity, acct->provider_name);
  /* Update the right pane's titlebar to reflect the account selection. */
  if (acct->identity != NULL)
    {
      adw_navigation_page_set_title (self->message_list_page, acct->identity);
      adw_window_title_set_title (self->message_list_title, acct->identity);
      adw_window_title_set_subtitle (self->message_list_title, "");
    }
  recompute_nav_stack (self);
}

static void
on_account_added (MailSidebar *sidebar,
                  MailAccount *acct,
                  gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  /* Recompute sidebar width whenever the row set changes so a freshly-
   * added account with a long identity grows the sidebar to fit. */
  schedule_sidebar_width_update (self);
  if (acct == NULL || acct->sync == NULL)
    return;
  /* Use connect_object so the handler is auto-dropped if either object
   * is finalized first; avoids dangling on shutdown order. */
  g_signal_connect_object (acct->sync, "notify::running",
                           G_CALLBACK (on_sync_running_notify),
                           self, 0);
}

static gboolean
update_sidebar_width_idle (gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  if (self->split_view == NULL || self->sidebar == NULL)
    return G_SOURCE_REMOVE;
  int min = 0, nat = 0;
  gtk_widget_measure (GTK_WIDGET (self->sidebar), GTK_ORIENTATION_HORIZONTAL, -1,
                      &min, &nat, NULL, NULL);
  int target = CLAMP (nat, SIDEBAR_MIN_FLOOR_PX, SIDEBAR_MAX_CAP_PX);
  /* Pin both min and max to the same value so AdwOverlaySplitView's
   * fraction-times-window-width calculation is clamped to that exact
   * width — the sidebar stays the same size whether the window is
   * fullscreen or small, and it grows only when content demands it. */
  adw_overlay_split_view_set_max_sidebar_width (self->split_view, target);
  adw_overlay_split_view_set_min_sidebar_width (self->split_view, target);
  return G_SOURCE_REMOVE;
}

static void
schedule_sidebar_width_update (MailWindow *self)
{
  /* Defer to idle so the row widget is realised before we measure. */
  g_idle_add (update_sidebar_width_idle, self);
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
    {
      mail_sidebar_reload_folders (self->sidebar, acct);
      /* A new folder with a longer-than-current name may have arrived
       * — re-measure so the sidebar grows to fit. */
      schedule_sidebar_width_update (self);
    }

  /* If the just-synced account's folder is what the user is looking at,
   * re-load the message-list so newly-arrived messages appear at the
   * top without requiring a manual reselect. */
  if (acct != NULL && acct == self->current_account && self->current_folder_id != NULL && acct->store_backend != NULL && !account_is_syncing (acct))
    mail_message_list_load (self->message_list, acct->store_backend,
                            self->current_folder_id);

  /* notify::running -> FALSE will have fired before us; the stack swap
   * happens in that handler, so nothing else to do here. */
}

static void
on_sync_requested (MailAccountPage *page,
                   gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  /* The page only emits this when it's visible — i.e. when the user
   * has the account page in the right pane — which means an account
   * was selected, which means self->current_account is set. */
  MailAccount *acct = self->current_account;
  if (acct == NULL || acct->sync == NULL || acct->remote_backend == NULL || acct->store == NULL)
    return;
  if (mail_sync_is_running (acct->sync))
    return;

  /* Set the account page state to point at this sync. The page is
   * the same widget instance every time; we just rebind its state. */
  g_clear_object (&self->current_pass_cancel);
  self->current_pass_cancel = g_cancellable_new ();
  self->current_pass_account = acct;
  mail_account_page_set_state (self->account_page,
                               acct->sync,
                               self->current_pass_cancel,
                               acct->identity,
                               acct->provider_name);

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
  /* We only react if the sync is for the currently-shown account. */
  if (self->current_account == NULL || self->current_account->sync != MAIL_SYNC (src))
    return;
  /* If a sync just started on the current account and the user is in
   * folder-mode, hop the sidebar selection over to the account row.
   * That selection triggers on_account_selected, which rebinds the
   * account page and calls recompute_nav_stack. */
  if (mail_sync_is_running (MAIL_SYNC (src)) && !self->account_mode)
    mail_sidebar_select_account (self->sidebar, self->current_account);
  else
    recompute_nav_stack (self);
}

/* --- toggle plumbing --------------------------------------------- */

/* MailMessageViewMode ↔ "rendered"|"plain"|"source" string name on the
 * AdwToggleGroup. Bidirectional binding (the user clicks the toggle,
 * we hear it; we set view-mode after a load, the toggle highlights). */
static gboolean
view_mode_to_name (GBinding *binding,
                   const GValue *from,
                   GValue *to,
                   gpointer user_data)
{
  switch (g_value_get_enum (from))
    {
    case MAIL_MESSAGE_VIEW_MODE_RENDERED:
      g_value_set_string (to, "rendered");
      break;
    case MAIL_MESSAGE_VIEW_MODE_PLAIN:
      g_value_set_string (to, "plain");
      break;
    case MAIL_MESSAGE_VIEW_MODE_SOURCE:
      g_value_set_string (to, "source");
      break;
    default:
      g_value_set_string (to, "rendered");
      break;
    }
  return TRUE;
}

static gboolean
name_to_view_mode (GBinding *binding,
                   const GValue *from,
                   GValue *to,
                   gpointer user_data)
{
  const char *name = g_value_get_string (from);
  if (g_strcmp0 (name, "plain") == 0)
    g_value_set_enum (to, MAIL_MESSAGE_VIEW_MODE_PLAIN);
  else if (g_strcmp0 (name, "source") == 0)
    g_value_set_enum (to, MAIL_MESSAGE_VIEW_MODE_SOURCE);
  else
    g_value_set_enum (to, MAIL_MESSAGE_VIEW_MODE_RENDERED);
  return TRUE;
}

/* The Plain toggle is sensitive only when the message has a
 * text/plain alternative. Tracked via the view's has-plain-part
 * notify so it updates on every load. */
static void
on_has_plain_part_notify (GObject *src, GParamSpec *pspec, gpointer user_data)
{
  MailWindow *self = MAIL_WINDOW (user_data);
  gboolean has_plain = FALSE;
  g_object_get (src, "has-plain-part", &has_plain, NULL);
  AdwToggle *plain = adw_toggle_group_get_toggle_by_name (self->view_mode_group, "plain");
  if (plain != NULL)
    adw_toggle_set_enabled (plain, has_plain);
}

/* --- GObject ----------------------------------------------------- */

static void
mail_window_dispose (GObject *object)
{
  MailWindow *self = MAIL_WINDOW (object);
  if (self->current_pass_cancel != NULL)
    g_cancellable_cancel (self->current_pass_cancel);
  g_clear_object (&self->current_pass_cancel);
  g_clear_pointer (&self->current_folder_id, g_free);
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
                                               "/org/gnome/Magpie/ui/window.ui");
  gtk_widget_class_bind_template_child (widget_class, MailWindow, split_view);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, nav_view);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, sidebar_toggle);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, sidebar);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_list);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_view);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_list_page);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_list_title);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, message_view_page);
  gtk_widget_class_bind_template_child (widget_class, MailWindow, view_mode_group);
}

static void
mail_window_init (MailWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  g_object_bind_property (self->split_view, "show-sidebar",
                          self->sidebar_toggle, "active",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  g_object_bind_property_full (self->message_view, "view-mode",
                               self->view_mode_group, "active-name",
                               G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE,
                               view_mode_to_name, name_to_view_mode,
                               NULL, NULL);
  g_signal_connect (self->message_view, "notify::has-plain-part",
                    G_CALLBACK (on_has_plain_part_notify), self);
  on_has_plain_part_notify (G_OBJECT (self->message_view), NULL, self);

  /* Add the account page to the nav view so it can be swapped in via
   * adw_navigation_view_replace_with_tags. It is never shown as the
   * initial page. */
  self->account_page = MAIL_ACCOUNT_PAGE (mail_account_page_new ());
  adw_navigation_view_add (self->nav_view, ADW_NAVIGATION_PAGE (self->account_page));

  g_signal_connect (self->sidebar, "folder-selected",
                    G_CALLBACK (on_folder_selected), self);
  g_signal_connect (self->sidebar, "account-selected",
                    G_CALLBACK (on_account_selected), self);
  g_signal_connect (self->message_list, "message-activated",
                    G_CALLBACK (on_message_activated), self);
  g_signal_connect (self->sidebar, "account-added",
                    G_CALLBACK (on_account_added), self);
  g_signal_connect (self->account_page, "sync-requested",
                    G_CALLBACK (on_sync_requested), self);
}

MailWindow *
mail_window_new (MailApplication *app)
{
  return g_object_new (MAIL_TYPE_WINDOW,
                       "application", app,
                       NULL);
}
